/**
 * core/hle/kernel/ipc parse + encode, byte-exact, no emulator: requests
 * built by tests/ipc_fixtures.c's independent encoder (libnx layout
 * restated), replies decoded the way libnx's cmifParseResponse does.
 */
#define CHECK_NAME "ipc_parse_test"
#include "check.h"

#include "hle/kernel/ipc.h"
#include "ipc_fixtures.h"

#include <stdio.h>
#include <string.h>

static IPC_Request g_request;
static IPC_Response g_response;

static void parse(const Test_Ipc_Message *m, bool domain) {
  test_ipc_build(m, g_request.buffer);
  CHECK_OK(ipc_parse_request(&g_request, domain));
}

static void test_cmif_full_request(void) {
  const uint8_t payload[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = 1234;
  m.token = 77;
  m.send_pid = true;
  m.copy_handles[0] = 0x10001; m.copy_handles[1] = 0x18002; m.copy_count = 2;
  m.move_handles[0] = 0x20003; m.move_count = 1;
  /* 39-bit addresses exercise the low/mid/high split. */
  m.statics[0] = (Test_Ipc_Buffer){0x7F12345678ull, 0x40, 3}; m.static_count = 1;
  m.sends[0] = (Test_Ipc_Buffer){0x5512345000ull, 0x123456789ull, 1}; m.send_count = 1;
  m.receives[0] = (Test_Ipc_Buffer){0x0000100000ull, 0x2000, 0}; m.receive_count = 1;
  m.exchanges[0] = (Test_Ipc_Buffer){0x1234567000ull, 0x10, 3}; m.exchange_count = 1;
  m.receive_list[0] = (Test_Ipc_Buffer){0x8800001000ull, 0x300, 0};
  m.receive_list[1] = (Test_Ipc_Buffer){0x8800002000ull, 0x400, 0};
  m.receive_list_count = 2;
  m.payload = payload;
  m.payload_size = sizeof(payload);
  parse(&m, false);

  CHECK(g_request.kind == IPC_MESSAGE_CMIF_REQUEST);
  CHECK(g_request.framing_error == 0);
  CHECK(g_request.command_id == 1234 && g_request.token == 77);
  CHECK(g_request.has_pid);
  CHECK(g_request.copy_handle_count == 2 && g_request.copy_handles[1] == 0x18002);
  CHECK(g_request.move_handle_count == 1 && g_request.move_handles[0] == 0x20003);
  CHECK(g_request.static_count == 1);
  CHECK(g_request.statics[0].gva == 0x7F12345678ull && g_request.statics[0].size == 0x40 &&
        g_request.statics[0].index == 3);
  CHECK(g_request.send_count == 1);
  CHECK(g_request.sends[0].gva == 0x5512345000ull && g_request.sends[0].size == 0x123456789ull &&
        g_request.sends[0].mode == 1);
  CHECK(g_request.receives[0].gva == 0x100000ull && g_request.receives[0].size == 0x2000);
  CHECK(g_request.exchanges[0].gva == 0x1234567000ull && g_request.exchanges[0].mode == 3);
  CHECK(g_request.receive_list_count == 2);
  CHECK(g_request.receive_list[1].gva == 0x8800002000ull && g_request.receive_list[1].size == 0x400);
  /* The payload starts at the 16-aligned SFCI header + 16. */
  CHECK(g_request.payload_offset % 16 == 0);
  uint8_t back[12];
  CHECK_OK(ipc_request_read_bytes(&g_request, 0, back, sizeof(back)));
  CHECK(memcmp(back, payload, sizeof(back)) == 0);
  uint64_t u64 = 0;
  CHECK_OK(ipc_request_read_u64(&g_request, 4, &u64));
  CHECK(u64 == 0x0C0B0A0908070605ull);
  uint32_t u32 = 0;
  /* payload_size includes libnx's trailing padding, never more than the
   * data words hold. */
  CHECK_CODE(ipc_request_read_u32(&g_request, g_request.payload_size, &u32), RESULT_INVALID_ARGUMENT);
}

static void test_domain_and_tipc(void) {
  const uint8_t payload[8] = {0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4};
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.domain = true;
  m.domain_type = 1;
  m.object_id = 5;
  m.in_objects[0] = 9; m.in_objects[1] = 11; m.in_object_count = 2;
  m.command_id = 3;
  m.payload = payload;
  m.payload_size = sizeof(payload);
  parse(&m, true);
  CHECK(g_request.is_domain_message && g_request.framing_error == 0);
  CHECK(g_request.domain_request_type == IPC_DOMAIN_SEND_MESSAGE);
  CHECK(g_request.domain_object_id == 5 && g_request.command_id == 3);
  CHECK(g_request.in_object_count == 2 && g_request.in_objects[1] == 11);
  CHECK(g_request.payload_size == sizeof(payload));

  /* The same bytes on a NON-domain session misparse at the CMIF layer:
   * the domain header is not SFCI. Only the session knows the framing. */
  CHECK_OK(ipc_parse_request(&g_request, false));
  CHECK(g_request.framing_error == IPC_RESULT_SF_INVALID_IN_HEADER);

  /* Domain close carries no CMIF header. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.domain = true;
  m.domain_type = 2;
  m.object_id = 4;
  parse(&m, true);
  CHECK(g_request.domain_request_type == IPC_DOMAIN_CLOSE && g_request.domain_object_id == 4);
  CHECK(g_request.framing_error == 0);

  /* TIPC: type 16 + id, no SFCI, payload at the data words. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_TIPC;
  m.command_id = 1;
  m.payload = payload;
  m.payload_size = sizeof(payload);
  parse(&m, false);
  CHECK(g_request.kind == IPC_MESSAGE_TIPC_REQUEST && g_request.command_id == 1);
  CHECK(g_request.payload_size == sizeof(payload));
  CHECK_OK(ipc_request_read_u32(&g_request, 0, &(uint32_t){0}));

  /* Control and close kinds. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CONTROL;
  m.command_id = IPC_CONTROL_QUERY_POINTER_BUFFER_SIZE;
  parse(&m, true); /* control is never domain-framed */
  CHECK(g_request.kind == IPC_MESSAGE_CMIF_CONTROL && !g_request.is_domain_message);
  CHECK(g_request.command_id == IPC_CONTROL_QUERY_POINTER_BUFFER_SIZE);
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF_CLOSE;
  parse(&m, false);
  CHECK(g_request.kind == IPC_MESSAGE_CMIF_CLOSE);
}

static void test_rejections(void) {
  /* HIPC-level: refused outright. */
  memset(g_request.buffer, 0, sizeof(g_request.buffer));
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT); /* type 0 */
  g_request.buffer[0] = 1;
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT); /* legacy */
  g_request.buffer[0] = 3;
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT); /* legacy control */
  g_request.buffer[0] = 9;
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT); /* undefined */
  /* 0x3FF data words = 4092 bytes. */
  uint8_t *b = g_request.buffer;
  memset(b, 0, 0x100);
  b[0] = 4;
  b[4] = 0xFF; b[5] = 0x03;
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT);
  /* 15 copy + 15 move handles + PID + 15 X + 15 A ... overruns. */
  memset(b, 0, 0x100);
  b[0] = 4;
  b[2] = 0xFF; b[3] = 0xFF; /* 15 statics, 15 A, 15 B, 15 W */
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT);
  /* Receive list past the end. */
  memset(b, 0, 0x100);
  b[0] = 4;
  b[4] = 0x3C; /* 60 data words = 240 bytes, then a recv list that cannot fit */
  b[5] = (uint8_t)(15u << 2);
  CHECK_CODE(ipc_parse_request(&g_request, false), RESULT_INVALID_ARGUMENT);

  /* CMIF-level: delivered, service answers. Missing SFCI magic. */
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = 1;
  test_ipc_build(&m, g_request.buffer);
  for (uint32_t i = 8; i < 0x100; i++) {
    if (test_le32(g_request.buffer + i) == 0x49434653u) { g_request.buffer[i] = 'X'; break; }
  }
  CHECK_OK(ipc_parse_request(&g_request, false));
  CHECK(g_request.framing_error == IPC_RESULT_SF_INVALID_IN_HEADER);
  /* Too many in-objects for the data words. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.domain = true;
  m.domain_type = 1;
  m.object_id = 1;
  test_ipc_build(&m, g_request.buffer);
  for (uint32_t i = 8; i < 0x100; i++) {
    if (g_request.buffer[i] == 1 && (i % 16) == 0) { g_request.buffer[i + 1] = 8; break; }
  }
  CHECK_OK(ipc_parse_request(&g_request, true));
  CHECK(g_request.framing_error == IPC_RESULT_SF_INVALID_IN_OBJECT);
  /* Unknown domain request type. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.domain = true;
  m.domain_type = 7;
  m.object_id = 1;
  test_ipc_build(&m, g_request.buffer);
  CHECK_OK(ipc_parse_request(&g_request, true));
  CHECK(g_request.framing_error == IPC_RESULT_SF_INVALID_IN_HEADER);
}

static void test_responses(void) {
  uint8_t out[0x100];
  Test_Ipc_Reply reply;

  /* CMIF: SFCO, result, token echo, data, handles in the special header. */
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = 1;
  m.token = 0xBEEF;
  parse(&m, false);
  memset(&g_response, 0, sizeof(g_response));
  g_response.result = 0x1234;
  CHECK_OK(ipc_response_push_u64(&g_response, 0x1122334455667788ull));
  CHECK_OK(ipc_response_push_copy_handle(&g_response, 0xA));
  CHECK_OK(ipc_response_push_move_handle(&g_response, 0xB));
  CHECK_OK(ipc_write_response(&g_request, &g_response, false, out));
  CHECK(test_le32(out) == 0); /* response type */
  test_ipc_parse_reply(out, TEST_IPC_CMIF, false, 8, &reply);
  CHECK(reply.sfco_ok && reply.result == 0x1234 && reply.token == 0xBEEF);
  CHECK(test_le64(reply.data) == 0x1122334455667788ull);
  CHECK(reply.copy_count == 1 && reply.copy_handles[0] == 0xA);
  CHECK(reply.move_count == 1 && reply.move_handles[0] == 0xB);

  /* Domain CMIF: domain out header + object ids after the out data. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.domain = true;
  m.domain_type = 1;
  m.object_id = 1;
  parse(&m, true);
  memset(&g_response, 0, sizeof(g_response));
  CHECK_OK(ipc_response_push_u32(&g_response, 0xCAFE));
  g_response.out_object_count = 2;
  g_response.out_object_ids[0] = 2;
  g_response.out_object_ids[1] = 3;
  CHECK_OK(ipc_write_response(&g_request, &g_response, true, out));
  test_ipc_parse_reply(out, TEST_IPC_CMIF, true, 4, &reply);
  CHECK(reply.sfco_ok && reply.result == 0);
  CHECK(test_le32(reply.data) == 0xCAFE);
  CHECK(reply.object_count == 2 && reply.object_ids[0] == 2 && reply.object_ids[1] == 3);

  /* TIPC: result word then data, no SFCO. */
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_TIPC;
  m.command_id = 1;
  parse(&m, false);
  memset(&g_response, 0, sizeof(g_response));
  g_response.result = 0xE15;
  CHECK_OK(ipc_response_push_u32(&g_response, 42));
  CHECK_OK(ipc_write_response(&g_request, &g_response, false, out));
  test_ipc_parse_reply(out, TEST_IPC_TIPC, false, 4, &reply);
  CHECK(reply.result == 0xE15 && test_le32(reply.data) == 42);

  /* Pushes past the buffer set `overflowed` and fail. */
  memset(&g_response, 0, sizeof(g_response));
  uint8_t big[0x100];
  memset(big, 0, sizeof(big));
  CHECK_OK(ipc_response_push_bytes(&g_response, big, sizeof(big)));
  CHECK_CODE(ipc_response_push_u32(&g_response, 1), RESULT_INVALID_ARGUMENT);
  CHECK(g_response.overflowed);
  /* ...and a full data area cannot be encoded behind a CMIF header. */
  CHECK_CODE(ipc_write_response(&g_request, &g_response, false, out), RESULT_INVALID_ARGUMENT);
}

int main(void) {
  test_cmif_full_request();
  test_domain_and_tipc();
  test_rejections();
  test_responses();
  printf("[ipc_parse_test] passed\n");
  return 0;
}
