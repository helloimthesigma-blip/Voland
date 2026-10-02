/**
 * hid - the human-interface-device service (§12 priority list, §18
 * input). Registered with sm: as "hid".
 *
 * Games never ask hid for controller state over IPC: they map the
 * 0x40000-byte HidSharedMemory block (CreateAppletResource ->
 * IAppletResource GetSharedMemoryHandle -> svcMapSharedMemory) and read
 * the per-controller LIFOs straight out of it. This service owns that
 * block and refreshes it from the input region (§18, input_region.h) at
 * the console's 200Hz npad sampling rate in virtual time.
 *
 * Layout (offsets per the public libnx hid.h structs):
 *   HidSharedMemory.npad              @ 0x9A00, 10 entries x 0x5000
 *     entries 0-7 = No1-No8, 8 = Handheld, 9 = Other
 *   HidNpadInternalState (in each entry)
 *     style_set @0x0, joy_assignment_mode @0x4, full_key_color @0x8,
 *     joy_color @0x14, then seven HidNpadCommonLifo (0x350 bytes each):
 *     full_key @0x28, handheld @0x378, joy_dual @0x6C8, joy_left @0xA18,
 *     joy_right @0xD68, palma @0x10B8, system_ext @0x1408;
 *     device_type @0x4188, system_properties @0x4190,
 *     battery_level[3] @0x419C
 *   HidNpadCommonLifo: header {buffer_count @8, tail @0x10, count @0x18},
 *     17 storages of 0x30 {sampling_number, HidNpadCommonState}
 *   HidNpadCommonState (0x28): sampling_number, buttons, stick_l {x,y},
 *     stick_r {x,y}, attributes
 * The reader (libnx _hidGetStates) treats `tail` as the newest entry and
 * requires consecutive entries' sampling numbers to differ by exactly 1.
 *
 * Controller assignment: input-region slot i drives npad No(i+1), in the
 * first style the title declared support for, preferring FullKey (a Pro
 * Controller). If the title supports neither FullKey nor No1 but does
 * support Handheld, slot 0 drives the Handheld entry instead. Stick
 * direction bits (HidNpadButton 16-23) are derived from the sticks.
 *
 * Touch screen (HidTouchScreenLifo @0x400: header, 17 storages of 0x298
 * {sampling_number, HidTouchScreenState {sampling_number, count,
 * reserved, HidTouchState[16] of 0x28}}) is fed from the input region's
 * touch block at the same 200Hz: a point that just went down carries the
 * Start attribute and a fresh finger id.
 *
 * Scope, stated: mouse, keyboard, gestures, six-axis and vibration are
 * accepted and inert (their LIFOs stay empty; vibration values are
 * dropped). Home/Capture go to the applet layer, not here.
 */
#ifndef SWITCH_HLE_SERVICES_HID_HID_H
#define SWITCH_HLE_SERVICES_HID_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/kernel/shared_memory.h"
#include "hle/services/sm/sm.h"

#define HID_SHARED_MEMORY_BYTES 0x40000u
#define HID_NPAD_COUNT 10u           /* No1-No8, Handheld, Other */
#define HID_NPAD_HANDHELD_INDEX 8u
#define HID_NPAD_OTHER_INDEX 9u
#define HID_NPAD_ID_HANDHELD 0x20u   /* HidNpadIdType */
#define HID_NPAD_ID_OTHER 0x10u
#define HID_LIFO_ENTRIES 17u
#define HID_TICKS_PER_SAMPLE 96000u  /* 19.2MHz / 200Hz */
#define HID_TOUCH_SECTION_OFFSET 0x400u
/* The Nintendo SDK's NpadCondition block (its hid client reads it from
 * shared memory, not over IPC): u32 joy hold type @+8, u8 initialized
 * @+0xC. Written at activation and on every SetNpadJoyHoldType. */
#define HID_NPAD_CONDITION_OFFSET 0x3E200u
#define HID_NPAD_CONDITION_HOLD_TYPE 0x8u
#define HID_NPAD_CONDITION_INITIALIZED 0xCu
#define HID_TOUCH_STORAGE_BYTES 0x298u
#define HID_TOUCH_STATE_BYTES 0x290u
#define HID_TOUCH_POINT_BYTES 0x28u
#define HID_TOUCH_POINTS_OFFSET 0x10u /* within a HidTouchScreenState */
#define HID_TOUCH_ATTRIBUTE_START (1u << 0)
#define HID_TOUCH_ATTRIBUTE_END (1u << 1)
#define HID_TOUCH_DIAMETER 15u

/* Shared-memory layout (see above). */
#define HID_NPAD_SECTION_OFFSET 0x9A00u
#define HID_NPAD_ENTRY_BYTES 0x5000u
#define HID_NPAD_STYLE_SET 0x0u
#define HID_NPAD_JOY_ASSIGNMENT_MODE 0x4u
#define HID_NPAD_FULL_KEY_COLOR 0x8u
#define HID_NPAD_JOY_COLOR 0x14u
#define HID_NPAD_DEVICE_TYPE 0x4188u
#define HID_NPAD_SYSTEM_PROPERTIES 0x4190u
#define HID_NPAD_BATTERY_LEVEL 0x419Cu
#define HID_NPAD_LIFO_BYTES 0x350u
#define HID_LIFO_HEADER_BUFFER_COUNT 0x8u
#define HID_LIFO_HEADER_TAIL 0x10u
#define HID_LIFO_HEADER_COUNT 0x18u
#define HID_LIFO_STORAGE_OFFSET 0x20u
#define HID_LIFO_STORAGE_BYTES 0x30u
#define HID_LIFO_STATE_OFFSET 0x8u   /* within a storage, after its sampling number */
#define HID_NPAD_STATE_BYTES 0x28u

/* HidNpadStyleTag. */
#define HID_STYLE_FULL_KEY (1u << 0)
#define HID_STYLE_HANDHELD (1u << 1)
#define HID_STYLE_JOY_DUAL (1u << 2)
#define HID_STYLE_JOY_LEFT (1u << 3)
#define HID_STYLE_JOY_RIGHT (1u << 4)
#define HID_STYLE_STANDARD 0x1Fu

/* HidNpadAttribute. */
#define HID_ATTR_IS_CONNECTED (1u << 0)
#define HID_ATTR_IS_WIRED (1u << 1)
#define HID_ATTR_IS_LEFT_CONNECTED (1u << 2)
#define HID_ATTR_IS_LEFT_WIRED (1u << 3)
#define HID_ATTR_IS_RIGHT_CONNECTED (1u << 4)
#define HID_ATTR_IS_RIGHT_WIRED (1u << 5)

/* HidNpadButton stick-direction bits, and the deflection that sets them. */
#define HID_BUTTON_STICK_L_LEFT (1u << 16)
#define HID_BUTTON_STICK_L_UP (1u << 17)
#define HID_BUTTON_STICK_L_RIGHT (1u << 18)
#define HID_BUTTON_STICK_L_DOWN (1u << 19)
#define HID_BUTTON_STICK_R_LEFT (1u << 20)
#define HID_BUTTON_STICK_R_UP (1u << 21)
#define HID_BUTTON_STICK_R_RIGHT (1u << 22)
#define HID_BUTTON_STICK_R_DOWN (1u << 23)
#define HID_STICK_DIRECTION_THRESHOLD 16384

/* HidDeviceTypeBits. */
#define HID_DEVICE_FULL_KEY (1u << 0)
#define HID_DEVICE_HANDHELD_LEFT (1u << 2)
#define HID_DEVICE_HANDHELD_RIGHT (1u << 3)
#define HID_DEVICE_JOY_LEFT (1u << 4)
#define HID_DEVICE_JOY_RIGHT (1u << 5)

typedef struct Hid_Npad {
  uint32_t style;            /* the one style tag it is presented as; 0 = disconnected */
  uint32_t tail;             /* LIFO cursor shared by all seven LIFOs */
  uint32_t count;
  Kernel_Event *style_event; /* AcquireNpadStyleSetUpdateEventHandle; NULL until asked */
} Hid_Npad;

typedef struct Hid_State {
  Service_Interface interface;          /* "hid" */
  Service_Interface applet_resource;    /* IAppletResource */
  Service_Interface vibration_list;     /* IActiveVibrationDeviceList */
  Shared_Memory_Pool *pool;
  Kernel_Shared_Memory *shared_memory;  /* created on first CreateAppletResource */
  uint32_t supported_styles;
  uint32_t supported_ids;               /* bit n = No(n+1), bit 8 Handheld, bit 9 Other */
  uint64_t joy_hold_type;
  uint64_t handheld_activation_mode;
  uint64_t communication_mode;
  uint64_t sampling_number;
  uint64_t last_sample_ticks;
  bool sampled_once;
  Hid_Npad npads[HID_NPAD_COUNT];
  /* touch screen LIFO */
  uint32_t touch_tail, touch_count;
  uint32_t touch_down;                  /* points down at the last sample */
  uint32_t finger_ids[2];
  uint32_t next_finger_id;
} Hid_State;

/* Resets the state and initializes the interfaces. */
void hid_init(Hid_State *state, Shared_Memory_Pool *pool);

Error hid_register(Hid_State *state, SM_Registry *registry);

/* Samples the input region into shared memory if a 200Hz period of
 * virtual time has passed since the last sample (always on the first
 * call). `input_region` is the §18 region base. No-op until the guest has
 * created the applet resource. */
void hid_update(Hid_State *state, HLE_Context *context, const void *input_region, uint64_t now_ticks);

#endif /* SWITCH_HLE_SERVICES_HID_HID_H */
