/**
 * nvdrv over real IPC (§12, §13): sm: GetServiceHandle("nvdrv"), Open of
 * each device, and the ioctls a homebrew/NVN startup issues - nvmap
 * lifecycle, address-space mapping, GPU channel GPFIFO submit with a
 * fence, syncpoint read/wait on nvhost-ctrl, a VIC submit, GPU
 * characteristics, and EVENT_WAIT_ASYNC completed through the GPU
 * completion ring. Buffers live in the main thread's stack; requests are
 * built by tests/ipc_fixtures.c (libnx layout restated).
 */
#define CHECK_NAME "nvdrv_test"
#include "check.h"

#include "gpu/syncpoint.h"
#include "hle/kernel/handle_table.h"
#include "hle/services/nvdrv/nvdrv.h"
#include "ipc_fixtures.h"

#include <stdio.h>
#include <string.h>

#define IOWR(type, nr, size) ((3u << 30) | ((uint32_t)(size) << 16) | ((type) << 8) | (nr))
#define IOW(type, nr, size) ((1u << 30) | ((uint32_t)(size) << 16) | ((type) << 8) | (nr))

static Emulator g_emu;
static uint32_t g_nvdrv;
static uint64_t g_in_gva, g_out_gva, g_path_gva;
static uint8_t g_reply[TEST_IPC_BUFFER_BYTES];

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static Test_Ipc_Reply call(uint32_t command, const void *payload, uint32_t size, const Test_Ipc_Message *extra) {
  Test_Ipc_Message m;
  if (extra) m = *extra;
  else memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = command;
  m.payload = payload;
  m.payload_size = size;
  CHECK(ipc_fixture_send(&g_emu, g_nvdrv, &m, g_reply) == 0);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, TEST_IPC_CMIF, false, 8, &reply);
  CHECK(reply.sfco_ok && reply.result == 0);
  return reply;
}

static uint32_t open_device(const char *path) {
  CHECK_OK(vmm_write_block(g_emu.vmm, g_path_gva, path, strlen(path) + 1));
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.sends[0] = (Test_Ipc_Buffer){g_path_gva, strlen(path), 0};
  m.send_count = 1;
  const Test_Ipc_Reply reply = call(0, NULL, 0, &m);
  return rd32(reply.data + 4) == 0 ? rd32(reply.data) : 0xFFFFFFFFu;
}

/* Runs an ioctl with `data` as the in/out struct; returns the NvError. */
static uint32_t ioctl(uint32_t fd, uint32_t request, uint8_t *data) {
  const uint32_t size = (request >> 16) & 0x3FFFu;
  CHECK_OK(vmm_write_block(g_emu.vmm, g_in_gva, data, size));
  const uint32_t in[2] = {fd, request};
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.sends[0] = (Test_Ipc_Buffer){g_in_gva, size, 0};
  m.send_count = 1;
  m.receives[0] = (Test_Ipc_Buffer){g_out_gva, size, 0};
  m.receive_count = 1;
  const Test_Ipc_Reply reply = call(1, in, sizeof(in), &m);
  if ((request >> 30) & 2u) CHECK_OK(vmm_read_block(g_emu.vmm, g_out_gva, data, size));
  return rd32(reply.data);
}

static void get_nvdrv(void) {
  uint32_t sm = 0;
  CHECK(ipc_fixture_connect(&g_emu, "sm:", &sm) == 0);
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.send_pid = true;
  CHECK(ipc_fixture_send(&g_emu, sm, &m, g_reply) == 0); /* RegisterClient */
  uint64_t name = 0;
  memcpy(&name, "nvdrv", 5);
  m.send_pid = false;
  m.command_id = 1;
  m.payload = &name;
  m.payload_size = sizeof(name);
  CHECK(ipc_fixture_send(&g_emu, sm, &m, g_reply) == 0);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, TEST_IPC_CMIF, false, 0, &reply);
  CHECK(reply.result == 0 && reply.move_count == 1);
  g_nvdrv = reply.move_handles[0];
}

int main(void) {
  ipc_fixture_boot(&g_emu);
  const uint64_t stack = g_emu.process.main_thread_stack.base;
  g_path_gva = stack + 0x1000;
  g_in_gva = stack + 0x2000;
  g_out_gva = stack + 0x6000;
  get_nvdrv();

  /* Initialize, then a bad path. */
  const uint32_t init_in = 0x300000;
  CHECK(rd32(call(3, &init_in, 4, NULL).data) == 0);
  CHECK(open_device("/dev/nope") == 0xFFFFFFFFu);

  const uint32_t nvmap = open_device("/dev/nvmap");
  const uint32_t ctrl = open_device("/dev/nvhost-ctrl");
  const uint32_t as = open_device("/dev/nvhost-as-gpu");
  const uint32_t gpu = open_device("/dev/nvhost-gpu");
  const uint32_t vic = open_device("/dev/nvhost-vic");
  const uint32_t ctrl_gpu = open_device("/dev/nvhost-ctrl-gpu");
  CHECK(nvmap != 0xFFFFFFFFu && ctrl != 0xFFFFFFFFu && as != 0xFFFFFFFFu && gpu != 0xFFFFFFFFu &&
        vic != 0xFFFFFFFFu && ctrl_gpu != 0xFFFFFFFFu && nvmap != ctrl);

  /* nvmap: create, alloc, param, id round trip, two-step free. */
  uint8_t d[0x200];
  memset(d, 0, sizeof(d));
  wr32(d, 0x10000);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x01u, 8), d) == 0);
  const uint32_t handle = rd32(d + 4);
  CHECK(handle != 0);
  memset(d, 0, sizeof(d));
  wr32(d, handle); wr32(d + 4, 1); wr32(d + 8, 1); wr32(d + 12, 0x1000); d[16] = 0; wr64(d + 24, stack + 0x10000);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x04u, 32), d) == 0);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x04u, 32), d) == NV_ALREADY_ALLOCATED);
  memset(d, 0, sizeof(d));
  wr32(d, handle); wr32(d + 4, 1);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x09u, 12), d) == 0 && rd32(d + 8) == 0x10000);
  memset(d, 0, sizeof(d));
  wr32(d + 4, handle);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x0Eu, 8), d) == 0);
  const uint32_t id = rd32(d);
  memset(d, 0, sizeof(d));
  wr32(d, id);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x03u, 8), d) == 0 && rd32(d + 4) == handle);

  /* Address space: map the buffer, get the VA regions. */
  memset(d, 0, sizeof(d));
  wr32(d + 4, 0xFFFFFFFFu); wr32(d + 8, handle);
  CHECK(ioctl(as, IOWR(0x41u, 0x06u, 40), d) == 0);
  const uint64_t gpu_va = rd64(d + 32);
  CHECK(gpu_va >= 0x04000000ull && rd32(d + 12) == 0x1000);
  memset(d, 0, sizeof(d));
  wr32(d + 8, 48);
  CHECK(ioctl(as, IOWR(0x41u, 0x08u, 64), d) == 0 && rd64(d + 16) == 0x04000000ull);
  memset(d, 0, sizeof(d));
  wr64(d, gpu_va);
  CHECK(ioctl(as, IOWR(0x41u, 0x05u, 8), d) == 0);
  CHECK(ioctl(as, IOWR(0x41u, 0x05u, 8), d) == NV_BAD_VALUE); /* already unmapped */

  /* GPU channel: GPFIFO gives a fence; a submission with FENCE_GET completes it. */
  memset(d, 0, sizeof(d));
  wr32(d, 0x800);
  CHECK(ioctl(gpu, IOWR(0x48u, 0x1Au, 32), d) == 0);
  const uint32_t syncpoint = rd32(d + 12);
  CHECK(syncpoint != 0);
  memset(d, 0, sizeof(d));
  wr32(d + 8, 1); wr32(d + 12, 1u << 1); wr64(d + 24, 0x2000000000000000ull);
  CHECK(ioctl(gpu, IOWR(0x48u, 0x08u, 32), d) == 0);
  CHECK(rd32(d + 16) == syncpoint && rd32(d + 20) == 1);

  /* nvhost-ctrl sees it: read 1, wait(1) ok, wait(5) times out. */
  memset(d, 0, sizeof(d));
  wr32(d, syncpoint);
  CHECK(ioctl(ctrl, IOWR(0x00u, 0x14u, 8), d) == 0 && rd32(d + 4) == 1);
  memset(d, 0, sizeof(d));
  wr32(d, syncpoint); wr32(d + 4, 1); wr32(d + 8, 0xFFFFFFFFu);
  CHECK(ioctl(ctrl, IOWR(0x00u, 0x1Du, 16), d) == 0 && rd32(d + 12) == 1);
  wr32(d + 4, 5);
  CHECK(ioctl(ctrl, IOWR(0x00u, 0x1Du, 16), d) == NV_TIMEOUT);

  /* VIC: one syncpoint-increment entry of 2 -> fence threshold 2. */
  memset(d, 0, sizeof(d));
  wr32(d + 8, 1); wr32(d + 12, 1);              /* 1 incr, 1 fence */
  wr32(d + 16, 0); wr32(d + 20, 2);             /* syncpt_incrs[0] = {id, 2} */
  CHECK(ioctl(vic, IOWR(0x00u, 0x01u, 28), d) == 0 && rd32(d + 24) == 2);
  memset(d, 0, sizeof(d));
  CHECK(ioctl(vic, IOWR(0x00u, 0x02u, 8), d) == 0 && rd32(d + 4) != 0 && rd32(d + 4) != syncpoint);

  /* Characteristics: GM20B. */
  memset(d, 0, sizeof(d));
  wr64(d, 0xA0);
  CHECK(ioctl(ctrl_gpu, IOWR(0x47u, 0x05u, 0xB0), d) == 0);
  CHECK(rd32(d + 16) == 0x120 && rd32(d + 16 + 0x5C) == 0xB197 && rd64(d + 16 + 0x90) == 0x6230326D67ull);

  /* Ioctl3 (the Nintendo SDK's NvRm): the characteristics land in the
   * second output buffer. */
  {
    memset(d, 0, sizeof(d));
    wr64(d, 0xA0);
    CHECK_OK(vmm_write_block(g_emu.vmm, g_in_gva, d, 0xB0));
    const uint64_t extra_gva = g_out_gva + 0x800u;
    CHECK_OK(vmm_write_block(g_emu.vmm, extra_gva, d, 0xA0)); /* zeros */
    const uint32_t in3[2] = {ctrl_gpu, IOWR(0x47u, 0x05u, 0xB0)};
    Test_Ipc_Message m3;
    memset(&m3, 0, sizeof(m3));
    m3.sends[0] = (Test_Ipc_Buffer){g_in_gva, 0xB0, 0};
    m3.send_count = 1;
    m3.receives[0] = (Test_Ipc_Buffer){g_out_gva, 0xB0, 0};
    m3.receives[1] = (Test_Ipc_Buffer){extra_gva, 0xA0, 0};
    m3.receive_count = 2;
    const Test_Ipc_Reply r3 = call(12, in3, sizeof(in3), &m3);
    CHECK(rd32(r3.data) == 0);
    uint8_t chars[0xA0];
    CHECK_OK(vmm_read_block(g_emu.vmm, extra_gva, chars, sizeof(chars)));
    CHECK(rd32(chars) == 0x120 && rd32(chars + 0x5C) == 0xB197);
  }
  /* ZCULL_GET_INFO: GM20B's geometry (NVN sizes zcull storage from it). */
  memset(d, 0, sizeof(d));
  CHECK(ioctl(ctrl_gpu, IOWR(0x47u, 0x02u, 0x28), d) == 0);
  CHECK(rd32(d) == 0x20 && rd32(d + 0x08) == 0x400 && rd32(d + 0x0C) == 0x800 && rd32(d + 0x24) == 0x10);
  /* NUM_VSMS: two SMs. */
  memset(d, 0, sizeof(d));
  CHECK(ioctl(ctrl_gpu, IOWR(0x47u, 0x13u, 8), d) == 0 && rd32(d) == 2);

  /* EVENT_WAIT_ASYNC: event from QueryEvent, signalled by a completion
   * posted to the GPU completion ring and drained by nvdrv. */
  const uint32_t query_in[2] = {ctrl, 3};
  Test_Ipc_Reply q = call(4, query_in, sizeof(query_in), NULL);
  CHECK(rd32(q.data) == 0 && q.copy_count == 1);
  Kernel_Event *event = (Kernel_Event *)handle_table_get(&g_emu.process.handles, q.copy_handles[0],
                                                         KERNEL_OBJECT_EVENT_READABLE);
  CHECK(event != NULL && !event->signaled);
  memset(d, 0, sizeof(d));
  wr32(d, syncpoint); wr32(d + 4, 2); wr32(d + 8, 0xFFFFFFFFu); wr32(d + 12, 3);
  CHECK(ioctl(ctrl, IOWR(0x00u, 0x1Eu, 16), d) == NV_TIMEOUT);
  nvdrv_poll_completions(&g_emu.nvdrv, &g_emu.hle);
  CHECK(!event->signaled);
  CHECK(completion_ring_push(syncpoint, 2));
  nvdrv_poll_completions(&g_emu.nvdrv, &g_emu.hle);
  CHECK(event->signaled);

  /* nvmap free: first leaves the FROM_ID reference, second frees. */
  memset(d, 0, sizeof(d));
  wr32(d, handle);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x05u, 24), d) == 0 && rd32(d + 20) == 1 && rd64(d + 8) == stack + 0x10000);
  memset(d, 0, sizeof(d));
  wr32(d, handle);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x05u, 24), d) == 0 && rd32(d + 20) == 0);
  CHECK(ioctl(nvmap, IOWR(0x01u, 0x05u, 24), d) == NV_BAD_VALUE);

  /* Close; a closed fd is rejected. */
  CHECK(rd32(call(2, &gpu, 4, NULL).data) == 0);
  CHECK(ioctl(gpu, IOWR(0x48u, 0x08u, 32), d) == NV_BAD_VALUE);
  CHECK(g_emu.nvdrv.ioctl_count > 20);

  emulator_destroy(&g_emu);
  printf("[nvdrv_test] passed\n");
  return 0;
}
