/**
 * SVC path for IPC: ConnectToNamedPort, SendSyncRequest, CloseHandle and
 * sm:, driven through hle_on_svc on the noop backend with the command
 * buffer in the main thread's real TLS block (svc_ipc.h, sm.h).
 */
#define CHECK_NAME "svc_ipc_test"
#include "check.h"

#include "hle/hle.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/svc_ipc.h"
#include "ipc_fixtures.h"

#include <stdio.h>
#include <string.h>

#define TEST_POINTER_BUFFER_SIZE 0x500u
#define CMD_ECHO 0
#define CMD_OPEN_SUBOBJECT 1
#define CMD_FAIL_AFTER_HANDLE 2
#define CMD_OVERFLOW 3
#define CMD_OBJECT_AND_HANDLE 4
#define TEST_FAILURE ((uint32_t)((5u << 9) | 200u))

static Emulator g_emu; /* ~280KB with the IPC pools; static, not stack */

/* --- A test service: echo(+1), sub-object, fail-after-handle, overflow. --- */

static HLE_ServiceResult test_echo(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                   IPC_Response *res) {
  (void)c;
  uint32_t value = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &value))) return IPC_RESULT_SF_INVALID_HEADER_SIZE;
  (void)ipc_response_push_u32(res, value + 1u + (uint32_t)self->state);
  return HLE_RESULT_SUCCESS;
}

static const Service_Interface *test_interface(void);

static HLE_ServiceResult test_open_subobject(HLE_Context *c, Service_Object *self,
                                             const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, test_interface(), self->state + 100u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult test_fail_after_handle(HLE_Context *c, Service_Object *self,
                                                const IPC_Request *req, IPC_Response *res) {
  (void)self;
  (void)req;
  uint32_t handle = 0;
  CHECK(ipc_open_session_handle(c, test_interface(), 0, &handle) == HLE_RESULT_SUCCESS);
  (void)ipc_response_push_move_handle(res, handle);
  return TEST_FAILURE; /* the handle must not leak */
}

static HLE_ServiceResult test_overflow(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  for (uint32_t i = 0; i < 100; i++) (void)ipc_response_push_u64(res, i);
  return HLE_RESULT_SUCCESS;
}

/* One out object (state 100) AND one explicit move handle (state 0). */
static HLE_ServiceResult test_object_and_handle(HLE_Context *c, Service_Object *self,
                                                const IPC_Request *req, IPC_Response *res) {
  (void)self;
  (void)req;
  uint32_t handle = 0;
  CHECK(ipc_open_session_handle(c, test_interface(), 0, &handle) == HLE_RESULT_SUCCESS);
  (void)ipc_response_push_move_handle(res, handle);
  (void)ipc_response_push_object(res, test_interface(), 100u);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_test_commands[] = {
    {CMD_ECHO, test_echo, "Echo"},
    {CMD_OPEN_SUBOBJECT, test_open_subobject, "OpenSubObject"},
    {CMD_FAIL_AFTER_HANDLE, test_fail_after_handle, "FailAfterHandle"},
    {CMD_OVERFLOW, test_overflow, "Overflow"},
    {CMD_OBJECT_AND_HANDLE, test_object_and_handle, "ObjectAndHandle"},
};

static const Service_Interface k_test_interface = {
    "test:a", k_test_commands, sizeof(k_test_commands) / sizeof(k_test_commands[0]),
    TEST_POINTER_BUFFER_SIZE, NULL, NULL,
};

static const Service_Interface *test_interface(void) { return &k_test_interface; }

/* --- Helpers. --- */

static uint8_t g_reply[TEST_IPC_BUFFER_BYTES];

static Test_Ipc_Message cmif(uint32_t command, const void *payload, uint32_t size) {
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = command;
  m.payload = payload;
  m.payload_size = size;
  return m;
}

static uint64_t service_name(const char *name) {
  uint64_t wire = 0;
  for (uint32_t i = 0; i < 8 && name[i]; i++) wire |= (uint64_t)(uint8_t)name[i] << (8u * i);
  return wire;
}

static Test_Ipc_Reply send_cmif(uint32_t handle, const Test_Ipc_Message *m, uint32_t out_size) {
  CHECK(ipc_fixture_send(&g_emu, handle, m, g_reply) == HLE_RESULT_SUCCESS);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, m->framing, m->domain, out_size, &reply);
  CHECK(reply.sfco_ok);
  return reply;
}

static uint32_t echo(uint32_t handle, uint32_t value) {
  const Test_Ipc_Message m = cmif(CMD_ECHO, &value, sizeof(value));
  const Test_Ipc_Reply reply = send_cmif(handle, &m, 4);
  CHECK(reply.result == 0);
  return test_le32(reply.data);
}

static uint32_t get_service(uint32_t sm, const char *name, uint32_t *out_handle) {
  const uint64_t wire = service_name(name);
  const Test_Ipc_Message m = cmif(1, &wire, sizeof(wire));
  const Test_Ipc_Reply reply = send_cmif(sm, &m, 0);
  if (reply.result == 0) {
    CHECK(reply.move_count == 1);
    *out_handle = reply.move_handles[0];
  } else {
    CHECK(reply.move_count == 0);
  }
  return reply.result;
}

static uint32_t close_handle(uint32_t handle) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[0] = handle;
  hle_on_svc(g_emu.cpu_state, HLE_SVC_CLOSE_HANDLE, &g_emu.hle);
  return (uint32_t)regs->x[0];
}

static const Handle_Table *handles(void) { return &g_emu.process.handles; }

/* --- Tests. --- */

static void test_main_thread_handle_and_constants(void) {
  CHECK(handle_table_get(handles(), PROCESS_MAIN_THREAD_HANDLE, KERNEL_OBJECT_THREAD) != NULL);
  CHECK(g_emu.process.main_thread_handle == PROCESS_MAIN_THREAD_HANDLE);
  /* libnx result.h values. */
  CHECK(HLE_RESULT_NOT_IMPLEMENTED == ((33u << 9) | 1u));
  CHECK(HLE_RESULT_CONNECTION_CLOSED == ((123u << 9) | 1u));
  CHECK(HLE_RESULT_NOT_FOUND == ((121u << 9) | 1u));
  CHECK(IPC_RESULT_SF_UNKNOWN_COMMAND == 0x1BA0Au);
  CHECK(SM_RESULT_NOT_REGISTERED == ((7u << 9) | 21u));
  /* An unknown SVC answers the corrected NotImplemented. */
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  hle_on_svc(g_emu.cpu_state, 0x7E, &g_emu.hle);
  CHECK(regs->x[0] == 0x4201u);
}

static void test_connect(void) {
  uint32_t handle = 0;
  CHECK(ipc_fixture_connect(&g_emu, "nope", &handle) == HLE_RESULT_NOT_FOUND);
  CHECK(ipc_fixture_connect(&g_emu, "abcdefghijkl", &handle) == HLE_RESULT_OUT_OF_RANGE);

  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[1] = 0x10; /* unmapped */
  hle_on_svc(g_emu.cpu_state, HLE_SVC_CONNECT_TO_NAMED_PORT, &g_emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_USER_POINTER);

  /* A name ending right at the top of the mapped stack, unmapped above. */
  const Address_Region stack = g_emu.process.main_thread_stack;
  const uint64_t top = stack.base + stack.size;
  CHECK_OK(vmm_write_block(g_emu.vmm, top - 4, "sm:", 4));
  regs->x[1] = top - 4;
  hle_on_svc(g_emu.cpu_state, HLE_SVC_CONNECT_TO_NAMED_PORT, &g_emu.hle);
  CHECK(regs->x[0] == 0);
  const uint32_t sm = (uint32_t)regs->x[1];
  CHECK(handle_table_get(handles(), sm, KERNEL_OBJECT_SESSION) != NULL);
  CHECK(close_handle(sm) == 0);
}

static void test_send_rejections(uint32_t sm) {
  const Test_Ipc_Message m = cmif(0, NULL, 0);
  CHECK(ipc_fixture_send(&g_emu, 0, &m, g_reply) == HLE_RESULT_INVALID_HANDLE);
  CHECK(ipc_fixture_send(&g_emu, HANDLE_PSEUDO_CURRENT_THREAD, &m, g_reply) == HLE_RESULT_INVALID_HANDLE);
  CHECK(ipc_fixture_send(&g_emu, PROCESS_MAIN_THREAD_HANDLE, &m, g_reply) == HLE_RESULT_INVALID_HANDLE);

  /* HIPC-level garbage: MessageTooLarge, TLS untouched. */
  const uint64_t tls = ipc_fixture_tls(&g_emu);
  uint8_t garbage[TEST_IPC_BUFFER_BYTES];
  memset(garbage, 0, sizeof(garbage));
  garbage[0] = 4;
  garbage[4] = 0xFF;
  garbage[5] = 0x03;
  CHECK_OK(vmm_write_block(g_emu.vmm, tls, garbage, sizeof(garbage)));
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[0] = sm;
  hle_on_svc(g_emu.cpu_state, HLE_SVC_SEND_SYNC_REQUEST, &g_emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_MESSAGE_TOO_LARGE);
  uint8_t after[TEST_IPC_BUFFER_BYTES];
  CHECK_OK(vmm_read_block(g_emu.vmm, tls, after, sizeof(after)));
  CHECK(memcmp(after, garbage, sizeof(after)) == 0);
}

static void test_sm(uint32_t sm, uint32_t *out_test_handle) {
  uint32_t handle = 0;
  /* Before RegisterClient. */
  CHECK(get_service(sm, "test:a", &handle) == SM_RESULT_INVALID_CLIENT);

  Test_Ipc_Message reg = cmif(0, NULL, 0);
  reg.send_pid = true;
  CHECK(send_cmif(sm, &reg, 0).result == 0);

  CHECK(get_service(sm, "fsp-srv", &handle) == SM_RESULT_NOT_REGISTERED);
  CHECK(get_service(sm, "", &handle) == SM_RESULT_INVALID_SERVICE_NAME);
  const uint64_t gap = 0x4100000000000041ull; /* "A\0\0...\0A" */
  const Test_Ipc_Message bad = cmif(1, &gap, sizeof(gap));
  CHECK(send_cmif(sm, &bad, 0).result == SM_RESULT_INVALID_SERVICE_NAME);

  /* Unknown command: sf UnknownCommandId inside a delivered reply. */
  const Test_Ipc_Message unknown = cmif(99, NULL, 0);
  CHECK(send_cmif(sm, &unknown, 0).result == IPC_RESULT_SF_UNKNOWN_COMMAND);

  /* The whole path: sm: hands out a session that reaches the service. */
  CHECK(get_service(sm, "test:a", &handle) == 0);
  CHECK(handle_table_get(handles(), handle, KERNEL_OBJECT_SESSION) != NULL);
  CHECK(echo(handle, 41) == 42);

  /* TIPC GetServiceHandle (type 17) on the same, CMIF-registered session. */
  const uint64_t wire = service_name("test:a");
  Test_Ipc_Message tipc;
  memset(&tipc, 0, sizeof(tipc));
  tipc.framing = TEST_IPC_TIPC;
  tipc.command_id = 1;
  tipc.payload = &wire;
  tipc.payload_size = sizeof(wire);
  CHECK(ipc_fixture_send(&g_emu, sm, &tipc, g_reply) == 0);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, TEST_IPC_TIPC, false, 0, &reply);
  CHECK(reply.result == 0 && reply.move_count == 1);
  CHECK(echo(reply.move_handles[0], 1) == 2);
  CHECK(close_handle(reply.move_handles[0]) == 0);

  *out_test_handle = handle;
}

static void test_control_and_domains(uint32_t test) {
  /* QueryPointerBufferSize. */
  Test_Ipc_Message control;
  memset(&control, 0, sizeof(control));
  control.framing = TEST_IPC_CONTROL;
  control.command_id = IPC_CONTROL_QUERY_POINTER_BUFFER_SIZE;
  Test_Ipc_Reply reply = send_cmif(test, &control, 2);
  CHECK(reply.result == 0 && (reply.data[0] | (reply.data[1] << 8)) == TEST_POINTER_BUFFER_SIZE);

  /* Non-domain sub-object: a move handle to a new session. */
  const Test_Ipc_Message open = cmif(CMD_OPEN_SUBOBJECT, NULL, 0);
  reply = send_cmif(test, &open, 0);
  CHECK(reply.result == 0 && reply.move_count == 1);
  CHECK(echo(reply.move_handles[0], 1) == 102); /* state 100 */
  CHECK(close_handle(reply.move_handles[0]) == 0);

  /* libnx order: out objects are the FIRST move handles. */
  const Test_Ipc_Message both = cmif(CMD_OBJECT_AND_HANDLE, NULL, 0);
  reply = send_cmif(test, &both, 0);
  CHECK(reply.result == 0 && reply.move_count == 2);
  CHECK(echo(reply.move_handles[0], 1) == 102); /* the object, state 100 */
  CHECK(echo(reply.move_handles[1], 1) == 2);   /* the plain handle, state 0 */
  CHECK(close_handle(reply.move_handles[0]) == 0);
  CHECK(close_handle(reply.move_handles[1]) == 0);

  /* Clone. */
  control.command_id = IPC_CONTROL_CLONE_CURRENT_OBJECT;
  reply = send_cmif(test, &control, 0);
  CHECK(reply.result == 0 && reply.move_count == 1);
  const uint32_t clone = reply.move_handles[0];
  CHECK(echo(clone, 5) == 6);

  /* Convert the clone to a domain: object id 1. */
  control.command_id = IPC_CONTROL_CONVERT_CURRENT_OBJECT_TO_DOMAIN;
  reply = send_cmif(clone, &control, 4);
  CHECK(reply.result == 0 && test_le32(reply.data) == 1);

  /* Domain requests to object 1 work; object 7 does not exist. */
  uint32_t value = 10;
  Test_Ipc_Message domain_echo = cmif(CMD_ECHO, &value, sizeof(value));
  domain_echo.domain = true;
  domain_echo.domain_type = 1;
  domain_echo.object_id = 1;
  reply = send_cmif(clone, &domain_echo, 4);
  CHECK(reply.result == 0 && test_le32(reply.data) == 11);
  domain_echo.object_id = 7;
  CHECK(send_cmif(clone, &domain_echo, 4).result == IPC_RESULT_SF_TARGET_NOT_FOUND);

  /* A sub-object on a domain is an object id, not a handle. */
  Test_Ipc_Message domain_open = cmif(CMD_OPEN_SUBOBJECT, NULL, 0);
  domain_open.domain = true;
  domain_open.domain_type = 1;
  domain_open.object_id = 1;
  const uint32_t table_before = handles()->count;
  reply = send_cmif(clone, &domain_open, 0);
  CHECK(reply.result == 0 && reply.move_count == 0);
  CHECK(reply.object_count == 1 && reply.object_ids[0] == 2);
  CHECK(handles()->count == table_before);
  domain_echo.object_id = 2;
  reply = send_cmif(clone, &domain_echo, 4);
  CHECK(test_le32(reply.data) == 111); /* 10 + 1 + state 100 */

  /* Domain close frees id 2. */
  Test_Ipc_Message domain_close = cmif(0, NULL, 0);
  domain_close.domain = true;
  domain_close.domain_type = 2;
  domain_close.object_id = 2;
  CHECK(ipc_fixture_send(&g_emu, clone, &domain_close, g_reply) == 0);
  CHECK(send_cmif(clone, &domain_echo, 4).result == IPC_RESULT_SF_TARGET_NOT_FOUND);

  /* Domain object table exhaustion: OutOfDomainEntries, nothing leaked. */
  uint32_t opened = 0;
  for (;;) {
    reply = send_cmif(clone, &domain_open, 0);
    if (reply.result != 0) break;
    opened++;
  }
  CHECK(reply.result == IPC_RESULT_SF_OUT_OF_DOMAIN_ENTRIES);
  CHECK(opened == IPC_DOMAIN_MAX_OBJECTS - 1u);

  CHECK(close_handle(clone) == 0);
}

static void test_failures_do_not_leak(uint32_t test) {
  const uint32_t sessions_before = g_emu.sessions.live_count;
  const uint32_t handles_before = handles()->count;

  const Test_Ipc_Message fail = cmif(CMD_FAIL_AFTER_HANDLE, NULL, 0);
  Test_Ipc_Reply reply = send_cmif(test, &fail, 0);
  CHECK(reply.result == TEST_FAILURE && reply.move_count == 0);
  CHECK(g_emu.sessions.live_count == sessions_before && handles()->count == handles_before);

  const Test_Ipc_Message overflow = cmif(CMD_OVERFLOW, NULL, 0);
  reply = send_cmif(test, &overflow, 0);
  CHECK(reply.result == IPC_RESULT_SF_INVALID_OUT_RAW_SIZE);

  /* Session pool exhaustion: OutOfSessions, no handle consumed. */
  uint32_t opened[IPC_MAX_SESSIONS];
  uint32_t count = 0;
  for (;;) {
    uint32_t handle = 0;
    const uint32_t result = ipc_fixture_connect(&g_emu, "sm:", &handle);
    if (result != 0) {
      CHECK(result == HLE_RESULT_OUT_OF_SESSIONS);
      break;
    }
    opened[count++] = handle;
  }
  CHECK(g_emu.sessions.live_count == IPC_MAX_SESSIONS);
  const uint32_t full_count = handles()->count;
  uint32_t ignored = 0;
  CHECK(ipc_fixture_connect(&g_emu, "sm:", &ignored) == HLE_RESULT_OUT_OF_SESSIONS);
  CHECK(handles()->count == full_count);
  for (uint32_t i = 0; i < count; i++) CHECK(close_handle(opened[i]) == 0);
  CHECK(g_emu.sessions.live_count == sessions_before);
}

static void test_close(uint32_t test) {
  /* Close message: the session goes away; libnx's follow-up CloseHandle
   * then fails harmlessly. */
  Test_Ipc_Message close_message;
  memset(&close_message, 0, sizeof(close_message));
  close_message.framing = TEST_IPC_CMIF_CLOSE;
  const uint32_t before = g_emu.sessions.live_count;
  CHECK(ipc_fixture_send(&g_emu, test, &close_message, g_reply) == 0);
  CHECK(g_emu.sessions.live_count == before - 1u);
  CHECK(close_handle(test) == HLE_RESULT_INVALID_HANDLE);
  CHECK(close_handle(HANDLE_PSEUDO_CURRENT_PROCESS) == 0);
  /* Closing the main thread's handle drops the entry only. */
  CHECK(close_handle(PROCESS_MAIN_THREAD_HANDLE) == 0);
  CHECK(close_handle(PROCESS_MAIN_THREAD_HANDLE) == HLE_RESULT_INVALID_HANDLE);
}

int main(void) {
  ipc_fixture_boot(&g_emu);
  CHECK_OK(sm_registry_add(&g_emu.sm, "test:a", &k_test_interface));
  CHECK_CODE(sm_registry_add(&g_emu.sm, "test:a", &k_test_interface), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(sm_registry_add(&g_emu.sm, "toolongname", &k_test_interface), RESULT_INVALID_ARGUMENT);

  test_main_thread_handle_and_constants();
  test_connect();

  uint32_t sm = 0;
  CHECK(ipc_fixture_connect(&g_emu, "sm:", &sm) == 0);
  test_send_rejections(sm);
  uint32_t test = 0;
  test_sm(sm, &test);
  test_control_and_domains(test);
  test_failures_do_not_leak(test);
  test_close(test);

  /* Unloading drops every session with the process. */
  CHECK(g_emu.sessions.live_count > 0);
  emulator_unload_program(&g_emu);
  CHECK(g_emu.sessions.live_count == 0);
  emulator_destroy(&g_emu);
  printf("[svc_ipc_test] passed\n");
  return 0;
}
