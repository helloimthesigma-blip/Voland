/**
 * IPC and handle SVCs: ConnectToNamedPort (0x1F), SendSyncRequest (0x21),
 * CloseHandle (0x16). PROPOSED HEADER - awaiting maintainer review; no
 * implementation exists yet. Phase 1, §25 "Minimal IPC + sm: stub";
 * §12 priority list item 3.
 *
 * Register ABI, verified against libnx's svc.s:
 *
 *   ConnectToNamedPort  C: Result(Handle* out, const char* name)
 *                       in:  X1 = name (guest VA, NUL-terminated, at most
 *                            IPC_PORT_NAME_MAX_BYTES including the NUL)
 *                       out: W0 = Result, W1 = session handle
 *                       (libnx's stub keeps `out` on its own stack; the
 *                       kernel never sees X0)
 *
 *   SendSyncRequest     C: Result(Handle session)
 *                       in:  W0 = session handle
 *                       out: W0 = Result; the reply is in the calling
 *                            thread's TLS command buffer
 *
 *   CloseHandle         C: Result(Handle handle)
 *                       in:  W0 = handle
 *                       out: W0 = Result
 *
 * DOC BUG FOUND WHILE VERIFYING, flagged for review: §12's dispatch table
 * lists CloseHandle as 0x26. libnx's svc.s has svcCloseHandle = 0x16 and
 * svcBreak = 0x26. This header uses 0x16; the §12 table is corrected in
 * the same change that implements it.
 *
 * Behavior:
 *   ConnectToNamedPort - reads the name with vmm_read_block one byte at a
 *     time up to the limit (a name may end right before an unmapped page);
 *     no NUL within the limit -> HLE_RESULT_OUT_OF_RANGE; unmapped ->
 *     HLE_RESULT_INVALID_POINTER; unknown port -> HLE_RESULT_NOT_FOUND
 *     (libnx retries sm: on NotFound with a sleep, so answering it for
 *     sm: would spin forever - sm: is always present); otherwise opens a
 *     session on the port's interface and adds it to the handle table
 *     (HLE_RESULT_OUT_OF_SESSIONS / HLE_RESULT_OUT_OF_HANDLES on
 *     exhaustion, with nothing leaked).
 *
 *   SendSyncRequest - pseudo-handles and non-session handles ->
 *     HLE_RESULT_INVALID_HANDLE. TLS block address = the calling
 *     CPU_State's tpidrro_el0 (set once at thread creation, thread.h).
 *     One vmm_read_block of IPC_COMMAND_BUFFER_BYTES, ipc_parse_request,
 *     ipc_dispatch, ipc_write_response, one vmm_write_block back. Parse
 *     failure -> HLE_RESULT_INVALID_STATE, TLS untouched. A Close message
 *     closes the session and its handle and returns success (libnx
 *     ignores the result and closes the handle itself; the second close
 *     then fails INVALID_HANDLE harmlessly, as on hardware).
 *     Does not block: every Phase 1 service answers synchronously. A
 *     service that must wait (a deferred sm: GetServiceHandle, any event
 *     wait) needs the Phase 2 scheduler (§7) and is out of scope.
 *
 *   CloseHandle - pseudo-handles succeed and do nothing (Horizon); a
 *     session handle closes the session (ipc_session_pool_close); a thread
 *     handle only drops the table entry (the thread object outlives it);
 *     anything else -> HLE_RESULT_INVALID_HANDLE.
 */
#ifndef SWITCH_HLE_KERNEL_SVC_IPC_H
#define SWITCH_HLE_KERNEL_SVC_IPC_H

#include "cpu/cpu.h"
#include "hle/hle.h"

#define HLE_SVC_CLOSE_HANDLE 0x16u
#define HLE_SVC_CONNECT_TO_NAMED_PORT 0x1Fu
#define HLE_SVC_SEND_SYNC_REQUEST 0x21u

/* Same contract as svc_memory.h's handlers: called from hle_on_svc with
 * non-NULL arguments, a loaded process, results written to X0 (+ X1 for
 * ConnectToNamedPort). */
void hle_svc_connect_to_named_port(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_send_sync_request(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_close_handle(HLE_Context *context, CPU_State *cpu_state);

#endif /* SWITCH_HLE_KERNEL_SVC_IPC_H */
