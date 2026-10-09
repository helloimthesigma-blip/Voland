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
 * Scope, stated: GPFIFO submissions on /dev/nvhost-gpu are run by the
 * CPU-side command processor (gpu/gpu_channel.h, v3.40): host methods
 * and the DMA copy engine execute; 3D/compute/2D methods are counted and
 * ignored until the GPU worker's engines land. Every submission - GPFIFO
 * and NVDEC/VIC alike - completes at once, so fences a title waits on
 * are already reached ("black frames that complete", §25) and syncpoint
 * waits never block. The GPU
 * completion ring (gpu/syncpoint.h) is drained every scheduler slice, so
 * once the GPU worker posts real completions they flow through the same
 * path, and EVENT_WAIT_ASYNC waiters are signalled from it.
 */
#ifndef SWITCH_HLE_SERVICES_NVDRV_NVDRV_H
#define SWITCH_HLE_SERVICES_NVDRV_NVDRV_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu/gpu_channel.h"
#include "gpu/gpu_thread.h"
#include "gpu/syncpoint.h"
#include "hle/kernel/event.h"
#include "hle/services/nvdrv/nvdec.h"
#include "video/video_stream.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define NVDRV_MAX_FDS 64u
/* Big titles allocate thousands of buffers (Super Smash Bros. Ultimate maps
 * each twice, a plain and a compressible-kind view). */
#define NVMAP_MAX_HANDLES 8192u
#define NVDRV_MAX_GPU_MAPPINGS 16384u
#define NVDRV_MAX_EVENTS 64u
#define NVDRV_GPU_LATENCY_RING 256u /* queued submissions tracked for nvdrv_bound_gpu_latency */
#define NVDRV_MAX_CHANNELS 8u     /* GPU channels with command processing */
#define NVDRV_NO_CHANNEL UINT32_MAX
#define NVDRV_IOCTL_MAX_BYTES 0x4000u /* the 14-bit size field */
#define NVDRV_MAX_CMDBUF_WORDS 0x4000u /* a host1x command buffer read at once (nvdec.h) */

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

/* GPU page table (GPU VA -> mapping slot): 64 KiB granules, two levels.
 * An L1 entry covers 64 MiB and names an L2 table from a fixed pool; an
 * L2 entry is a slot + 1, 0 (none) or NVDRV_PT_MIXED (several mappings
 * share the granule: translation scans). */
#define NVDRV_GRANULE_SHIFT 16u
#define NVDRV_PT_L2_BITS 10u
#define NVDRV_PT_VA_BITS 40u
#define NVDRV_PT_L1_ENTRIES (1u << (NVDRV_PT_VA_BITS - NVDRV_GRANULE_SHIFT - NVDRV_PT_L2_BITS))
#define NVDRV_PT_L2_ENTRIES (1u << NVDRV_PT_L2_BITS)
#define NVDRV_PT_L2_TABLES 512u /* 32 GiB of GPU VA with entries; beyond, translation scans */
#define NVDRV_PT_MIXED 0xFFFFu

typedef struct Nv_Fd {
  Nv_Device device;
  uint32_t syncpoint;   /* channels: their syncpoint, allocated on open/gpfifo */
  uint32_t nvmap_fd;
  uint32_t submissions; /* diagnostics */
  uint32_t channel;     /* nvhost-gpu: index into Nvdrv_State.channels, or NVDRV_NO_CHANNEL */
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
  bool registered;      /* EVENT_REGISTER: EVENT_WAIT may pick it */
  bool waiting;         /* EVENT_WAIT(_ASYNC) armed it on a syncpoint threshold */
  uint32_t syncpoint;
  uint32_t threshold;
} Nv_Event_Slot;

typedef struct Nvdrv_State {
  Service_Interface interface; /* service_state points back here */
  Nv_Fd fds[NVDRV_MAX_FDS];
  Nvmap_Handle handles[NVMAP_MAX_HANDLES]; /* nvmap handle n is handles[n - 1] */
  Gpu_Mapping mappings[NVDRV_MAX_GPU_MAPPINGS];
  uint32_t mapping_end;    /* one past the highest slot ever used: scans stop here */
  uint32_t last_mapping;   /* the slot the last translation hit (checked first) */
  /* Then the GPU page table: compute and vertex fetch alternate between
   * buffers, and a scan of every mapping costs thousands of compares. */
  uint16_t pt_l1[NVDRV_PT_L1_ENTRIES];                      /* L2 table + 1, 0 = none */
  uint16_t pt_l2[NVDRV_PT_L2_TABLES][NVDRV_PT_L2_ENTRIES];  /* slot + 1, 0, or NVDRV_PT_MIXED */
  uint32_t pt_l2_used;
  uint64_t next_gpu_va;
  Syncpoints syncpoints;
  Nv_Event_Slot events[NVDRV_MAX_EVENTS];
  uint8_t ioctl_buffer[NVDRV_IOCTL_MAX_BYTES];
  uint32_t ioctl_in_bytes; /* the bytes of ioctl_buffer this ioctl's in-data fills */
  uint8_t extra_buffer[NVDRV_IOCTL_MAX_BYTES];
  bool extra_out;          /* the current ioctl has Ioctl3's second output buffer */
  uint64_t ioctl_count;
  /* GPU command processing (gpu/gpu_channel.h): channel state lives in
   * caller-owned memory (NVDRV_MAX_CHANNELS of them); `hle` is the
   * context of the ioctl being run, for guest memory access. */
  Gpu_Channel *channels;
  bool channel_used[NVDRV_MAX_CHANNELS];
  HLE_Context *hle;
  Raster3d *renderer; /* 3D reference renderer for submissions (NULL: none) */
  /* Runs submissions and everything else that touches the renderer
   * (gpu_thread.h); never NULL once the emulator is set up. */
  Gpu_Thread *gpu_thread;
  /* Host1x multimedia engines (nvdec.h): buffer IOVAs and register files. */
  Video_Stream *video; /* decode requests and frames (NULL: decoding off) */
  Mm_Iova iova;
  Mm_Engine nvdec;
  Mm_Engine vic;
  Mm_Video mm_video;
  uint32_t cmdbuf[NVDRV_MAX_CMDBUF_WORDS];
  /* Async GPU: when each queued submission was made (virtual ticks) and
   * the GPU thread's call count after it (nvdrv_bound_gpu_latency). */
  uint64_t submit_ticks[NVDRV_GPU_LATENCY_RING];
  uint32_t submit_calls[NVDRV_GPU_LATENCY_RING];
  uint32_t submit_head, submit_tail;
  uint64_t stalls_for_latency; /* waits nvdrv_bound_gpu_latency made */
  uint64_t captured_submissions, captured_bytes, uncaptured_submissions; /* async: commands carried / run in place */
} Nvdrv_State;

/* Resets the state and initializes `state->interface`. `channels`:
 * NVDRV_MAX_CHANNELS Gpu_Channels the caller owns (NULL: submissions
 * complete without running their commands, the Phase 3 behavior). */
void nvdrv_init(Nvdrv_State *state, Gpu_Channel *channels);

/* GPU VA -> guest VA through the address space's mappings: the guest
 * address and how many bytes from there stay inside the mapping. */
bool nvdrv_gpu_translate(const Nvdrv_State *state, uint64_t gpu_va, uint64_t *guest_va, uint64_t *contiguous);

/* Registers the four service names with sm:. */
Error nvdrv_register(Nvdrv_State *state, SM_Registry *registry);

/* The guest VA and size behind an nvmap id (ids are handles here), for
 * the display compositor. False if the id is unknown or unallocated. */
bool nvdrv_nvmap_lookup(const Nvdrv_State *state, uint32_t id, uint64_t *address, uint64_t *size);

/* Async GPU (gpu_thread.h): a submission still queued `max_ticks` of
 * virtual time after it was made is waited for, in host time. A real GPU
 * keeps up with the CPU; a GPU thread that does not lets virtual time run
 * on while the guest waits for a fence, and the guest's timeouts expire
 * (MK8DX's presentation thread then deadlocks with its main thread).
 * Called every slice; resets in synchronous mode. */
void nvdrv_bound_gpu_latency(Nvdrv_State *state, uint64_t now_ticks, uint64_t max_ticks);

/* Applies GPU completion-ring records and signals async waiters whose
 * syncpoint threshold was reached. Called every scheduler slice. */
void nvdrv_poll_completions(Nvdrv_State *state, HLE_Context *context);

#endif /* SWITCH_HLE_SERVICES_NVDRV_NVDRV_H */
