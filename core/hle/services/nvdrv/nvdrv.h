/**
 * nvdrv - the NVIDIA driver service (§12 priority list item 6; §25 Phase 3
 * "Minimal nvdrv stub (nvmap + channel submit) + syncpoint skeleton").
 * Registered with sm: as "nvdrv", "nvdrv:a", "nvdrv:s" and "nvdrv:t".
 *
 * Commands (libnx services/nv.c): 0 Open (path in an A/X buffer -> fd,
 * error), 1 Ioctl (fd, request; in A/X, out B/C -> error), 2 Close,
 * 3 Initialize, 4 QueryEvent (-> copy handle), 8 SetAruid, 11 Ioctl2
 * (extra in buffer), 12 Ioctl3 (extra out buffer), 13
 * SetGraphicsFirmwareMemoryMarginEnabled. Ioctl requests use the Linux
 * encoding (dir<<30 | size<<16 | type<<8 | nr); results are NvError codes
 * in the reply data, the IPC result is success.
 *
 * Devices and ioctls (struct layouts per libnx nvidia/ioctl/):
 *   /dev/nvmap          CREATE, FROM_ID, ALLOC, FREE, PARAM, GET_ID
 *   /dev/nvhost-ctrl    SYNCPT_READ/INCR/WAIT/WAIT_EX, EVENT_WAIT(_ASYNC),
 *                       EVENT_REGISTER/UNREGISTER, SYNCPT_CLEAR_EVENT_WAIT
 *   /dev/nvhost-ctrl-gpu ZCULL_GET_CTX_SIZE/INFO, GET_CHARACTERISTICS (GM20B),
 *                       GET_TPC_MASKS, GET_ACTIVE_SLOT_MASK, ZBC_*
 *   /dev/nvhost-as-gpu  BIND_CHANNEL, ALLOC/FREE_SPACE, MAP_BUFFER_EX,
 *                       UNMAP_BUFFER, GET_VA_REGIONS, INITIALIZE_EX
 *   /dev/nvhost-gpu     SET_NVMAP_FD, ALLOC_GPFIFO_EX2, SUBMIT_GPFIFO,
 *                       KICKOFF_PB, ALLOC_OBJ_CTX, priority/timeout/notifier
 *   /dev/nvhost-nvdec, /dev/nvhost-vic, /dev/nvhost-nvjpg
 *                       channel SUBMIT, GET_SYNCPOINT, GET_WAITBASE,
 *                       MAP/UNMAP_BUFFER, clock/timeout setters
 *
 * Phase 3 scope, stated: GPU work is not executed yet (the command ring
 * and Maxwell engines are Phase 4). Every submission - GPFIFO and
 * NVDEC/VIC alike - is accepted and its syncpoint increments complete at
 * once, so fences a title waits on are already reached ("black frames
 * that complete", §25). Syncpoint waits therefore never block. The GPU
 * completion ring (gpu/syncpoint.h) is drained every scheduler slice, so
 * once the GPU worker posts real completions they flow through the same
 * path, and EVENT_WAIT_ASYNC waiters are signalled from it.
 */
#ifndef SWITCH_HLE_SERVICES_NVDRV_NVDRV_H
#define SWITCH_HLE_SERVICES_NVDRV_NVDRV_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu/syncpoint.h"
#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define NVDRV_MAX_FDS 64u
#define NVMAP_MAX_HANDLES 1024u
#define NVDRV_MAX_GPU_MAPPINGS 512u
#define NVDRV_MAX_EVENTS 64u
#define NVDRV_IOCTL_MAX_BYTES 0x4000u /* the 14-bit size field */

/* NvError codes (libnx result.h LibnxNvidiaError mapping). */
#define NV_SUCCESS 0u
#define NV_NOT_IMPLEMENTED 1u
#define NV_NOT_SUPPORTED 2u
#define NV_NOT_INITIALIZED 3u
#define NV_BAD_PARAMETER 4u
#define NV_TIMEOUT 5u
#define NV_INSUFFICIENT_MEMORY 6u
#define NV_BAD_VALUE 0xBu
#define NV_ALREADY_ALLOCATED 0xDu
#define NV_FILE_OPERATION_FAILED 0x30003u

typedef enum Nv_Device {
  NV_DEVICE_NONE = 0,
  NV_DEVICE_NVMAP,
  NV_DEVICE_CTRL,
  NV_DEVICE_CTRL_GPU,
  NV_DEVICE_AS_GPU,
  NV_DEVICE_GPU,
  NV_DEVICE_NVDEC,
  NV_DEVICE_VIC,
  NV_DEVICE_NVJPG,
} Nv_Device;

typedef struct Nv_Fd {
  Nv_Device device;
  uint32_t syncpoint;   /* channels: their syncpoint, allocated on open/gpfifo */
  uint32_t nvmap_fd;
  uint32_t submissions; /* diagnostics */
} Nv_Fd;

typedef struct Nvmap_Handle {
  uint32_t references; /* 0 = free */
  uint32_t size;
  uint32_t align;
  uint32_t flags;
  uint32_t heap_mask;
  uint8_t kind;
  bool allocated;
  uint64_t address;    /* guest VA backing the buffer */
} Nvmap_Handle;

typedef struct Gpu_Mapping {
  bool in_use;
  uint64_t gpu_va;
  uint64_t size;
  uint32_t nvmap_handle;
  uint64_t buffer_offset;
} Gpu_Mapping;

typedef struct Nv_Event_Slot {
  Kernel_Event *event;  /* from QueryEvent; NULL until queried */
  uint32_t readable_handle;
  bool waiting;         /* EVENT_WAIT_ASYNC registered */
  uint32_t syncpoint;
  uint32_t threshold;
} Nv_Event_Slot;

typedef struct Nvdrv_State {
  Service_Interface interface; /* service_state points back here */
  Nv_Fd fds[NVDRV_MAX_FDS];
  Nvmap_Handle handles[NVMAP_MAX_HANDLES]; /* nvmap handle n is handles[n - 1] */
  Gpu_Mapping mappings[NVDRV_MAX_GPU_MAPPINGS];
  uint64_t next_gpu_va;
  Syncpoints syncpoints;
  Nv_Event_Slot events[NVDRV_MAX_EVENTS];
  uint8_t ioctl_buffer[NVDRV_IOCTL_MAX_BYTES];
  uint8_t extra_buffer[NVDRV_IOCTL_MAX_BYTES];
  uint64_t ioctl_count;
} Nvdrv_State;

/* Resets the state and initializes `state->interface`. */
void nvdrv_init(Nvdrv_State *state);

/* Registers the four service names with sm:. */
Error nvdrv_register(Nvdrv_State *state, SM_Registry *registry);

/* Applies GPU completion-ring records and signals async waiters whose
 * syncpoint threshold was reached. Called every scheduler slice. */
void nvdrv_poll_completions(Nvdrv_State *state, HLE_Context *context);

#endif /* SWITCH_HLE_SERVICES_NVDRV_NVDRV_H */
