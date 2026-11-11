/*
 * Voland build hooks for the vendored libopus (CUSTOM_SUPPORT). Voland
 * only uses the *_get_size / *_init entry points over storage it owns, so
 * libopus's heap allocators are compiled to "out of memory": the object
 * links no malloc/realloc/free, and an accidental *_create call fails
 * cleanly instead of touching the heap.
 */
#ifndef VOLAND_OPUS_CUSTOM_SUPPORT_H
#define VOLAND_OPUS_CUSTOM_SUPPORT_H

#include <stddef.h>

#define OVERRIDE_OPUS_ALLOC
#define OVERRIDE_OPUS_REALLOC
#define OVERRIDE_OPUS_FREE

static inline void *opus_alloc(size_t size) {
  (void)size;
  return NULL;
}

static inline void *opus_realloc(void *ptr, size_t size) {
  (void)ptr;
  (void)size;
  return NULL;
}

static inline void opus_free(void *ptr) { (void)ptr; }

#endif /* VOLAND_OPUS_CUSTOM_SUPPORT_H */
