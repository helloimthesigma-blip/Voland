/**
 * vi: display service, IHOSBinderDriver / IGraphicBufferProducer, and
 * the vsync compositor. See vi.h.
 */
#include "hle/services/vi/vi.h"

#include "common/log.h"
#include "gpu/block_linear.h"
#include "gpu/framebuffer.h"
#include "hle/services/service_util.h"

/* IGraphicBufferProducer transaction codes. */
enum {
  BQ_REQUEST_BUFFER = 1,
  BQ_SET_BUFFER_COUNT = 2,
  BQ_DEQUEUE_BUFFER = 3,
  BQ_DETACH_BUFFER = 4,
  BQ_DETACH_NEXT_BUFFER = 5,
  BQ_ATTACH_BUFFER = 6,
  BQ_QUEUE_BUFFER = 7,
  BQ_CANCEL_BUFFER = 8,
  BQ_QUERY = 9,
  BQ_CONNECT = 10,
  BQ_DISCONNECT = 11,
  BQ_SET_SIDEBAND_STREAM = 12,
  BQ_ALLOCATE_BUFFERS = 13,
  BQ_SET_PREALLOCATED_BUFFER = 14,
};

/* Android status codes (negated errno). */
#define BQ_OK 0
#define BQ_BAD_VALUE (-22)
#define BQ_WOULD_BLOCK (-11)
#define BQ_NO_INIT (-19)

/* Android PIXEL_FORMAT_* the compositor converts. */
#define PIXEL_RGBA_8888 1u
#define PIXEL_RGBX_8888 2u
#define PIXEL_RGB_565 4u
#define PIXEL_BGRA_8888 5u
#define PIXEL_RGBA_4444 7u
#define NV_LAYOUT_PITCH 1u
#define NV_LAYOUT_BLOCK_LINEAR 3u

#define PARCEL_HEADER_BYTES 0x10u
#define NV_MULTI_FENCE_BYTES 0x24u
#define BQ_OUTPUT_BYTES 0x10u
#define NATIVE_WINDOW_PARCEL_BYTES 0x28u
#define VI_DISPLAY_NAME_BYTES 0x40u
#define VI_NATIVE_WINDOW_BYTES 0x100u
#define QUERY_WIDTH 0
#define QUERY_HEIGHT 1
#define QUERY_FORMAT 2
#define QUERY_MIN_UNDEQUEUED 3

/* GBFR flattening (libnx bqSetPreallocatedBuffer) and NvGraphicBuffer
 * offsets within its ints[] (struct offset - the 12-byte NativeHandle). */
#define GBFR_MAGIC 0x47424652u
#define GBFR_HEADER_BYTES 0x28u
#define GBFR_NUM_INTS 0x24u
#define GB_NVMAP_ID 0x04u
#define GB_FORMAT 0x1Cu
#define GB_PLANE0 0x34u
#define PLANE_WIDTH 0x00u
#define PLANE_HEIGHT 0x04u
#define PLANE_LAYOUT 0x10u
#define PLANE_PITCH 0x14u
#define PLANE_OFFSET 0x1Cu
#define PLANE_BLOCK_HEIGHT_LOG2 0x24u
#define PLANE_SIZE 0x38u
#define PLANE_BYTES 0x58u

static Vi_State *state_of(Service_Object *self) { return (Vi_State *)self->interface->service_state; }

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* ------------------------------------------------------------------ */
/* Parcels.                                                            */
/* ------------------------------------------------------------------ */

typedef struct Parcel_Reader {
  const uint8_t *data;
  uint32_t size;
  uint32_t pos;
  bool failed;
} Parcel_Reader;

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

static const uint8_t *parcel_read(Parcel_Reader *r, uint32_t size) {
  const uint32_t aligned = align4(size);
  if (r->failed || aligned > r->size - r->pos) {
    r->failed = true;
    return NULL;
  }
  const uint8_t *p = r->data + r->pos;
  r->pos += aligned;
  return p;
}

static int32_t parcel_i32(Parcel_Reader *r) {
  const uint8_t *p = parcel_read(r, 4);
  return p ? (int32_t)rd32(p) : 0;
}

static void parcel_skip_token(Parcel_Reader *r) {
  (void)parcel_i32(r); /* strict-mode policy */
  const int32_t length = parcel_i32(r);
  if (length >= 0) (void)parcel_read(r, ((uint32_t)length + 1u) * 2u);
}

static const uint8_t *parcel_flattened(Parcel_Reader *r, uint32_t *size) {
  const int32_t length = parcel_i32(r);
  const int32_t fds = parcel_i32(r);
  if (length < 0 || fds != 0) {
    r->failed = true;
    return NULL;
  }
  *size = (uint32_t)length;
  return parcel_read(r, (uint32_t)length);
}

typedef struct Parcel_Writer {
  uint8_t *data; /* VI_PARCEL_MAX_BYTES */
  uint32_t payload;
} Parcel_Writer;

static void parcel_put(Parcel_Writer *w, const void *bytes, uint32_t size) {
  const uint32_t aligned = align4(size);
  if (PARCEL_HEADER_BYTES + w->payload + aligned > VI_PARCEL_MAX_BYTES) return;
  memset(w->data + PARCEL_HEADER_BYTES + w->payload, 0, aligned);
  memcpy(w->data + PARCEL_HEADER_BYTES + w->payload, bytes, size);
  w->payload += aligned;
}

static void parcel_put_i32(Parcel_Writer *w, int32_t v) { parcel_put(w, &v, sizeof(v)); }

static void parcel_put_flattened(Parcel_Writer *w, const void *bytes, uint32_t size) {
  parcel_put_i32(w, (int32_t)size);
  parcel_put_i32(w, 0);
  parcel_put(w, bytes, size);
}

/* Header: payload_size, payload_off, objects_size, objects_off. */
static uint32_t parcel_finish(Parcel_Writer *w) {
  const uint32_t header[4] = {w->payload, PARCEL_HEADER_BYTES, 0, PARCEL_HEADER_BYTES + w->payload};
  memcpy(w->data, header, sizeof(header));
  return PARCEL_HEADER_BYTES + w->payload;
}

/* ------------------------------------------------------------------ */
/* Layers and slots.                                                   */
/* ------------------------------------------------------------------ */

static Vi_Layer *layer_by_id(Vi_State *s, uint64_t id) {
  for (uint32_t i = 0; i < VI_MAX_LAYERS; i++) {
    if (s->layers[i].used && s->layers[i].id == id) return &s->layers[i];
  }
  return NULL;
}

/* Binder ids are layer index + 1. */
static Vi_Layer *layer_by_binder(Vi_State *s, int32_t binder) {
  if (binder < 1 || (uint32_t)binder > VI_MAX_LAYERS || !s->layers[binder - 1].used) return NULL;
  return &s->layers[binder - 1];
}

static int32_t binder_of(const Vi_State *s, const Vi_Layer *layer) { return (int32_t)(layer - s->layers) + 1; }

static Vi_Layer *open_layer(Vi_State *s, uint64_t id) {
  Vi_Layer *existing = layer_by_id(s, id);
  if (existing) return existing;
  for (uint32_t i = 0; i < VI_MAX_LAYERS; i++) {
    if (s->layers[i].used) continue;
    Kernel_Event *event = s->layers[i].release_event; /* survives close: the guest may hold handles */
    memset(&s->layers[i], 0, sizeof(s->layers[i]));
    s->layers[i].used = true;
    s->layers[i].id = id;
    s->layers[i].release_event = event;
    return &s->layers[i];
  }
  return NULL;
}

/* The release event is signalled exactly while a buffer is free. */
static void update_release_event(HLE_Context *c, Vi_Layer *layer) {
  if (!layer->release_event) return;
  bool any_free = false;
  for (uint32_t i = 0; i < VI_MAX_SLOTS && !any_free; i++) {
    any_free = layer->slots[i].preallocated && layer->slots[i].state == VI_SLOT_FREE;
  }
  if (any_free) hle_signal_event(c, layer->release_event);
  else layer->release_event->signaled = false;
}

static int32_t oldest_slot(const Vi_Layer *layer, Vi_Slot_State state) {
  int32_t best = -1;
  for (uint32_t i = 0; i < VI_MAX_SLOTS; i++) {
    const Vi_Slot *slot = &layer->slots[i];
    if (!slot->preallocated || slot->state != state) continue;
    if (best < 0 || slot->queue_order < layer->slots[best].queue_order) best = (int32_t)i;
  }
  return best;
}

/* ------------------------------------------------------------------ */
/* Compositor.                                                         */
/* ------------------------------------------------------------------ */

static uint32_t bytes_per_pixel(uint32_t format) {
  return format == PIXEL_RGB_565 || format == PIXEL_RGBA_4444 ? 2u : 4u;
}

static uint8_t expand(uint32_t value, uint32_t bits) {
  return (uint8_t)((value * 255u + ((1u << bits) - 1u) / 2u) / ((1u << bits) - 1u));
}

/* Rewrites one row (left to right in place for 4-byte formats; right to
 * left for the 16-bit ones, whose packed source sits at the row start). */
static void convert_row(uint8_t *row, uint32_t width, uint32_t format) {
  switch (format) {
  case PIXEL_RGBX_8888:
    for (uint32_t x = 0; x < width; x++) row[x * 4u + 3u] = 0xFF;
    break;
  case PIXEL_BGRA_8888:
    for (uint32_t x = 0; x < width; x++) {
      const uint8_t b = row[x * 4u];
      row[x * 4u] = row[x * 4u + 2u];
      row[x * 4u + 2u] = b;
    }
    break;
  case PIXEL_RGB_565:
    for (uint32_t x = width; x-- > 0;) {
      const uint32_t v = (uint32_t)row[x * 2u] | (uint32_t)row[x * 2u + 1u] << 8;
      uint8_t *out = row + x * 4u;
      out[0] = expand(v >> 11, 5);
      out[1] = expand((v >> 5) & 0x3Fu, 6);
      out[2] = expand(v & 0x1Fu, 5);
      out[3] = 0xFF;
    }
    break;
  case PIXEL_RGBA_4444:
    for (uint32_t x = width; x-- > 0;) {
      const uint32_t v = (uint32_t)row[x * 2u] | (uint32_t)row[x * 2u + 1u] << 8;
      uint8_t *out = row + x * 4u;
      out[0] = expand(v & 0xFu, 4);
      out[1] = expand((v >> 4) & 0xFu, 4);
      out[2] = expand((v >> 8) & 0xFu, 4);
      out[3] = expand(v >> 12, 4);
    }
    break;
  default:
    break; /* RGBA_8888: already the slot format */
  }
}

/* QueueBufferInput (flattened): {s64 timestamp, s32 auto timestamp,
 * Rect crop {l, t, r, b}, s32 scaling mode, u32 transform, ...}. */
#define QBI_CROP 12u
#define QBI_TRANSFORM 32u
#define VI_TRANSFORM_FLIP_H 1u /* NATIVE_WINDOW_TRANSFORM_FLIP_H */
#define VI_TRANSFORM_FLIP_V 2u

static void composite(Vi_State *s, HLE_Context *c, const Vi_Slot *slot) {
  uint64_t base = 0, nvmap_size = 0;
  if (!s->scratch || !nvdrv_nvmap_lookup(s->nvdrv, slot->nvmap_id, &base, &nvmap_size)) {
    s->frames_dropped++;
    return;
  }
  const uint32_t bpp = bytes_per_pixel(slot->format);
  uint32_t width = slot->width, height = slot->height;
  const uint64_t slot_capacity = LAYOUT_FRAMEBUFFER_SLOT_BYTES;
  if ((uint64_t)width * 4u * height > slot_capacity) height = (uint32_t)(slot_capacity / ((uint64_t)width * 4u));
  const bool block_linear = slot->layout == NV_LAYOUT_BLOCK_LINEAR;
  const uint64_t surface = block_linear ? block_linear_size(slot->pitch, height, slot->block_height_log2)
                                        : (uint64_t)slot->pitch * height;
  if (!width || !height || surface > VI_SCRATCH_BYTES || slot->offset + surface > nvmap_size ||
      !error_is_ok(vmm_read_block(c->vmm, base + slot->offset, s->scratch, surface))) {
    s->frames_dropped++;
    return;
  }
  uint8_t *pixels = NULL;
  const uint32_t fb_slot = framebuffer_acquire(&pixels);
  if (fb_slot == FRAMEBUFFER_BUSY) {
    s->frames_dropped++;
    return;
  }
  const uint32_t stride = width * 4u;
  if (block_linear) {
    block_linear_to_pitch(s->scratch, pixels, stride, width * bpp, height, slot->block_height_log2);
  } else {
    for (uint32_t y = 0; y < height; y++) {
      memcpy(pixels + (uint64_t)y * stride, s->scratch + (uint64_t)y * slot->pitch, (size_t)width * bpp);
    }
  }
  /* The source rectangle QueueBuffer named: pack it to the top left
   * (rows only move towards the start, so in-place copies are safe). */
  uint32_t out_w = width, out_h = height;
  if (slot->crop_right > slot->crop_left && slot->crop_bottom > slot->crop_top) {
    const uint32_t left = (uint32_t)(slot->crop_left < 0 ? 0 : slot->crop_left);
    const uint32_t top = (uint32_t)(slot->crop_top < 0 ? 0 : slot->crop_top);
    const uint32_t right = (uint32_t)slot->crop_right < width ? (uint32_t)slot->crop_right : width;
    const uint32_t bottom = (uint32_t)slot->crop_bottom < height ? (uint32_t)slot->crop_bottom : height;
    if (right > left && bottom > top && (right - left != width || bottom - top != height)) {
      out_w = right - left;
      out_h = bottom - top;
      for (uint32_t y = 0; y < out_h; y++)
        memmove(pixels + (uint64_t)y * out_w * 4u, pixels + (uint64_t)(y + top) * stride + (uint64_t)left * 4u,
                (size_t)out_w * 4u);
    }
  }
  const uint32_t out_stride = out_w * 4u;
  for (uint32_t y = 0; y < out_h; y++) convert_row(pixels + (uint64_t)y * out_stride, out_w, slot->format);
  if (slot->transform & VI_TRANSFORM_FLIP_H) {
    for (uint32_t y = 0; y < out_h; y++) {
      uint32_t *row = (uint32_t *)(void *)(pixels + (uint64_t)y * out_stride);
      for (uint32_t a = 0, b = out_w - 1u; a < b; a++, b--) {
        const uint32_t t = row[a];
        row[a] = row[b];
        row[b] = t;
      }
    }
  }
  if (slot->transform & VI_TRANSFORM_FLIP_V) {
    for (uint32_t a = 0, b = out_h - 1u; a < b; a++, b--) {
      uint32_t *ra = (uint32_t *)(void *)(pixels + (uint64_t)a * out_stride);
      uint32_t *rb = (uint32_t *)(void *)(pixels + (uint64_t)b * out_stride);
      for (uint32_t x = 0; x < out_w; x++) {
        const uint32_t t = ra[x];
        ra[x] = rb[x];
        rb[x] = t;
      }
    }
  }
  const Framebuffer_Frame frame = {out_w, out_h, out_stride, FRAMEBUFFER_FORMAT_RGBA8};
  framebuffer_publish(fb_slot, &frame);
  s->frames_presented++;
}

static void present_layer(Vi_State *s, HLE_Context *c, Vi_Layer *layer) {
  const int32_t next = oldest_slot(layer, VI_SLOT_QUEUED);
  if (next < 0) return;
  composite(s, c, &layer->slots[next]);
  for (uint32_t i = 0; i < VI_MAX_SLOTS; i++) {
    if (layer->slots[i].state == VI_SLOT_PRESENTED) layer->slots[i].state = VI_SLOT_FREE;
  }
  layer->slots[next].state = VI_SLOT_PRESENTED;
  update_release_event(c, layer);
}

void vi_update(Vi_State *s, HLE_Context *c, uint64_t now_ticks) {
  if (now_ticks < s->next_vsync_ticks) return;
  s->next_vsync_ticks = now_ticks + VI_TICKS_PER_VSYNC;
  for (uint32_t i = 0; i < VI_MAX_LAYERS; i++) {
    if (s->layers[i].used) present_layer(s, c, &s->layers[i]);
  }
  if (s->vsync_event) hle_signal_event(c, s->vsync_event);
}

uint64_t vi_next_wake(const Vi_State *s) {
  bool display = s->vsync_event != NULL;
  for (uint32_t i = 0; i < VI_MAX_LAYERS && !display; i++) display = s->layers[i].used;
  return display ? s->next_vsync_ticks : UINT64_MAX;
}

/* ------------------------------------------------------------------ */
/* IGraphicBufferProducer.                                             */
/* ------------------------------------------------------------------ */

static void put_buffer_output(Parcel_Writer *w, const Vi_Layer *layer) {
  uint32_t pending = 0;
  for (uint32_t i = 0; i < VI_MAX_SLOTS; i++) pending += layer->slots[i].state == VI_SLOT_QUEUED;
  const uint32_t out[4] = {VI_DISPLAY_WIDTH, VI_DISPLAY_HEIGHT, 0, pending};
  parcel_put(w, out, sizeof(out));
}

static void parse_graphic_buffer(Vi_Slot *slot, const uint8_t *gbfr, uint32_t size) {
  memset(slot, 0, sizeof(*slot));
  if (size < GBFR_HEADER_BYTES || rd32(gbfr) != GBFR_MAGIC) return;
  const uint32_t num_ints = rd32(gbfr + GBFR_NUM_INTS);
  const uint8_t *ints = gbfr + GBFR_HEADER_BYTES;
  if ((uint64_t)GBFR_HEADER_BYTES + (uint64_t)num_ints * 4u > size || num_ints * 4u < GB_PLANE0 + PLANE_BYTES) return;
  const uint8_t *plane = ints + GB_PLANE0;
  slot->preallocated = true;
  slot->nvmap_id = rd32(ints + GB_NVMAP_ID);
  slot->format = rd32(ints + GB_FORMAT);
  slot->width = rd32(plane + PLANE_WIDTH);
  slot->height = rd32(plane + PLANE_HEIGHT);
  slot->layout = rd32(plane + PLANE_LAYOUT);
  slot->pitch = rd32(plane + PLANE_PITCH);
  slot->offset = rd32(plane + PLANE_OFFSET);
  slot->block_height_log2 = rd32(plane + PLANE_BLOCK_HEIGHT_LOG2);
  slot->size = rd64(plane + PLANE_SIZE);
  slot->gbfr_size = size <= VI_GBFR_MAX_BYTES ? size : 0;
  if (slot->gbfr_size) memcpy(slot->gbfr, gbfr, size);
}

/* Runs one transaction: `r` positioned after the interface token. */
static int32_t transact(Vi_State *s, HLE_Context *c, Vi_Layer *layer, uint32_t code, Parcel_Reader *r,
                        Parcel_Writer *w) {
  switch (code) {
  case BQ_CONNECT:
    (void)parcel_i32(r); /* has listener */
    layer->connected = true;
    put_buffer_output(w, layer);
    return BQ_OK;
  case BQ_DISCONNECT:
    layer->connected = false;
    for (uint32_t i = 0; i < VI_MAX_SLOTS; i++) memset(&layer->slots[i], 0, sizeof(layer->slots[i]));
    update_release_event(c, layer);
    return BQ_OK;
  case BQ_SET_PREALLOCATED_BUFFER: {
    const int32_t index = parcel_i32(r);
    const int32_t has_input = parcel_i32(r);
    if (index < 0 || (uint32_t)index >= VI_MAX_SLOTS) return BQ_BAD_VALUE;
    Vi_Slot *slot = &layer->slots[index];
    if (!has_input) {
      memset(slot, 0, sizeof(*slot));
    } else {
      uint32_t size = 0;
      const uint8_t *gbfr = parcel_flattened(r, &size);
      if (!gbfr) return BQ_BAD_VALUE;
      parse_graphic_buffer(slot, gbfr, size);
    }
    update_release_event(c, layer);
    return BQ_OK;
  }
  case BQ_REQUEST_BUFFER: {
    const int32_t index = parcel_i32(r);
    if (index < 0 || (uint32_t)index >= VI_MAX_SLOTS) return BQ_BAD_VALUE;
    const Vi_Slot *slot = &layer->slots[index];
    parcel_put_i32(w, slot->gbfr_size ? 1 : 0);
    if (slot->gbfr_size) parcel_put_flattened(w, slot->gbfr, slot->gbfr_size);
    return BQ_OK;
  }
  case BQ_DEQUEUE_BUFFER: {
    const int32_t async = parcel_i32(r);
    int32_t index = oldest_slot(layer, VI_SLOT_FREE);
    if (index < 0 && !async) {
      index = oldest_slot(layer, VI_SLOT_QUEUED); /* reclaim: that frame is dropped */
      if (index >= 0) s->frames_dropped++;
    }
    if (index < 0) {
      log_debug("[vi] dequeue would block (release event %s)",
                layer->release_event && layer->release_event->signaled ? "SIGNALED" : "clear");
      parcel_put_i32(w, -1);
      parcel_put_i32(w, 0);
      return BQ_WOULD_BLOCK;
    }
    layer->slots[index].state = VI_SLOT_DEQUEUED;
    update_release_event(c, layer);
    uint8_t fence[NV_MULTI_FENCE_BYTES];
    memset(fence, 0, sizeof(fence)); /* num_fences 0: already signalled */
    parcel_put_i32(w, index);
    parcel_put_i32(w, 1);
    parcel_put_flattened(w, fence, sizeof(fence));
    return BQ_OK;
  }
  case BQ_QUEUE_BUFFER: {
    const int32_t index = parcel_i32(r);
    if (index < 0 || (uint32_t)index >= VI_MAX_SLOTS || layer->slots[index].state != VI_SLOT_DEQUEUED) {
      put_buffer_output(w, layer);
      return BQ_BAD_VALUE;
    }
    Vi_Slot *slot = &layer->slots[index];
    uint32_t input_size = 0;
    const uint8_t *input = parcel_flattened(r, &input_size);
    slot->crop_left = slot->crop_top = slot->crop_right = slot->crop_bottom = 0;
    slot->transform = 0;
    if (input && input_size >= QBI_TRANSFORM + 4u) {
      slot->crop_left = (int32_t)rd32(input + QBI_CROP);
      slot->crop_top = (int32_t)rd32(input + QBI_CROP + 4u);
      slot->crop_right = (int32_t)rd32(input + QBI_CROP + 8u);
      slot->crop_bottom = (int32_t)rd32(input + QBI_CROP + 12u);
      slot->transform = rd32(input + QBI_TRANSFORM);
    }
    slot->state = VI_SLOT_QUEUED;
    slot->queue_order = ++layer->queue_counter;
    put_buffer_output(w, layer);
    return BQ_OK;
  }
  case BQ_CANCEL_BUFFER: {
    const int32_t index = parcel_i32(r);
    if (index >= 0 && (uint32_t)index < VI_MAX_SLOTS && layer->slots[index].state == VI_SLOT_DEQUEUED) {
      layer->slots[index].state = VI_SLOT_FREE;
      update_release_event(c, layer);
    }
    return BQ_OK;
  }
  case BQ_QUERY: {
    const int32_t what = parcel_i32(r);
    int32_t value = 0;
    if (what == QUERY_WIDTH) value = VI_DISPLAY_WIDTH;
    else if (what == QUERY_HEIGHT) value = VI_DISPLAY_HEIGHT;
    else if (what == QUERY_FORMAT) value = PIXEL_RGBA_8888;
    else if (what == QUERY_MIN_UNDEQUEUED) value = 1;
    parcel_put_i32(w, value);
    return BQ_OK;
  }
  case BQ_DETACH_BUFFER: {
    const int32_t index = parcel_i32(r);
    if (index >= 0 && (uint32_t)index < VI_MAX_SLOTS) memset(&layer->slots[index], 0, sizeof(layer->slots[0]));
    update_release_event(c, layer);
    return BQ_OK;
  }
  case BQ_SET_BUFFER_COUNT:
  case BQ_SET_SIDEBAND_STREAM:
  case BQ_ALLOCATE_BUFFERS:
    return BQ_OK;
  default:
    log_warn("[vi] IGraphicBufferProducer: unimplemented transaction %u", code);
    return BQ_BAD_VALUE;
  }
}

/* {s32 binder id, u32 code, u32 flags} + in parcel (A/X) + out (B/C). */
static HLE_ServiceResult cmd_transact_parcel(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)res;
  Vi_State *s = state_of(self);
  uint32_t binder = 0, code = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &binder)) || !error_is_ok(ipc_request_read_u32(req, 4, &code))) {
    return IPC_RESULT_SF_INVALID_IN_HEADER;
  }
  Vi_Layer *layer = layer_by_binder(s, (int32_t)binder);
  const uint64_t in_size = service_read_in(c, req, 0, s->parcel_in, sizeof(s->parcel_in));
  Parcel_Reader r = {s->parcel_in, 0, 0, false};
  if (in_size >= PARCEL_HEADER_BYTES) {
    const uint32_t size = rd32(s->parcel_in), off = rd32(s->parcel_in + 4);
    if (off <= in_size && size <= in_size - off) r = (Parcel_Reader){s->parcel_in + off, size, 0, false};
  }
  parcel_skip_token(&r);
  memset(s->parcel_out, 0, PARCEL_HEADER_BYTES);
  Parcel_Writer w = {s->parcel_out, 0};
  const int32_t status = layer ? transact(s, c, layer, code, &r, &w) : BQ_NO_INIT;
  parcel_put_i32(&w, status);
  const uint32_t out_size = parcel_finish(&w);
  (void)service_write_out(c, req, 0, s->parcel_out, out_size);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_native_handle(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  Vi_State *s = state_of(self);
  uint32_t binder = 0;
  (void)ipc_request_read_u32(req, 0, &binder);
  Vi_Layer *layer = layer_by_binder(s, (int32_t)binder);
  if (!layer) return HLE_RESULT_INVALID_HANDLE;
  const HLE_ServiceResult result = service_push_event(c, res, &layer->release_event);
  if (result == HLE_RESULT_SUCCESS) update_release_event(c, layer);
  return result;
}

/* ------------------------------------------------------------------ */
/* Display services.                                                   */
/* ------------------------------------------------------------------ */

#define VI_GETTER(fn, field)                                                                           \
  static HLE_ServiceResult fn(HLE_Context *c, Service_Object *self, const IPC_Request *req,          \
                              IPC_Response *res) {                                                   \
    (void)c;                                                                                         \
    (void)req;                                                                                       \
    (void)ipc_response_push_object(res, &state_of(self)->field, 0);                                  \
    return HLE_RESULT_SUCCESS;                                                                       \
  }

VI_GETTER(cmd_get_display_service, application_display)
VI_GETTER(cmd_get_relay_service, relay)
VI_GETTER(cmd_get_system_display_service, system_display)
VI_GETTER(cmd_get_manager_display_service, manager_display)

static HLE_ServiceResult cmd_open_display(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, VI_DEFAULT_DISPLAY_ID);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_list_displays(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)self;
  /* DisplayInfo: name[0x40], u8 has_layer_limit, pad[7], s64 max_layers, s64 width, s64 height. */
  uint8_t info[0x60];
  memset(info, 0, sizeof(info));
  memcpy(info, "Default", sizeof("Default"));
  info[0x40] = 1;
  const uint64_t values[3] = {1, VI_DISPLAY_WIDTH, VI_DISPLAY_HEIGHT};
  memcpy(info + 0x48, values, sizeof(values));
  (void)ipc_response_push_u64(res, service_write_out(c, req, 0, info, sizeof(info)) ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_display_resolution(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, VI_DISPLAY_WIDTH);
  (void)ipc_response_push_u64(res, VI_DISPLAY_HEIGHT);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_logical_resolution(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, VI_DISPLAY_WIDTH);
  (void)ipc_response_push_u32(res, VI_DISPLAY_HEIGHT);
  return HLE_RESULT_SUCCESS;
}

/* The native window parcel OpenLayer / CreateStrayLayer return: its
 * payload names the layer's IGraphicBufferProducer binder (payload[2]). */
static uint64_t write_native_window(HLE_Context *c, const IPC_Request *req, int32_t binder) {
  uint8_t window[VI_NATIVE_WINDOW_BYTES];
  memset(window, 0, sizeof(window));
  const uint32_t header[4] = {NATIVE_WINDOW_PARCEL_BYTES, PARCEL_HEADER_BYTES, 0,
                              PARCEL_HEADER_BYTES + NATIVE_WINDOW_PARCEL_BYTES};
  memcpy(window, header, sizeof(header));
  const uint32_t payload[3] = {2u /* type */, 1u /* pid */, (uint32_t)binder};
  memcpy(window + PARCEL_HEADER_BYTES, payload, sizeof(payload));
  memcpy(window + PARCEL_HEADER_BYTES + 0x18, "dispdrv", sizeof("dispdrv"));
  const uint64_t size = PARCEL_HEADER_BYTES + NATIVE_WINDOW_PARCEL_BYTES;
  return service_write_out(c, req, 0, window, sizeof(window)) ? size : 0;
}

/* {name[0x40], u64 layer_id, u64 aruid} + B buffer -> u64 size. */
static HLE_ServiceResult cmd_open_layer(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  Vi_State *s = state_of(self);
  uint64_t id = 0;
  if (!error_is_ok(ipc_request_read_u64(req, VI_DISPLAY_NAME_BYTES, &id))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  Vi_Layer *layer = open_layer(s, id);
  if (!layer) return HLE_RESULT_OUT_OF_RANGE;
  (void)ipc_response_push_u64(res, write_native_window(c, req, binder_of(s, layer)));
  return HLE_RESULT_SUCCESS;
}

/* {u32 flags, pad, u64 display} + B buffer -> {u64 layer_id, u64 size}. */
static HLE_ServiceResult cmd_create_stray_layer(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)req;
  Vi_State *s = state_of(self);
  const uint64_t id = s->next_stray_layer_id++;
  Vi_Layer *layer = open_layer(s, id);
  if (!layer) return HLE_RESULT_OUT_OF_RANGE;
  (void)ipc_response_push_u64(res, id);
  (void)ipc_response_push_u64(res, write_native_window(c, req, binder_of(s, layer)));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_close_layer(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)res;
  Vi_State *s = state_of(self);
  uint64_t id = 0;
  (void)ipc_request_read_u64(req, 0, &id);
  Vi_Layer *layer = layer_by_id(s, id);
  if (layer) {
    Kernel_Event *event = layer->release_event;
    memset(layer, 0, sizeof(*layer));
    layer->release_event = event;
  }
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_managed_layer(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)c;
  (void)req;
  Vi_State *s = state_of(self);
  (void)ipc_response_push_u64(res, s->next_stray_layer_id++);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_vsync_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->vsync_event);
}

static HLE_ServiceResult cmd_convert_scaling_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)c;
  (void)self;
  uint32_t mode = 0;
  (void)ipc_request_read_u32(req, 0, &mode);
  (void)ipc_response_push_u64(res, mode);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_z_order_max(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, 0xFF);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Tables.                                                             */
/* ------------------------------------------------------------------ */

static const Service_Command k_root_commands[] = {
    {0, cmd_get_display_service, "GetDisplayService"},
    {1, cmd_get_display_service, "GetDisplayServiceWithProxyNameExchange"},
    {2, cmd_get_display_service, "GetDisplayService2"},
};

static const Service_Command k_application_display_commands[] = {
    {100, cmd_get_relay_service, "GetRelayService"},
    {101, cmd_get_system_display_service, "GetSystemDisplayService"},
    {102, cmd_get_manager_display_service, "GetManagerDisplayService"},
    {103, cmd_get_relay_service, "GetIndirectDisplayTransactionService"},
    {1000, cmd_list_displays, "ListDisplays"},
    {1010, cmd_open_display, "OpenDisplay"},
    {1011, cmd_open_display, "OpenDefaultDisplay"},
    {1020, service_cmd_ok, "CloseDisplay"},
    {1101, service_cmd_ok, "SetDisplayEnabled_stub"},
    {1102, cmd_get_display_resolution, "GetDisplayResolution"},
    {2020, cmd_open_layer, "OpenLayer"},
    {2021, cmd_close_layer, "CloseLayer"},
    {2030, cmd_create_stray_layer, "CreateStrayLayer"},
    {2031, cmd_close_layer, "DestroyStrayLayer"},
    {2101, service_cmd_ok, "SetLayerScalingMode_stub"},
    {2102, cmd_convert_scaling_mode, "ConvertScalingMode"},
    {5202, cmd_get_vsync_event, "GetDisplayVsyncEvent"},
    {5203, cmd_get_vsync_event, "GetDisplayVsyncEventForDebug"},
};

static const Service_Command k_relay_commands[] = {
    {0, cmd_transact_parcel, "TransactParcel"},
    {1, service_cmd_ok, "AdjustRefcount"},
    {2, cmd_get_native_handle, "GetNativeHandle"},
    {3, cmd_transact_parcel, "TransactParcelAuto"},
};

static const Service_Command k_system_display_commands[] = {
    {1200, service_cmd_out_u64_zero, "GetZOrderCountMin"},
    {1202, cmd_get_z_order_max, "GetZOrderCountMax"},
    {1203, cmd_get_logical_resolution, "GetDisplayLogicalResolution"},
    {1204, service_cmd_ok, "SetDisplayMagnification_stub"},
    {2201, service_cmd_ok, "SetLayerPosition_stub"},
    {2203, service_cmd_ok, "SetLayerSize_stub"},
    {2205, service_cmd_ok, "SetLayerZ_stub"},
    {2207, service_cmd_ok, "SetLayerVisibility_stub"},
};

static const Service_Command k_manager_display_commands[] = {
    {2010, cmd_create_managed_layer, "CreateManagedLayer"},
    {2011, cmd_close_layer, "DestroyManagedLayer"},
    {6000, service_cmd_ok, "AddToLayerStack_stub"},
    {6002, service_cmd_ok, "SetLayerVisibility_stub"},
    {7000, service_cmd_ok, "SetContentVisibility_stub"},
};

#define VI_STRAY_LAYER_BASE 0x10000u /* stray/managed ids, clear of am's managed-layer ids */

void vi_init(Vi_State *s, Nvdrv_State *nvdrv, uint8_t *scratch) {
  memset(s, 0, sizeof(*s));
  s->nvdrv = nvdrv;
  s->scratch = scratch;
  s->next_stray_layer_id = VI_STRAY_LAYER_BASE;
  s->root = SERVICE_INTERFACE("vi:u", k_root_commands, 0, s);
  s->application_display = SERVICE_INTERFACE("IApplicationDisplayService", k_application_display_commands, 0, s);
  s->relay = SERVICE_INTERFACE("IHOSBinderDriver", k_relay_commands, 0, s);
  s->system_display = SERVICE_INTERFACE("ISystemDisplayService", k_system_display_commands, 0, s);
  s->manager_display = SERVICE_INTERFACE("IManagerDisplayService", k_manager_display_commands, 0, s);
}

Error vi_register(Vi_State *s, SM_Registry *registry) { return sm_registry_add(registry, "vi:u", &s->root); }
