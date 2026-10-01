/**
 * Seqlock reader/writer over the §18 input region. See input_region.h for
 * the slot layout and the protocol.
 *
 * Every access is a 32-bit atomic: the sequence with acquire/release,
 * the payload words relaxed. Relaxed payload accesses keep the reader
 * free of data races in the C11 sense while the fences around them give
 * the seqlock its ordering; on every target Voland builds for, a relaxed
 * 32-bit atomic load is a plain load.
 */
#include "common/input_region.h"

#include <stdatomic.h>
#include <stddef.h>

#define INPUT_PAYLOAD_WORDS 6u /* buttons, 4 axis pairs, flags */
#define INPUT_PAYLOAD_FIRST_WORD (INPUT_SLOT_OFFSET_BUTTONS / 4u)

typedef _Atomic uint32_t Input_Word;

static Input_Word *slot_words(const void *region_base, uint32_t slot) {
  return (Input_Word *)((uintptr_t)region_base + (uintptr_t)slot * INPUT_REGION_SLOT_BYTES);
}

static void unpack(const uint32_t words[INPUT_PAYLOAD_WORDS], Input_Controller_State *out) {
  out->buttons = words[0];
  for (uint32_t pair = 0; pair < INPUT_AXIS_COUNT / 2u; pair++) {
    out->axes[pair * 2u] = (int16_t)(uint16_t)(words[1u + pair] & 0xFFFFu);
    out->axes[pair * 2u + 1u] = (int16_t)(uint16_t)(words[1u + pair] >> 16);
  }
  out->flags = words[5];
}

static void pack(const Input_Controller_State *state, uint32_t words[INPUT_PAYLOAD_WORDS]) {
  words[0] = state->buttons;
  for (uint32_t pair = 0; pair < INPUT_AXIS_COUNT / 2u; pair++) {
    words[1u + pair] = (uint32_t)(uint16_t)state->axes[pair * 2u] |
                       ((uint32_t)(uint16_t)state->axes[pair * 2u + 1u] << 16);
  }
  words[5] = state->flags;
}

bool input_region_read_slot(const void *region_base, uint32_t slot, Input_Controller_State *out) {
  if (!region_base || !out || slot >= INPUT_REGION_SLOT_COUNT) return false;
  Input_Word *words = slot_words(region_base, slot);
  for (uint32_t attempt = 0; attempt < INPUT_REGION_READ_ATTEMPTS; attempt++) {
    const uint32_t before = atomic_load_explicit(&words[0], memory_order_acquire);
    if (before & 1u) continue; /* writer mid-update */
    uint32_t payload[INPUT_PAYLOAD_WORDS];
    for (uint32_t i = 0; i < INPUT_PAYLOAD_WORDS; i++) {
      payload[i] = atomic_load_explicit(&words[INPUT_PAYLOAD_FIRST_WORD + i], memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_acquire);
    const uint32_t after = atomic_load_explicit(&words[0], memory_order_relaxed);
    if (before != after) continue; /* torn: a write landed during the copy */
    unpack(payload, out);
    return true;
  }
  return false;
}

void input_region_write_begin(void *region_base, uint32_t slot) {
  if (!region_base || slot >= INPUT_REGION_SLOT_COUNT) return;
  Input_Word *words = slot_words(region_base, slot);
  atomic_fetch_add_explicit(&words[0], 1u, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
}

void input_region_write_payload(void *region_base, uint32_t slot,
                                const Input_Controller_State *state) {
  if (!region_base || !state || slot >= INPUT_REGION_SLOT_COUNT) return;
  Input_Word *words = slot_words(region_base, slot);
  uint32_t payload[INPUT_PAYLOAD_WORDS];
  pack(state, payload);
  for (uint32_t i = 0; i < INPUT_PAYLOAD_WORDS; i++) {
    atomic_store_explicit(&words[INPUT_PAYLOAD_FIRST_WORD + i], payload[i], memory_order_relaxed);
  }
  atomic_store_explicit(&words[INPUT_SLOT_OFFSET_RESERVED / 4u], 0u, memory_order_relaxed);
}

void input_region_write_end(void *region_base, uint32_t slot) {
  if (!region_base || slot >= INPUT_REGION_SLOT_COUNT) return;
  Input_Word *words = slot_words(region_base, slot);
  atomic_fetch_add_explicit(&words[0], 1u, memory_order_release);
}
