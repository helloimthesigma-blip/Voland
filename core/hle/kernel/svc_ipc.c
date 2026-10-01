/**
 * ConnectToNamedPort, SendSyncRequest, CloseHandle. See svc_ipc.h for the
 * register ABI and the behavior table.
 */
#include "hle/kernel/svc_ipc.h"

#include "common/log.h"
#include "hle/kernel/event.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/ipc.h"

#include <string.h>

#define SVC_ARG_NAME 1u      /* ConnectToNamedPort: X1 = name */
#define SVC_OUT_HANDLE 1u    /* ConnectToNamedPort: W1 = handle */
#define SVC_ARG_HANDLE 0u    /* SendSyncRequest / CloseHandle: W0 */

static bool is_pseudo_handle(uint32_t handle) {
  return handle == HANDLE_PSEUDO_CURRENT_THREAD || handle == HANDLE_PSEUDO_CURRENT_PROCESS;
}

void hle_svc_connect_to_named_port(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint64_t name_gva = regs->x[SVC_ARG_NAME];

  /* Byte at a time: a short name may sit right before an unmapped page. */
  char name[IPC_PORT_NAME_MAX_BYTES];
  uint32_t length = 0;
  for (; length < IPC_PORT_NAME_MAX_BYTES; length++) {
    uint8_t byte = 0;
    if (!error_is_ok(vmm_read8(context->vmm, name_gva + length, &byte))) {
      regs->x[0] = HLE_RESULT_INVALID_USER_POINTER;
      return;
    }
    name[length] = (char)byte;
    if (byte == 0) break;
  }
  if (length == IPC_PORT_NAME_MAX_BYTES) {
    regs->x[0] = HLE_RESULT_OUT_OF_RANGE;
    return;
  }

  const Service_Interface *interface = ipc_find_named_port(context, name);
  if (!interface) {
    log_warn("[ipc] ConnectToNamedPort: no port named \"%s\"", name);
    regs->x[0] = HLE_RESULT_NOT_FOUND;
    return;
  }
  uint32_t handle = HANDLE_INVALID;
  const uint32_t result = ipc_open_session_handle(context, interface, 0, &handle);
  regs->x[0] = result;
  if (result == HLE_RESULT_SUCCESS) regs->x[SVC_OUT_HANDLE] = handle;
}

static void close_session_handle(HLE_Context *context, uint32_t handle) {
  void *object = NULL;
  if (error_is_ok(handle_table_remove(&context->process->handles, handle, NULL, &object))) {
    ipc_session_pool_close(context->sessions, (IPC_Session *)object);
  }
}

void hle_svc_send_sync_request(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint32_t handle = (uint32_t)regs->x[SVC_ARG_HANDLE];
  IPC_Session *session = (IPC_Session *)handle_table_get(&context->process->handles, handle,
                                                         KERNEL_OBJECT_SESSION);
  if (!session) {
    regs->x[0] = HLE_RESULT_INVALID_HANDLE;
    return;
  }

  const uint64_t tls = context->cpu_backend->get_sys_reg(cpu_state, CPU_SYSREG_TPIDRRO_EL0);
  /* Static: an IPC_Request + IPC_Response is ~1.5KB, and HLE is
   * single-threaded by construction (§12), so one of each suffices and
   * keeps the guest-thread host stack small. */
  static IPC_Request request;
  static IPC_Response response;
  if (!error_is_ok(vmm_read_block(context->vmm, tls, request.buffer, IPC_COMMAND_BUFFER_BYTES))) {
    regs->x[0] = HLE_RESULT_INVALID_USER_POINTER;
    return;
  }
  if (!error_is_ok(ipc_parse_request(&request, session->is_domain))) {
    regs->x[0] = HLE_RESULT_MESSAGE_TOO_LARGE;
    return;
  }

  const bool was_domain = session->is_domain;
  const uint32_t kernel_result = ipc_dispatch(context, session, &request, &response);
  if (kernel_result == HLE_RESULT_CONNECTION_CLOSED) {
    /* A Close message: the server end goes away. libnx closes the handle
     * itself afterwards; that second close fails harmlessly. */
    close_session_handle(context, handle);
    regs->x[0] = HLE_RESULT_SUCCESS;
    return;
  }

  uint8_t reply[IPC_COMMAND_BUFFER_BYTES];
  if (!error_is_ok(ipc_write_response(&request, &response, was_domain, reply))) {
    log_error("[ipc] reply did not fit the command buffer");
    regs->x[0] = HLE_RESULT_MESSAGE_TOO_LARGE;
    return;
  }
  if (!error_is_ok(vmm_write_block(context->vmm, tls, reply, IPC_COMMAND_BUFFER_BYTES))) {
    regs->x[0] = HLE_RESULT_INVALID_USER_POINTER;
    return;
  }
  regs->x[0] = kernel_result;
}

void hle_svc_close_handle(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint32_t handle = (uint32_t)regs->x[SVC_ARG_HANDLE];
  if (is_pseudo_handle(handle)) {
    regs->x[0] = HLE_RESULT_SUCCESS;
    return;
  }
  Kernel_Object_Type type = KERNEL_OBJECT_NONE;
  void *object = NULL;
  if (!error_is_ok(handle_table_remove(&context->process->handles, handle, &type, &object))) {
    regs->x[0] = HLE_RESULT_INVALID_HANDLE;
    return;
  }
  if (type == KERNEL_OBJECT_SESSION) ipc_session_pool_close(context->sessions, (IPC_Session *)object);
  if (type == KERNEL_OBJECT_EVENT_READABLE || type == KERNEL_OBJECT_EVENT_WRITABLE) {
    event_release((Kernel_Event *)object);
  }
  /* A thread handle only drops the table entry; the thread outlives it. */
  regs->x[0] = HLE_RESULT_SUCCESS;
}
