/**
 * Top-level emulator wiring. The core entry point for every platform.
 * Phase 0 scope: create/destroy, wire the no-op CPU backend into the stub
 * HLE dispatcher, expose the bounded run/step contract.
 */
#ifndef SWITCH_EMULATOR_H
#define SWITCH_EMULATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"
#include "common/result.h"
#include "common/vmm.h"
#include "cpu/cpu.h"
#include "gpu/gpu_stream.h"
#include "video/video_stream.h"
#include "hle/hle.h"
#include "hle/kernel/page_allocator.h"
#include "hle/kernel/process.h"
#include "hle/kernel/event.h"
#include "hle/kernel/scheduler.h"
#include "hle/kernel/shared_memory.h"
#include "hle/kernel/transfer_memory.h"
#include "hle/services/am/am.h"
#include "hle/services/acc/acc.h"
#include "hle/services/apm/apm.h"
#include "hle/services/audio/audout.h"
#include "hle/services/audio/audren.h"
#include "hle/services/fs/fs.h"
#include "hle/services/hid/hid.h"
#include "hle/services/network/network.h"
#include "hle/services/pl/pl.h"
#include "hle/services/misc/misc.h"
#include "hle/services/social/social.h"
#include "hle/services/audio/hwopus.h"
#include "hle/services/set/set.h"
#include "hle/services/time/time.h"
#include "hle/services/vi/vi.h"
#include "hle/services/nvdrv/nvdrv.h"
#include "hle/services/ns/ns.h"
#include "hle/loader/byte_source.h"
#include "hle/loader/nca_parse.h"
#include "hle/loader/nca_compressed.h"

/* Scratch for one bootstrap: the ExeFS directory plus the largest
 * compressed NSO segment staged for LZ4. A shipping title's biggest
 * `main` .text compresses to ~50-60MB; created for the duration of
 * emulator_load_program() only, then freed. */
#define EMULATOR_LOADER_ARENA_BYTES ((size_t)96 * 1024 * 1024)
#define EMULATOR_RAMFS_BYTES ((uint64_t)128 * 1024 * 1024) /* SD card + saves */
#define EMULATOR_SERVICE_ARENA_BYTES ((size_t)20 * 1024 * 1024)
#define EMULATOR_ARGV_BYTES 0x800u
#define EMULATOR_DEFAULT_NRO_PATH "/switch/homebrew.nro"

typedef struct Emulator
{
  /* Softmmu (§5). Created after the layout and before the CPU backend,
   * which receives it at CPU_State creation; shared by every CPU_State
   * and by HLE. */
  VMM_Context *vmm;
  const CPU_Backend *cpu_backend;
  /* One CPU_State stands in for "the" guest thread until the Phase 2
   * scheduler (§7) exists to multiplex real ones. */
  CPU_State *cpu_state;
  HLE_Context hle;

  /* Guest physical pages (§4) and the loaded process (§12). `process`
   * is valid only while `program_loaded` is true. */
  Page_Allocator pages;
  Process process;
  bool program_loaded;

  /* IPC kernel state and the sm: registry (§12), reachable from HLE
   * through hle.sessions / hle.sm. Reset by
   * emulator_unload_program (all sessions die with the process); the
   * registry is filled once at emulator_create and survives reloads. */
  IPC_Session_Pool sessions;
  SM_Registry sm;

  /* Guest threads (§7). The main thread wraps `cpu_state`; CreateThread
   * threads own their own states. Reset by emulator_unload_program. */
  Scheduler scheduler;
  Event_Pool events; /* kernel events (event.h); reset with the process */
  Nvdrv_State nvdrv; /* the nvdrv service (§13); reset with the process */
  Shared_Memory_Pool shared_memory; /* shared_memory.h; reset with the process */
  Transfer_Memory_Pool transfer_memory;
  Hid_State hid;     /* the hid service (§18); reset with the process */
  Am_State am;       /* appletOE/appletAE (§12); reset with the process */
  Apm_State apm;
  Set_State set;
  Time_State time;
  Network_State network; /* offline bsd/nifm */
  Pl_State pl;           /* shared font */
  Misc_State misc;       /* psm, ts */
  Social_State social;
  Hwopus_State hwopus;   /* Opus decoding (silent until a decoder lands) */   /* prepo, friend, bcat, caps, fatal, nfp: offline answers */
  Audout_State audout;   /* PCM audio out (§14) */
  Audren_State *audren;  /* the audio renderer (§14); service arena */
  Ns_State ns;           /* application records (§12) */
  Acc_State acc;         /* one local user */
  const uint8_t *shared_font;
  uint32_t shared_font_size;
  Vi_State vi;       /* display + BufferQueue (§13); reset with the process */
  Fs_State fs;       /* fsp-srv; open files reset with the process */
  Ramfs_Pool ramfs;  /* SD card + saves (§15); lives as long as the Emulator */
  Arena service_arena; /* large service buffers (applet storages, compositor scratch) */
  uint8_t *am_storage_pool;
  uint8_t *vi_scratch;
  Gpu_Channel *gpu_channels; /* NVDRV_MAX_CHANNELS */
  Arena renderer_arena;      /* the 3D reference renderer's surfaces, caches */
  Raster3d renderer;
  bool ramfs_ready;

  /* The loaded program's file (§12): kept readable until unload so the
   * RomFS can be served (fsp-srv OpenDataStorageByCurrentProcess). */
  Byte_Source content;
  NCA_File content_nca;
  Byte_Source_Slice romfs_slice;
  const Byte_Source *romfs; /* NULL: the program has no RomFS */
  /* A compressed RomFS section (nca_compressed.h): its raw bytes, the
   * decompressing view, and the arena holding its table and block cache
   * (created at load, destroyed at unload). */
  Byte_Source_Slice romfs_raw;
  NCA_Compressed romfs_compressed;
  Arena content_arena;
  bool content_arena_live;

  /* Homebrew (hbloader ABI, v3.39). An NRO runs as "sdmc:<program_path>"
   * (copied onto the SD card at load, since libnx reads its own RomFS
   * from there); when it exits having set a next-load path, that NRO is
   * loaded from the SD card, and when a chain-loaded app exits the first
   * NRO (the menu) comes back - hbloader's loop. */
  bool is_homebrew;
  char program_path[FS_MAX_PATH_BYTES];
  char home_path[FS_MAX_PATH_BYTES];
  char next_argv[EMULATOR_ARGV_BYTES];
  uint32_t content_node;    /* ramfs node of a chain-loaded NRO */
  int64_t rtc;       /* Unix seconds the next process boots at (emulator_set_rtc) */
  uint32_t frame_skip; /* emulator_set_frame_skip; survives process reloads */
  Parallel *parallel;  /* emulator_set_host_cores; NULL = serial */
  bool no_poll_coalescing; /* emulator_set_poll_coalescing(false); survives reloads */
  bool free_running;   /* emulator_set_free_running; with host cores only */
  /* Wall-clock pacing (emulator_set_pacing): virtual time never falls
   * behind wall time since the origin. */
  bool pacing;
  uint64_t pacing_origin_ns;    /* host ns at the origin; 0 = take it at the next slice */
  uint64_t pacing_origin_ticks; /* virtual ticks at the origin */
  uint64_t paced_ticks;         /* virtual time added by pacing (statistics) */
  /* GPU mode (emulator_set_gpu_mode): the renderer's stream to the GPU
   * worker, in the layout's gpu_ring region. */
  Gpu_Stream gpu_stream;
  bool gpu_stream_ready;
  /* Video decode (§13): NVDEC requests and decoded frames, in the
   * layout's video region (video/video_stream.h). Set up once. */
  Video_Stream video;
  bool video_ready;
} Emulator;

/* What one emulator_run_slice() did (§7 scheduler status). */
typedef enum Emulator_Status {
  EMULATOR_RUNNING,  /* a guest thread ran */
  EMULATOR_IDLE,     /* every thread sleeps; virtual time jumped ahead */
  EMULATOR_EXITED,   /* ExitProcess or every thread exited */
  EMULATOR_CRASHED,  /* svcBreak or an unhandled fault/undefined instruction */
  EMULATOR_DEADLOCK, /* every thread waits forever */
  EMULATOR_NOT_LOADED,
} Emulator_Status;

/* Reserves the linear memory layout (§4), creates the softmmu (§5), and
 * wires the active CPU backend (§8) to the stub HLE dispatcher. There is
 * no Emulator_Config: guest RAM size and every other region size are
 * fixed by common/layout.h, not caller-configurable - on web the single
 * WebAssembly.Memory is created by the boot sequence (§16) before the
 * core module is even instantiated. */
Error emulator_create(Emulator *out);

/* As emulator_create, with an explicit CPU backend instead of the
 * configure-time one (tests run the interpreter under every preset). */
Error emulator_create_with_backend(Emulator *out, const CPU_Backend *backend);
void emulator_destroy(Emulator *emulator);

/* "Load a game" (§12): parses the decrypted PROGRAM NCA in `nca` (§1.6:
 * pre-decrypted only; RESULT_ENCRYPTED_INPUT otherwise, message naming
 * docs/DUMP.md), bootstraps the process (process.h) and arms `cpu_state`
 * with the main thread's entry state. `aslr_seed` 0 disables ASLR. The
 * source (the struct is copied; what it reads from is not) must stay
 * readable until emulator_unload_program(): fsp-srv serves the program's
 * RomFS straight from it (v3.39). One program per
 * Emulator: a second call fails with RESULT_INVALID_ARGUMENT until
 * emulator_unload_program(). */
Error emulator_load_program(Emulator *emulator, const Byte_Source *nca, uint64_t aslr_seed);

/* "Load homebrew" (§12): an NRO file (nro.h) becomes the process's single
 * `main` module, with a synthesized npdm (39-bit address space, priority
 * 44, core 0, 1MB stack, program id EMULATOR_HOMEBREW_PROGRAM_ID). Entered
 * with the Horizon ABI (X0 = 0, X1 = main thread handle). */
#define EMULATOR_HOMEBREW_PROGRAM_ID 0x0500000000000001ull
Error emulator_load_nro(Emulator *emulator, const Byte_Source *nro, uint64_t aslr_seed);

/* Loads whichever executable `source` is, decided structurally: an "NRO0"
 * magic at 0x10 is homebrew (emulator_load_nro); anything else is treated
 * as a PROGRAM NCA (emulator_load_program), whose own checks report
 * encrypted or foreign input (§1.6). */
Error emulator_load(Emulator *emulator, const Byte_Source *source, uint64_t aslr_seed);

/* Unmaps the process and resets the page allocator. No-op if nothing is
 * loaded. */
void emulator_unload_program(Emulator *emulator);

/* One scheduler slice (§7): the highest-priority runnable guest thread
 * runs for at most `cycle_budget` cycles. */
Emulator_Status emulator_run_slice(Emulator *emulator, uint64_t cycle_budget);

/* Parallel guest threads (docs/PARALLEL.md): with `cores` >= 1, guest
 * threads run on that many host threads during emulator_run_slice and the
 * caller waits for the slice (serving host calls the cores need). 0 (the
 * default) is the serial scheduler. One core is bit-identical to serial;
 * from two on, exclusives become host compare-and-swaps and the run is
 * no longer deterministic. Returns the core count in effect (0 when the
 * build or the backend cannot run in parallel). Call between slices. */
uint32_t emulator_set_host_cores(Emulator *emulator, uint32_t cores);

/* Runs guest code for about `host_ms` of host time (or until the program
 * stops). In slice mode: slices of `cycle_budget` until the deadline. In
 * free-running mode (with host cores): the cores run without per-slice
 * barriers and devices update as they come due (docs/PARALLEL.md
 * "Free-running mode"). Guest code runs only inside the call either way. */
Emulator_Status emulator_run_for(Emulator *emulator, uint64_t host_ms, uint64_t cycle_budget);

/* Wall-clock pacing (off by default; the web turns it on): before each
 * slice (or free-running burst) virtual time that has fallen behind wall
 * time since the origin jumps forward to it, as an idle jump does, and
 * due timers and device events fire at the new time. The guest sees large
 * frame deltas and runs at real speed with fewer frames when the host is
 * slower than the Switch. A jump is capped at EMULATOR_PACING_MAX_JUMP_MS;
 * further behind (a stall: a hidden tab, GC), the origin resyncs instead
 * of skipping the gap. Virtual time stays monotonic. */
#define EMULATOR_PACING_MAX_JUMP_MS 100u
void emulator_set_pacing(Emulator *emulator, bool on);
/* Restarts the pacing clock at the current virtual time (run start, resume
 * after a pause: paused time does not count). */
void emulator_pacing_resync(Emulator *emulator);

/* Free-running mode (prototype, off by default): emulator_run_for lets the
 * host cores run continuously. Needs emulator_set_host_cores >= 1. */
void emulator_set_free_running(Emulator *emulator, bool on);

/* Poll coalescing (scheduler.h, docs/PARALLEL.md "Polling threads"): on
 * by default. Off runs the plain scheduler, bit for bit. */
void emulator_set_poll_coalescing(Emulator *emulator, bool on);

/* Compatibility wrapper: one slice, reported as the backend's exit reason
 * (CPU_EXIT_HALT when no thread ran). With nothing loaded it runs the
 * bare CPU_State, as before the scheduler existed. */
CPU_ExitReason emulator_run(Emulator *emulator, uint64_t cycle_budget);

/* The wall-clock time (Unix seconds) the next loaded program sees at
 * boot; its clocks then advance with virtual time. Default
 * TIME_DEFAULT_RTC, so headless runs are deterministic. */
void emulator_set_rtc(Emulator *emulator, int64_t unix_seconds);

/* Frame skip (a speed setting, 0 = off): of every n + 1 frames the guest
 * presents, only one is rasterised and shown; game logic, audio and every
 * other GPU command run in full. Faster where rendering dominates, at the
 * cost of smoothness - and a one-time render landing in a skipped frame is
 * missing until the game draws it again. Capped at EMULATOR_MAX_FRAME_SKIP. */
#define EMULATOR_MAX_FRAME_SKIP 5u
void emulator_set_frame_skip(Emulator *emulator, uint32_t n);

/* GPU mode (§13): draws, clears, copies and presents of GPU-rendered
 * frames stream to the GPU worker's WebGPU renderer through the layout's
 * gpu_ring region (gpu/gpu_stream.h) instead of being rasterised here.
 * The platform turns it on once a consumer exists (the producer waits for
 * room in the ring). Off: the software reference renderer. */
void emulator_set_gpu_mode(Emulator *emulator, bool on);
/* Host nanoseconds the core has waited for the GPU or video worker to
 * drain a full stream ring (web only; 0 natively). */
uint64_t emulator_stream_wait_ns(void);

/* A synchronous video decoder for NVDEC (native platforms); without one
 * the requests go to the video region's ring (the web's video worker). */
void emulator_set_video_backend(Emulator *emulator, const Video_Backend *backend);

/* Seeds the emulated SD card (§15): creates `path` (absolute, '/'-
 * separated; missing parent directories are created) holding `size`
 * bytes, replacing an existing file. For platform importers (the CLI's
 * --sdmc, the web shell's drop target).
 *
 * Host paths: these calls, the manifest and emulator_sd_card_read_file
 * also address guest save data as "save:SS:<128 hex digits>/path" - the
 * save's space id and its 0x40-byte SaveDataAttribute - so a platform
 * persists and restores saves exactly like SD files. Writing creates the
 * save; reading never does. */
Error emulator_sd_card_write_file(Emulator *emulator, const char *path, const void *data, uint64_t size);
Error emulator_sd_card_create_directory(Emulator *emulator, const char *path);
/* Empties the SD card (every file and directory). */
Error emulator_sd_card_clear(Emulator *emulator);

/* Guest-write mirroring (§15): a counter that changes whenever any
 * emulated filesystem changes, and a manifest of the SD card's and every
 * save's files - one "version size path\n" line each, saves under their
 * "save:" host paths (the loaded program's own copy is left out) - so a
 * host can persist what changed. Returns the bytes the
 * manifest needs; it is written only if that fits in `max`. */
uint64_t emulator_sd_card_generation(const Emulator *emulator);
uint64_t emulator_sd_card_manifest(const Emulator *emulator, char *out, uint64_t max);
/* Software keyboard (§12 library applets): true while a title waits for
 * text; `out` gets the request. emulator_text_respond answers it
 * (`accepted` false = cancelled). */
bool emulator_text_request(const Emulator *emulator, Am_Text_Request *out);
void emulator_text_respond(Emulator *emulator, const char *utf8, bool accepted);

/* Reads up to `max` bytes of an SD file; returns its size, or -1. */
int64_t emulator_sd_card_read_file(Emulator *emulator, const char *path, void *out, uint64_t max);

/* Committed saves (§15, hle/fs/save_archive.h): the guest's IFileSystem::
 * Commit snapshots a save as one archive, "commit:/SS-<attribute hex>"
 * (read with emulator_sd_card_read_file). The host persists those - one
 * file per save, atomically - rather than the live tree. */
uint64_t emulator_save_commits(const Emulator *emulator);
uint64_t emulator_save_committed_manifest(const Emulator *emulator, char *out, uint64_t max);
/* Replaces save `name` ("SS-<attribute hex>") with an archive's contents. */
Error emulator_save_restore_archive(Emulator *emulator, const char *name, const void *bytes, uint64_t size);

/* Where the next NRO loaded from the host appears on the SD card (and so
 * its argv[0], "sdmc:<path>"); default EMULATOR_DEFAULT_NRO_PATH. Platforms
 * pass "/" + the file's name. */
void emulator_set_program_path(Emulator *emulator, const char *sd_path);

/* The font pl:u serves as every system font (§1.6: never Nintendo's).
 * The bytes are the caller's and must outlive the Emulator; takes effect
 * at the next program load. */
void emulator_set_shared_font(Emulator *emulator, const uint8_t *ttf, uint32_t size);

/* Routes svcOutputDebugString text to the platform. */
void emulator_set_debug_output(Emulator *emulator, HLE_Debug_Output_Fn fn, void *userdata);

/* ------------------------------------------------------------------ */
/* Save states: the whole machine, frozen and resumed (DESIGN.md §15).  */
/*                                                                      */
/* The core names the linear-memory ranges that hold the machine (the   */
/* Emulator, page tables, the used guest RAM, service/ramfs/content     */
/* arenas, every thread's registers) and the host copies them out or    */
/* in. They hold absolute linear-memory addresses (PTEs, arena          */
/* pointers), so a state restores only into the same layout: the same  */
/* build and session layout, which the plan's ranges identify - a host  */
/* compares a stored plan's addresses and capacities with the current   */
/* one and refuses a mismatch. Caches (JIT, predecode, decoded          */
/* textures, GPU render targets) are not saved; they are revalidated.   */
/* ------------------------------------------------------------------ */

#define SAVESTATE_VERSION 1u
#define SAVESTATE_MAX_RANGES 12u

typedef enum Savestate_Kind {
  SAVESTATE_RANGE_EMULATOR = 1,
  SAVESTATE_RANGE_VMM = 2,
  SAVESTATE_RANGE_VMM_L2 = 3,
  SAVESTATE_RANGE_PAGE_TABLE_L1 = 4,
  SAVESTATE_RANGE_GUEST_RAM = 5,
  SAVESTATE_RANGE_SERVICE_ARENA = 6,
  SAVESTATE_RANGE_RAMFS_ARENA = 7,
  SAVESTATE_RANGE_CONTENT_ARENA = 8,
  SAVESTATE_RANGE_REGISTERS = 9,
} Savestate_Kind;

typedef struct Savestate_Range {
  uint64_t address;  /* linear-memory offset */
  uint64_t bytes;    /* what holds state now (<= capacity) */
  uint64_t capacity; /* what may be written there on restore */
  uint32_t kind;     /* Savestate_Kind */
  uint32_t reserved;
} Savestate_Range;

typedef struct Savestate_Plan {
  uint32_t version;     /* SAVESTATE_VERSION */
  uint32_t range_count;
  uint64_t program_id;
  uint64_t emulator_bytes; /* sizeof(Emulator): a build check */
  uint64_t virtual_ticks;  /* the moment captured (display) */
  Savestate_Range ranges[SAVESTATE_MAX_RANGES];
} Savestate_Plan;

/* Quiesces the machine (guest threads back on the serial scheduler),
 * captures every thread's registers and fills `plan`; the host then
 * copies the ranges out and calls emulator_savestate_end_save, which
 * resumes the previous host-core setting. RESULT_INVALID_ARGUMENT
 * without a loaded program. */
Error emulator_savestate_begin_save(Emulator *emulator, Savestate_Plan *plan);
void emulator_savestate_end_save(Emulator *emulator);

/* Quiesces the machine and keeps what a restore must not replace (the
 * host side: renderer caches, GPU/video streams, host cores, pacing); fills
 * `plan` with the current ranges to compare with the stored one. The host
 * then writes the stored ranges (zero-filling what the state does not
 * hold) and calls emulator_savestate_finish_restore(emulator, true); or,
 * having written nothing, (emulator, false) to just resume. */
Error emulator_savestate_begin_restore(Emulator *emulator, Savestate_Plan *plan);
Error emulator_savestate_finish_restore(Emulator *emulator, bool applied);

/* Single-step. */
CPU_ExitReason emulator_step(Emulator *emulator);

/* Diagnostics. */
const char *emulator_backend_name(const Emulator *emulator);

#endif /* SWITCH_EMULATOR_H */
