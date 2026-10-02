/**
 * Input region: the host -> core controller transport (§18). Phase 1,
 * §25 "Input region + seqlock writer/reader".
 *
 * The region is already reserved by layout.c (LAYOUT_INPUT_REGION_*:
 * 8 slots x 32 bytes, base = layout_get()->input_region_base). This
 * header fixes the byte layout inside a slot and provides the READER;
 * the WRITER is platform code (web: main thread, once per rAF -
 * platform/web/src/input/input-region.ts; native: the platform's input
 * thread, through input_region_write_* below). It is host memory, not guest RAM: nothing here goes through
 * vmm, and the guest never sees this layout - the Phase 4 hid:
 * shared-memory writer (§12) transforms it into Horizon's npad rings.
 *
 * Slot layout (little-endian, §18's table made exact):
 *   +0   u32  sequence   seqlock counter; odd = write in progress
 *   +4   u32  buttons    INPUT_BUTTON_* bits below
 *   +8   i16  axes[8]    INPUT_AXIS_* indices; sticks in
 *                        [-INPUT_AXIS_MAX, INPUT_AXIS_MAX], +Y = up
 *   +24  u32  flags      INPUT_FLAG_* fields below
 *   +28  u32  reserved   written 0
 *
 * Buttons. Bits 0-15 and 24-27 are Horizon's HidNpadButton bits
 * (libnx services/hid.h) so the hid: writer copies them unchanged - the
 * A<->B / X<->Y swap relative to the Standard Gamepad layout (§18
 * "Controller mapping") happens in the platform writer, once. Bits 16-23
 * (Horizon's stick pseudo-buttons) are NOT transported: the hid: writer
 * derives them from the axes, so a writer leaves them 0. Bits 28/29
 * carry Home and Capture, which are not npad buttons on Horizon (they
 * reach the system applet, not the game); the hid: writer masks them
 * out of npad state.
 *
 * Flags. Bit 0 = connected. Bits 8-15 = device kind (INPUT_DEVICE_*), the
 * input to npad style negotiation (§18): which styles this physical
 * device can present. Bits 16-31 = profile id (the mapping profile the
 * writer applied; diagnostic, and the key the settings UI shows).
 * Connect/disconnect also travel over postMessage as lifecycle events
 * (§16); the flag is what the reader trusts.
 *
 * Seqlock protocol (§18):
 *   writer: sequence += 1 (now odd) -> write payload -> sequence += 1
 *           (now even), the second increment with release ordering.
 *           Single writer per slot.
 *   reader: s1 = sequence (acquire); if odd, retry; copy payload;
 *           acquire fence; s2 = sequence; if s1 != s2, retry. At most
 *           INPUT_REGION_READ_ATTEMPTS attempts - never waits, never
 *           spins unbounded. On failure the caller keeps its previous
 *           state for this poll (a 60 Hz writer is mid-write for well
 *           under a microsecond; a failed read costs one stale sample).
 * Touch block (after the slots, same seqlock protocol): the touch
 * screen, in the console's 1280x720 coordinates.
 *   +0   u32  sequence
 *   +4   u32  count      touching points, 0..INPUT_TOUCH_MAX
 *   +8   u16  x, u16 y   per point (INPUT_TOUCH_MAX of them)
 *   rest reserved, written 0
 * The C reader uses C11 <stdatomic.h> on a 32-bit word; the web writer
 * uses Atomics.add on an Int32Array over the same shared memory - both
 * sequentially consistent at least, which subsumes acquire/release.
 */
#ifndef SWITCH_COMMON_INPUT_REGION_H
#define SWITCH_COMMON_INPUT_REGION_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"

#define INPUT_REGION_SLOT_COUNT ((uint32_t)LAYOUT_INPUT_REGION_MAX_CONTROLLERS)
#define INPUT_REGION_SLOT_BYTES ((uint32_t)LAYOUT_INPUT_REGION_SLOT_BYTES)

/* Byte offsets within a slot. */
#define INPUT_SLOT_OFFSET_SEQUENCE 0u
#define INPUT_SLOT_OFFSET_BUTTONS 4u
#define INPUT_SLOT_OFFSET_AXES 8u
#define INPUT_SLOT_OFFSET_FLAGS 24u
#define INPUT_SLOT_OFFSET_RESERVED 28u

#define INPUT_AXIS_COUNT 8u
#define INPUT_AXIS_MAX 32767 /* Horizon's stick range (HidAnalogStickState) */

typedef enum Input_Axis {
  INPUT_AXIS_LEFT_X = 0,
  INPUT_AXIS_LEFT_Y = 1,
  INPUT_AXIS_RIGHT_X = 2,
  INPUT_AXIS_RIGHT_Y = 3,
  /* 4-7 reserved (written 0): motion arrives with its own §18 source. */
} Input_Axis;

/* HidNpadButton-compatible bits. */
#define INPUT_BUTTON_A (1u << 0)
#define INPUT_BUTTON_B (1u << 1)
#define INPUT_BUTTON_X (1u << 2)
#define INPUT_BUTTON_Y (1u << 3)
#define INPUT_BUTTON_STICK_L (1u << 4)
#define INPUT_BUTTON_STICK_R (1u << 5)
#define INPUT_BUTTON_L (1u << 6)
#define INPUT_BUTTON_R (1u << 7)
#define INPUT_BUTTON_ZL (1u << 8)
#define INPUT_BUTTON_ZR (1u << 9)
#define INPUT_BUTTON_PLUS (1u << 10)
#define INPUT_BUTTON_MINUS (1u << 11)
#define INPUT_BUTTON_DPAD_LEFT (1u << 12)
#define INPUT_BUTTON_DPAD_UP (1u << 13)
#define INPUT_BUTTON_DPAD_RIGHT (1u << 14)
#define INPUT_BUTTON_DPAD_DOWN (1u << 15)
#define INPUT_BUTTON_LEFT_SL (1u << 24)
#define INPUT_BUTTON_LEFT_SR (1u << 25)
#define INPUT_BUTTON_RIGHT_SL (1u << 26)
#define INPUT_BUTTON_RIGHT_SR (1u << 27)
/* Region-only (not npad): */
#define INPUT_BUTTON_HOME (1u << 28)
#define INPUT_BUTTON_CAPTURE (1u << 29)

#define INPUT_BUTTON_TRANSPORTED_MASK ((uint32_t)0x3F00FFFFu) /* bits 0-15, 24-29 */

#define INPUT_FLAG_CONNECTED (1u << 0)
#define INPUT_FLAG_DEVICE_KIND_SHIFT 8u
#define INPUT_FLAG_DEVICE_KIND_MASK ((uint32_t)0xFFu << INPUT_FLAG_DEVICE_KIND_SHIFT)
#define INPUT_FLAG_PROFILE_ID_SHIFT 16u
#define INPUT_FLAG_PROFILE_ID_MASK ((uint32_t)0xFFFFu << INPUT_FLAG_PROFILE_ID_SHIFT)

typedef enum Input_Device_Kind {
  INPUT_DEVICE_NONE = 0,
  INPUT_DEVICE_STANDARD_GAMEPAD = 1, /* Gamepad API, standard mapping: full-key styles */
  INPUT_DEVICE_PRO_CONTROLLER = 2,   /* WebHID 057E:2009 */
  INPUT_DEVICE_JOYCON_LEFT = 3,      /* WebHID 057E:2006 - single or dual */
  INPUT_DEVICE_JOYCON_RIGHT = 4,     /* WebHID 057E:2007 */
  INPUT_DEVICE_KEYBOARD = 5,         /* keyboard/pointer profile: full-key styles */
} Input_Device_Kind;

#define INPUT_REGION_READ_ATTEMPTS 4u

#define INPUT_TOUCH_OFFSET (INPUT_REGION_SLOT_COUNT * INPUT_REGION_SLOT_BYTES)
#define INPUT_TOUCH_OFFSET_SEQUENCE 0u
#define INPUT_TOUCH_OFFSET_COUNT 4u
#define INPUT_TOUCH_OFFSET_POINTS 8u
#define INPUT_TOUCH_MAX 2u
#define INPUT_TOUCH_WIDTH 1280u
#define INPUT_TOUCH_HEIGHT 720u

typedef struct Input_Touch_State {
  uint32_t count;
  uint16_t x[INPUT_TOUCH_MAX];
  uint16_t y[INPUT_TOUCH_MAX];
} Input_Touch_State;

/* Seqlock read of the touch block; same contract as input_region_read_slot. */
bool input_region_read_touch(const void *region_base, Input_Touch_State *out);
/* Writer side (tests, native hosts). */
void input_region_write_touch(void *region_base, const Input_Touch_State *state);

/* One slot's payload as the reader returns it. */
typedef struct Input_Controller_State {
  uint32_t buttons;
  int16_t axes[INPUT_AXIS_COUNT];
  uint32_t flags;
} Input_Controller_State;

/* Seqlock read of `slot` from the region at `region_base` (normally
 * layout_get()->input_region_base, cast; tests pass their own buffer).
 * true + `out` filled on a consistent read; false if `slot` is out of
 * range, an argument is NULL, or every attempt saw a write in progress
 * or a torn sequence - `out` is then left untouched. Never blocks. */
bool input_region_read_slot(const void *region_base, uint32_t slot, Input_Controller_State *out);

/* Field helpers so callers never shift flags by hand. */
static inline bool input_state_is_connected(const Input_Controller_State *state) {
  return (state->flags & INPUT_FLAG_CONNECTED) != 0u;
}

static inline Input_Device_Kind input_state_device_kind(const Input_Controller_State *state) {
  return (Input_Device_Kind)((state->flags & INPUT_FLAG_DEVICE_KIND_MASK) >>
                             INPUT_FLAG_DEVICE_KIND_SHIFT);
}

/* Writer half, C side: used by native platform layers and by the reader's
 * own tests (a torn-write test needs a writer it can stop half way).
 * The web writer is TypeScript and mirrors these exactly. */
void input_region_write_begin(void *region_base, uint32_t slot);
void input_region_write_payload(void *region_base, uint32_t slot,
                                const Input_Controller_State *state);
void input_region_write_end(void *region_base, uint32_t slot);

#endif /* SWITCH_COMMON_INPUT_REGION_H */
