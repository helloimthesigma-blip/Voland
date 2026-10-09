/**
 * nvdrv service: IPC commands, the device table and every device's
 * ioctls. See nvdrv.h for scope.
 */
#include "hle/services/nvdrv/nvdrv.h"

#include "common/log.h"
#include "hle/hle.h"
#include "hle/kernel/scheduler.h"
#include "video/host1x.h"

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
#define NV_CHANNEL_SET_NVMAP_FD 0x01u /* type 0x48, every channel */
#define SUBMIT_INCR_BYTES 20u
#define MAP_BUFFER_HEADER_BYTES 12u /* libnx: num_maps, reserved, u8 is_compressed (+ pad) */

#define NVMAP_PARAM_SIZE 1u
#define NVMAP_PARAM_ALIGNMENT 2u
#define NVMAP_PARAM_BASE 3u
#define NVMAP_PARAM_HEAP 4u
#define NVMAP_PARAM_KIND 5u
#define NVMAP_PARAM_COMPR 6u
#define NVMAP_HEAP_IOVMM 0x40000000u
#define NVMAP_MIN_ALIGN 0x1000u

#define GPU_VA_BASE 0x04000000ull        /* the small-page VA region start */
/* GM20B: 64KB big pages, 128KB compression pages. NVN rounds image
 * storage sizes up to the big page size; reporting 128KB made a Unity
 * title's 1080p swapchain overrun the memory pool it sized for 64KB. */
#define GPU_BIG_PAGE_SIZE 0x10000u
#define GPU_COMPRESSION_PAGE_SIZE 0x20000u
#define GPU_SMALL_PAGE_SIZE 0x1000u
#define SUBMIT_FLAG_FENCE_GET (1u << 1)       /* the driver appends one increment and returns the fence */
#define SUBMIT_FLAG_INCREMENT_VALUE (1u << 8) /* fence.value in: the increments the commands make */
#define MAP_FLAG_MODIFY (1u << 8)

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
#define GM20B_TPC_MASK 0x3u /* both TPCs of the single GPC */
#define GM20B_SM_COUNT 2u   /* one SM per TPC */

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
    log_debug("[nvmap] ALLOC handle %u size 0x%llx address 0x%llx flags 0x%x", rd32(d), (unsigned long long)h->size,
              (unsigned long long)h->address, h->flags);
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
    log_debug("[nvmap] PARAM handle %u param %u -> 0x%x", rd32(d), rd32(d + 4), value);
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

#define NV_EVENT_SYNCPOINT_SHIFT 16u /* EVENT_WAIT's returned value: {syncpoint id, event slot} */

static uint32_t ctrl_ioctl(Nvdrv_State *s, uint32_t nr, uint8_t *d) {
  Syncpoints *sp = &s->syncpoints;
  switch (nr) {
  case 0x14: /* SYNCPT_READ {id; value out} */
    if (rd32(d) >= SYNCPOINT_COUNT) return NV_BAD_VALUE;
    wr32(d + 4, syncpoint_min(sp, rd32(d)));
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
    if (nr == 0x19) wr32(d + 12, syncpoint_min(sp, id));
    return syncpoint_reached(sp, id, rd32(d + 4)) ? NV_SUCCESS : NV_TIMEOUT;
  }
  case 0x1D: { /* EVENT_WAIT {id, threshold, timeout, value inout} */
    const uint32_t id = rd32(d), threshold = rd32(d + 4), timeout = rd32(d + 8);
    if (id >= SYNCPOINT_COUNT) return NV_BAD_VALUE;
    if (syncpoint_reached(sp, id, threshold)) {
      wr32(d + 12, syncpoint_min(sp, id));
      return NV_SUCCESS;
    }
    if (!timeout) return NV_TIMEOUT;
    /* Not there yet: a free registered event is armed on the threshold and
     * "try again" returns it as {syncpoint id << 16 | event slot} - NVN
     * waits on that slot's event (QueryEvent) and indexes its 64-entry
     * event table with the low half. 0: no event (NVN polls). */
    uint32_t slot = NVDRV_MAX_EVENTS;
    for (uint32_t i = 0; i < NVDRV_MAX_EVENTS && slot == NVDRV_MAX_EVENTS; i++)
      if (s->events[i].registered && !s->events[i].waiting) slot = i;
    if (slot == NVDRV_MAX_EVENTS) {
      wr32(d + 12, 0);
      return NV_TIMEOUT;
    }
    s->events[slot].waiting = true;
    s->events[slot].syncpoint = id;
    s->events[slot].threshold = threshold;
    wr32(d + 12, (id << NV_EVENT_SYNCPOINT_SHIFT) | slot);
    return NV_TIMEOUT;
  }
  case 0x1E: { /* EVENT_WAIT_ASYNC {id, threshold, timeout, event_id} */
    const uint32_t id = rd32(d), threshold = rd32(d + 4), event_id = rd32(d + 12);
    if (id >= SYNCPOINT_COUNT || event_id >= NVDRV_MAX_EVENTS) return NV_BAD_VALUE;
    log_debug("[nvdrv] EVENT_WAIT_ASYNC syncpoint %u threshold %u (min %u max %u) event %u timeout %d", id, threshold,
              syncpoint_min(sp, id), syncpoint_max(sp, id), event_id, (int32_t)rd32(d + 8));
    if (syncpoint_reached(sp, id, threshold)) return NV_SUCCESS;
    s->events[event_id].waiting = true;
    s->events[event_id].syncpoint = id;
    s->events[event_id].threshold = threshold;
    return NV_TIMEOUT; /* "try again"; the event is signalled on completion */
  }
  case 0x1C: case 0x1F: case 0x20: /* SYNCPT_CLEAR_EVENT_WAIT, EVENT_REGISTER, EVENT_UNREGISTER {event_id} */
    if (rd32(d) >= NVDRV_MAX_EVENTS) return NV_BAD_VALUE;
    if (nr != 0x1F) s->events[rd32(d)].waiting = false;
    if (nr != 0x1C) s->events[rd32(d)].registered = nr == 0x1F;
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
  wr32(c + 0x2C, GPU_COMPRESSION_PAGE_SIZE); /* compression_page_size */
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

/* `extra`: Ioctl3's second output buffer, or NULL. The Nintendo SDK's
 * NvRm passes GET_CHARACTERISTICS / GET_TPC_MASKS through Ioctl3 and
 * reads the result from that buffer, not from the struct's address. */
static uint32_t ctrl_gpu_ioctl(uint32_t nr, uint8_t *d, uint32_t size, uint8_t *extra) {
  switch (nr) {
  case 0x01: wr32(d, GM20B_ZCULL_CTX_BYTES); return NV_SUCCESS; /* ZCULL_GET_CTX_SIZE */
  case 0x02: /* ZCULL_GET_INFO: GM20B's zcull geometry. NVN sizes depth
             * buffers' zcull storage from it - all zeros made that size 0,
             * and a Unity title's video-memory pool came up 64KB short. */
    memset(d, 0, size);
    wr32(d + 0x00, 0x20);   /* width_align_pixels */
    wr32(d + 0x04, 0x20);   /* height_align_pixels */
    wr32(d + 0x08, 0x400);  /* pixel_squares_by_aliquots */
    wr32(d + 0x0C, 0x800);  /* aliquot_total */
    wr32(d + 0x10, 0x20);   /* region_byte_multiplier */
    wr32(d + 0x14, 0x20);   /* region_header_size */
    wr32(d + 0x18, 0xC0);   /* subregion_header_size */
    wr32(d + 0x1C, 0x20);   /* subregion_width_align_pixels */
    wr32(d + 0x20, 0x40);   /* subregion_height_align_pixels */
    wr32(d + 0x24, 0x10);   /* subregion_count */
    return NV_SUCCESS;
  case 0x03: case 0x04: return NV_SUCCESS;                      /* ZBC_SET/QUERY_TABLE */
  case 0x05:                                                    /* GET_CHARACTERISTICS {u64 size, u64 addr, chars} */
    wr64(d, CHARACTERISTICS_BYTES);
    if (size >= 16u + CHARACTERISTICS_BYTES) fill_characteristics(d + 16);
    if (extra) fill_characteristics(extra);
    return NV_SUCCESS;
  case 0x06: /* GET_TPC_MASKS {u32 buf_size; pad; u64 addr; u32 mask...} */
    if (size >= 0x18) wr32(d + 0x10, GM20B_TPC_MASK);
    if (extra) wr32(extra, GM20B_TPC_MASK);
    return NV_SUCCESS;
  case 0x13: wr32(d, GM20B_SM_COUNT); return NV_SUCCESS;          /* NUM_VSMS {u32 count, reserved} */
  case 0x14: wr32(d, 0x07); wr32(d + 4, 0x01); return NV_SUCCESS; /* GET_ACTIVE_SLOT_MASK */
  case 0x1C: if (size >= 8) memset(d, 0, size); return NV_SUCCESS;  /* GET_GPU_TIME */
  default: return NV_NOT_IMPLEMENTED;
  }
}

/* ------------------------------------------------------------------ */
/* /dev/nvhost-as-gpu                                                  */
/* ------------------------------------------------------------------ */

static void pt_map(Nvdrv_State *s, uint32_t slot);
static void pt_unmap(Nvdrv_State *s, uint32_t slot);

#define AS_REMAP_NR 0x14u
#define AS_REMAP_ENTRY_BYTES 0x14u
#define AS_REMAP_PAGE_SHIFT 16u /* Remap counts in 64KB big pages */

/* REMAP: entries {u16 flags, u16 kind, u32 nvmap handle, u32 buffer
 * offset, u32 GPU offset, u32 pages}, all in big pages, as many as the
 * buffer holds - sparse resources point VA ranges at memory (handle) or
 * back at nothing (handle 0). */
static uint32_t as_remap(Nvdrv_State *s, uint8_t *d, uint32_t bytes) {
  for (uint32_t at = 0; at + AS_REMAP_ENTRY_BYTES <= bytes; at += AS_REMAP_ENTRY_BYTES) {
    const uint8_t *e = d + at;
    const uint32_t handle = rd32(e + 4);
    const uint64_t buffer_offset = (uint64_t)rd32(e + 8) << AS_REMAP_PAGE_SHIFT;
    const uint64_t gpu_va = (uint64_t)rd32(e + 12) << AS_REMAP_PAGE_SHIFT;
    const uint64_t size = (uint64_t)rd32(e + 16) << AS_REMAP_PAGE_SHIFT;
    for (uint32_t i = 0; i < s->mapping_end; i++) { /* whatever was at that VA goes */
      Gpu_Mapping *m = &s->mappings[i];
      if (m->in_use && m->gpu_va == gpu_va) {
        pt_unmap(s, i);
        m->in_use = false;
      }
    }
    if (!handle || !size) continue;
    if (!nvmap_of(s, handle)) return NV_BAD_VALUE;
    uint32_t i = 0;
    while (i < NVDRV_MAX_GPU_MAPPINGS && s->mappings[i].in_use) i++;
    if (i == NVDRV_MAX_GPU_MAPPINGS) return NV_INSUFFICIENT_MEMORY;
    s->mappings[i] = (Gpu_Mapping){true, gpu_va, size, handle, buffer_offset};
    if (i + 1u > s->mapping_end) s->mapping_end = i + 1u;
    pt_map(s, i);
  }
  return NV_SUCCESS;
}

static uint32_t as_gpu_ioctl(Nvdrv_State *s, uint32_t nr, uint8_t *d) {
  switch (nr) {
  case AS_REMAP_NR:
    return as_remap(s, d, s->ioctl_in_bytes);
  case 0x09: /* INITIALIZE_EX {big page size, as fd, flags, reserved, va range start, end, split} */
    log_debug("[as] INITIALIZE_EX big page 0x%x flags 0x%x range 0x%llx..0x%llx split 0x%llx", rd32(d), rd32(d + 8),
              (unsigned long long)rd64(d + 16), (unsigned long long)rd64(d + 24), (unsigned long long)rd64(d + 32));
    return NV_SUCCESS;
  case 0x01: case 0x03: /* BIND_CHANNEL, FREE_SPACE */
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
    log_debug("[as] ALLOC_SPACE pages 0x%llx page 0x%llx flags 0x%llx -> 0x%llx", (unsigned long long)pages,
              (unsigned long long)page_size, (unsigned long long)flags, (unsigned long long)offset);
    return NV_SUCCESS;
  }
  case 0x05: { /* UNMAP_BUFFER {u64 offset} */
    for (uint32_t i = 0; i < s->mapping_end; i++) {
      if (s->mappings[i].in_use && s->mappings[i].gpu_va == rd64(d)) {
        pt_unmap(s, i);
        s->mappings[i].in_use = false;
        return NV_SUCCESS;
      }
    }
    return NV_BAD_VALUE;
  }
  case 0x06: { /* MAP_BUFFER_EX {flags, kind, handle, page_size io, buffer_offset, mapping_size, offset io} */
    const uint32_t flags = rd32(d), handle = rd32(d + 8);
    Nvmap_Handle *h = nvmap_of(s, handle);
    if (flags & MAP_FLAG_MODIFY) {
      /* Modify (flag 0x100, no handle): re-kind part of an existing mapping
       * - e.g. deko3d marking image memory compressible. Kinds do not
       * change how memory reads here, so the range just has to exist. */
      const uint64_t va = rd64(d + 32), size = rd64(d + 24);
      for (uint32_t i = 0; i < s->mapping_end; i++) {
        const Gpu_Mapping *m = &s->mappings[i];
        if (m->in_use && va >= m->gpu_va && va - m->gpu_va + size <= m->size) return NV_SUCCESS;
      }
      return NV_BAD_VALUE;
    }
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
      if (i + 1u > s->mapping_end) s->mapping_end = i + 1u;
      pt_map(s, i);
      log_debug("[as] MAP_BUFFER_EX flags 0x%x kind 0x%x handle %u page 0x%x buf_off 0x%llx size 0x%llx -> va 0x%llx",
                flags, rd32(d + 4), handle, page_size, (unsigned long long)rd64(d + 16), (unsigned long long)size,
                (unsigned long long)gpu_va);
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



/* ---- GPU memory for the command processor ------------------------- */

/* ---- The GPU page table (nvdrv.h) --------------------------------- */

#define PT_GRANULE_BYTES (1ull << NVDRV_GRANULE_SHIFT)

/* The L2 entry of granule `g`, or NULL when its table does not exist
 * (and cannot be made: `make` false, or the pool is used up). */
static uint16_t *pt_entry(Nvdrv_State *s, uint64_t g, bool make) {
  const uint64_t l1 = g >> NVDRV_PT_L2_BITS;
  if (l1 >= NVDRV_PT_L1_ENTRIES) return NULL;
  if (!s->pt_l1[l1]) {
    if (!make || s->pt_l2_used == NVDRV_PT_L2_TABLES) return NULL;
    s->pt_l1[l1] = (uint16_t)(++s->pt_l2_used);
  }
  return &s->pt_l2[s->pt_l1[l1] - 1u][g & (NVDRV_PT_L2_ENTRIES - 1u)];
}

static bool mapping_covers(const Gpu_Mapping *m, uint64_t gpu_va) {
  return m->in_use && gpu_va >= m->gpu_va && gpu_va - m->gpu_va < m->size;
}

/* A slot may own a granule's entry alone when it covers all of it, or
 * when nothing else live is there. */
static bool covers_granule(const Gpu_Mapping *m, uint64_t g) {
  return m->gpu_va <= g * PT_GRANULE_BYTES && m->gpu_va + m->size >= (g + 1u) * PT_GRANULE_BYTES;
}

static void pt_map(Nvdrv_State *s, uint32_t slot) {
  const Gpu_Mapping *m = &s->mappings[slot];
  if (!m->size) return;
  const uint64_t first = m->gpu_va >> NVDRV_GRANULE_SHIFT, last = (m->gpu_va + m->size - 1u) >> NVDRV_GRANULE_SHIFT;
  for (uint64_t g = first; g <= last; g++) {
    uint16_t *e = pt_entry(s, g, true);
    if (!e) continue;
    const uint32_t old = *e;
    const bool alone = !old || (old != NVDRV_PT_MIXED && !s->mappings[old - 1u].in_use);
    /* The newest mapping wins a granule it covers whole, as page table
     * entries written over older ones would. */
    *e = (uint16_t)(alone || covers_granule(m, g) ? slot + 1u : NVDRV_PT_MIXED);
  }
}

/* Granule `g`'s entry from scratch, `skip` left out: a live mapping that
 * covers it whole, else the only one touching it, else MIXED. */
static uint16_t pt_rebuild_entry(const Nvdrv_State *s, uint64_t g, uint32_t skip) {
  uint32_t touching = 0, last = 0;
  for (uint32_t i = 0; i < s->mapping_end; i++) {
    const Gpu_Mapping *m = &s->mappings[i];
    if (i == skip || !m->in_use || !m->size) continue;
    if (m->gpu_va >= (g + 1u) * PT_GRANULE_BYTES || m->gpu_va + m->size <= g * PT_GRANULE_BYTES) continue;
    if (covers_granule(m, g)) return (uint16_t)(i + 1u);
    touching++;
    last = i;
  }
  return touching == 0 ? 0 : touching == 1 ? (uint16_t)(last + 1u) : (uint16_t)NVDRV_PT_MIXED;
}

static void pt_unmap(Nvdrv_State *s, uint32_t slot) {
  const Gpu_Mapping *m = &s->mappings[slot];
  if (!m->size) return;
  const uint64_t first = m->gpu_va >> NVDRV_GRANULE_SHIFT, last = (m->gpu_va + m->size - 1u) >> NVDRV_GRANULE_SHIFT;
  for (uint64_t g = first; g <= last; g++) {
    uint16_t *e = pt_entry(s, g, false);
    /* What this mapping hid or shared the granule with takes it back. */
    if (e && (*e == slot + 1u || *e == NVDRV_PT_MIXED)) *e = pt_rebuild_entry(s, g, slot);
  }
}

/* The previous hit first (consecutive accesses nearly always fall in the
 * same buffer; only a hint), then the page table, which is exact: a
 * granule's entry names the one mapping there or says none is. Only
 * granules several mappings share, and VA the table could not cover (its
 * pool used up), scan every mapping. */
bool nvdrv_gpu_translate(const Nvdrv_State *s, uint64_t gpu_va, uint64_t *guest_va, uint64_t *contiguous) {
  const uint32_t hint = __atomic_load_n(&s->last_mapping, __ATOMIC_RELAXED);
  uint32_t found = UINT32_MAX;
  if (hint < s->mapping_end && mapping_covers(&s->mappings[hint], gpu_va)) found = hint;
  if (found == UINT32_MAX) {
    const uint64_t g = gpu_va >> NVDRV_GRANULE_SHIFT, l1 = g >> NVDRV_PT_L2_BITS;
    if (l1 >= NVDRV_PT_L1_ENTRIES) return false;
    const bool exact = s->pt_l1[l1] || s->pt_l2_used < NVDRV_PT_L2_TABLES; /* no table and room left: nothing mapped */
    const uint32_t e = s->pt_l1[l1] ? s->pt_l2[s->pt_l1[l1] - 1u][g & (NVDRV_PT_L2_ENTRIES - 1u)] : 0u;
    if (exact && e != NVDRV_PT_MIXED) {
      if (!e || !mapping_covers(&s->mappings[e - 1u], gpu_va)) return false;
      found = e - 1u;
    }
    for (uint32_t i = 0; found == UINT32_MAX && i < s->mapping_end; i++)
      if (mapping_covers(&s->mappings[i], gpu_va)) found = i;
    if (found == UINT32_MAX) return false;
  }
  const Gpu_Mapping *m = &s->mappings[found];
  uint64_t base = 0, size = 0;
  if (!nvdrv_nvmap_lookup(s, m->nvmap_handle, &base, &size)) return false;
  __atomic_store_n((uint32_t *)&s->last_mapping, found, __ATOMIC_RELAXED);
  *guest_va = base + m->buffer_offset + (gpu_va - m->gpu_va);
  *contiguous = m->size - (gpu_va - m->gpu_va);
  return true;
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

static uint64_t gpu_extent(void *user, uint64_t gpu_va) {
  uint64_t guest_va = 0, contiguous = 0;
  return nvdrv_gpu_translate((const Nvdrv_State *)user, gpu_va, &guest_va, &contiguous) ? contiguous : 0u;
}

static bool gpu_translate(void *user, uint64_t gpu_va, uint64_t *guest_va) {
  uint64_t contiguous = 0;
  return nvdrv_gpu_translate((const Nvdrv_State *)user, gpu_va, guest_va, &contiguous);
}

/* The submission being processed (on the GPU thread, or the submitting
 * thread when synchronous): its channel syncpoint and the fence its
 * increments may reach. Increments the submission promised advance only
 * the reached value, up to that fence - a later submission's fence is
 * never reached early. Unpromised ones promise and reach at once. */
static _Thread_local uint32_t t_submit_syncpoint, t_submit_fence;
static _Thread_local bool t_submit_promised;

static void gpu_syncpoint_increment(void *user, uint32_t id) {
  Nvdrv_State *s = (Nvdrv_State *)user;
  if (id == 0 || id >= SYNCPOINT_COUNT) return;
  if (t_submit_promised && id == t_submit_syncpoint &&
      (int32_t)(t_submit_fence - syncpoint_min(&s->syncpoints, id)) > 0) {
    (void)syncpoint_increment_min(&s->syncpoints, id, t_submit_fence);
    return;
  }
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

/* One submission for the GPU thread: its entries follow. */
typedef struct Gpfifo_Call {
  Nvdrv_State *s;
  Gpu_Channel *channel;
  uint32_t syncpoint;
  uint32_t fence;    /* the promised value: reached once the commands ran */
  bool promised;     /* increments were promised */
  uint32_t count;
} Gpfifo_Call;

static void gpfifo_run(void *user, const void *payload, uint32_t bytes) {
  (void)user;
  (void)bytes;
  Gpfifo_Call call;
  memcpy(&call, payload, sizeof(call));
  uint64_t entries[GPFIFO_MAX_ENTRIES];
  memcpy(entries, (const uint8_t *)payload + sizeof(call), (size_t)call.count * 8u);
  Nvdrv_State *s = call.s;
  const Gpu_Memory memory = {s, gpu_read, gpu_write, gpu_syncpoint_increment, s->renderer, gpu_translate, gpu_extent};
  t_submit_syncpoint = call.syncpoint;
  t_submit_fence = call.fence;
  t_submit_promised = call.promised;
  gpu_channel_submit(call.channel, &memory, entries, call.count);
  t_submit_promised = false;
  /* Whatever the commands did, the promise is kept. */
  if (call.promised) syncpoint_complete(&s->syncpoints, call.syncpoint, call.fence);
}

/* Async GPU: this submission's virtual time and the GPU thread's call
 * count after it, for nvdrv_bound_gpu_latency (a full ring waits for its
 * oldest first). */
static void note_submission(Nvdrv_State *s, uint32_t calls) {
  if (s->submit_tail - s->submit_head >= NVDRV_GPU_LATENCY_RING) {
    gpu_thread_wait_until(s->gpu_thread, s->submit_calls[s->submit_head % NVDRV_GPU_LATENCY_RING]);
    s->submit_head++;
  }
  const uint32_t at = s->submit_tail++ % NVDRV_GPU_LATENCY_RING;
  s->submit_ticks[at] = s->hle && s->hle->scheduler ? s->hle->scheduler->ticks : 0u;
  s->submit_calls[at] = calls;
}

void nvdrv_bound_gpu_latency(Nvdrv_State *s, uint64_t now_ticks, uint64_t max_ticks) {
  if (!gpu_thread_async(s->gpu_thread)) {
    s->submit_head = s->submit_tail;
    return;
  }
  uint32_t finished = gpu_thread_progress(s->gpu_thread);
  while (s->submit_head != s->submit_tail) {
    const uint32_t at = s->submit_head % NVDRV_GPU_LATENCY_RING;
    if ((int32_t)(finished - s->submit_calls[at]) < 0) {
      if (now_ticks - s->submit_ticks[at] <= max_ticks) break;
      gpu_thread_wait_until(s->gpu_thread, s->submit_calls[at]); /* in host time: the GPU is that far behind */
      finished = gpu_thread_progress(s->gpu_thread);
      s->stalls_for_latency++;
    }
    s->submit_head++;
  }
}

/* Queues the submission (or runs it, synchronously): its syncpoint
 * promise is made now, in submission order; returns the fence. */
static uint32_t run_gpfifo(Nvdrv_State *s, Nv_Fd *f, uint8_t *d) {
  const uint32_t flags = rd32(d + 12);
  const uint32_t id = ensure_syncpoint(s, f);
  const uint32_t increments = ((flags & SUBMIT_FLAG_INCREMENT_VALUE) ? rd32(d + 20) : 0u) +
                              ((flags & SUBMIT_FLAG_FENCE_GET) ? 1u : 0u);
  const uint32_t fence = increments ? syncpoint_add_max(&s->syncpoints, id, increments) : syncpoint_max(&s->syncpoints, id);
  f->submissions++;
  Gpu_Channel *channel = channel_of(s, f);
  if (!channel || !s->hle) {
    syncpoint_complete(&s->syncpoints, id, fence);
    return fence;
  }
  uint32_t count = rd32(d + 8);
  if (count > GPFIFO_MAX_ENTRIES) count = GPFIFO_MAX_ENTRIES;
  static _Thread_local uint8_t payload[sizeof(Gpfifo_Call) + GPFIFO_MAX_ENTRIES * 8u];
  const Gpfifo_Call call = {s, channel, id, fence, increments != 0u, count};
  memcpy(payload, &call, sizeof(call));
  memcpy(payload + sizeof(call), d + GPFIFO_HEADER_BYTES, (size_t)count * 8u);
  const uint32_t bytes = (uint32_t)sizeof(call) + count * 8u;
  if (gpu_thread_async(s->gpu_thread)) {
    gpu_thread_call(s->gpu_thread, gpfifo_run, NULL, payload, bytes);
    note_submission(s, gpu_thread_queued(s->gpu_thread));
    return fence;
  }
  /* Synchronously the commands run outside the kernel lock
   * (scheduler.h scheduler_gpu_*). Other cores' ioctls may reuse the
   * shared ioctl buffer meanwhile: the request's bytes are put back
   * afterwards for the reply - from a buffer of this host thread's own,
   * since another core's submission may run between the GPU lock's
   * release and the kernel lock's return. */
  static _Thread_local uint8_t saved_request[NVDRV_IOCTL_MAX_BYTES];
  const uint32_t request_bytes = GPFIFO_HEADER_BYTES + count * 8u;
  memcpy(saved_request, d, request_bytes);
  const bool extra_out = s->extra_out; /* Ioctl3's second output: zeros for a submission */
  Scheduler *sched = s->hle->scheduler;
  Kernel_Suspend suspended;
  scheduler_gpu_begin(sched, &suspended);
  gpu_thread_call(s->gpu_thread, gpfifo_run, NULL, payload, bytes);
  scheduler_gpu_end(sched, &suspended);
  memcpy(d, saved_request, request_bytes);
  s->extra_out = extra_out;
  if (extra_out) memset(s->extra_buffer, 0, sizeof(s->extra_buffer));
  return fence;
}

static uint32_t gpu_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t nr, uint8_t *d) {
  switch (nr) {
  case 0x01: f->nvmap_fd = rd32(d); return NV_SUCCESS;           /* SET_NVMAP_FD */
  case 0x03: case 0x0B: case 0x0C: case 0x0D: return NV_SUCCESS; /* SET_TIMEOUT, ZCULL_BIND, SET_ERROR_NOTIFIER, SET_PRIORITY */
  case 0x08: case 0x1B: { /* SUBMIT_GPFIFO / KICKOFF_PB {u64 gpfifo, u32 num, u32 flags, fence io} */
    const uint32_t flags = rd32(d + 12);
    const uint32_t value = run_gpfifo(s, f, d);
    if (flags & (SUBMIT_FLAG_FENCE_GET | SUBMIT_FLAG_INCREMENT_VALUE)) {
      wr32(d + 16, f->syncpoint);
      wr32(d + 20, value);
    }
    return NV_SUCCESS;
  }
  case 0x09: wr64(d + 8, rd32(d)); return NV_SUCCESS; /* ALLOC_OBJ_CTX {class, flags, u64 obj_id out} */
  case 0x16: case 0x17: return NV_SUCCESS;            /* GET_ERROR_INFO / NOTIFICATION: none */
  case 0x1D: return NV_SUCCESS;                        /* SET_TIMESLICE: one channel runs at a time anyway */
  case 0x1A: { /* ALLOC_GPFIFO_EX2 {num, flags, unk0, fence out{id,value}, unk1..3} */
    const uint32_t id = ensure_syncpoint(s, f);
    if (!id) return NV_INSUFFICIENT_MEMORY;
    wr32(d + 12, id);
    wr32(d + 16, syncpoint_max(&s->syncpoints, id));
    return NV_SUCCESS;
  }
  default:
    return NV_NOT_IMPLEMENTED;
  }
}

/* VIC wrote into an nvmap buffer: textures over its GPU mappings are re-read. */
static void mm_written(void *user, uint32_t handle, uint64_t offset, uint64_t bytes) {
  Nvdrv_State *s = (Nvdrv_State *)user;
  if (!s->renderer) return;
  const Gpu_Memory memory = {s, gpu_read, gpu_write, gpu_syncpoint_increment, s->renderer, gpu_translate, gpu_extent};
  for (uint32_t i = 0; i < s->mapping_end; i++) {
    const Gpu_Mapping *m = &s->mappings[i];
    if (!m->in_use || m->nvmap_handle != handle) continue;
    const uint64_t start = offset > m->buffer_offset ? offset : m->buffer_offset;
    const uint64_t end = offset + bytes < m->buffer_offset + m->size ? offset + bytes : m->buffer_offset + m->size;
    if (start < end) raster3d_sync_range(s->renderer, &memory, m->gpu_va + (start - m->buffer_offset), end - start, true);
  }
}

/* SUBMIT's command buffers ({nvmap handle, byte offset, words}) run on
 * the device's engine (nvdec.h). Relocations would patch buffer
 * addresses into the words; the multimedia stack writes IOVAs itself. */
static void run_cmdbufs(Nvdrv_State *s, Nv_Fd *f, const uint8_t *d, uint32_t cmdbufs, uint32_t relocs) {
  Mm_Engine *engine = f->device == NV_DEVICE_NVDEC ? &s->nvdec : f->device == NV_DEVICE_VIC ? &s->vic : NULL;
  if (!engine || !s->hle) return;
  (void)relocs; /* the multimedia stack writes IOVAs into its commands itself */
  const Mm_Context context = {s->hle->vmm, &s->iova, s->video, &s->mm_video, mm_written, s};
  for (uint32_t i = 0; i < cmdbufs; i++) {
    const uint8_t *e = d + 16u + i * 12u;
    const Nvmap_Handle *h = nvmap_of(s, rd32(e));
    uint32_t words = rd32(e + 8);
    if (!h || !h->allocated) continue;
    if (words > NVDRV_MAX_CMDBUF_WORDS) words = NVDRV_MAX_CMDBUF_WORDS;
    if (!error_is_ok(vmm_read_block(s->hle->vmm, h->address + rd32(e + 4), s->cmdbuf, (uint64_t)words * 4u))) continue;
    mm_engine_submit(engine, &context, s->cmdbuf, words);
  }
}

/* host1x channel-common ioctls (type 0x00 on nvdec/vic/nvjpg/gpu). */
static uint32_t channel_common_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t nr, uint8_t *d, uint32_t size) {
  switch (nr) {
  case 0x01: { /* SUBMIT {num_cmdbufs, num_relocs, num_syncpt_incrs, num_fences, arrays...} */
    const uint32_t cmdbufs = rd32(d), relocs = rd32(d + 4), incrs = rd32(d + 8), fences = rd32(d + 12);
    /* cmdbuf 12 bytes, reloc 16, reloc shift 4, syncpt incr 20 ({id,
     * increments, waitbase, next, prev} - libnx's nvioctl_syncpt_incr),
     * fence threshold 4: threshold i is where incr i's syncpoint lands.
     * The engines' work itself is not emulated (decoded video is black,
     * §13): its syncpoints complete at once. */
    const uint64_t incr_at = 16u + (uint64_t)cmdbufs * 12u + (uint64_t)relocs * 20u;
    const uint64_t fence_at = incr_at + (uint64_t)incrs * SUBMIT_INCR_BYTES;
    if (fence_at + (uint64_t)fences * 4u > size) return NV_BAD_PARAMETER;
    run_cmdbufs(s, f, d, cmdbufs, relocs);
    const uint32_t own = ensure_syncpoint(s, f);
    for (uint32_t i = 0; i < incrs; i++) {
      const uint8_t *e = d + incr_at + i * SUBMIT_INCR_BYTES;
      uint32_t id = rd32(e);
      if (id == 0 || id >= SYNCPOINT_COUNT) id = own;
      const uint32_t value = syncpoint_add_max(&s->syncpoints, id, rd32(e + 4));
      syncpoint_complete(&s->syncpoints, id, value);
      if (i < fences) wr32(d + fence_at + i * 4u, value);
    }
    f->submissions++;
    log_debug("[nvdrv] host1x submit on device %u: %u cmdbufs, %u relocs, %u incrs, %u fences", f->device, cmdbufs,
              relocs, incrs, fences);
    return NV_SUCCESS;
  }
  case 0x02: wr32(d + 4, ensure_syncpoint(s, f)); return NV_SUCCESS; /* GET_SYNCPOINT {module; syncpt out} */
  case 0x03: wr32(d + 4, 0); return NV_SUCCESS;                      /* GET_WAITBASE */
  case 0x07: case 0x08: return NV_SUCCESS;                           /* SET_SUBMIT_TIMEOUT, SET_MODULE_CLOCK_RATE */
  case 0x14: case 0x23: wr32(d, 0); return NV_SUCCESS;               /* GET_MODULE_CLOCK_RATE {rate out, module} */
  case 0x09: { /* MAP_BUFFER {num, reserved, u8 compressed, pad, maps[]{handle, address out}} (header 12 bytes) */
    const uint32_t count = rd32(d);
    if (MAP_BUFFER_HEADER_BYTES + (uint64_t)count * 8u > size) return NV_BAD_PARAMETER;
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t handle = rd32(d + 12 + i * 8u);
      Nvmap_Handle *h = nvmap_of(s, handle);
      wr32(d + 12 + i * 8u + 4u, h && h->allocated ? mm_iova_map(&s->iova, handle, h->address, h->size) : 0u);
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

static uint32_t dispatch_device_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t request, uint8_t *data);

static uint32_t dispatch_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t request, uint8_t *data) {
  s->ioctl_count++;
  /* What GPU command processing reads (nvmap handles, GPU mappings) and
   * the multimedia engines' writes into the renderer's textures change
   * only under the GPU lock (scheduler.h). */
  const bool gpu_state = f->device == NV_DEVICE_NVMAP || f->device == NV_DEVICE_AS_GPU || f->device == NV_DEVICE_NVDEC ||
                         f->device == NV_DEVICE_VIC || f->device == NV_DEVICE_NVJPG;
  Scheduler *sched = s->hle ? s->hle->scheduler : NULL;
  if (gpu_state) {
    scheduler_gpu_lock(sched);
    gpu_thread_lock(s->gpu_thread); /* and the GPU thread is between calls */
  }
  const uint32_t result = dispatch_device_ioctl(s, f, request, data);
  if (gpu_state) {
    gpu_thread_unlock(s->gpu_thread);
    scheduler_gpu_unlock(sched);
  }
  return result;
}

static uint32_t dispatch_device_ioctl(Nvdrv_State *s, Nv_Fd *f, uint32_t request, uint8_t *data) {
  const uint32_t type = NV_IOC_TYPE(request), nr = NV_IOC_NR(request), size = NV_IOC_SIZE(request);
  switch (f->device) {
  case NV_DEVICE_NVMAP:
    return type == NV_TYPE_NVMAP ? nvmap_ioctl(s, nr, data) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_CTRL:
    return type == NV_TYPE_CTRL ? ctrl_ioctl(s, nr, data) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_CTRL_GPU:
    return type == NV_TYPE_CTRL_GPU ? ctrl_gpu_ioctl(nr, data, size, s->extra_out ? s->extra_buffer : NULL) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_AS_GPU:
    return type == NV_TYPE_AS_GPU ? as_gpu_ioctl(s, nr, data) : NV_NOT_IMPLEMENTED;
  case NV_DEVICE_GPU:
    if (type == NV_TYPE_GPU) return gpu_ioctl(s, f, nr, data);
    if (type == NV_TYPE_CTRL_GPU && nr == 0x14) return NV_SUCCESS; /* SET_USER_DATA (type 0x47) */
    return type == NV_TYPE_CTRL ? channel_common_ioctl(s, f, nr, data, size) : NV_NOT_IMPLEMENTED;
  default: /* nvdec, vic, nvjpg */
    if (type == NV_TYPE_GPU && nr == NV_CHANNEL_SET_NVMAP_FD) {
      f->nvmap_fd = rd32(data);
      return NV_SUCCESS;
    }
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
  uint32_t size = NV_IOC_SIZE(request);
  const uint32_t dir = NV_IOC_DIR(request);
  /* REMAP's struct is one entry; the buffer holds as many as are passed. */
  if (f->device == NV_DEVICE_AS_GPU && NV_IOC_NR(request) == AS_REMAP_NR) {
    const IPC_Buffer *in = in_buffer(req, 0);
    if (in && in->size > size) size = in->size < sizeof(s->ioctl_buffer) ? (uint32_t)in->size : (uint32_t)sizeof(s->ioctl_buffer);
  }
  s->ioctl_in_bytes = size;
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
  memset(s->extra_buffer, 0, sizeof(s->extra_buffer));
  s->extra_out = extra_out != 0;
  const uint32_t error = dispatch_ioctl(s, f, request, s->ioctl_buffer);
  log_debug("[nvdrv] ioctl 0x%08x on device %d -> %u", request, (int)f->device, error);
  if (dir & NV_IOC_READ) {
    const IPC_Buffer *out = out_buffer(req, 0);
    if (!out || out->size < size || !error_is_ok(vmm_write_block(c->vmm, out->gva, s->ioctl_buffer, size))) {
      return NV_BAD_PARAMETER;
    }
  }
  if (extra_out) {
    const IPC_Buffer *out = out_buffer(req, extra_out);
    if (out && out->size) {
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
  else log_debug("[nvdrv] Open(\"%s\") -> fd %u", path, fd);
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
  mm_iova_init(&state->iova);
  mm_engine_init(&state->nvdec, HOST1X_CLASS_NVDEC);
  mm_engine_init(&state->vic, HOST1X_CLASS_VIC);
  mm_video_init(&state->mm_video);
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
