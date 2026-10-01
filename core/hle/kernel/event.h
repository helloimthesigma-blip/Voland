/**
 * Kernel events (§12): one object reachable through a writable handle
 * (SignalEvent) and a readable handle (WaitSynchronization, ResetSignal).
 * A signaled event stays signaled until cleared - Horizon's readable
 * events do not auto-reset on a successful wait. Services hand readable
 * handles to the guest (nvdrv QueryEvent; vsync and friends in Phase 4).
 *
 * Objects live in a fixed pool owned by the Emulator; each is reference
 * counted by the handles naming it (CloseHandle drops one reference) plus
 * any service that keeps signalling it.
 */
#ifndef SWITCH_HLE_KERNEL_EVENT_H
#define SWITCH_HLE_KERNEL_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#define EVENT_POOL_CAPACITY 256u

typedef struct Kernel_Event {
  uint32_t references; /* 0 = free slot */
  bool signaled;
} Kernel_Event;

typedef struct Event_Pool {
  Kernel_Event events[EVENT_POOL_CAPACITY];
} Event_Pool;

void event_pool_init(Event_Pool *pool);

/* A fresh, unsignaled event with one reference, or NULL when full. */
Kernel_Event *event_create(Event_Pool *pool);
void event_retain(Kernel_Event *event);
void event_release(Kernel_Event *event);

#endif /* SWITCH_HLE_KERNEL_EVENT_H */
