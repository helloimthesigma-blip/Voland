/**
 * Thread, synchronization, info and debug SVCs over the scheduler (§7,
 * §12 priority list item 4). Phase 2, §25 "Threading + sync HLE".
 *
 * Register ABI per libnx's svc.s (out-pointer SVCs shift their real
 * arguments up one register and return outputs in W1/X1+):
 *   0x07 ExitProcess
 *   0x08 CreateThread          X1 entry, X2 arg, X3 stack top, W4 prio, W5 core (-2 = default) -> W1 handle
 *   0x09 StartThread           W0 handle
 *   0x0A ExitThread
 *   0x0B SleepThread           X0 ns (0/-1/-2: yield)
 *   0x0C GetThreadPriority     W1 handle -> W1 priority
 *   0x0D SetThreadPriority     W0 handle, W1 priority
 *   0x0E GetThreadCoreMask     W2 handle -> W1 core, X2 mask
 *   0x0F SetThreadCoreMask     W0 handle, W1 core, X2 mask
 *   0x10 GetCurrentProcessorNumber -> W0 core (a value, not a Result)
 *   0x18 WaitSynchronization   X1 handles, W2 count, X3 timeout -> W1 index
 *   0x19 CancelSynchronization W0 thread handle
 *   0x1A ArbitrateLock         W0 owner handle, X1 mutex, W2 requester tag
 *   0x1B ArbitrateUnlock       X0 mutex
 *   0x1C WaitProcessWideKeyAtomic X0 mutex, X1 key, W2 tag, X3 timeout
 *   0x1D SignalProcessWideKey  X0 key, W1 count (<= 0: all)
 *   0x1E GetSystemTick         -> X0 ticks (a value)
 *   0x24 GetProcessId          W1 handle -> X1 pid
 *   0x25 GetThreadId           W1 handle -> X1 thread id
 *   0x26 Break                 W0 reason (bit 31: notification only), X1, X2
 *   0x27 OutputDebugString     X0 string, X1 length
 *   0x29 GetInfo               W1 type, W2 handle, X3 subtype -> X1 value
 *   0x03 SetMemoryAttribute    accepted as a no-op (cache attributes only)
 *   0x34 WaitForAddress        X0 address, W1 type, W2 value, X3 timeout
 *   0x35 SignalToAddress       X0 address, W1 type, W2 value, W3 count
 *
 * Mutex words follow Horizon: owner handle | HLE_MUTEX_HAS_WAITERS. The
 * kernel only touches them through vmm (§5). Ownership is transferred on
 * unlock to the highest-priority waiter (FIFO within a priority);
 * signalled condition-variable waiters re-acquire their mutex in the
 * kernel, timed-out ones do not (libnx re-locks itself on TimedOut).
 *
 * Deviations, stated: events and their SVCs (Signal/Clear/ResetSignal,
 * CreateEvent) arrive with the services that hand them out (Phase 4);
 * WaitSynchronization therefore waits on thread handles only. Core
 * affinity is recorded but does not restrict scheduling (one worker,
 * §7). Priority is not inherited through mutexes.
 */
#ifndef SWITCH_HLE_KERNEL_SVC_THREAD_H
#define SWITCH_HLE_KERNEL_SVC_THREAD_H

#include "cpu/cpu.h"
#include "hle/hle.h"

#define HLE_MUTEX_HAS_WAITERS 0x40000000u
#define HLE_BREAK_NOTIFICATION_ONLY 0x80000000u
#define HLE_PROCESS_ID 0x51u
#define HLE_DEBUG_STRING_MAX 4096u

void hle_svc_exit_process(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_create_thread(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_start_thread(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_exit_thread(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_sleep_thread(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_thread_priority(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_set_thread_priority(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_thread_core_mask(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_set_thread_core_mask(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_current_processor_number(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_wait_synchronization(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_cancel_synchronization(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_arbitrate_lock(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_arbitrate_unlock(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_wait_process_wide_key_atomic(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_signal_process_wide_key(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_system_tick(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_process_id(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_thread_id(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_break(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_output_debug_string(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_get_info(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_set_memory_attribute(HLE_Context *context, CPU_State *cpu_state);
/* Events (event.h): 0x11 SignalEvent W0, 0x12 ClearEvent W0, 0x17
 * ResetSignal W0, 0x45 CreateEvent -> W1 writable, W2 readable. */
void hle_svc_signal_event(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_clear_event(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_reset_signal(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_create_event(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_wait_for_address(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_signal_to_address(HLE_Context *context, CPU_State *cpu_state);

#endif /* SWITCH_HLE_KERNEL_SVC_THREAD_H */
