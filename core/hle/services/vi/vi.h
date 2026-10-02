/**
 * vi - the display service and its BufferQueue (§13 "the present path is
 * vi:, not raw framebuffers"). Registered as "vi:u" (applications; libnx
 * falls back to it from vi:m / vi:s).
 *
 *   vi:u 0 GetDisplayService -> IApplicationDisplayService:
 *     100 GetRelayService / 103 GetIndirectDisplayTransactionService
 *         -> IHOSBinderDriver; 101 ISystemDisplayService; 102
 *         IManagerDisplayService; 1000 ListDisplays; 1010 OpenDisplay;
 *         1011 OpenDefaultDisplay; 1020 CloseDisplay; 1101
 *         SetDisplayEnabled; 1102 GetDisplayResolution; 2020 OpenLayer;
 *         2021 CloseLayer; 2030/2031 Create/DestroyStrayLayer; 2101
 *         SetLayerScalingMode; 5202 GetDisplayVsyncEvent.
 *   IHOSBinderDriver: 0/3 TransactParcel(Auto), 1 AdjustRefcount,
 *     2 GetNativeHandle (the buffer-release event).
 *
 * The binder speaks Android's IGraphicBufferProducer over parcels (libnx
 * display/buffer_producer.c documents the wire form): CONNECT,
 * SET_PREALLOCATED_BUFFER (the GBFR-flattened NvGraphicBuffer: nvmap id,
 * offset, pitch, format, block-linear layout), DEQUEUE_BUFFER (always with
 * an empty NvMultiFence - work completes at submit, §13 Phase 3 scope),
 * REQUEST_BUFFER, QUEUE_BUFFER, CANCEL_BUFFER, QUERY, DISCONNECT.
 *
 * Presentation: vsync ticks at 60Hz of virtual time (vi_update). Each
 * vsync, every layer's oldest queued buffer is composited - read from
 * guest memory through vmm (nvmap id -> VA), deswizzled from block-linear
 * (gpu/block_linear.h) or copied from pitch-linear, converted to RGBA8 -
 * into the §6 framebuffer slots and published; the buffer it replaces on
 * screen is released and the layer's release event signalled. The release
 * event's signalled state always equals "a buffer is free", so libnx's
 * eventWait + async-dequeue loop never spins. A synchronous dequeue with
 * nothing free reclaims the oldest queued buffer (that frame is dropped)
 * instead of blocking - HLE never blocks (§7).
 */
#ifndef SWITCH_HLE_SERVICES_VI_VI_H
#define SWITCH_HLE_SERVICES_VI_VI_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/nvdrv/nvdrv.h"
#include "hle/services/sm/sm.h"

#define VI_MAX_LAYERS 4u
#define VI_MAX_SLOTS 16u
#define VI_DISPLAY_WIDTH 1280u
#define VI_DISPLAY_HEIGHT 720u
#define VI_DEFAULT_DISPLAY_ID 1u
#define VI_TICKS_PER_VSYNC 320000u /* 19.2MHz / 60Hz */
#define VI_GBFR_MAX_BYTES 0x200u
#define VI_PARCEL_MAX_BYTES 0x1000u
#define VI_SCRATCH_BYTES ((uint64_t)9 * 1024 * 1024) /* a 1920x1080 RGBA8 block-linear surface */

typedef enum Vi_Slot_State {
  VI_SLOT_FREE = 0,
  VI_SLOT_DEQUEUED,
  VI_SLOT_QUEUED,
  VI_SLOT_PRESENTED,
} Vi_Slot_State;

typedef struct Vi_Slot {
  Vi_Slot_State state;
  bool preallocated;
  uint64_t queue_order;
  /* From the NvGraphicBuffer. */
  uint32_t nvmap_id;
  uint32_t offset;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;      /* bytes */
  uint32_t format;     /* PIXEL_FORMAT_* */
  uint32_t layout;     /* NvLayout: 1 pitch, 3 block-linear */
  uint32_t block_height_log2;
  uint64_t size;
  uint8_t gbfr[VI_GBFR_MAX_BYTES]; /* REQUEST_BUFFER echoes it */
  uint32_t gbfr_size;
  /* From the last QueueBuffer: the source rectangle shown (empty = the
   * whole buffer) and the NATIVE_WINDOW_TRANSFORM_* flips. */
  int32_t crop_left, crop_top, crop_right, crop_bottom;
  uint32_t transform;
} Vi_Slot;

typedef struct Vi_Layer {
  bool used;
  uint64_t id;
  bool connected;
  uint64_t queue_counter;
  Kernel_Event *release_event;
  Vi_Slot slots[VI_MAX_SLOTS];
} Vi_Layer;

typedef struct Vi_State {
  Service_Interface root;
  Service_Interface root_system;  /* vi:s - same display service */
  Service_Interface root_manager; /* vi:m */
  Service_Interface application_display;
  Service_Interface relay;
  Service_Interface system_display;
  Service_Interface manager_display;
  Vi_Layer layers[VI_MAX_LAYERS];
  Kernel_Event *vsync_event;
  Nvdrv_State *nvdrv;
  uint8_t *scratch;          /* VI_SCRATCH_BYTES, caller-owned */
  uint64_t next_vsync_ticks;
  uint64_t next_stray_layer_id;
  uint64_t frames_presented;
  uint64_t frames_dropped;
  uint8_t parcel_in[VI_PARCEL_MAX_BYTES];
  uint8_t parcel_out[VI_PARCEL_MAX_BYTES];
} Vi_State;

void vi_init(Vi_State *state, Nvdrv_State *nvdrv, uint8_t *scratch);
Error vi_register(Vi_State *state, SM_Registry *registry);

/* Vsync: composites queued buffers and signals events when a 60Hz period
 * of virtual time has passed. Called every scheduler slice. */
void vi_update(Vi_State *state, HLE_Context *context, uint64_t now_ticks);

/* When the next vsync can wake a waiting thread: the next vsync time once
 * the program has a display (a layer or a vsync event), else UINT64_MAX. */
uint64_t vi_next_wake(const Vi_State *state);

#endif /* SWITCH_HLE_SERVICES_VI_VI_H */
