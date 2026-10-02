/**
 * hid + shared memory over real IPC and SVCs (§12, §18): sm:
 * GetServiceHandle("hid"), CreateAppletResource -> GetSharedMemoryHandle,
 * svcMapSharedMemory (and its error paths), npad configuration, then the
 * input region sampled into the mapped block and read back the way
 * libnx's _hidGetStates reads it - through the guest mapping, via vmm.
 */
#define CHECK_NAME "hid_test"
#include "check.h"

#include "common/input_region.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/shared_memory.h"
#include "hle/kernel/svc_memory.h"
#include "hle/services/hid/hid.h"
#include "ipc_fixtures.h"

#include <string.h>

#define SVC_MAP_SHARED_MEMORY 0x13u
#define SVC_UNMAP_SHARED_MEMORY 0x14u
#define SVC_CLOSE_HANDLE 0x16u
#define SVC_RESET_SIGNAL 0x17u
#define SVC_QUERY_MEMORY 0x06u
#define PERM_R 1u
#define PERM_RW 3u
#define NPAD_ID_HANDHELD 0x20u

static Emulator g_emu;
static uint8_t g_reply[TEST_IPC_BUFFER_BYTES];
static _Alignas(8) uint8_t g_region[LAYOUT_INPUT_REGION_SIZE];
static uint64_t g_shmem_gva;
static uint64_t g_ticks;

static uint32_t svc(uint32_t number, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t *out_x1) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[0] = x0;
  regs->x[1] = x1;
  regs->x[2] = x2;
  regs->x[3] = x3;
  hle_on_svc(g_emu.cpu_state, number, &g_emu.hle);
  if (out_x1) *out_x1 = regs->x[1];
  return (uint32_t)regs->x[0];
}

static Test_Ipc_Reply call(uint32_t handle, uint32_t command, const void *payload, uint32_t size,
                           const Test_Ipc_Message *extra) {
  Test_Ipc_Message m;
  if (extra) m = *extra;
  else memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = command;
  m.payload = payload;
  m.payload_size = size;
  CHECK(ipc_fixture_send(&g_emu, handle, &m, g_reply) == 0);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, TEST_IPC_CMIF, false, 0, &reply);
  CHECK(reply.sfco_ok);
  return reply;
}

static uint32_t get_service(const char *name) {
  uint32_t sm = 0;
  CHECK(ipc_fixture_connect(&g_emu, "sm:", &sm) == 0);
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.send_pid = true;
  (void)call(sm, 0, NULL, 0, &m); /* RegisterClient */
  uint64_t wire = 0;
  memcpy(&wire, name, strlen(name));
  const Test_Ipc_Reply reply = call(sm, 1, &wire, sizeof(wire), NULL);
  CHECK(reply.result == 0 && reply.move_count == 1);
  return reply.move_handles[0];
}

/* A free, page-aligned spot in the ASLR region outside heap/alias/stack. */
static uint64_t find_free_view(uint64_t size) {
  const Address_Space *as = &g_emu.process.address_space;
  const Address_Region *avoid[] = {&as->heap, &as->alias, &as->stack, &as->code, &as->tls_io};
  for (uint64_t at = as->aslr.base; at + size <= as->aslr.base + as->aslr.size; at += 0x200000u) {
    bool clear = true;
    for (size_t i = 0; i < sizeof(avoid) / sizeof(avoid[0]) && clear; i++) {
      const Address_Region *r = avoid[i];
      if (r->size && at < r->base + r->size && r->base < at + size) clear = false;
    }
    VMM_Region_Info info;
    if (clear && error_is_ok(vmm_query(g_emu.vmm, at, &info)) && !info.is_mapped && info.base_gva + info.size >= at + size) {
      return at;
    }
  }
  CHECK(!"no free view");
  return 0;
}

static void set_slot(uint32_t slot, uint32_t buttons, int16_t lx, int16_t ly, bool connected) {
  Input_Controller_State state;
  memset(&state, 0, sizeof(state));
  state.buttons = buttons;
  state.axes[INPUT_AXIS_LEFT_X] = lx;
  state.axes[INPUT_AXIS_LEFT_Y] = ly;
  state.flags = connected ? (INPUT_FLAG_CONNECTED | (INPUT_DEVICE_STANDARD_GAMEPAD << INPUT_FLAG_DEVICE_KIND_SHIFT)) : 0;
  input_region_write_begin(g_region, slot);
  input_region_write_payload(g_region, slot, &state);
  input_region_write_end(g_region, slot);
}

static void sample(void) {
  hid_update(&g_emu.hid, &g_emu.hle, g_region, g_ticks);
  g_ticks += HID_TICKS_PER_SAMPLE;
}

static uint64_t npad_gva(uint32_t index) { return g_shmem_gva + HID_NPAD_SECTION_OFFSET + index * HID_NPAD_ENTRY_BYTES; }

static uint32_t rd32(uint64_t gva) {
  uint32_t v = 0;
  CHECK_OK(vmm_read32(g_emu.vmm, gva, &v));
  return v;
}
static uint64_t rd64(uint64_t gva) {
  uint64_t v = 0;
  CHECK_OK(vmm_read64(g_emu.vmm, gva, &v));
  return v;
}

typedef struct Npad_State {
  uint64_t sampling, buttons;
  int32_t lx, ly, rx, ry;
  uint32_t attributes;
} Npad_State;

/* libnx _hidGetStates, restated: newest first, from `tail` backwards. */
static uint32_t read_lifo(uint32_t index, uint32_t lifo_offset, Npad_State *out, uint32_t max) {
  const uint64_t lifo = npad_gva(index) + lifo_offset;
  uint32_t total = (uint32_t)rd64(lifo + HID_LIFO_HEADER_COUNT);
  if (total > max) total = max;
  const uint32_t tail = (uint32_t)rd64(lifo + HID_LIFO_HEADER_TAIL);
  for (uint32_t i = 0; i < total; i++) {
    const uint32_t pos = (tail + HID_LIFO_ENTRIES - i) % HID_LIFO_ENTRIES;
    const uint64_t storage = lifo + HID_LIFO_STORAGE_OFFSET + pos * HID_LIFO_STORAGE_BYTES;
    const uint64_t st = storage + HID_LIFO_STATE_OFFSET;
    CHECK(rd64(storage) == rd64(st)); /* storage and state sampling numbers agree */
    out[i] = (Npad_State){rd64(st), rd64(st + 8), (int32_t)rd32(st + 0x10), (int32_t)rd32(st + 0x14),
                          (int32_t)rd32(st + 0x18), (int32_t)rd32(st + 0x1C), rd32(st + 0x20)};
  }
  return total;
}

#define LIFO_FULL_KEY 0x28u
#define LIFO_HANDHELD 0x378u

static void test_shared_memory_and_npads(void) {
  const uint32_t hid = get_service("hid");
  const uint64_t aruid = 0;
  Test_Ipc_Message pid;
  memset(&pid, 0, sizeof(pid));
  pid.send_pid = true;

  /* GetSharedMemoryHandle before CreateAppletResource is impossible (no
   * object yet); CreateAppletResource returns IAppletResource. */
  Test_Ipc_Reply reply = call(hid, 0, &aruid, sizeof(aruid), &pid);
  CHECK(reply.result == 0 && reply.move_count == 1);
  const uint32_t resource = reply.move_handles[0];
  reply = call(resource, 0, NULL, 0, NULL);
  CHECK(reply.result == 0 && reply.copy_count == 1);
  const uint32_t shmem = reply.copy_handles[0];
  Kernel_Shared_Memory *object =
      handle_table_get(&g_emu.process.handles, shmem, KERNEL_OBJECT_SHARED_MEMORY);
  CHECK(object != NULL && object->size == HID_SHARED_MEMORY_BYTES && object->references == 2);
  /* A second CreateAppletResource reuses the same block. */
  reply = call(hid, 0, &aruid, sizeof(aruid), &pid);
  CHECK(reply.result == 0 && g_emu.hid.shared_memory == object);

  /* svcMapSharedMemory error paths, then the real map. */
  const uint64_t view = find_free_view(HID_SHARED_MEMORY_BYTES);
  const Address_Space *as = &g_emu.process.address_space;
  CHECK(svc(SVC_MAP_SHARED_MEMORY, 0x1234, view, HID_SHARED_MEMORY_BYTES, PERM_R, NULL) == HLE_RESULT_INVALID_HANDLE);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, shmem, view + 1, HID_SHARED_MEMORY_BYTES, PERM_R, NULL) == HLE_RESULT_INVALID_POINTER);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, shmem, view, 0x1000, PERM_R, NULL) == HLE_RESULT_INVALID_SIZE);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, shmem, view, HID_SHARED_MEMORY_BYTES, PERM_RW, NULL) ==
        HLE_RESULT_INVALID_NEW_MEMORY_PERMISSION);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, shmem, as->heap.base, HID_SHARED_MEMORY_BYTES, PERM_R, NULL) ==
        HLE_RESULT_INVALID_MEMORY_RANGE);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, shmem, view, HID_SHARED_MEMORY_BYTES, PERM_R, NULL) == 0);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, shmem, view, HID_SHARED_MEMORY_BYTES, PERM_R, NULL) == HLE_RESULT_INVALID_STATE);
  g_shmem_gva = view;

  /* QueryMemory names it SharedMem, read-only; guest writes fault. */
  const uint64_t info_gva = g_emu.process.main_thread_stack.base + 0x3000;
  CHECK(svc(SVC_QUERY_MEMORY, info_gva, 0, view + 0x100, 0, NULL) == 0);
  CHECK(rd64(info_gva) == view && rd64(info_gva + 8) == HID_SHARED_MEMORY_BYTES);
  CHECK(rd32(info_gva + 16) == HLE_MEMTYPE_SHARED && rd32(info_gva + 24) == PERM_R);
  CHECK(!error_is_ok(vmm_write32(g_emu.vmm, view, 1)));

  /* Before any sample every npad says "no controller". */
  CHECK(rd32(npad_gva(0) + HID_NPAD_STYLE_SET) == 0);
  CHECK(rd32(npad_gva(0) + HID_NPAD_FULL_KEY_COLOR) == 2u);

  /* A title supporting FullKey + Handheld on No1 and Handheld. */
  const struct { uint32_t style, pad; uint64_t aruid; } styles = {HID_STYLE_FULL_KEY | HID_STYLE_HANDHELD, 0, 0};
  CHECK(call(hid, 100, &styles, sizeof(styles), &pid).result == 0);
  reply = call(hid, 101, &aruid, sizeof(aruid), &pid);
  CHECK(test_le32(reply.data) == (HID_STYLE_FULL_KEY | HID_STYLE_HANDHELD));
  const uint64_t ids_gva = g_emu.process.main_thread_stack.base + 0x2000;
  const uint32_t ids[2] = {0, NPAD_ID_HANDHELD};
  CHECK_OK(vmm_write_block(g_emu.vmm, ids_gva, ids, sizeof(ids)));
  Test_Ipc_Message x = pid;
  x.statics[0] = (Test_Ipc_Buffer){ids_gva, sizeof(ids), 0};
  x.static_count = 1;
  CHECK(call(hid, 102, &aruid, sizeof(aruid), &x).result == 0);
  CHECK(g_emu.hid.supported_ids == ((1u << 0) | (1u << HID_NPAD_HANDHELD_INDEX)));
  const struct { uint32_t revision, pad; uint64_t aruid; } revision = {3, 0, 0};
  CHECK(call(hid, 109, &revision, sizeof(revision), &pid).result == 0);

  /* The style-set event: signalled on acquire. */
  const struct { uint32_t id, pad; uint64_t aruid, ptr; } ev_in = {0, 0, 0, 0};
  reply = call(hid, 106, &ev_in, sizeof(ev_in), &pid);
  CHECK(reply.result == 0 && reply.copy_count == 1);
  const uint32_t style_event = reply.copy_handles[0];
  Kernel_Event *event = handle_table_get(&g_emu.process.handles, style_event, KERNEL_OBJECT_EVENT_READABLE);
  CHECK(event && event->signaled);
  CHECK(svc(SVC_RESET_SIGNAL, style_event, 0, 0, 0, NULL) == 0 && !event->signaled);

  /* Player 1 holds A + D-pad up, left stick hard right and down. */
  set_slot(0, INPUT_BUTTON_A | INPUT_BUTTON_DPAD_UP | INPUT_BUTTON_HOME, 30000, -20000, true);
  sample();
  CHECK(rd32(npad_gva(0) + HID_NPAD_STYLE_SET) == HID_STYLE_FULL_KEY);
  CHECK(rd32(npad_gva(0) + HID_NPAD_DEVICE_TYPE) == HID_DEVICE_FULL_KEY);
  CHECK(rd32(npad_gva(0) + HID_NPAD_FULL_KEY_COLOR) == 0u);
  CHECK(rd32(npad_gva(0) + HID_NPAD_BATTERY_LEVEL) == 4u);
  CHECK(rd32(npad_gva(HID_NPAD_HANDHELD_INDEX) + HID_NPAD_STYLE_SET) == 0);
  CHECK(event->signaled); /* the style changed */
  Npad_State states[HID_LIFO_ENTRIES];
  CHECK(read_lifo(0, LIFO_FULL_KEY, states, HID_LIFO_ENTRIES) == 1);
  CHECK(states[0].buttons == (INPUT_BUTTON_A | INPUT_BUTTON_DPAD_UP | HID_BUTTON_STICK_L_RIGHT | HID_BUTTON_STICK_L_DOWN));
  CHECK(states[0].lx == 30000 && states[0].ly == -20000 && states[0].rx == 0 && states[0].ry == 0);
  CHECK(states[0].attributes == HID_ATTR_IS_CONNECTED);
  /* The other LIFOs of the same npad advance in step, empty. */
  CHECK(read_lifo(0, LIFO_HANDHELD, states, HID_LIFO_ENTRIES) == 1);
  CHECK(states[0].buttons == 0 && states[0].attributes == 0);

  /* 200Hz: a second update inside the period writes nothing. */
  const uint64_t sampling = g_emu.hid.sampling_number;
  hid_update(&g_emu.hid, &g_emu.hle, g_region, g_ticks - 1);
  CHECK(g_emu.hid.sampling_number == sampling);

  /* Twenty more samples: the LIFO caps at 17 with consecutive numbers. */
  for (int i = 0; i < 20; i++) {
    set_slot(0, (uint32_t)i & 1u ? INPUT_BUTTON_B : 0, 0, 0, true);
    sample();
  }
  const uint32_t n = read_lifo(0, LIFO_FULL_KEY, states, HID_LIFO_ENTRIES);
  CHECK(n == HID_LIFO_ENTRIES);
  for (uint32_t i = 1; i < n; i++) CHECK(states[i - 1].sampling - states[i].sampling == 1);
  CHECK(states[0].buttons == INPUT_BUTTON_B && states[1].buttons == 0);

  /* Disconnect: style 0, colors say NoController, event signalled. */
  CHECK(svc(SVC_RESET_SIGNAL, style_event, 0, 0, 0, NULL) == 0);
  set_slot(0, 0, 0, 0, false);
  sample();
  CHECK(rd32(npad_gva(0) + HID_NPAD_STYLE_SET) == 0 && rd32(npad_gva(0) + HID_NPAD_FULL_KEY_COLOR) == 2u);
  CHECK(event->signaled);

  /* A handheld-only title: slot 0 becomes the Handheld npad. */
  const struct { uint32_t style, pad; uint64_t aruid; } handheld = {HID_STYLE_HANDHELD, 0, 0};
  CHECK(call(hid, 100, &handheld, sizeof(handheld), &pid).result == 0);
  set_slot(0, INPUT_BUTTON_X, 0, 0, true);
  sample();
  CHECK(rd32(npad_gva(0) + HID_NPAD_STYLE_SET) == 0);
  CHECK(rd32(npad_gva(HID_NPAD_HANDHELD_INDEX) + HID_NPAD_STYLE_SET) == HID_STYLE_HANDHELD);
  CHECK(read_lifo(HID_NPAD_HANDHELD_INDEX, LIFO_HANDHELD, states, 1) == 1);
  CHECK(states[0].buttons == INPUT_BUTTON_X && (states[0].attributes & 0x3Fu) == 0x3Fu);

  /* Player 2 on an unsupported id stays disconnected. */
  set_slot(1, INPUT_BUTTON_A, 0, 0, true);
  sample();
  CHECK(rd32(npad_gva(1) + HID_NPAD_STYLE_SET) == 0);

  /* Unmap (exact range only), close: hid keeps its reference. */
  CHECK(svc(SVC_UNMAP_SHARED_MEMORY, shmem, view + 0x1000, HID_SHARED_MEMORY_BYTES, 0, NULL) == HLE_RESULT_INVALID_MEMORY_RANGE);
  CHECK(svc(SVC_UNMAP_SHARED_MEMORY, shmem, view, HID_SHARED_MEMORY_BYTES, 0, NULL) == 0);
  VMM_Region_Info info;
  CHECK_OK(vmm_query(g_emu.vmm, view, &info));
  CHECK(!info.is_mapped);
  CHECK(svc(SVC_CLOSE_HANDLE, shmem, 0, 0, 0, NULL) == 0);
  CHECK(object->references == 1 && g_emu.hid.shared_memory == object);
  sample(); /* still writes physical memory without a view */

  /* Re-acquire and re-map works (same object). */
  reply = call(resource, 0, NULL, 0, NULL);
  CHECK(reply.copy_count == 1);
  CHECK(svc(SVC_MAP_SHARED_MEMORY, reply.copy_handles[0], view, HID_SHARED_MEMORY_BYTES, PERM_R, NULL) == 0);
  CHECK(rd32(npad_gva(HID_NPAD_HANDHELD_INDEX) + HID_NPAD_STYLE_SET) == HID_STYLE_HANDHELD);
}

/* The touch-screen LIFO (HidTouchScreenLifo @0x400) as libnx reads it. */
static uint64_t touch_latest(void) {
  const uint64_t lifo = g_shmem_gva + HID_TOUCH_SECTION_OFFSET;
  CHECK(rd64(lifo + HID_LIFO_HEADER_BUFFER_COUNT) == HID_LIFO_ENTRIES);
  const uint64_t tail = rd64(lifo + HID_LIFO_HEADER_TAIL);
  CHECK(tail < HID_LIFO_ENTRIES);
  return lifo + HID_LIFO_STORAGE_OFFSET + tail * HID_TOUCH_STORAGE_BYTES + 8u; /* the HidTouchScreenState */
}

static void set_touch(uint32_t count, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  Input_Touch_State t;
  memset(&t, 0, sizeof(t));
  t.count = count;
  t.x[0] = x0; t.y[0] = y0; t.x[1] = x1; t.y[1] = y1;
  input_region_write_touch(g_region, &t);
}

static void test_touch_screen(void) {
  set_touch(1, 640, 360, 0, 0);
  sample();
  uint64_t state = touch_latest();
  const uint64_t first_sampling = rd64(state);
  CHECK(rd32(state + 8) == 1);
  uint64_t point = state + HID_TOUCH_POINTS_OFFSET;
  CHECK(rd32(point + 0x08) == HID_TOUCH_ATTRIBUTE_START);
  const uint32_t finger = rd32(point + 0x0C);
  CHECK(rd32(point + 0x10) == 640 && rd32(point + 0x14) == 360);
  CHECK(rd32(point + 0x18) == HID_TOUCH_DIAMETER);

  /* Held and moved: same finger, no Start; a second finger starts. */
  set_touch(2, 650, 370, 10, 20);
  sample();
  state = touch_latest();
  CHECK(rd64(state) == first_sampling + 1);
  CHECK(rd32(state + 8) == 2);
  point = state + HID_TOUCH_POINTS_OFFSET;
  CHECK(rd32(point + 0x08) == 0 && rd32(point + 0x0C) == finger);
  CHECK(rd32(point + 0x10) == 650 && rd32(point + 0x14) == 370);
  point += HID_TOUCH_POINT_BYTES;
  CHECK(rd32(point + 0x08) == HID_TOUCH_ATTRIBUTE_START && rd32(point + 0x0C) != finger);
  CHECK(rd32(point + 0x10) == 10 && rd32(point + 0x14) == 20);

  /* Released: zero points; a new touch gets a new finger id. */
  set_touch(0, 0, 0, 0, 0);
  sample();
  CHECK(rd32(touch_latest() + 8) == 0);
  set_touch(1, 1, 2, 0, 0);
  sample();
  point = touch_latest() + HID_TOUCH_POINTS_OFFSET;
  CHECK(rd32(point + 0x08) == HID_TOUCH_ATTRIBUTE_START && rd32(point + 0x0C) != finger);
  /* Every sample (the npad test's too) pushed an entry: the LIFO is full. */
  CHECK(rd64(g_shmem_gva + HID_TOUCH_SECTION_OFFSET + HID_LIFO_HEADER_COUNT) == HID_LIFO_ENTRIES);
  set_touch(0, 0, 0, 0, 0);
}

static void test_misc_commands(void) {
  const uint32_t hid = get_service("hid");
  const struct { uint32_t id, pad; uint64_t aruid; } led = {2, 0, 0};
  Test_Ipc_Reply reply = call(hid, 108, &led, sizeof(led), NULL);
  CHECK(reply.result == 0 && test_le64(reply.data) == 0x7);
  const struct { uint64_t aruid, type; } hold = {0, 1};
  CHECK(call(hid, 120, &hold, sizeof(hold), NULL).result == 0);
  reply = call(hid, 121, &hold.aruid, sizeof(hold.aruid), NULL);
  CHECK(test_le64(reply.data) == 1);
  reply = call(hid, 203, NULL, 0, NULL);
  CHECK(reply.result == 0 && reply.move_count == 1);
  const uint32_t device = 0x00010000u;
  CHECK(call(reply.move_handles[0], 0, &device, sizeof(device), NULL).result == 0);
  reply = call(hid, 200, &device, sizeof(device), NULL);
  CHECK(test_le32(reply.data) == 1 && test_le32(reply.data + 4) == 2);
  /* An unknown command answers the sf result, not a crash. */
  reply = call(hid, 9999, NULL, 0, NULL);
  CHECK(reply.result == IPC_RESULT_SF_UNKNOWN_COMMAND);
}

int main(void) {
  ipc_fixture_boot(&g_emu);
  test_shared_memory_and_npads();
  test_touch_screen();
  test_misc_commands();
  /* Unload releases everything; a reload starts clean. */
  emulator_unload_program(&g_emu);
  CHECK(g_emu.hid.shared_memory == NULL);
  for (uint32_t i = 0; i < SHARED_MEMORY_POOL_CAPACITY; i++) CHECK(g_emu.shared_memory.objects[i].references == 0);
  emulator_destroy(&g_emu);
  printf("[hid_test] passed\n");
  return 0;
}
