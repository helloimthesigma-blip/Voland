#include "hle/hle.h"
#include "common/assert.h"
#include "common/log.h"
#include "hle/kernel/svc32.h"
#include "hle/kernel/scheduler.h"
#include "hle/kernel/svc_ipc.h"
#include "hle/kernel/svc_memory.h"
#include "hle/kernel/svc_thread.h"

#include <stddef.h>

void hle_context_init(HLE_Context *context, const CPU_Backend *backend,
                      VMM_Context *vmm, Process *process, Page_Allocator *pages,
                      IPC_Session_Pool *sessions, SM_Registry *sm)
{
  SWITCH_ASSERT_ALWAYS(context != NULL, "hle_context_init: context is NULL");
  SWITCH_ASSERT_ALWAYS(backend != NULL, "hle_context_init: backend is NULL");
  context->cpu_backend = backend;
  context->vmm = vmm;
  context->process = process;
  context->pages = pages;
  context->sessions = sessions;
  context->sm = sm;
  context->scheduler = NULL;
  context->events = NULL;
  context->shared_memory = NULL;
  context->transfer_memory = NULL;
  context->debug_output = NULL;
  context->debug_userdata = NULL;
  context->svc_call_count = 0;
}

static const char *svc_name(uint32_t swi)
{
  switch (swi)
  {
  case 0x01:
    return "SetHeapSize";
  case 0x02:
    return "SetMemoryPermission";
  case 0x03:
    return "SetMemoryAttribute";
  case 0x04:
    return "MapMemory";
  case 0x05:
    return "UnmapMemory";
  case 0x06:
    return "QueryMemory";
  case 0x08:
    return "CreateThread";
  case 0x09:
    return "StartThread";
  case 0x0A:
    return "ExitThread";
  case 0x0B:
    return "SleepThread";
  case 0x0C:
    return "GetThreadPriority";
  case 0x11:
    return "SignalEvent";
  case 0x12:
    return "ClearEvent";
  case 0x13:
    return "MapSharedMemory";
  case 0x14:
    return "UnmapSharedMemory";
  case 0x15:
    return "CreateTransferMemory";
  case 0x16:
    return "CloseHandle";
  case 0x18:
    return "WaitSynchronization";
  case 0x19:
    return "CancelSynchronization";
  case 0x1A:
    return "ArbitrateLock";
  case 0x1B:
    return "ArbitrateUnlock";
  case 0x1C:
    return "WaitProcessWideKeyAtomic";
  case 0x1D:
    return "SignalProcessWideKey";
  case 0x1F:
    return "ConnectToNamedPort";
  case 0x21:
    return "SendSyncRequest";
  case 0x22:
    return "SendSyncRequestWithUserBuffer";
  case 0x24:
    return "GetProcessId";
  case 0x25:
    return "GetThreadId";
  case 0x26:
    return "Break";
  case 0x27:
    return "OutputDebugString";
  case 0x29:
    return "GetInfo";
  default:
    return "<unknown>";
  }
}

static void dispatch_svc(HLE_Context *context, CPU_State *cpu_state, uint32_t swi);

/* Every SVC runs under the scheduler's kernel lock (a no-op in serial
 * mode; docs/PARALLEL.md). */
void hle_on_svc(CPU_State *cpu_state, uint32_t swi, void *userdata)
{
  HLE_Context *context = (HLE_Context *)userdata;
  SWITCH_ASSERT_ALWAYS(context != NULL, "hle_on_svc: context is NULL");
  scheduler_kernel_enter(context->scheduler, cpu_state);
  {
    /* Any SVC but a yield is not polling (svc_thread.c sets the mark). */
    Sched_Thread *self = context->scheduler ? scheduler_current(context->scheduler) : NULL;
    if (self) self->spinning = false;
  }
  /* A 32-bit process (it runs on the A32 backend) has its arguments
   * rearranged into the AArch64 calling convention around the call
   * (hle/kernel/svc32.h). */
  if (context->cpu_backend->aarch32) {
    CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
    Svc32_Frame frame;
    svc32_enter(regs, swi, &frame);
    dispatch_svc(context, cpu_state, swi);
    svc32_exit(regs, swi, &frame);
  } else {
    dispatch_svc(context, cpu_state, swi);
  }
  scheduler_kernel_exit(context->scheduler);
}

static void dispatch_svc(HLE_Context *context, CPU_State *cpu_state, uint32_t swi)
{
  SWITCH_ASSERT_ALWAYS(context->cpu_backend != NULL, "hle_on_svc: backend is NULL");

  const CPU_Backend *cpu = context->cpu_backend;
  CPU_Register_File *regs = cpu->get_register_file(cpu_state);

  context->svc_call_count++;
  if (swi < HLE_SVC_COUNT) context->svc_counts[swi]++;
  const uint64_t arg0 = regs->x[0], arg1 = regs->x[1]; /* for the failure log below */

  /* Handlers below that call vmm_guest_to_host bracket each individual
   * call in its own vmm_borrow_scope_begin/end, right where it happens
   * (svc_memory.c) - NOT one scope around this whole dispatch. A
   * dispatch-wide scope was tried first and immediately hit vmm's debug
   * borrow cap (VMM_DEBUG_MAX_BORROWS=64, vmm.c): SetHeapSize's shrink
   * path can call vmm_guest_to_host once per released page, hundreds of
   * times in one call, and the cap exists to catch a handler that LEAKS
   * borrows, not to size a loop's borrow budget. Scoping tightly around
   * each call keeps at most one borrow live at a time regardless of how
   * many pages a handler walks. */

  /* §12's dispatch table, Phase 1 slice: the four memory SVCs (thin
   * layers over vmm/process/pages - hle/kernel/svc_memory.h), the IPC
   * and handle SVCs (hle/kernel/svc_ipc.h), plus the §12 unimplemented-surface policy
   * default arm for everything else: log, then HLE_RESULT_NOT_IMPLEMENTED
   * in W0. */
  switch (swi)
  {
  case 0x01:
    hle_svc_set_heap_size(context, cpu_state);
    break;
  case 0x04:
    hle_svc_map_memory(context, cpu_state);
    break;
  case 0x2c:
    hle_svc_map_physical_memory(context, cpu_state);
    break;
  case 0x2d:
    hle_svc_unmap_physical_memory(context, cpu_state);
    break;
  case 0x05:
    hle_svc_unmap_memory(context, cpu_state);
    break;
  case 0x06:
    hle_svc_query_memory(context, cpu_state);
    break;
  case 0x02: hle_svc_set_memory_permission(context, cpu_state); break;
  case 0x03: hle_svc_set_memory_attribute(context, cpu_state); break;
  case 0x07: hle_svc_exit_process(context, cpu_state); break;
  case 0x08: hle_svc_create_thread(context, cpu_state); break;
  case 0x09: hle_svc_start_thread(context, cpu_state); break;
  case 0x0A: hle_svc_exit_thread(context, cpu_state); break;
  case 0x0B: hle_svc_sleep_thread(context, cpu_state); break;
  case 0x0C: hle_svc_get_thread_priority(context, cpu_state); break;
  case 0x0D: hle_svc_set_thread_priority(context, cpu_state); break;
  case 0x0E: hle_svc_get_thread_core_mask(context, cpu_state); break;
  case 0x0F: hle_svc_set_thread_core_mask(context, cpu_state); break;
  case 0x10: hle_svc_get_current_processor_number(context, cpu_state); break;
  case 0x18: hle_svc_wait_synchronization(context, cpu_state); break;
  case 0x19: hle_svc_cancel_synchronization(context, cpu_state); break;
  case 0x1A: hle_svc_arbitrate_lock(context, cpu_state); break;
  case 0x1B: hle_svc_arbitrate_unlock(context, cpu_state); break;
  case 0x1C: hle_svc_wait_process_wide_key_atomic(context, cpu_state); break;
  case 0x1D: hle_svc_signal_process_wide_key(context, cpu_state); break;
  case 0x1E: hle_svc_get_system_tick(context, cpu_state); break;
  case 0x24: hle_svc_get_process_id(context, cpu_state); break;
  case 0x25: hle_svc_get_thread_id(context, cpu_state); break;
  case 0x26: hle_svc_break(context, cpu_state); break;
  case 0x27: hle_svc_output_debug_string(context, cpu_state); break;
  case 0x29: hle_svc_get_info(context, cpu_state); break;
  case 0x32: hle_svc_set_thread_activity(context, cpu_state); break;
  case 0x33: hle_svc_get_thread_context3(context, cpu_state); break;
  case 0x34: hle_svc_wait_for_address(context, cpu_state); break;
  case 0x11: hle_svc_signal_event(context, cpu_state); break;
  case 0x13: hle_svc_map_shared_memory(context, cpu_state); break;
  case 0x14: hle_svc_unmap_shared_memory(context, cpu_state); break;
  case 0x15: hle_svc_create_transfer_memory(context, cpu_state); break;
  case 0x12: hle_svc_clear_event(context, cpu_state); break;
  case 0x17: hle_svc_reset_signal(context, cpu_state); break;
  case 0x45: hle_svc_create_event(context, cpu_state); break;
  case 0x35: hle_svc_signal_to_address(context, cpu_state); break;
  /* FlushEntireDataCache, FlushDataCache, FlushProcessDataCache: guest
   * memory is one coherent host buffer, there is nothing to write back. */
  case 0x2A:
  case 0x2B:
  case 0x5F:
    regs->x[0] = HLE_RESULT_SUCCESS;
    break;
  case HLE_SVC_CLOSE_HANDLE:
    hle_svc_close_handle(context, cpu_state);
    break;
  case HLE_SVC_CONNECT_TO_NAMED_PORT:
    hle_svc_connect_to_named_port(context, cpu_state);
    break;
  case HLE_SVC_SEND_SYNC_REQUEST:
    hle_svc_send_sync_request(context, cpu_state);
    break;
  default:
    log_warn("[hle] SVC 0x%02x (%s) at PC 0x%016llx - unimplemented",
             swi,
             svc_name(swi),
             (unsigned long long)regs->pc);
    regs->x[0] = HLE_RESULT_NOT_IMPLEMENTED;
    break;
  }
  /* A bad handle is almost always an emulation gap (a kernel object or a
   * pseudo-handle not understood): say which call saw it. */
  if ((uint32_t)regs->x[0] == HLE_RESULT_INVALID_HANDLE)
    log_debug("[hle] SVC 0x%02x (%s) -> InvalidHandle at PC 0x%016llx (x0 %llx, x1 %llx)", swi, svc_name(swi),
              (unsigned long long)regs->pc, (unsigned long long)arg0, (unsigned long long)arg1);
  else if ((uint32_t)regs->x[0] != HLE_RESULT_SUCCESS && (uint32_t)regs->x[0] != HLE_RESULT_TIMED_OUT)
    log_debug("[hle] SVC 0x%02x (%s) -> 0x%x at PC 0x%016llx (x0 %llx, x1 %llx)", swi, svc_name(swi),
              (uint32_t)regs->x[0], (unsigned long long)regs->pc, (unsigned long long)arg0, (unsigned long long)arg1);
}

void hle_on_undefined(CPU_State *cpu_state, uint32_t instruction, void *userdata)
{
  HLE_Context *context = (HLE_Context *)userdata;
  const uint64_t pc = context && context->cpu_backend
                          ? context->cpu_backend->get_pc(cpu_state)
                          : 0;
  log_error("[hle] undefined instruction 0x%08x at PC 0x%016llx",
            instruction,
            (unsigned long long)pc);
}
