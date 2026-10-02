/**
 * core/common/input_region: seqlock reader/writer (§18). Byte layout is
 * checked against tests/data/input_region_vectors.txt, which the web
 * writer's unit test consumes too, so the C and TypeScript writers
 * cannot drift apart.
 */
#define CHECK_NAME "input_region_test"
#include "check.h"

#include "common/input_region.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SLOT_BYTES 32u
#define SLOT_COUNT 8u
#define CANARY 0xA5u

/* 8 slots plus a canary slot each side, 4-byte aligned. */
static _Alignas(8) uint8_t g_region[(SLOT_COUNT + 2u) * SLOT_BYTES];
#define REGION (g_region + SLOT_BYTES)

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_slot(uint32_t slot, const Input_Controller_State *state) {
  input_region_write_begin(REGION, slot);
  input_region_write_payload(REGION, slot, state);
  input_region_write_end(REGION, slot);
}

static void test_constants(void) {
  /* HidNpadButton bit positions (libnx services/hid.h), restated. */
  CHECK(INPUT_BUTTON_A == 1u << 0 && INPUT_BUTTON_B == 1u << 1);
  CHECK(INPUT_BUTTON_X == 1u << 2 && INPUT_BUTTON_Y == 1u << 3);
  CHECK(INPUT_BUTTON_L == 1u << 6 && INPUT_BUTTON_ZR == 1u << 9);
  CHECK(INPUT_BUTTON_PLUS == 1u << 10 && INPUT_BUTTON_MINUS == 1u << 11);
  CHECK(INPUT_BUTTON_DPAD_LEFT == 1u << 12 && INPUT_BUTTON_DPAD_DOWN == 1u << 15);
  CHECK(INPUT_BUTTON_LEFT_SL == 1u << 24 && INPUT_BUTTON_RIGHT_SR == 1u << 27);
  /* Offsets restated from §18. */
  CHECK(INPUT_SLOT_OFFSET_SEQUENCE == 0 && INPUT_SLOT_OFFSET_BUTTONS == 4);
  CHECK(INPUT_SLOT_OFFSET_AXES == 8 && INPUT_SLOT_OFFSET_FLAGS == 24 && INPUT_SLOT_OFFSET_RESERVED == 28);
  CHECK(INPUT_REGION_SLOT_BYTES == SLOT_BYTES && INPUT_REGION_SLOT_COUNT == SLOT_COUNT);
}

static void test_vectors(const char *path) {
  FILE *file = fopen(path, "r");
  CHECK(file != NULL);
  char line[512];
  uint32_t vectors = 0;
  while (fgets(line, sizeof(line), file)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    Input_Controller_State state;
    int axes[8];
    char hex[65];
    CHECK(sscanf(line, "%x %d %d %d %d %d %d %d %d %x %64s", &state.buttons, &axes[0], &axes[1],
                 &axes[2], &axes[3], &axes[4], &axes[5], &axes[6], &axes[7], &state.flags, hex) == 11);
    for (uint32_t i = 0; i < 8; i++) state.axes[i] = (int16_t)axes[i];

    memset(g_region, 0, sizeof(g_region));
    write_slot(3, &state);
    for (uint32_t i = 0; i < SLOT_BYTES; i++) {
      unsigned byte = 0;
      CHECK(sscanf(hex + 2u * i, "%2x", &byte) == 1);
      CHECK(REGION[3u * SLOT_BYTES + i] == byte);
    }
    Input_Controller_State back;
    CHECK(input_region_read_slot(REGION, 3, &back));
    CHECK(back.buttons == state.buttons && back.flags == state.flags);
    CHECK(memcmp(back.axes, state.axes, sizeof(back.axes)) == 0);
    vectors++;
  }
  fclose(file);
  CHECK(vectors >= 5);
}

static void test_slots_are_independent(void) {
  memset(g_region, CANARY, sizeof(g_region));
  for (uint32_t slot = 0; slot < SLOT_COUNT; slot++) {
    memset(REGION + slot * SLOT_BYTES, 0, SLOT_BYTES);
  }
  for (uint32_t slot = 0; slot < SLOT_COUNT; slot++) {
    Input_Controller_State state;
    memset(&state, 0, sizeof(state));
    state.buttons = 1u << slot;
    state.axes[INPUT_AXIS_LEFT_X] = (int16_t)(slot * 1000);
    state.flags = INPUT_FLAG_CONNECTED | ((uint32_t)INPUT_DEVICE_STANDARD_GAMEPAD << INPUT_FLAG_DEVICE_KIND_SHIFT);
    write_slot(slot, &state);
  }
  for (uint32_t slot = 0; slot < SLOT_COUNT; slot++) {
    Input_Controller_State back;
    CHECK(input_region_read_slot(REGION, slot, &back));
    CHECK(back.buttons == 1u << slot && back.axes[0] == (int16_t)(slot * 1000));
    CHECK(input_state_is_connected(&back));
    CHECK(input_state_device_kind(&back) == INPUT_DEVICE_STANDARD_GAMEPAD);
  }
  for (uint32_t i = 0; i < SLOT_BYTES; i++) {
    CHECK(g_region[i] == CANARY);
    CHECK(g_region[(SLOT_COUNT + 1u) * SLOT_BYTES + i] == CANARY);
  }
}

static void test_torn_reads(void) {
  memset(g_region, 0, sizeof(g_region));
  Input_Controller_State first;
  memset(&first, 0, sizeof(first));
  first.buttons = INPUT_BUTTON_A;
  write_slot(0, &first);

  /* Writer stopped mid-update: sequence odd -> read fails, out untouched. */
  Input_Controller_State second = first;
  second.buttons = INPUT_BUTTON_B;
  input_region_write_begin(REGION, 0);
  input_region_write_payload(REGION, 0, &second);
  CHECK(le32(REGION) % 2u == 1u);
  Input_Controller_State out;
  memset(&out, 0x5A, sizeof(out));
  const Input_Controller_State untouched = out;
  CHECK(!input_region_read_slot(REGION, 0, &out));
  CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
  input_region_write_end(REGION, 0);
  CHECK(input_region_read_slot(REGION, 0, &out));
  CHECK(out.buttons == INPUT_BUTTON_B);
  CHECK(le32(REGION) == 4u); /* two complete writes */

  /* Bad arguments. */
  CHECK(!input_region_read_slot(REGION, SLOT_COUNT, &out));
  CHECK(!input_region_read_slot(NULL, 0, &out));
  CHECK(!input_region_read_slot(REGION, 0, NULL));
}

/* Two real threads: every state the writer publishes is internally
 * consistent (every field derived from one counter), so any torn read the
 * seqlock lets through shows up as a mismatch. */
#define STRESS_WRITES 200000u
static atomic_bool g_stop;

static void stress_state(uint32_t n, Input_Controller_State *state) {
  state->buttons = n;
  for (uint32_t i = 0; i < 8; i++) state->axes[i] = (int16_t)(n * (i + 1u));
  state->flags = ~n;
}

static void *stress_writer(void *arg) {
  (void)arg;
  for (uint32_t n = 1; n <= STRESS_WRITES; n++) {
    Input_Controller_State state;
    stress_state(n, &state);
    write_slot(5, &state);
  }
  atomic_store(&g_stop, true);
  return NULL;
}

static void test_concurrent_stress(void) {
  memset(g_region, 0, sizeof(g_region));
  atomic_store(&g_stop, false);
  pthread_t writer;
  CHECK(pthread_create(&writer, NULL, stress_writer, NULL) == 0);
  uint64_t good = 0;
  uint64_t retried = 0;
  while (!atomic_load(&g_stop)) {
    Input_Controller_State seen;
    if (!input_region_read_slot(REGION, 5, &seen)) {
      retried++;
      continue;
    }
    Input_Controller_State expected;
    stress_state(seen.buttons, &expected);
    if (seen.buttons != 0) {
      CHECK(seen.flags == expected.flags);
      CHECK(memcmp(seen.axes, expected.axes, sizeof(seen.axes)) == 0);
    }
    good++;
  }
  CHECK(pthread_join(writer, NULL) == 0);
  CHECK(good > 0);
  printf("[input_region_test] stress: %llu consistent reads, %llu bounded failures\n",
         (unsigned long long)good, (unsigned long long)retried);
}

/* Touch block: round trip, layout, count clamp, torn read. */
static void test_touch(void) {
  static _Alignas(8) uint8_t region[LAYOUT_INPUT_REGION_SIZE];
  memset(region, 0, sizeof(region));
  CHECK(INPUT_TOUCH_OFFSET + LAYOUT_INPUT_REGION_TOUCH_BYTES <= sizeof(region));
  Input_Touch_State in, out;
  memset(&in, 0, sizeof(in));
  in.count = 2;
  in.x[0] = 100; in.y[0] = 200; in.x[1] = 1279; in.y[1] = 719;
  input_region_write_touch(region, &in);
  const uint8_t *block = region + INPUT_TOUCH_OFFSET;
  CHECK(le32(block + INPUT_TOUCH_OFFSET_SEQUENCE) == 2u);
  CHECK(le32(block + INPUT_TOUCH_OFFSET_COUNT) == 2u);
  CHECK(le32(block + INPUT_TOUCH_OFFSET_POINTS) == (100u | (200u << 16)));
  memset(&out, 0, sizeof(out));
  CHECK(input_region_read_touch(region, &out));
  CHECK(out.count == 2 && out.x[0] == 100 && out.y[0] == 200 && out.x[1] == 1279 && out.y[1] == 719);
  /* The slots are untouched by the touch writer. */
  for (uint32_t i = 0; i < INPUT_TOUCH_OFFSET; i++) CHECK(region[i] == 0);
  /* A count beyond INPUT_TOUCH_MAX reads clamped. */
  region[INPUT_TOUCH_OFFSET + INPUT_TOUCH_OFFSET_COUNT] = 9;
  CHECK(input_region_read_touch(region, &out) && out.count == INPUT_TOUCH_MAX);
  /* Odd sequence (writer mid-update): fails, out untouched. */
  region[INPUT_TOUCH_OFFSET + INPUT_TOUCH_OFFSET_SEQUENCE] = 3;
  const Input_Touch_State before = out;
  CHECK(!input_region_read_touch(region, &out));
  CHECK(memcmp(&before, &out, sizeof(out)) == 0);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  test_constants();
  test_vectors(argv[1]);
  test_slots_are_independent();
  test_torn_reads();
  test_touch();
  test_concurrent_stress();
  printf("[input_region_test] passed\n");
  return 0;
}
