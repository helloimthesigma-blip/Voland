/**
 * Offline networking over real IPC (§12, §20): bsd:u RegisterClient
 * succeeds with errno 0 (nn::socket::Initialize aborts otherwise), socket
 * calls fail with ENETDOWN, Close succeeds.
 */
#define CHECK_NAME "network_test"
#include "check.h"

#include "hle/services/network/network.h"
#include "ipc_fixtures.h"

#include <string.h>

#define BSD_REGISTER_CLIENT 0u
#define BSD_SOCKET 2u
#define BSD_CLOSE_COMMAND 26u
#define AF_INET_VALUE 2u
#define SOCK_STREAM_VALUE 1u

static Emulator g_emu;
static uint8_t g_reply[TEST_IPC_BUFFER_BYTES];

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

static int32_t word(const Test_Ipc_Reply *reply, uint32_t index) {
  int32_t v = 0;
  memcpy(&v, reply->data + index * sizeof(v), sizeof(v));
  return v;
}

int main(void) {
  ipc_fixture_boot(&g_emu);
  const uint32_t bsd = get_service("bsd:u");

  /* LibraryConfigData (0x20 bytes) + pid placeholder + transfer memory size. */
  uint8_t config[0x30];
  memset(config, 0, sizeof(config));
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.send_pid = true;
  Test_Ipc_Reply reply = call(bsd, BSD_REGISTER_CLIENT, config, sizeof(config), &m);
  CHECK(reply.result == 0);
  CHECK(word(&reply, 0) == 0);

  const uint32_t socket_args[3] = {AF_INET_VALUE, SOCK_STREAM_VALUE, 0};
  reply = call(bsd, BSD_SOCKET, socket_args, sizeof(socket_args), NULL);
  CHECK(reply.result == 0);
  CHECK(word(&reply, 0) == -1 && word(&reply, 1) == BSD_ENETDOWN);

  const uint32_t fd = 3;
  reply = call(bsd, BSD_CLOSE_COMMAND, &fd, sizeof(fd), NULL);
  CHECK(reply.result == 0);
  CHECK(word(&reply, 0) == 0 && word(&reply, 1) == 0);

  emulator_destroy(&g_emu);
  printf("[network_test] passed\n");
  return 0;
}
