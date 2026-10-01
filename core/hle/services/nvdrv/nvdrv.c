/**
 * nvdrv service: IPC commands, the device table and every device's
 * ioctls. See nvdrv.h for scope.
 */
#include "hle/services/nvdrv/nvdrv.h"

#include "common/log.h"
#include "hle/hle.h"

#include <string.h>

/* ---- ioctl encoding (Linux ioctl.h, as libnx nvidia/ioctl.h) ------- */
#define NV_IOC_NR(r) ((r) & 0xFFu)
#define NV_IOC_TYPE(r) (((r) >> 8) & 0xFFu)
#define NV_IOC_SIZE(r) (((r) >> 16) & 0x3FFFu)
#define NV_IOC_DIR(r) ((r) >> 30)
#define NV_IOC_WRITE 1u /* data flows guest -> driver */
#define NV_IOC_READ 2u  /* driver -> guest */

#define NV_TYPE_CTRL 0x00u    /* nvhost-ctrl and the host1x channel-common set */
#define NV_TYPE_NVMAP 0x01u
#define NV_TYPE_AS_GPU 0x41u
#define NV_TYPE_CTRL_GPU 0x47u
#define NV_TYPE_GPU 0x48u

#define NVMAP_PARAM_SIZE 1u
#define NVMAP_PARAM_ALIGNMENT 2u
#define NVMAP_PARAM_BASE 3u
#define NVMAP_PARAM_HEAP 4u
#define NVMAP_PARAM_KIND 5u
#define NVMAP_PARAM_COMPR 6u
#define NVMAP_HEAP_IOVMM 0x40000000u
#define NVMAP_MIN_ALIGN 0x1000u

#define GPU_VA_BASE 0x04000000ull        /* the small-page VA region start */
#define GPU_BIG_PAGE_SIZE 0x20000u
#define GPU_SMALL_PAGE_SIZE 0x1000u
#define SUBMIT_FLAG_FENCE_GET (1u << 1)

/* GM20B (Tegra X1) facts returned by GET_CHARACTERISTICS. */
#define GM20B_ARCH 0x120u
#define GM20B_IMPL 0xBu
#define GM20B_REV 0xA1u
#define GM20B_CLASS_2D 0x902Du
#define GM20B_CLASS_3D 0xB197u
#define GM20B_CLASS_COMPUTE 0xB1C0u
#define GM20B_CLASS_GPFIFO 0xB06Fu
#define GM20B_CLASS_INLINE_TO_MEMORY 0xA140u
#define GM20B_CLASS_DMA_COPY 0xB0B5u
#define GM20B_L2_BYTES 0x40000u
#define GM20B_ZCULL_CTX_BYTES 0x16000u
#define CHARACTERISTICS_BYTES 0xA0u

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static Nvdrv_State *state_of(Service_Object *self) { return (Nvdrv_State *)self->interface->service_state; }

static Nv_Fd *fd_of(Nvdrv_State *s, uint32_t fd) {
  if (fd >= NVDRV_MAX_FDS || s->fds[fd].device == NV_DEVICE_NONE) return NULL;
  return &s->fds[fd];
}

static Nvmap_Handle *nvmap_of(Nvdrv_State *s, uint32_t handle) {
  if (handle == 0 || handle > NVMAP_MAX_HANDLES || !s->handles[handle - 1u].references) return NULL;
  return &s->handles[handle - 1u];
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

/* Guest buffer descriptor i: A/X for input, B/C for output (libnx's
 * HipcAutoSelect sends the unused one with size 0). */
static const IPC_Buffer *in_buffer(const IPC_Request *req, uint32_t i) {
  if (i < req->send_count && req->sends[i].size) return &req->sends[i];
  if (i < req->static_count && req->statics[i].size) return &req->statics[i];
  return i < req->send_count ? &req->sends[i] : NULL;
}
static const IPC_Buffer *out_buffer(const IPC_Request *req, uint32_t i) {
  if (i < req->receive_count && req->receives[i].size) return &req->receives[i];
  if (i < req->receive_list_count && req->receive_list[i].size) return &req->receive_list[i];
  return i < req->receive_count ? &req->receives[i] : NULL;
}

/* ------------------------------------------------------------------ */
/* /dev/nvmap                                                          */
/* ------------------------------------------------------------------ */

static uint32_t nvmap_ioctl(Nvdrv_State *s, uint32_t nr, uint8_t *d) {
  switch (nr) {
  case 0x01: { /* CREATE {u32 size; u32 handle out} */
    const uint32_t size = rd32(d);
    if (size == 0) return NV_BAD_VALUE;
    for (uint32_t i = 0; i < NVMAP_MAX_HANDLES; i++) {
      Nvmap_Handle *h = &s->handles[i];
      if (h->references) continue;
      memset(h, 0, sizeof(*h));
      h->references = 1;
      h->size = (uint32_t)align_up(size, NVMAP_MIN_ALIGN);
      wr32(d + 4, i + 1u);
      return NV_SUCCESS;
    }
    return NV_INSUFFICIENT_MEMORY;
  }
  case 0x03: { /* FROM_ID {u32 id; u32 handle out}: ids are handles */
    Nvmap_Handle *h = nvmap_of(s, rd32(d));
    if (!h) return NV_BAD_VALUE;
    h->references++;
    wr32(d + 4, rd32(d));
    return NV_SUCCESS;
  }
  case 0x04: { /* ALLOC {handle, heapmask, flags, align, u8 kind, pad[7], u64 addr} */
    Nvmap_Handle *h = nvmap_of(s, rd32(d));
    if (!h) return NV_BAD_VALUE;
    if (h->allocated) return NV_ALREADY_ALLOCATED;
    uint32_t align = rd32(d + 12);
    if (align < NVMAP_MIN_ALIGN) align = NVMAP_MIN_ALIGN;
    if (align & (align - 1u)) return NV_BAD_VALUE;
    h->heap_mask = rd32(d + 4);
    h->flags = rd32(d + 8);
    h->align = align;
    h->kind = d[16];
    h->address = rd64(d + 24);
    h->allocated = true;
    return NV_SUCCESS;
  }
  case 0x05: { /* FREE {u32 handle; pad; u64 address out; u32 size out; u32 flags out} */
    Nvmap_Handle *h = nvmap_of(s, rd32(d));
    if (!h) return NV_BAD_VALUE;
    wr64(d + 8, h->address);
    wr32(d + 16, h->size);
    h->references--;
    wr32(d + 20, h->references ? 1u : 0u); /* 1 = not freed yet (still referenced) */
    if (!h->references) memset(h, 0, sizeof(*h));
    return NV_SUCCESS;
  }
  case 0x09: { /* PARAM {handle, param, result out} */
    Nvmap_Handle *h = nvmap_of(s, rd32(d));
    if (!h) return NV_BAD_VALUE;
    uint32_t value;
    switch (rd32(d + 4)) {
    case NVMAP_PARAM_SIZE: value = h->size; break;
    case NVMAP_PARAM_ALIGNMENT: value = h->align; break;
    case NVMAP_PARAM_BASE: value = 0; break;
    case NVMAP_PARAM_HEAP: value = h->allocated ? NVMAP_HEAP_IOVMM : 0; break;
    case NVMAP_PARAM_KIND: value = h->kind; break;
    case NVMAP_PARAM_COMPR: value = 0; break;
    default: return NV_BAD_VALUE;
    }
    wr32(d + 8, value);
    return NV_SUCCESS;
  }
  case 0x0E: { /* GET_ID {u32 id out; u32 handle} */
    if (!nvmap_of(s, rd32(d + 4))) return NV_BAD_VALUE;
    wr32(d, rd32(d + 4));
    return NV_SUCCESS;
  }
  default:
    return NV_NOT_IMPLEMENTED;
  }
}

/* ------------------------------------------------------------------ */
/* /dev/nvhost-ctrl                                                    */
/* ------------------------------------------------------------------ */

static uint32_t ctrl_ioctl(Nvdrv_State *s, uint32_t nr, uint8_t *d) {
  Syncpoints *sp = &s->syncpoints;
  switch (nr) {
  case 0x14: /* SYNCPT_READ {id; value out} */
    if (rd32(d) >= SYNCPOINT_COUNT) return NV_BAD_VALUE;
    wr32(d + 4, sp->min[rd32(d)]);
    return NV_SUCCESS;
  case 0x15: { /* SYNCPT_INCR {id} */
    const uint32_t id = rd32(d);
    if (id >= SYNCPOINT_COUNT) return NV_BAD_VALUE;
    syncpoint_complete(sp, id, syncpoint_increment_max(sp, id));
    return NV_SUCCESS;
  }
  case 0x16: /* SYNCPT_WAIT {id, threshold, timeout} */
  case 0x19: { /* SYNCPT_WAIT_EX {id, threshold, timeout, value inout} */
    const uint32_t id = rd32(d);
    if (id >= SYNCPOINT_COUNT) return NV_BAD_VALUE;
    if (nr == 0x19) wr32(d + 12, sp->min[id]);
    return syncpoint_reached(sp, id, rd32(d + 4)) ? NV_SUCCESS : NV_TIMEOUT;
  }
  case 0x1D: { /* EVENT_WAIT {id, threshold, timeout, value inout} */
    const uint32_t id = rd32(d);
    if (id >= SYNCPOINT_COUNT) return NV_BAD_VALUE;
    wr32(d + 12, sp->min[id]);
    return syncpoint_reached(sp, id, rd32(d + 4)) ? NV_SUCCESS : NV_TIMEOUT;
  }
  case 0x1E: { /* EVENT_WAIT_ASYNC {id, threshold, timeout, event_id} */
    const uint32_t id = rd32(d), threshold = rd32(d + 4), event_id = rd32(d + 12);
    if (id >= SYNCPOINT_COUNT || event_id >= NVDRV_MAX_EVENTS) return NV_BAD_VALUE;
    if (syncpoint_reached(sp, id, threshold)) return NV_SUCCESS;
    s->events[event_id].waiting = true;
    s->events[event_id].syncpoint = id;
    s->events[event_id].threshold = threshold;
    return NV_TIMEOUT; /* "try again"; the event is signalled on completion */
  }
  case 0x1C: case 0x1F: case 0x20: /* SYNCPT_CLEAR_EVENT_WAIT, EVENT_REGISTER, EVENT_UNREGISTER {event_id} */
    if (rd32(d) >= NVDRV_MAX_EVENTS) return NV_BAD_VALUE;
    if (nr != 0x1F) s->events[rd32(d)].waiting = false;
    return NV_SUCCESS;
  case 0x1B: /* GET_CONFIG: no configuration keys exist here */
    return NV_NOT_SUPPORTED;
  default:
    return NV_NOT_IMPLEMENTED;
  }
}

/* ------------------------------------------------------------------ */
/* /dev/nvhost-ctrl-gpu                                                */
/* ------------------------------------------------------------------ */

/* nvioctl_gpu_characteristics, libnx nvidia/ioctl.h - GM20B values. */
static void fill_characteristics(uint8_t *c) {
  memset(c, 0, CHARACTERISTICS_BYTES);
  wr32(c + 0x00, GM20B_ARCH);
  wr32(c + 0x04, GM20B_IMPL);
  wr32(c + 0x08, GM20B_REV);
  wr32(c + 0x0C, 1);                   /* num_gpc */
  wr64(c + 0x10, GM20B_L2_BYTES);      /* L2_cache_size */
  wr64(c + 0x18, 0);                   /* on_board_video_memory_size */
  wr32(c + 0x20, 2);                   /* num_tpc_per_gpc */
  wr32(c + 0x24, 0x20);                /* bus_type: AXI */
  wr32(c + 0x28, GPU_BIG_PAGE_SIZE);   /* big_page_size */
  wr32(c + 0x2C, GPU_BIG_PAGE_SIZE);   /* compression_page_size */
  wr32(c + 0x30, 0x1B);                /* pde_coverage_bit_count */
  wr32(c + 0x34, 0x30000);             /* available_big_page_sizes */
  wr32(c + 0x38, 1);                   /* gpc_mask */
  wr32(c + 0x3C, 0x503);               /* sm_arch_sm_version */
  wr32(c + 0x40, 0x503);               /* sm_arch_spa_version */
  wr32(c + 0x44, 0x80);                /* sm_arch_warp_count */
  wr32(c + 0x48, 0x28);                /* gpu_va_bit_count */
  wr64(c + 0x50, 0x55);                /* flags */
  wr32(c + 0x58, GM20B_CLASS_2D);
  wr32(c + 0x5C, GM20B_CLASS_3D);
  wr32(c + 0x60, GM20B_CLASS_COMPUTE);
  wr32(c + 0x64, GM20B_CLASS_GPFIFO);
  wr32(c + 0x68, GM20B_CLASS_INLINE_TO_MEMORY);
  wr32(c + 0x6C, GM20B_CLASS_DMA_COPY);
  wr32(c + 0x70, 1);                   /* max_fbps_count */
  wr32(c + 0x74, 0);                   /* fbp_en_mask */
  wr32(c + 0x78, 2);                   /* max_ltc_per_fbp */
  wr32(c + 0x7C, 1);                   /* max_lts_per_ltc */
  wr32(c + 0x80, 0);                   /* max_tex_per_tpc */
  wr32(c + 0x84, 1);                   /* max_gpc_count */
  wr32(c + 0x88, 0x21D70);             /* rop_l2_en_mask_0 */
  wr32(c + 0x8C, 0);                   /* rop_l2_en_mask_1 */
  wr64(c + 0x90, 0x6230326D67ull);     /* chipname "gm20b" */
  wr64(c + 0x98, 0);                   /* gr_compbit_store_base_hw */
}

static uint32_t ctrl_gpu_ioctl(uint32_t nr, uint8_t *d, uint32_t size) {
  switch (nr) {
  case 0x01: wr32(d, GM20B_ZCULL_CTX_BYTES); return NV_SUCCESS; /* ZCULL_GET_CTX_SIZE */
  case 0x02: memset(d, 0, size); wr32(d, 0x20); wr32(d + 4, 0x20); return NV_SUCCESS; /* ZCULL_GET_INFO */
  case 0x03: case 0x04: return NV_SUCCESS;                      /* ZBC_SET/QUERY_TABLE */
  case 0x05:                                                    /* GET_CHARACTERISTICS {u64 size, u64 addr, chars} */
    wr64(d, CHARACTERISTICS_BYTES);
    if (size >= 16u + CHARACTERISTICS_BYTES) fill_characteristics(d + 16);
    return NV_SUCCESS;
  case 0x06: /* GET_TPC_MASKS {u32 buf_size; pad; u64 addr; u32 mask...} */
    if (size >= 0x18) wr32(d + 0x10, 0x3);
    return NV_SUCCESS;
  case 0x14: wr32(d, 0x07); wr32(d + 4, 0x01); return NV_SUCCESS; /* GET_ACTIVE_SLOT_MASK */
  case 0x1C: if (size >= 8) memset(d, 0, size); return NV_SUCCESS;  /* GET_GPU_TIME */
  default: return NV_NOT_IMPLEMENTED;
  }
}

/* ------------------------------------------------------------------ */
/* /dev/nvhost-as-gpu                                                  */
/* ------------------------------------------------------------------ */

static uint32_t as_gpu_ioctl(Nvdrv_State *s, uint32_t nr, uint8_t *d) {
  switch (nr) {
  case 0x01: case 0x03: case 0x09: /* BIND_CHANNEL, FREE_SPACE, INITIALIZE_EX */
    return NV_SUCCESS;
  case 0x02: { /* ALLOC_SPACE {pages, page_size, flags, pad, offset/align} */
    const uint64_t pages = rd32(d), page_size = rd32(d + 4), flags = rd32(d + 8);
    if (!page_size || (page_size & (page_size - 1u))) return NV_BAD_VALUE;
    uint64_t offset;
    if (flags & 1u) {
      offset = rd64(d + 16);
    } else {
      offset = align_up(s->next_gpu_va, page_size);
      s->next_gpu_va = offset + pages * page_size;
    }
    wr64(d + 16, offset);
    return NV_SUCCESS;
  }
  case 0x05: { /* UNMAP_BUFFER {u64 offset} */
    for (uint32_t i = 0; i < NVDRV_MAX_GPU_MAPPINGS; i++) {
      if (s->mappings[i].in_use && s->mappings[i].gpu_va == rd64(d)) {
        s->mappings[i].in_use = false;
        return NV_SUCCESS;
      }
    }
    return NV_BAD_VALUE;
  }
  case 0x06: { /* MAP_BUFFER_EX {flags, kind, handle, page_size io, buffer_offset, mapping_size, offset io} */
    const uint32_t flags = rd32(d), handle = rd32(d + 8);
    Nvmap_Handle *h = nvmap_of(s, handle);
    if (!h) return NV_BAD_VALUE;
    uint64_t size = rd64(d + 24);
    if (size == 0) size = h->size;
    uint32_t page_size = rd32(d + 12);
    if (page_size == 0) page_size = GPU_SMALL_PAGE_SIZE;
    uint64_t gpu_va;
    if (flags & 1u) {
      gpu_va = rd64(d + 32);
    } else {
      gpu_va = align_up(s->next_gpu_va, page_size);
      s->next_gpu_va = align_up(gpu_va + size, page_size);
    }
    for (uint32_t i = 0; i < NVDRV_MAX_GPU_MAPPINGS; i++) {
      Gpu_Mapping *m = &s->mappings[i];
      if (m->in_use) continue;
      *m = (Gpu_Mapping){true, gpu_va, size, handle, rd64(d + 16)};
      wr32(d + 12, page_size);
      wr64(d + 32, gpu_va);
      return NV_SUCCESS;
    }
    return NV_INSUFFICIENT_MEMORY;
  }
  case 0x08: { /* GET_VA_REGIONS {not_used, u32 bufsize io, pad, regions[2] of 24 bytes} */
    wr32(d + 8, 2u * 24u);
    uint8_t *r = d + 16;
    wr64(r, GPU_VA_BASE);
    wr32(r + 8, GPU_SMALL_PAGE_SIZE);
    wr64(r + 16, 0x3FBFFull);
    wr64(r + 24, 0x0400000000ull);
    wr32(r + 32, GPU_BIG_PAGE_SIZE);
    wr64(r + 40, 0x1BFFFull);
    return NV_SUCCESS;
  }
  default:
    return NV_NOT_IMPLEMENTED;
  }
}

/* ------------------------------------------------------------------ */
/* Channels: /dev/nvhost-gpu and the host1x clients (nvdec/vic/nvjpg).  */
/* ------------------------------------------------------------------ */

static uint32_t ensure_syncpoint(Nvdrv_State *s, Nv_Fd *f) {
  if (!f->syncpoint) f->syncpoint = syncpoint_allocate(&s->syncpoints);
  return f->syncpoint;
}

/* A submission completes at once (nvdrv.h): returns the reached fence. */
static uint32_t complete_submission(Nvdrv_State *s, Nv_Fd *f, uint32_t increments) {
  const uint32_t id = ensure_syncpoint(s, f);
  uint32_t value = s->syncpoints.max[id];
  for (uint32_t i = 0; i < increments; i++) value = syncpoint_increment_max(&s->syncpoints, id);
  syncpoint_complete(&s->syncpoints, id, value);
  f->submissions++;
  return value;
}

/* ---- GPU memory for the command processor ------------------------- */

bool nvdrv_gpu_translate(const Nvdrv_State *s, uint64_t gpu_va, uint64_t *guest_va, uint64_t *contiguous) {
  for (uint32_t i = 0; i < NVDRV_MAX_GPU_MAPPINGS; i++) {
    const Gpu_Mapping *m = &s->mappings[i];
    if (!m->in_use || gpu_va < m->gpu_va || gpu_va - m->gpu_va >= m->size) continue;
    uint64_t base = 0, size = 0;
    if (!nvdrv_nvmap_lookup(s, m->nvmap_handle, &base, &size)) return false;
    *guest_va = base + m->buffer_offset + (gpu_va - m->gpu_va);
    *contiguous = m->size - (gpu_va - m->gpu_va);
    return true;
  }
  return false;
}

static bool gpu_access(void *user, uint64_t gpu_va, void *buffer, uint64_t size, bool write) {
  const Nvdrv_State *s = (const Nvdrv_State *)user;
  for (uint64_t done = 0; done < size;) {
    uint64_t guest = 0, run = 0;
    if (!nvdrv_gpu_translate(s, gpu_va + done, &guest, &run)) return false;
    const uint64_t n = size - done < run ? size - done : run;
    const Error err = write ? vmm_write_block(s->hle->vmm, guest, (const uint8_t *)buffer + done, n)
                            : vmm_read_block(s->hle->vmm, guest, (uint8_t *)buffer + done, n);
    if (!error_is_ok(err)) return false;
    done += n;
  }
  return true;
}

static bool gpu_read(void *user, uint64_t gpu_va, void *out, uint64_t size) { return gpu_access(user, gpu_va, out, size, false); }
static bool gpu_write(void *user, uint64_t gpu_va, const void *src, uint64_t size) {
  return gpu_access(user, gpu_va, (void *)(uintptr_t)src, size, true);
}

static void gpu_syncpoint_increment(void *user, uint32_t id) {
  Nvdrv_State *s = (Nvdrv_State *)user;
  if (id == 0 || id >= SYNCPOINT_COUNT) return;
  syncpoint_complete(&s->syncpoints, id, syncpoint_increment_max(&s->syncpoints, id));
}

static Gpu_Channel *channel_of(Nvdrv_State *s, Nv_Fd *f) {
  if (!s->channels) return NULL;
  if (f->channel == NVDRV_NO_CHANNEL) {
    for (uint32_t i = 0; i < NVDRV_MAX_CHANNELS; i++) {
      if (s->channel_used[i]) continue;
      s->channel_used[i] = true;
      gpu_channel_init(&s->channels[i]);
      f->channel = i;
      break;
    }
    if (f->channel == NVDRV_NO_CHANNEL) return NULL;
  }
  return &s->channels[f->channel];
}

/* SUBMIT_GPFIFO / KICKOFF_PB: the entries follow the 24-byte header
 * (inline, or Ioctl2's extra buffer appended - run_ioctl). */
#define GPFIFO_HEADER_BYTES 24u
#define GPFIFO_MAX_ENTRIES ((NVDRV_IOCTL_MAX_BYTES - GPFIFO_HEADER_BYTES) / 8u)

static void run_gpfifo(Nvdrv_State *s, Nv_Fd *f, const uint8_t *d) {
  Gpu_Channel *channel = channel_of(s, f);
  if (!channel || !s->hle) return;
  uint32_t count = rd32(d + 8);
  if (count > GPFIFO_MAX_ENTRIES) count = GPFIFO_MAX_ENTRIES;
  uint64_t entries[GPFIFO_MAX_ENTRIES];
  memcpy(entries, d + GPFIFO_HEADER_BYTES, (size_t)count * 8u);
  const Gpu_Memory memory = {s, gpu_read, gpu_write, gpu_syncpoint_increment};
  gpu_channel_submit(channel, &memory, entries, count);
}

static uint32_t gpu_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t nr, uint8_t *d) {
  switch (nr) {
  case 0x01: f->nvmap_fd = rd32(d); return NV_SUCCESS;           /* SET_NVMAP_FD */
  case 0x03: case 0x0B: case 0x0C: case 0x0D: return NV_SUCCESS; /* SET_TIMEOUT, ZCULL_BIND, SET_ERROR_NOTIFIER, SET_PRIORITY */
  case 0x08: case 0x1B: { /* SUBMIT_GPFIFO / KICKOFF_PB {u64 gpfifo, u32 num, u32 flags, fence io} */
    const uint32_t flags = rd32(d + 12);
    run_gpfifo(s, f, d);
    const uint32_t value = complete_submission(s, f, 1);
    if (flags & SUBMIT_FLAG_FENCE_GET) {
      wr32(d + 16, f->syncpoint);
      wr32(d + 20, value);
    }
    return NV_SUCCESS;
  }
  case 0x09: wr64(d + 8, rd32(d)); return NV_SUCCESS; /* ALLOC_OBJ_CTX {class, flags, u64 obj_id out} */
  case 0x16: case 0x17: return NV_SUCCESS;            /* GET_ERROR_INFO / NOTIFICATION: none */
  case 0x1A: { /* ALLOC_GPFIFO_EX2 {num, flags, unk0, fence out{id,value}, unk1..3} */
    const uint32_t id = ensure_syncpoint(s, f);
    if (!id) return NV_INSUFFICIENT_MEMORY;
    wr32(d + 12, id);
    wr32(d + 16, s->syncpoints.max[id]);
    return NV_SUCCESS;
  }
  default:
    return NV_NOT_IMPLEMENTED;
  }
}

/* host1x channel-common ioctls (type 0x00 on nvdec/vic/nvjpg/gpu). */
static uint32_t channel_common_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t nr, uint8_t *d, uint32_t size) {
  switch (nr) {
  case 0x01: { /* SUBMIT {num_cmdbufs, num_relocs, num_syncpt_incrs, num_fences, arrays...} */
    const uint32_t cmdbufs = rd32(d), relocs = rd32(d + 4), incrs = rd32(d + 8), fences = rd32(d + 12);
    /* cmdbuf 12 bytes, reloc 16, reloc shift 4, syncpt incr 8, fence threshold 4. */
    const uint64_t incr_at = 16u + (uint64_t)cmdbufs * 12u + (uint64_t)relocs * 20u;
    const uint64_t fence_at = incr_at + (uint64_t)incrs * 8u;
    if (fence_at + (uint64_t)fences * 4u > size) return NV_BAD_PARAMETER;
    uint32_t value = 0;
    for (uint32_t i = 0; i < incrs; i++) {
      const uint32_t count = rd32(d + incr_at + i * 8u + 4u);
      value = complete_submission(s, f, count);
    }
    for (uint32_t i = 0; i < fences; i++) wr32(d + fence_at + i * 4u, value);
    return NV_SUCCESS;
  }
  case 0x02: wr32(d + 4, ensure_syncpoint(s, f)); return NV_SUCCESS; /* GET_SYNCPOINT {module; syncpt out} */
  case 0x03: wr32(d + 4, 0); return NV_SUCCESS;                      /* GET_WAITBASE */
  case 0x07: case 0x08: return NV_SUCCESS;                           /* SET_SUBMIT_TIMEOUT, SET_MODULE_CLOCK_RATE */
  case 0x14: case 0x23: wr32(d, 0); return NV_SUCCESS;               /* GET_MODULE_CLOCK_RATE {rate out, module} */
  case 0x09: { /* MAP_BUFFER {num, reserved, u8 compressed, pad, maps[]{handle, address out}} */
    const uint32_t count = rd32(d);
    if (16u + (uint64_t)count * 8u > size) return NV_BAD_PARAMETER;
    for (uint32_t i = 0; i < count; i++) {
      Nvmap_Handle *h = nvmap_of(s, rd32(d + 12 + i * 8u));
      wr32(d + 12 + i * 8u + 4u, h ? (uint32_t)h->address : 0u);
    }
    return NV_SUCCESS;
  }
  case 0x0A: return NV_SUCCESS; /* UNMAP_BUFFER */
  default: return NV_NOT_IMPLEMENTED;
  }
}

/* ------------------------------------------------------------------ */
/* Device table and ioctl dispatch.                                    */
/* ------------------------------------------------------------------ */

typedef struct Device_Path {
  const char *path;
  Nv_Device device;
} Device_Path;

static const Device_Path k_devices[] = {
    {"/dev/nvmap", NV_DEVICE_NVMAP},
    {"/dev/nvhost-ctrl", NV_DEVICE_CTRL},
    {"/dev/nvhost-ctrl-gpu", NV_DEVICE_CTRL_GPU},
    {"/dev/nvhost-as-gpu", NV_DEVICE_AS_GPU},
    {"/dev/nvhost-gpu", NV_DEVICE_GPU},
    {"/dev/nvhost-nvdec", NV_DEVICE_NVDEC},
    {"/dev/nvhost-vic", NV_DEVICE_VIC},
    {"/dev/nvhost-nvjpg", NV_DEVICE_NVJPG},
};

static uint32_t dispatch_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t request, uint8_t *data) {
  const uint32_t type = NV_IOC_TYPE(request), nr = NV_IOC_NR(request), size = NV_IOC_SIZE(request);
  s->ioctl_count++;
  switch (f->device) {
  case NV_DEVICE_NVMAP:
    return type == NV_TYPE_NVMAP ? nvmap_ioctl(s, nr, data) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_CTRL:
    return type == NV_TYPE_CTRL ? ctrl_ioctl(s, nr, data) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_CTRL_GPU:
    return type == NV_TYPE_CTRL_GPU ? ctrl_gpu_ioctl(nr, data, size) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_AS_GPU:
    return type == NV_TYPE_AS_GPU ? as_gpu_ioctl(s, nr, data) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_GPU:
    if (type == NV_TYPE_GPU) return gpu_ioctl(s, f, nr, data);
    if (type == NV_TYPE_CTRL_GPU && nr == 0x14) return NV_SUCCESS; /* SET_USER_DATA (type 0x47) */
    return type == NV_TYPE_CTRL ? channel_common_ioctl(s, f, nr, data, size) : NV_NOT_IMPLEMENTED;
  default: /* nvdec, vic, nvjpg */
    return type == NV_TYPE_CTRL ? channel_common_ioctl(s, f, nr, data, size) : NV_NOT_IMPLEMENTED;
  }
}

/* Reads the in-data, runs the ioctl, writes the out-data. `extra_in` /
 * `extra_out` are Ioctl2/Ioctl3's second buffers (appended after the
 * struct, as the kernel driver sees them). */
static uint32_t run_ioctl(HLE_Context *c, Nvdrv_State *s, const IPC_Request *req, uint32_t extra_in, uint32_t extra_out) {
  uint32_t fd = 0, request = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &fd)) || !error_is_ok(ipc_request_read_u32(req, 4, &request))) {
    return NV_BAD_PARAMETER;
  }
  Nv_Fd *f = fd_of(s, fd);
  if (!f) return NV_BAD_VALUE;
  const uint32_t size = NV_IOC_SIZE(request), dir = NV_IOC_DIR(request);
  memset(s->ioctl_buffer, 0, sizeof(s->ioctl_buffer));
  if (dir & NV_IOC_WRITE) {
    const IPC_Buffer *in = in_buffer(req, 0);
    if (!in || in->size < size || !error_is_ok(vmm_read_block(c->vmm, in->gva, s->ioctl_buffer, size))) {
      return NV_BAD_PARAMETER;
    }
  }
  uint64_t total = size;
  if (extra_in) {
    const IPC_Buffer *in = in_buffer(req, extra_in);
    const uint64_t extra = in ? in->size : 0;
    if (size + extra > sizeof(s->ioctl_buffer)) return NV_BAD_PARAMETER;
    if (extra && !error_is_ok(vmm_read_block(c->vmm, in->gva, s->ioctl_buffer + size, extra))) return NV_BAD_PARAMETER;
    total += extra;
  }
  (void)total;
  s->hle = c;
  const uint32_t error = dispatch_ioctl(s, f, request, s->ioctl_buffer);
  log_debug("[nvdrv] ioctl dev %d req %08x -> %u", (int)f->device, request, error);
  if (dir & NV_IOC_READ) {
    const IPC_Buffer *out = out_buffer(req, 0);
    if (!out || out->size < size || !error_is_ok(vmm_write_block(c->vmm, out->gva, s->ioctl_buffer, size))) {
      return NV_BAD_PARAMETER;
    }
  }
  if (extra_out) {
    const IPC_Buffer *out = out_buffer(req, extra_out);
    if (out && out->size) {
      memset(s->extra_buffer, 0, out->size < sizeof(s->extra_buffer) ? out->size : sizeof(s->extra_buffer));
      (void)vmm_write_block(c->vmm, out->gva, s->extra_buffer, out->size < sizeof(s->extra_buffer) ? out->size : sizeof(s->extra_buffer));
    }
  }
  if (error == NV_NOT_IMPLEMENTED) {
    log_warn("[nvdrv] unimplemented ioctl 0x%08x (type 0x%02x nr 0x%02x) on device %d", request,
             NV_IOC_TYPE(request), NV_IOC_NR(request), (int)f->device);
  }
  return error;
}

/* ------------------------------------------------------------------ */
/* IPC commands.                                                       */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_open(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  Nvdrv_State *s = state_of(self);
  char path[64] = {0};
  const IPC_Buffer *in = in_buffer(req, 0);
  const uint64_t length = in ? (in->size < sizeof(path) - 1u ? in->size : sizeof(path) - 1u) : 0;
  if (length && !error_is_ok(vmm_read_block(c->vmm, in->gva, path, length))) path[0] = '\0';
  uint32_t fd = 0xFFFFFFFFu, error = NV_FILE_OPERATION_FAILED;
  for (size_t d = 0; d < sizeof(k_devices) / sizeof(k_devices[0]); d++) {
    if (strcmp(path, k_devices[d].path) != 0) continue;
    for (uint32_t i = 1; i < NVDRV_MAX_FDS; i++) { /* fd 0 is never handed out */
      if (s->fds[i].device != NV_DEVICE_NONE) continue;
      memset(&s->fds[i], 0, sizeof(s->fds[i]));
      s->fds[i].device = k_devices[d].device;
      s->fds[i].channel = NVDRV_NO_CHANNEL;
      fd = i;
      error = NV_SUCCESS;
      break;
    }
    if (error != NV_SUCCESS) error = NV_INSUFFICIENT_MEMORY;
    break;
  }
  if (error == NV_FILE_OPERATION_FAILED) log_warn("[nvdrv] Open(\"%s\"): no such device", path);
  (void)ipc_response_push_u32(res, fd);
  (void)ipc_response_push_u32(res, error);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_ioctl(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)ipc_response_push_u32(res, run_ioctl(c, state_of(self), req, 0, 0));
  return HLE_RESULT_SUCCESS;
}
static HLE_ServiceResult cmd_ioctl2(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)ipc_response_push_u32(res, run_ioctl(c, state_of(self), req, 1, 0));
  return HLE_RESULT_SUCCESS;
}
static HLE_ServiceResult cmd_ioctl3(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)ipc_response_push_u32(res, run_ioctl(c, state_of(self), req, 0, 1));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_close(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  Nvdrv_State *s = state_of(self);
  uint32_t fd = 0;
  (void)ipc_request_read_u32(req, 0, &fd);
  Nv_Fd *f = fd_of(s, fd);
  if (f) {
    if (f->syncpoint) syncpoint_free(&s->syncpoints, f->syncpoint);
    if (f->channel < NVDRV_MAX_CHANNELS) s->channel_used[f->channel] = false;
    memset(f, 0, sizeof(*f));
    f->channel = NVDRV_NO_CHANNEL;
  }
  (void)ipc_response_push_u32(res, f ? NV_SUCCESS : NV_BAD_VALUE);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_initialize(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req; /* the transfer memory is the driver's private heap; nothing here needs it */
  (void)ipc_response_push_u32(res, NV_SUCCESS);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_query_event(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  Nvdrv_State *s = state_of(self);
  uint32_t fd = 0, event_id = 0;
  (void)ipc_request_read_u32(req, 0, &fd);
  (void)ipc_request_read_u32(req, 4, &event_id);
  const uint32_t slot = event_id % NVDRV_MAX_EVENTS; /* channel events use high ids; fold them */
  if (!fd_of(s, fd)) {
    (void)ipc_response_push_u32(res, NV_BAD_VALUE);
    return HLE_RESULT_SUCCESS;
  }
  Nv_Event_Slot *e = &s->events[slot];
  if (!e->event) {
    const uint32_t result = hle_create_event(c, &e->readable_handle, NULL, &e->event);
    if (result != HLE_RESULT_SUCCESS) return result;
    event_retain(e->event); /* the driver keeps its own reference */
  } else {
    /* Each query hands out a new readable handle to the same event. */
    event_retain(e->event);
    uint32_t handle = 0;
    if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_EVENT_READABLE, e->event, &handle))) {
      event_release(e->event);
      return HLE_RESULT_OUT_OF_HANDLES;
    }
    e->readable_handle = handle;
  }
  (void)ipc_response_push_copy_handle(res, e->readable_handle);
  (void)ipc_response_push_u32(res, NV_SUCCESS);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_aruid(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, NV_SUCCESS);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_fw_margin_stub(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_nvdrv_commands[] = {
    {0, cmd_open, "Open"},
    {1, cmd_ioctl, "Ioctl"},
    {2, cmd_close, "Close"},
    {3, cmd_initialize, "Initialize"},
    {4, cmd_query_event, "QueryEvent"},
    {8, cmd_set_aruid, "SetAruid"},
    {11, cmd_ioctl2, "Ioctl2"},
    {12, cmd_ioctl3, "Ioctl3"},
    {13, cmd_set_fw_margin_stub, "SetGraphicsFirmwareMemoryMarginEnabled_stub"},
};

void nvdrv_init(Nvdrv_State *state, Gpu_Channel *channels) {
  memset(state, 0, sizeof(*state));
  state->channels = channels;
  syncpoints_init(&state->syncpoints);
  state->next_gpu_va = GPU_VA_BASE;
  state->interface.name = "nvdrv";
  state->interface.commands = k_nvdrv_commands;
  state->interface.command_count = sizeof(k_nvdrv_commands) / sizeof(k_nvdrv_commands[0]);
  state->interface.pointer_buffer_size = 0; /* libnx then uses A/B buffers for ioctls */
  state->interface.service_state = state;
}

Error nvdrv_register(Nvdrv_State *state, SM_Registry *registry) {
  static const char *const k_names[] = {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"};
  for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
    const Error err = sm_registry_add(registry, k_names[i], &state->interface);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}

void nvdrv_poll_completions(Nvdrv_State *state, HLE_Context *context) {
  (void)completion_ring_drain(&state->syncpoints);
  for (uint32_t i = 0; i < NVDRV_MAX_EVENTS; i++) {
    Nv_Event_Slot *e = &state->events[i];
    if (!e->waiting || !syncpoint_reached(&state->syncpoints, e->syncpoint, e->threshold)) continue;
    e->waiting = false;
    if (e->event) hle_signal_event(context, e->event);
  }
}

bool nvdrv_nvmap_lookup(const Nvdrv_State *state, uint32_t id, uint64_t *address, uint64_t *size) {
  if (id == 0 || id > NVMAP_MAX_HANDLES) return false;
  const Nvmap_Handle *h = &state->handles[id - 1u];
  if (!h->references || !h->allocated) return false;
  *address = h->address;
  *size = h->size;
  return true;
}
