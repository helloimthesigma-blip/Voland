#include "hle/kernel/event.h"

#include <string.h>

void event_pool_init(Event_Pool *pool) { memset(pool, 0, sizeof(*pool)); }

Kernel_Event *event_create(Event_Pool *pool) {
  for (uint32_t i = 0; i < EVENT_POOL_CAPACITY; i++) {
    Kernel_Event *e = &pool->events[i];
    if (e->references) continue;
    e->references = 1;
    e->signaled = false;
    return e;
  }
  return NULL;
}

void event_retain(Kernel_Event *event) {
  if (event) event->references++;
}

void event_release(Kernel_Event *event) {
  if (event && event->references) event->references--;
}
