/**
 * hid service: commands (libnx services/hid.c ids) and the shared-memory
 * writer. See hid.h for layout and scope.
 */
#include "hle/services/hid/hid.h"

#include "common/input_region.h"
#include "hle/hle.h"
#include "hle/kernel/handle_table.h"
#include "hle/services/service_util.h"

#include <string.h>

#define HID_LIFO_KINDS 7u
#define HID_STYLE_PALMA (1u << 6)
#define HID_STYLE_SYSTEM_EXT (1u << 29)
#define HID_COLOR_ATTRIBUTE_OK 0u
#define HID_COLOR_ATTRIBUTE_NO_CONTROLLER 2u
#define HID_BATTERY_FULL 4u
#define HID_BATTERY_SLOTS 3u
#define HID_ID_BIT_HANDHELD (1u << HID_NPAD_HANDHELD_INDEX)
#define HID_ID_BIT_OTHER (1u << HID_NPAD_OTHER_INDEX)
#define HID_ALL_IDS 0x3FFu
#define HID_PLAYER_SLOTS 8u
#define HID_NPAD_BUTTON_MASK (INPUT_BUTTON_TRANSPORTED_MASK & ~(INPUT_BUTTON_HOME | INPUT_BUTTON_CAPTURE))

/* HidNpadSystemProperties bits. */
#define HID_PROP_IS_POWERED_SHIFT 3u
#define HID_PROP_IS_POWERED_ALL (7ull << HID_PROP_IS_POWERED_SHIFT)
#define HID_PROP_ABXY_ORIENTED (1ull << 11)
#define HID_PROP_PLUS_AVAILABLE (1ull << 13)
#define HID_PROP_MINUS_AVAILABLE (1ull << 14)
#define HID_PROP_DIRECTIONAL_AVAILABLE (1ull << 15)

/* Controller colors (RGBA), a grey Pro Controller and neon Joy-Con. */
#define HID_COLOR_PRO_MAIN 0x323232FFu
#define HID_COLOR_PRO_SUB 0xE6E6E6FFu
#define HID_COLOR_JOY_LEFT_MAIN 0x0AB9E6FFu
#define HID_COLOR_JOY_RIGHT_MAIN 0xFF3C28FFu
#define HID_COLOR_JOY_SUB 0x1E1E1EFFu

static const uint32_t k_lifo_offsets[HID_LIFO_KINDS] = {0x28u, 0x378u, 0x6C8u, 0xA18u, 0xD68u, 0x10B8u, 0x1408u};
static const uint32_t k_lifo_styles[HID_LIFO_KINDS] = {HID_STYLE_FULL_KEY, HID_STYLE_HANDHELD, HID_STYLE_JOY_DUAL,
                                                       HID_STYLE_JOY_LEFT, HID_STYLE_JOY_RIGHT, HID_STYLE_PALMA,
                                                       HID_STYLE_SYSTEM_EXT};

static Hid_State *state_of(Service_Object *self) { return (Hid_State *)self->interface->service_state; }

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

/* ------------------------------------------------------------------ */
/* Shared-memory writer.                                               */
/* ------------------------------------------------------------------ */

static uint64_t npad_pa(const Hid_State *s, uint32_t index) {
  return s->shared_memory->guest_pa + HID_NPAD_SECTION_OFFSET + (uint64_t)index * HID_NPAD_ENTRY_BYTES;
}

static uint32_t attributes_for(uint32_t style) {
  switch (style) {
  case HID_STYLE_FULL_KEY: return HID_ATTR_IS_CONNECTED;
  case HID_STYLE_HANDHELD:
    return HID_ATTR_IS_CONNECTED | HID_ATTR_IS_WIRED | HID_ATTR_IS_LEFT_CONNECTED | HID_ATTR_IS_LEFT_WIRED |
           HID_ATTR_IS_RIGHT_CONNECTED | HID_ATTR_IS_RIGHT_WIRED;
  case HID_STYLE_JOY_DUAL: return HID_ATTR_IS_CONNECTED | HID_ATTR_IS_LEFT_CONNECTED | HID_ATTR_IS_RIGHT_CONNECTED;
  case HID_STYLE_JOY_LEFT: return HID_ATTR_IS_CONNECTED | HID_ATTR_IS_LEFT_CONNECTED;
  case HID_STYLE_JOY_RIGHT: return HID_ATTR_IS_CONNECTED | HID_ATTR_IS_RIGHT_CONNECTED;
  default: return 0;
  }
}

static uint32_t device_type_for(uint32_t style) {
  switch (style) {
  case HID_STYLE_FULL_KEY: return HID_DEVICE_FULL_KEY;
  case HID_STYLE_HANDHELD: return HID_DEVICE_HANDHELD_LEFT | HID_DEVICE_HANDHELD_RIGHT;
  case HID_STYLE_JOY_DUAL: return HID_DEVICE_JOY_LEFT | HID_DEVICE_JOY_RIGHT;
  case HID_STYLE_JOY_LEFT: return HID_DEVICE_JOY_LEFT;
  case HID_STYLE_JOY_RIGHT: return HID_DEVICE_JOY_RIGHT;
  default: return 0;
  }
}

/* The style a player controller is presented as: FullKey first. */
static uint32_t player_style(uint32_t supported) {
  static const uint32_t k_order[] = {HID_STYLE_FULL_KEY, HID_STYLE_JOY_DUAL, HID_STYLE_JOY_LEFT, HID_STYLE_JOY_RIGHT};
  for (size_t i = 0; i < sizeof(k_order) / sizeof(k_order[0]); i++) {
    if (supported & k_order[i]) return k_order[i];
  }
  return 0;
}

/* The npad's static fields: style, colors, device type, properties. */
static void write_npad_header(HLE_Context *c, const Hid_State *s, uint32_t index, uint32_t style) {
  uint8_t head[0x28];
  memset(head, 0, sizeof(head));
  wr32(head + HID_NPAD_STYLE_SET, style);
  wr32(head + HID_NPAD_JOY_ASSIGNMENT_MODE, 0); /* Dual */
  const uint32_t color_attr = style ? HID_COLOR_ATTRIBUTE_OK : HID_COLOR_ATTRIBUTE_NO_CONTROLLER;
  wr32(head + HID_NPAD_FULL_KEY_COLOR, color_attr);
  wr32(head + HID_NPAD_FULL_KEY_COLOR + 4, HID_COLOR_PRO_MAIN);
  wr32(head + HID_NPAD_FULL_KEY_COLOR + 8, HID_COLOR_PRO_SUB);
  wr32(head + HID_NPAD_JOY_COLOR, color_attr);
  wr32(head + HID_NPAD_JOY_COLOR + 4, HID_COLOR_JOY_LEFT_MAIN);
  wr32(head + HID_NPAD_JOY_COLOR + 8, HID_COLOR_JOY_SUB);
  wr32(head + HID_NPAD_JOY_COLOR + 12, HID_COLOR_JOY_RIGHT_MAIN);
  wr32(head + HID_NPAD_JOY_COLOR + 16, HID_COLOR_JOY_SUB);
  const uint64_t base = npad_pa(s, index);
  (void)vmm_write_physical(c->vmm, base, head, sizeof(head));

  /* device_type @0x4188 .. battery_level[3] ending @0x41A8 */
  uint8_t tail[HID_NPAD_BATTERY_LEVEL + 4u * HID_BATTERY_SLOTS - HID_NPAD_DEVICE_TYPE];
  memset(tail, 0, sizeof(tail));
  wr32(tail, device_type_for(style));
  uint64_t props = 0;
  if (style) {
    props = HID_PROP_ABXY_ORIENTED | HID_PROP_PLUS_AVAILABLE | HID_PROP_MINUS_AVAILABLE | HID_PROP_DIRECTIONAL_AVAILABLE;
    if (style == HID_STYLE_HANDHELD) props |= HID_PROP_IS_POWERED_ALL;
    for (uint32_t i = 0; i < HID_BATTERY_SLOTS; i++) {
      wr32(tail + (HID_NPAD_BATTERY_LEVEL - HID_NPAD_DEVICE_TYPE) + 4u * i, HID_BATTERY_FULL);
    }
  }
  wr64(tail + (HID_NPAD_SYSTEM_PROPERTIES - HID_NPAD_DEVICE_TYPE), props);
  (void)vmm_write_physical(c->vmm, base + HID_NPAD_DEVICE_TYPE, tail, sizeof(tail));
}

static uint32_t stick_bits(int32_t x, int32_t y, uint32_t left, uint32_t up, uint32_t right, uint32_t down) {
  uint32_t bits = 0;
  if (x <= -HID_STICK_DIRECTION_THRESHOLD) bits |= left;
  if (x >= HID_STICK_DIRECTION_THRESHOLD) bits |= right;
  if (y >= HID_STICK_DIRECTION_THRESHOLD) bits |= up;
  if (y <= -HID_STICK_DIRECTION_THRESHOLD) bits |= down;
  return bits;
}

/* HidNpadCommonState for one sample of `input` (NULL = no input). */
static void build_state(uint8_t out[HID_NPAD_STATE_BYTES], uint64_t sampling, const Input_Controller_State *input,
                        uint32_t attributes) {
  memset(out, 0, HID_NPAD_STATE_BYTES);
  wr64(out, sampling);
  if (!input) return;
  const int32_t lx = input->axes[INPUT_AXIS_LEFT_X], ly = input->axes[INPUT_AXIS_LEFT_Y];
  const int32_t rx = input->axes[INPUT_AXIS_RIGHT_X], ry = input->axes[INPUT_AXIS_RIGHT_Y];
  uint64_t buttons = input->buttons & HID_NPAD_BUTTON_MASK;
  buttons |= stick_bits(lx, ly, HID_BUTTON_STICK_L_LEFT, HID_BUTTON_STICK_L_UP, HID_BUTTON_STICK_L_RIGHT,
                        HID_BUTTON_STICK_L_DOWN);
  buttons |= stick_bits(rx, ry, HID_BUTTON_STICK_R_LEFT, HID_BUTTON_STICK_R_UP, HID_BUTTON_STICK_R_RIGHT,
                        HID_BUTTON_STICK_R_DOWN);
  wr64(out + 0x08, buttons);
  wr32(out + 0x10, (uint32_t)lx);
  wr32(out + 0x14, (uint32_t)ly);
  wr32(out + 0x18, (uint32_t)rx);
  wr32(out + 0x1C, (uint32_t)ry);
  wr32(out + 0x20, attributes);
}

/* Pushes one sample into all seven LIFOs of npad `index`: the LIFO of
 * its current style gets `input`, the others an empty state. */
static void push_sample(HLE_Context *c, Hid_State *s, uint32_t index, const Input_Controller_State *input) {
  Hid_Npad *npad = &s->npads[index];
  const uint32_t tail = (npad->tail + 1u) % HID_LIFO_ENTRIES;
  const uint32_t count = npad->count < HID_LIFO_ENTRIES ? npad->count + 1u : HID_LIFO_ENTRIES;
  const uint64_t base = npad_pa(s, index);
  for (uint32_t k = 0; k < HID_LIFO_KINDS; k++) {
    const bool live = npad->style != 0 && npad->style == k_lifo_styles[k];
    uint8_t storage[HID_LIFO_STORAGE_BYTES];
    memset(storage, 0, sizeof(storage));
    wr64(storage, s->sampling_number);
    build_state(storage + HID_LIFO_STATE_OFFSET, s->sampling_number, live ? input : NULL,
                live ? attributes_for(npad->style) : 0);
    const uint64_t lifo = base + k_lifo_offsets[k];
    (void)vmm_write_physical(c->vmm, lifo + HID_LIFO_STORAGE_OFFSET + (uint64_t)tail * HID_LIFO_STORAGE_BYTES,
                             storage, sizeof(storage));
    uint8_t header[0x18];
    wr64(header, HID_LIFO_ENTRIES);    /* buffer_count @0x8 */
    wr64(header + 0x8, tail);          /* tail @0x10 */
    wr64(header + 0x10, count);        /* count @0x18 */
    (void)vmm_write_physical(c->vmm, lifo + HID_LIFO_HEADER_BUFFER_COUNT, header, sizeof(header));
  }
  npad->tail = tail;
  npad->count = count;
}

/* One touch-screen sample from the input region's touch block. */
static void push_touch(HLE_Context *c, Hid_State *s, const Input_Touch_State *touch) {
  const uint64_t base = s->shared_memory->guest_pa + HID_TOUCH_SECTION_OFFSET;
  const uint32_t tail = (s->touch_tail + 1u) % HID_LIFO_ENTRIES;
  const uint32_t count = s->touch_count < HID_LIFO_ENTRIES ? s->touch_count + 1u : HID_LIFO_ENTRIES;
  uint8_t storage[HID_TOUCH_STORAGE_BYTES];
  memset(storage, 0, sizeof(storage));
  wr64(storage, s->sampling_number);
  uint8_t *state = storage + 8u;
  wr64(state, s->sampling_number);
  const uint32_t points = touch ? touch->count : 0u;
  memcpy(state + 8u, &points, 4);
  for (uint32_t p = 0; p < points && p < 2u; p++) {
    uint8_t *point = state + HID_TOUCH_POINTS_OFFSET + p * HID_TOUCH_POINT_BYTES;
    uint32_t attributes = 0;
    if (p >= s->touch_down) { /* newly down */
      s->finger_ids[p] = s->next_finger_id++;
      attributes |= HID_TOUCH_ATTRIBUTE_START;
    }
    const uint32_t x = touch->x[p], y = touch->y[p], diameter = HID_TOUCH_DIAMETER;
    memcpy(point + 0x08, &attributes, 4);
    memcpy(point + 0x0C, &s->finger_ids[p], 4);
    memcpy(point + 0x10, &x, 4);
    memcpy(point + 0x14, &y, 4);
    memcpy(point + 0x18, &diameter, 4);
    memcpy(point + 0x1C, &diameter, 4);
  }
  s->touch_down = points;
  (void)vmm_write_physical(c->vmm, base + HID_LIFO_STORAGE_OFFSET + (uint64_t)tail * HID_TOUCH_STORAGE_BYTES, storage,
                           sizeof(storage));
  uint8_t header[0x18];
  wr64(header, HID_LIFO_ENTRIES);
  wr64(header + 0x8, tail);
  wr64(header + 0x10, count);
  (void)vmm_write_physical(c->vmm, base + HID_LIFO_HEADER_BUFFER_COUNT, header, sizeof(header));
  s->touch_tail = tail;
  s->touch_count = count;
}

void hid_update(Hid_State *s, HLE_Context *c, const void *input_region, uint64_t now_ticks) {
  if (!s->shared_memory) return;
  if (s->sampled_once && now_ticks - s->last_sample_ticks < HID_TICKS_PER_SAMPLE) return;
  s->sampled_once = true;
  s->last_sample_ticks = now_ticks;
  s->sampling_number++;

  /* Which npad each input slot drives, and in which style. */
  Input_Controller_State inputs[HID_NPAD_COUNT];
  bool has_input[HID_NPAD_COUNT];
  uint32_t styles[HID_NPAD_COUNT];
  memset(has_input, 0, sizeof(has_input));
  memset(styles, 0, sizeof(styles));
  const uint32_t player = player_style(s->supported_styles);
  for (uint32_t slot = 0; slot < HID_PLAYER_SLOTS && input_region; slot++) {
    Input_Controller_State in;
    if (!input_region_read_slot(input_region, slot, &in) || !input_state_is_connected(&in)) continue;
    uint32_t index = slot, style = player;
    if (slot == 0 && (!(s->supported_ids & 1u) || !player) && (s->supported_ids & HID_ID_BIT_HANDHELD) &&
        (s->supported_styles & HID_STYLE_HANDHELD)) {
      index = HID_NPAD_HANDHELD_INDEX;
      style = HID_STYLE_HANDHELD;
    }
    if (!(s->supported_ids & (1u << index)) || !style) continue;
    inputs[index] = in;
    has_input[index] = true;
    styles[index] = style;
  }

  for (uint32_t i = 0; i < HID_NPAD_COUNT; i++) {
    Hid_Npad *npad = &s->npads[i];
    if (npad->style != styles[i]) {
      npad->style = styles[i];
      write_npad_header(c, s, i, npad->style);
      if (npad->style_event) hle_signal_event(c, npad->style_event);
    }
    push_sample(c, s, i, has_input[i] ? &inputs[i] : NULL);
  }
  Input_Touch_State touch;
  memset(&touch, 0, sizeof(touch));
  /* A torn read skips this sample rather than reporting a release. */
  if (!input_region || input_region_read_touch(input_region, &touch)) push_touch(c, s, &touch);
}

/* ------------------------------------------------------------------ */
/* Commands.                                                           */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_create_applet_resource(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)req;
  Hid_State *s = state_of(self);
  if (!s->shared_memory) {
    const Error err =
        shared_memory_create(s->pool, c->vmm, HID_SHARED_MEMORY_BYTES, SHARED_MEMORY_PERM_R, &s->shared_memory);
    if (!error_is_ok(err)) return HLE_RESULT_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < HID_NPAD_COUNT; i++) write_npad_header(c, s, i, 0);
  }
  (void)ipc_response_push_object(res, &s->applet_resource, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_shared_memory_handle(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                      IPC_Response *res) {
  (void)req;
  Hid_State *s = state_of(self);
  if (!s->shared_memory) return HLE_RESULT_INVALID_STATE;
  uint32_t handle = 0;
  shared_memory_retain(s->shared_memory);
  if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_SHARED_MEMORY, s->shared_memory, &handle))) {
    shared_memory_release(s->pool, s->shared_memory);
    return HLE_RESULT_OUT_OF_HANDLES;
  }
  (void)ipc_response_push_copy_handle(res, handle);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_supported_style_set(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                     IPC_Response *res) {
  (void)c;
  (void)res;
  uint32_t styles = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &styles))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  state_of(self)->supported_styles = styles;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_supported_style_set(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                     IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->supported_styles);
  return HLE_RESULT_SUCCESS;
}

/* The npad ids arrive as u32s in an X (pointer) buffer. */
static HLE_ServiceResult cmd_set_supported_npad_id_type(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                        IPC_Response *res) {
  (void)res;
  Hid_State *s = state_of(self);
  const IPC_Buffer *buf = req->static_count ? &req->statics[0] : (req->send_count ? &req->sends[0] : NULL);
  if (!buf) return HLE_RESULT_SUCCESS;
  uint32_t ids = 0;
  const uint64_t n = buf->size / sizeof(uint32_t);
  for (uint64_t i = 0; i < n; i++) {
    uint32_t id = 0;
    if (!error_is_ok(vmm_read32(c->vmm, buf->gva + i * sizeof(uint32_t), &id))) return HLE_RESULT_INVALID_POINTER;
    if (id < HID_PLAYER_SLOTS) ids |= 1u << id;
    else if (id == HID_NPAD_ID_HANDHELD) ids |= HID_ID_BIT_HANDHELD;
    else if (id == HID_NPAD_ID_OTHER) ids |= HID_ID_BIT_OTHER;
  }
  s->supported_ids = ids;
  return HLE_RESULT_SUCCESS;
}

static int32_t npad_index_of(uint32_t id) {
  if (id < HID_PLAYER_SLOTS) return (int32_t)id;
  if (id == HID_NPAD_ID_HANDHELD) return (int32_t)HID_NPAD_HANDHELD_INDEX;
  if (id == HID_NPAD_ID_OTHER) return (int32_t)HID_NPAD_OTHER_INDEX;
  return -1;
}

/* {u32 id, pad, u64 aruid, u64 event_ptr} -> copy handle. The event is
 * signalled once now (the initial style is news) and on every change. */
static HLE_ServiceResult cmd_acquire_style_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  Hid_State *s = state_of(self);
  uint32_t id = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &id))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  const int32_t index = npad_index_of(id);
  if (index < 0) return HLE_RESULT_INVALID_ENUM_VALUE;
  Hid_Npad *npad = &s->npads[index];
  uint32_t handle = 0;
  if (!npad->style_event) {
    const uint32_t result = hle_create_event(c, &handle, NULL, &npad->style_event);
    if (result != HLE_RESULT_SUCCESS) return result;
    event_retain(npad->style_event); /* hid keeps signalling it */
  } else {
    event_retain(npad->style_event);
    if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_EVENT_READABLE, npad->style_event, &handle))) {
      event_release(npad->style_event);
      return HLE_RESULT_OUT_OF_HANDLES;
    }
  }
  hle_signal_event(c, npad->style_event);
  (void)ipc_response_push_copy_handle(res, handle);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_player_led_pattern(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)self;
  uint32_t id = 0;
  (void)ipc_request_read_u32(req, 0, &id);
  /* LEDs 1..4 for players 1..4, then the console's two-LED patterns. */
  static const uint64_t k_patterns[HID_PLAYER_SLOTS] = {0x1, 0x3, 0x7, 0xF, 0x9, 0x5, 0xD, 0x6};
  (void)ipc_response_push_u64(res, id < HID_PLAYER_SLOTS ? k_patterns[id] : 0);
  return HLE_RESULT_SUCCESS;
}

/* Joy hold type and friends: {u64 aruid, u64 value}. */
static HLE_ServiceResult cmd_set_joy_hold_type(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)res;
  (void)ipc_request_read_u64(req, 8, &state_of(self)->joy_hold_type);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_joy_hold_type(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, state_of(self)->joy_hold_type);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_handheld_activation_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                          IPC_Response *res) {
  (void)c;
  (void)res;
  (void)ipc_request_read_u64(req, 8, &state_of(self)->handheld_activation_mode);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_handheld_activation_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, state_of(self)->handheld_activation_mode);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_communication_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)res;
  (void)ipc_request_read_u64(req, 8, &state_of(self)->communication_mode);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_communication_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, state_of(self)->communication_mode);
  return HLE_RESULT_SUCCESS;
}

/* SetNpadJoyAssignmentModeSingleWithDestination -> {bool, u32 id}. */
static HLE_ServiceResult cmd_assignment_with_destination(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                         IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0);
  (void)ipc_response_push_u32(res, 0);
  return HLE_RESULT_SUCCESS;
}

/* GetVibrationDeviceInfo(handle) -> {u32 type, u32 position}. The handle
 * is {u8 style_index, u8 player, u8 device_index, u8 pad}; LinearResonant
 * actuators (type 1) on every standard style, left/right by index. */
static HLE_ServiceResult cmd_get_vibration_device_info(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                       IPC_Response *res) {
  (void)c;
  (void)self;
  uint32_t handle = 0;
  (void)ipc_request_read_u32(req, 0, &handle);
  (void)ipc_response_push_u32(res, 1u);
  (void)ipc_response_push_u32(res, ((handle >> 16) & 0xFFu) ? 2u : 1u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_vibration_list(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                   IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->vibration_list, 0);
  return HLE_RESULT_SUCCESS;
}

/* Sorted by id. "_stub" marks accepted-and-inert commands (§12). */
static const Service_Command k_hid_commands[] = {
    {0, cmd_create_applet_resource, "CreateAppletResource"},
    {1, service_cmd_ok, "ActivateDebugPad_stub"},
    {11, service_cmd_ok, "ActivateTouchScreen_stub"},
    {21, service_cmd_ok, "ActivateMouse_stub"},
    {31, service_cmd_ok, "ActivateKeyboard_stub"},
    {32, service_cmd_ok, "SendKeyboardLockKeyEvent_stub"},
    {66, service_cmd_ok, "StartSixAxisSensor_stub"},
    {67, service_cmd_ok, "StopSixAxisSensor_stub"},
    {68, service_cmd_out_u8_false, "IsSixAxisSensorFusionEnabled_stub"},
    {69, service_cmd_ok, "EnableSixAxisSensorFusion_stub"},
    {70, service_cmd_ok, "SetSixAxisSensorFusionParameters_stub"},
    {71, service_cmd_out_u64_zero, "GetSixAxisSensorFusionParameters_stub"},
    {72, service_cmd_ok, "ResetSixAxisSensorFusionParameters_stub"},
    {79, service_cmd_ok, "SetGyroscopeZeroDriftMode_stub"},
    {80, service_cmd_out_u64_zero, "GetGyroscopeZeroDriftMode_stub"},
    {81, service_cmd_ok, "ResetGyroscopeZeroDriftMode_stub"},
    {82, service_cmd_out_u8_true, "IsSixAxisSensorAtRest_stub"},
    {83, service_cmd_out_u8_false, "IsFirmwareUpdateAvailableForSixAxisSensor_stub"},
    {91, service_cmd_ok, "ActivateGesture_stub"},
    {100, cmd_set_supported_style_set, "SetSupportedNpadStyleSet"},
    {101, cmd_get_supported_style_set, "GetSupportedNpadStyleSet"},
    {102, cmd_set_supported_npad_id_type, "SetSupportedNpadIdType"},
    {103, service_cmd_ok, "ActivateNpad"},
    {104, service_cmd_ok, "DeactivateNpad"},
    {106, cmd_acquire_style_event, "AcquireNpadStyleSetUpdateEventHandle"},
    {107, service_cmd_ok, "DisconnectNpad_stub"},
    {108, cmd_get_player_led_pattern, "GetPlayerLedPattern"},
    {109, service_cmd_ok, "ActivateNpadWithRevision"},
    {120, cmd_set_joy_hold_type, "SetNpadJoyHoldType"},
    {121, cmd_get_joy_hold_type, "GetNpadJoyHoldType"},
    {122, service_cmd_ok, "SetNpadJoyAssignmentModeSingleByDefault_stub"},
    {123, service_cmd_ok, "SetNpadJoyAssignmentModeSingle_stub"},
    {124, service_cmd_ok, "SetNpadJoyAssignmentModeDual_stub"},
    {125, service_cmd_ok, "MergeSingleJoyAsDualJoy_stub"},
    {126, service_cmd_ok, "StartLrAssignmentMode_stub"},
    {127, service_cmd_ok, "StopLrAssignmentMode_stub"},
    {128, cmd_set_handheld_activation_mode, "SetNpadHandheldActivationMode"},
    {129, cmd_get_handheld_activation_mode, "GetNpadHandheldActivationMode"},
    {130, service_cmd_ok, "SwapNpadAssignment_stub"},
    {131, service_cmd_out_u8_false, "IsUnintendedHomeButtonInputProtectionEnabled_stub"},
    {132, service_cmd_ok, "EnableUnintendedHomeButtonInputProtection_stub"},
    {133, cmd_assignment_with_destination, "SetNpadJoyAssignmentModeSingleWithDestination_stub"},
    {134, service_cmd_ok, "SetNpadAnalogStickUseCenterClamp_stub"},
    {135, service_cmd_ok, "SetNpadCaptureButtonAssignment_stub"},
    {136, service_cmd_ok, "ClearNpadCaptureButtonAssignment_stub"},
    {200, cmd_get_vibration_device_info, "GetVibrationDeviceInfo"},
    {201, service_cmd_ok, "SendVibrationValue_stub"},
    {202, service_cmd_out_zero128, "GetActualVibrationValue_stub"},
    {203, cmd_create_vibration_list, "CreateActiveVibrationDeviceList"},
    {204, service_cmd_ok, "PermitVibration_stub"},
    {205, service_cmd_out_u8_true, "IsVibrationPermitted_stub"},
    {206, service_cmd_ok, "SendVibrationValues_stub"},
    {207, service_cmd_ok, "SendVibrationGcErmCommand_stub"},
    {208, service_cmd_out_u64_zero, "GetActualVibrationGcErmCommand_stub"},
    {209, service_cmd_ok, "BeginPermitVibrationSession_stub"},
    {210, service_cmd_ok, "EndPermitVibrationSession_stub"},
    {211, service_cmd_out_u8_true, "IsVibrationDeviceMounted_stub"},
    {300, service_cmd_ok, "ActivateConsoleSixAxisSensor_stub"},
    {301, service_cmd_ok, "StartConsoleSixAxisSensor_stub"},
    {302, service_cmd_ok, "StopConsoleSixAxisSensor_stub"},
    {303, service_cmd_ok, "ActivateSevenSixAxisSensor_stub"},
    {304, service_cmd_ok, "StartSevenSixAxisSensor_stub"},
    {305, service_cmd_ok, "StopSevenSixAxisSensor_stub"},
    {306, service_cmd_ok, "InitializeSevenSixAxisSensor_stub"},
    {307, service_cmd_ok, "FinalizeSevenSixAxisSensor_stub"},
    {308, service_cmd_ok, "SetSevenSixAxisSensorFusionStrength_stub"},
    {309, service_cmd_out_u64_zero, "GetSevenSixAxisSensorFusionStrength_stub"},
    {310, service_cmd_ok, "ResetSevenSixAxisSensorTimestamp_stub"},
    {400, service_cmd_out_u8_false, "IsUsbFullKeyControllerEnabled_stub"},
    {401, service_cmd_ok, "EnableUsbFullKeyController_stub"},
    {1000, cmd_set_communication_mode, "SetNpadCommunicationMode"},
    {1001, cmd_get_communication_mode, "GetNpadCommunicationMode"},
    {1002, service_cmd_ok, "SetTouchScreenConfiguration_stub"},
    {1003, service_cmd_out_u8_false, "IsFirmwareUpdateNeededForNotification_stub"},
};

static const Service_Command k_applet_resource_commands[] = {
    {0, cmd_get_shared_memory_handle, "GetSharedMemoryHandle"},
};

static const Service_Command k_vibration_list_commands[] = {
    {0, service_cmd_ok, "ActivateVibrationDevice_stub"},
};

void hid_init(Hid_State *state, Shared_Memory_Pool *pool) {
  memset(state, 0, sizeof(*state));
  state->pool = pool;
  state->supported_styles = HID_STYLE_STANDARD;
  state->supported_ids = HID_ALL_IDS;
  state->interface = SERVICE_INTERFACE("hid", k_hid_commands, 0, state);
  state->applet_resource = SERVICE_INTERFACE("IAppletResource", k_applet_resource_commands, 0, state);
  state->vibration_list = SERVICE_INTERFACE("IActiveVibrationDeviceList", k_vibration_list_commands, 0, state);
}

Error hid_register(Hid_State *state, SM_Registry *registry) { return sm_registry_add(registry, "hid", &state->interface); }
