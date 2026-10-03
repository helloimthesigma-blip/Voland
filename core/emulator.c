#include "emulator.h"

#include "common/arena.h"
#include "common/assert.h"
#include "audio/audio_ring.h"
#include "common/log.h"
#include "hle/loader/exefs.h"
#include "hle/loader/nca_parse.h"
#include "hle/loader/npdm.h"
#include "hle/loader/nro.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/thread.h"

#include <stdio.h>
#include <string.h>

#define EMULATOR_HOMEBREW_PRIORITY 44u
#define EMULATOR_HOMEBREW_STACK_BYTES 0x100000u
#define EMULATOR_HOMEBREW_ARENA_BYTES ((size_t)1024 * 1024)

/* Service state that lives and dies with a process. Registration with
 * sm: happens once (emulator_create); the interfaces' addresses never
 * change, so re-initializing in place keeps the registry valid. */
/* Locates the program's RomFS in `emulator->content`: an NRO's asset
 * section ("ASET" header right after the NRO image) or the program NCA's
 * RomFS section. Leaves emulator->romfs NULL when there is none. */
#define NRO_SIZE_OFFSET 0x18u
#define NRO_ASSET_HEADER_BYTES 0x38u
#define NRO_ASSET_ROMFS_OFFSET 0x28u

static void open_compressed_romfs(Emulator* emulator, uint32_t index);

static void release_content_arena(Emulator* emulator) {
  if (!emulator->content_arena_live) return;
  arena_destroy(&emulator->content_arena);
  emulator->content_arena_live = false;
  memset(&emulator->romfs_compressed, 0, sizeof(emulator->romfs_compressed));
}

static void locate_romfs(Emulator* emulator, bool is_nro) {
  emulator->romfs = NULL;
  release_content_arena(emulator);
  const Byte_Source* content = &emulator->content;
  if (is_nro) {
    uint8_t word[4];
    if (!error_is_ok(byte_source_read(content, NRO_SIZE_OFFSET, word, sizeof(word)))) return;
    const uint64_t asset = byte_source_le32(word);
    uint8_t header[NRO_ASSET_HEADER_BYTES];
    if (asset + sizeof(header) > content->size ||
        !error_is_ok(byte_source_read(content, asset, header, sizeof(header))) || memcmp(header, "ASET", 4) != 0) {
      return;
    }
    const uint64_t offset = byte_source_le64(header + NRO_ASSET_ROMFS_OFFSET);
    const uint64_t size = byte_source_le64(header + NRO_ASSET_ROMFS_OFFSET + 8);
    if (size && error_is_ok(byte_source_slice(content, asset + offset, size, &emulator->romfs_slice))) {
      emulator->romfs = &emulator->romfs_slice.source;
    }
    return;
  }
  if (!error_is_ok(nca_open(content, &emulator->content_nca))) return;
  const int index = nca_find_section(&emulator->content_nca, NCA_FS_ROMFS);
  if (index < 0) {
    log_info("[emulator] the program NCA has no RomFS section");
    return;
  }
  if (emulator->content_nca.header.sections[index].has_compression_info) {
    open_compressed_romfs(emulator, (uint32_t)index);
    return;
  }
  const Error probe = nca_probe_section(&emulator->content_nca, (uint32_t)index);
  if (!error_is_ok(probe)) {
    log_warn("[emulator] RomFS section %d unusable: %s", index, probe.message ? probe.message : "?");
    return;
  }
  emulator->romfs = nca_section_source(&emulator->content_nca, (uint32_t)index);
  log_info("[emulator] RomFS: section %d, %llu bytes", index,
           emulator->romfs ? (unsigned long long)emulator->romfs->size : 0ull);
}

/* A compressed RomFS section: the decompressing view, its data region
 * sliced out. Leaves emulator->romfs NULL on any failure (logged). */
static void open_compressed_romfs(Emulator* emulator, uint32_t index) {
  const NCA_Section_Info* section = &emulator->content_nca.header.sections[index];
  Error err = nca_section_raw(&emulator->content_nca, index, &emulator->romfs_raw);
  uint32_t entries = 0, max_block = 0;
  if (error_is_ok(err))
    err = nca_compressed_measure(&emulator->romfs_raw.source, section->compression_table_offset,
                                 section->compression_table_size, section->compression_bucket_header, &entries,
                                 &max_block);
  if (error_is_ok(err) && !arena_create(&emulator->content_arena, nca_compressed_arena_bytes(entries, max_block)))
    err = ERR(RESULT_OUT_OF_MEMORY, "no memory for the compression table");
  if (error_is_ok(err)) {
    emulator->content_arena_live = true;
    err = nca_compressed_open(&emulator->romfs_compressed, &emulator->romfs_raw.source,
                              section->compression_table_offset, section->compression_table_size,
                              section->compression_bucket_header, &emulator->content_arena);
  }
  if (error_is_ok(err))
    err = byte_source_slice(&emulator->romfs_compressed.source, 0, emulator->romfs_compressed.virtual_size,
                            &emulator->romfs_slice);
  if (!error_is_ok(err)) {
    log_warn("[emulator] compressed RomFS section %u unusable: %s", index, err.message ? err.message : "?");
    return;
  }
  emulator->romfs = &emulator->romfs_slice.source;
  log_info("[emulator] RomFS: compressed section %u, %u blocks (largest %u bytes), %llu bytes", index, entries,
           max_block, (unsigned long long)emulator->romfs_compressed.virtual_size);
}

/* ------------------------------------------------------------------ */
/* Homebrew ABI (switchbrew "Homebrew ABI"; libnx runtime/env.c).      */
/* ------------------------------------------------------------------ */

#define ENV_PAGES 3u
#define ENV_ENTRY_BYTES 24u
#define ENV_MAX_ENTRIES 16u
#define ENV_ARGV_OFFSET 0x200u
#define ENV_ARGV_BYTES 0x400u
#define ENV_INFO_OFFSET 0x600u
#define ENV_NEXT_PATH_OFFSET 0x1000u
#define ENV_NEXT_PATH_BYTES 0x200u
#define ENV_NEXT_ARGV_OFFSET 0x1400u
#define ENV_STUB_OFFSET 0x2000u
#define ENV_KEY_END 0u
#define ENV_KEY_MAIN_THREAD_HANDLE 1u
#define ENV_KEY_NEXT_LOAD_PATH 2u
#define ENV_KEY_ARGV 5u
#define ENV_KEY_SYSCALL_HINT 6u
#define ENV_KEY_APPLET_TYPE 7u
#define ENV_KEY_LAST_LOAD_RESULT 11u
#define ENV_KEY_RANDOM_SEED 14u
#define ENV_KEY_HOS_VERSION 16u
#define ENV_KEY_SYSCALL_HINT2 17u
#define ENV_FLAG_MANDATORY 1u
#define ENV_APPLET_TYPE_APPLICATION 0u
#define ENV_HOS_VERSION ((17u << 16) | (0u << 8) | 0u)
#define ENV_SEED_0 0x566F6C616E64ull /* "Voland" */
#define ENV_SEED_1 0x686F6D6562726577ull
#define ENV_SVC_EXIT_PROCESS 0xD40000E1u /* svc #0x7 */
#define ENV_BRANCH_SELF 0x14000000u      /* b . */
#define SDMC_PREFIX "sdmc:"
#define LOADER_INFO "Voland homebrew loader\n"

typedef struct Env_Writer {
  uint8_t page[2u * VMM_PAGE_SIZE];
  uint32_t count;
} Env_Writer;

static void env_entry(Env_Writer* w, uint32_t key, uint32_t flags, uint64_t v0, uint64_t v1) {
  if (w->count >= ENV_MAX_ENTRIES) return;
  uint8_t* e = w->page + w->count++ * ENV_ENTRY_BYTES;
  memcpy(e, &key, 4);
  memcpy(e + 4, &flags, 4);
  memcpy(e + 8, &v0, 8);
  memcpy(e + 16, &v1, 8);
}

/* First run of `pages` unmapped pages in the stack region, past the main
 * stack and a guard page. */
static uint64_t find_env_base(const Emulator* emulator, uint64_t pages) {
  const Address_Region* stack = &emulator->process.address_space.stack;
  const uint64_t bytes = pages * VMM_PAGE_SIZE;
  uint64_t at = emulator->process.main_thread_stack.base + emulator->process.main_thread_stack.size + VMM_PAGE_SIZE;
  if (!address_region_contains(stack, at, bytes)) at = stack->base;
  for (; at + bytes <= stack->base + stack->size; at += VMM_PAGE_SIZE) {
    VMM_Region_Info info;
    if (error_is_ok(vmm_query(emulator->vmm, at, &info)) && !info.is_mapped && info.base_gva + info.size >= at + bytes) {
      return at;
    }
  }
  return 0;
}

static Error setup_homebrew_env(Emulator* emulator) {
  Process* p = &emulator->process;
  const uint64_t base = find_env_base(emulator, ENV_PAGES);
  if (!base) return ERR(RESULT_OUT_OF_MEMORY, "homebrew env: no room in the stack region");
  uint64_t pa = 0;
  Error err = page_allocator_allocate(&emulator->pages, ENV_PAGES, &pa);
  if (!error_is_ok(err)) return err;
  (void)vmm_fill_physical(emulator->vmm, pa, 0, ENV_PAGES * VMM_PAGE_SIZE);
  err = vmm_map(emulator->vmm, base, pa, 2u * VMM_PAGE_SIZE, VMM_PERM_RW);
  if (error_is_ok(err)) {
    err = vmm_map(emulator->vmm, base + ENV_STUB_OFFSET, pa + ENV_STUB_OFFSET, VMM_PAGE_SIZE, VMM_PERM_RX);
    if (!error_is_ok(err)) (void)vmm_unmap(emulator->vmm, base, 2u * VMM_PAGE_SIZE);
  }
  if (!error_is_ok(err)) return err;
  p->loader_env = (Address_Region){base, ENV_PAGES * VMM_PAGE_SIZE};

  static Env_Writer w;
  memset(&w, 0, sizeof(w));
  const uint64_t all = UINT64_MAX;
  env_entry(&w, ENV_KEY_MAIN_THREAD_HANDLE, ENV_FLAG_MANDATORY, p->main_thread_handle, 0);
  env_entry(&w, ENV_KEY_NEXT_LOAD_PATH, 0, base + ENV_NEXT_PATH_OFFSET, base + ENV_NEXT_ARGV_OFFSET);
  env_entry(&w, ENV_KEY_ARGV, 0, 0, base + ENV_ARGV_OFFSET);
  env_entry(&w, ENV_KEY_SYSCALL_HINT, 0, all, all);
  env_entry(&w, ENV_KEY_SYSCALL_HINT2, 0, all, 0);
  env_entry(&w, ENV_KEY_APPLET_TYPE, 0, ENV_APPLET_TYPE_APPLICATION, 0);
  env_entry(&w, ENV_KEY_LAST_LOAD_RESULT, 0, 0, 0);
  env_entry(&w, ENV_KEY_RANDOM_SEED, 0, ENV_SEED_0, ENV_SEED_1);
  env_entry(&w, ENV_KEY_HOS_VERSION, 0, ENV_HOS_VERSION, 0);
  env_entry(&w, ENV_KEY_END, 0, base + ENV_INFO_OFFSET, sizeof(LOADER_INFO) - 1u);
  /* argv: what the loader was asked to pass, else the program's own path. */
  char* argv = (char*)w.page + ENV_ARGV_OFFSET;
  if (emulator->next_argv[0]) snprintf(argv, ENV_ARGV_BYTES, "%s", emulator->next_argv);
  else snprintf(argv, ENV_ARGV_BYTES, SDMC_PREFIX "%s", emulator->program_path);
  memcpy(w.page + ENV_INFO_OFFSET, LOADER_INFO, sizeof(LOADER_INFO) - 1u);
  (void)vmm_write_physical(emulator->vmm, pa, w.page, sizeof(w.page));
  const uint32_t stub[2] = {ENV_SVC_EXIT_PROCESS, ENV_BRANCH_SELF};
  (void)vmm_write_physical(emulator->vmm, pa + ENV_STUB_OFFSET, stub, sizeof(stub));

  /* X0 = the entry list, X1 = -1 (libnx crt0: a context with any other
   * X1 is an exception entry), LR = the "return to loader" stub. */
  emulator->cpu_backend->set_reg(emulator->cpu_state, CPU_REG_X0, base);
  emulator->cpu_backend->set_reg(emulator->cpu_state, CPU_REG_X0 + 1u, UINT64_MAX);
  emulator->cpu_backend->set_reg(emulator->cpu_state, CPU_REG_X30, base + ENV_STUB_OFFSET);
  return OK;
}

/* Copies the loaded NRO onto the SD card at program_path (libnx reads its
 * RomFS through argv[0]). Skipped when it already came from there. */
static void copy_program_to_sd(Emulator* emulator) {
  if (!emulator->ramfs_ready || emulator->content_node != RAMFS_NO_NODE) return;
  Ramfs_Pool* pool = &emulator->ramfs;
  const uint32_t root = emulator->fs.sd_root;
  if (!error_is_ok(emulator_sd_card_write_file(emulator, emulator->program_path, NULL, 0))) return;
  uint32_t node = 0;
  if (ramfs_lookup(pool, root, emulator->program_path, &node) != 0) return;
  for (uint64_t at = 0; at < emulator->content.size;) {
    const uint64_t n = emulator->content.size - at < FS_BOUNCE_BYTES ? emulator->content.size - at : FS_BOUNCE_BYTES;
    if (!error_is_ok(byte_source_read(&emulator->content, at, emulator->fs.bounce, n)) ||
        ramfs_write(pool, node, at, emulator->fs.bounce, n) != 0) {
      (void)ramfs_delete_file(pool, root, emulator->program_path);
      return;
    }
    at += n;
  }
}

static Error ramfs_source_read(void* user, uint64_t offset, void* out, uint64_t size) {
  Emulator* emulator = (Emulator*)user;
  uint64_t read = 0;
  if (ramfs_read(&emulator->ramfs, emulator->content_node, offset, out, size, &read) != 0 || read != size) {
    return ERR(RESULT_IO_ERROR, "sd card read failed");
  }
  return OK;
}

/* hbloader's loop: after an NRO exits, load what it asked for, or go
 * back to the first NRO. True if a program was (re)loaded. */
static bool chain_load(Emulator* emulator) {
  char next[ENV_NEXT_PATH_BYTES];
  memset(next, 0, sizeof(next));
  memset(emulator->next_argv, 0, sizeof(emulator->next_argv));
  const uint64_t env = emulator->process.loader_env.base;
  if (env) {
    (void)vmm_read_block(emulator->vmm, env + ENV_NEXT_PATH_OFFSET, next, sizeof(next) - 1u);
    (void)vmm_read_block(emulator->vmm, env + ENV_NEXT_ARGV_OFFSET, emulator->next_argv,
                         sizeof(emulator->next_argv) - 1u);
  }
  char target[FS_MAX_PATH_BYTES];
  if (next[0]) {
    const char* path = strncmp(next, SDMC_PREFIX, strlen(SDMC_PREFIX)) == 0 ? next + strlen(SDMC_PREFIX) : next;
    snprintf(target, sizeof(target), "%s", path);
  } else if (strcmp(emulator->program_path, emulator->home_path) != 0) {
    snprintf(target, sizeof(target), "%s", emulator->home_path);
    memset(emulator->next_argv, 0, sizeof(emulator->next_argv));
  } else {
    return false;
  }
  log_info("[emulator] homebrew chain-load: sdmc:%s", target);
  char home[FS_MAX_PATH_BYTES];
  snprintf(home, sizeof(home), "%s", emulator->home_path);
  char argv[EMULATOR_ARGV_BYTES];
  memcpy(argv, emulator->next_argv, sizeof(argv));
  emulator_unload_program(emulator);
  uint32_t node = 0;
  if (!emulator->ramfs_ready || ramfs_lookup(&emulator->ramfs, emulator->fs.sd_root, target, &node) != 0 ||
      emulator->ramfs.nodes[node].is_dir) {
    log_warn("[emulator] chain-load target sdmc:%s not found", target);
    return false;
  }
  emulator->content_node = node;
  snprintf(emulator->program_path, sizeof(emulator->program_path), "%s", target);
  memcpy(emulator->next_argv, argv, sizeof(argv));
  const Byte_Source source = {emulator, emulator->ramfs.nodes[node].size, ramfs_source_read};
  const Error err = emulator_load_nro(emulator, &source, 0);
  snprintf(emulator->home_path, sizeof(emulator->home_path), "%s", home);
  if (!error_is_ok(err)) {
    log_warn("[emulator] chain-load of sdmc:%s failed: %s", target, err.message ? err.message : "");
    return false;
  }
  return true;
}

static void reset_process_services(Emulator* emulator) {
  event_pool_init(&emulator->events);
  nvdrv_init(&emulator->nvdrv, emulator->gpu_channels); /* fds, nvmap handles, syncpoints die with the process */
  emulator->nvdrv.renderer = emulator->renderer.ready ? &emulator->renderer : NULL;
  shared_memory_pool_init(&emulator->shared_memory, &emulator->pages);
  memset(&emulator->transfer_memory, 0, sizeof(emulator->transfer_memory));
  hid_init(&emulator->hid, &emulator->shared_memory);
  am_init(&emulator->am, emulator->am_storage_pool);
  apm_init(&emulator->apm);
  set_init(&emulator->set);
  time_init(&emulator->time, &emulator->shared_memory, emulator->rtc);
  fs_reset_process(&emulator->fs, NULL);
  vi_init(&emulator->vi, &emulator->nvdrv, emulator->vi_scratch);
  emulator->vi.frame_skip = emulator->frame_skip;
  network_init(&emulator->network);
  misc_init(&emulator->misc);
  audout_init(&emulator->audout);
  if (emulator->audren) audren_init(emulator->audren);
  acc_init(&emulator->acc);
  ns_init(&emulator->ns);
  pl_init(&emulator->pl, &emulator->shared_memory, emulator->shared_font, emulator->shared_font_size);
}

static Error register_services(Emulator* emulator) {
  Error err = nvdrv_register(&emulator->nvdrv, &emulator->sm);
  if (error_is_ok(err)) err = hid_register(&emulator->hid, &emulator->sm);
  if (error_is_ok(err)) err = am_register(&emulator->am, &emulator->sm);
  if (error_is_ok(err)) err = apm_register(&emulator->apm, &emulator->sm);
  if (error_is_ok(err)) err = set_register(&emulator->set, &emulator->sm);
  if (error_is_ok(err)) err = time_register(&emulator->time, &emulator->sm);
  if (error_is_ok(err)) err = fs_register(&emulator->fs, &emulator->sm);
  if (error_is_ok(err)) err = vi_register(&emulator->vi, &emulator->sm);
  if (error_is_ok(err)) err = network_register(&emulator->network, &emulator->sm);
  if (error_is_ok(err)) err = pl_register(&emulator->pl, &emulator->sm);
  if (error_is_ok(err)) err = misc_register(&emulator->misc, &emulator->sm);
  if (error_is_ok(err)) err = audout_register(&emulator->audout, &emulator->sm);
  if (error_is_ok(err) && emulator->audren) err = audren_register(emulator->audren, &emulator->sm);
  if (error_is_ok(err)) err = acc_register(&emulator->acc, &emulator->sm);
  if (error_is_ok(err)) err = ns_register(&emulator->ns, &emulator->sm);
  return err;
}

Error emulator_create(Emulator* out) {
  return emulator_create_with_backend(out, cpu_get_active_backend());
}

Error emulator_create_with_backend(Emulator* out, const CPU_Backend* backend) {
  if (!out || !backend) {
    return ERR(RESULT_INVALID_ARGUMENT, "emulator_create: out is NULL");
  }

  memset(out, 0, sizeof(*out));

  /* 1. Linear memory layout (§4): every region reserved in one pass. */
  Error layout_err = layout_create();
  if (!error_is_ok(layout_err)) {
    return layout_err;
  }

  /* 2. Softmmu (§5): page tables over the layout's L1 region. Shared by
   * every CPU_State and by HLE. */
  out->vmm = vmm_create();
  if (!out->vmm) {
    layout_destroy();
    memset(out, 0, sizeof(*out));
    return ERR(RESULT_OUT_OF_MEMORY, "emulator_create: vmm_create failed");
  }

  /* 3. CPU backend selected at configure time; it receives the MMU, never
   * raw guest RAM (§8). */
  out->cpu_backend = backend;
  SWITCH_ASSERT_ALWAYS(out->cpu_backend != NULL, "no CPU backend registered");

  out->cpu_state = out->cpu_backend->create(out->vmm, &out->hle);
  if (!out->cpu_state) {
    vmm_destroy(out->vmm);
    layout_destroy();
    memset(out, 0, sizeof(*out));
    return ERR(RESULT_OUT_OF_MEMORY, "emulator_create: CPU backend failed to init");
  }

  /* 4. Guest physical pages (§4): all of guest RAM, handed out by the
   * bootstrap and later the memory HLE. */
  const Error pages_err = page_allocator_init_guest_ram(&out->pages);
  SWITCH_ASSERT_ALWAYS(error_is_ok(pages_err), "page allocator over guest RAM failed");

  /* 5. HLE context + svc/undefined hooks. `&out->process` is valid
   * before emulator_load_program() populates it - same pattern as
   * `out->cpu_state` above being handed `&out->hle` before
   * hle_context_init() has even run (see hle.h's doc comment). */
  /* IPC kernel state and the sm: registry (§12). The registry is filled
   * once here as services land; sessions die with each process. */
  ipc_session_pool_init(&out->sessions);
  sm_registry_init(&out->sm);
  hle_context_init(&out->hle, out->cpu_backend, out->vmm, &out->process, &out->pages,
                   &out->sessions, &out->sm);
  scheduler_init(&out->scheduler, out->cpu_backend);
  out->hle.scheduler = &out->scheduler;
  out->hle.events = &out->events;
  out->rtc = TIME_DEFAULT_RTC;
  audio_ring_reset();
  out->content_node = RAMFS_NO_NODE;
  snprintf(out->program_path, sizeof(out->program_path), "%s", EMULATOR_DEFAULT_NRO_PATH);
  if (arena_create(&out->service_arena, EMULATOR_SERVICE_ARENA_BYTES)) {
    out->am_storage_pool = ARENA_ALLOC_ARRAY(&out->service_arena, uint8_t, (size_t)AM_STORAGE_POOL_BYTES);
    out->vi_scratch = ARENA_ALLOC_ARRAY(&out->service_arena, uint8_t, (size_t)VI_SCRATCH_BYTES);
    out->gpu_channels = ARENA_ALLOC_ARRAY(&out->service_arena, Gpu_Channel, NVDRV_MAX_CHANNELS);
    out->audren = ARENA_ALLOC(&out->service_arena, Audren_State);
  }
  if (arena_create(&out->renderer_arena, raster3d_storage_bytes() + 64u)) {
    uint8_t *storage = (uint8_t *)arena_allocate(&out->renderer_arena, raster3d_storage_bytes(), 64u);
    raster3d_init(&out->renderer, storage, raster3d_storage_bytes());
  }
  if (!out->renderer.ready) log_warn("[emulator] no memory for the 3D renderer; draws will be skipped");
  out->ramfs_ready = ramfs_pool_init(&out->ramfs, EMULATOR_RAMFS_BYTES);
  if (!out->ramfs_ready) log_warn("[emulator] no RAM for the SD card / saves; fsp-srv filesystems will be full");
  fs_init(&out->fs, out->ramfs_ready ? &out->ramfs : NULL);
  out->hle.shared_memory = &out->shared_memory;
  out->hle.transfer_memory = &out->transfer_memory;
  /* Services (§12) register with sm: once; their state resets per process. */
  reset_process_services(out);
  const Error service_err = register_services(out);
  SWITCH_ASSERT_ALWAYS(error_is_ok(service_err), "services register into an empty sm: registry");
  out->cpu_backend->set_svc_handler(out->cpu_state, hle_on_svc);
  out->cpu_backend->set_undefined_handler(out->cpu_state, hle_on_undefined);

  log_info("[emulator] created (backend=%s %s)",
           out->cpu_backend->name,
           out->cpu_backend->version);

  return OK;
}

void emulator_destroy(Emulator* emulator) {
  if (!emulator) return;
  emulator_unload_program(emulator);
  if (emulator->cpu_backend && emulator->cpu_state) {
    emulator->cpu_backend->destroy(emulator->cpu_state);
  }
  if (emulator->ramfs_ready) ramfs_pool_destroy(&emulator->ramfs);
  arena_destroy(&emulator->service_arena);
  raster3d_shutdown(&emulator->renderer);
  arena_destroy(&emulator->renderer_arena);
  vmm_destroy(emulator->vmm);
  layout_destroy();
  memset(emulator, 0, sizeof(*emulator));
}

/* Shared tail of every load: arm the main thread (Horizon entry ABI) and
 * adopt it into the scheduler. On failure the process is torn down. */
static Error finish_load(Emulator* emulator, const Byte_Source* source, bool is_nro) {
  Error err = OK;
  err = process_enter_main_thread(&emulator->process, emulator->cpu_backend, emulator->cpu_state);
  if (!error_is_ok(err)) {
    process_teardown(&emulator->process, emulator->vmm, &emulator->pages);
    page_allocator_reset(&emulator->pages);
    return err;
  }
  /* The main thread joins the scheduler: it wraps the Emulator's own
   * CPU_State, and its bootstrap handle (0x8000) now names it. */
  scheduler_init(&emulator->scheduler, emulator->cpu_backend);
  Sched_Thread *main_thread = scheduler_new_thread(&emulator->scheduler);
  SWITCH_ASSERT_ALWAYS(main_thread != NULL, "empty scheduler has a slot");
  main_thread->thread.cpu_state = emulator->cpu_state;
  main_thread->thread.tls_gva = emulator->process.main_thread_tls_gva;
  main_thread->thread.entry_point = emulator->process.entry_point;
  main_thread->thread.stack_top = emulator->process.main_thread_stack.base + emulator->process.main_thread_stack.size;
  main_thread->thread.priority = emulator->process.npdm.main_thread_priority;
  main_thread->thread.preferred_core = emulator->process.npdm.main_thread_core_number;
  main_thread->core_mask = 1ull << main_thread->thread.preferred_core;
  main_thread->handle = emulator->process.main_thread_handle;
  main_thread->owns_cpu_state = false;
  main_thread->state = THREAD_STATE_RUNNABLE;
  err = handle_table_replace(&emulator->process.handles, main_thread->handle, KERNEL_OBJECT_THREAD, main_thread);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "main thread handle is live");

  emulator->content = *source;
  emulator->is_homebrew = is_nro;
  if (is_nro) {
    err = setup_homebrew_env(emulator);
    if (!error_is_ok(err)) {
      process_teardown(&emulator->process, emulator->vmm, &emulator->pages);
      page_allocator_reset(&emulator->pages);
      return err;
    }
    copy_program_to_sd(emulator);
    if (emulator->content_node == RAMFS_NO_NODE) {
      snprintf(emulator->home_path, sizeof(emulator->home_path), "%s", emulator->program_path);
    }
  }
  locate_romfs(emulator, is_nro);
  fs_reset_process(&emulator->fs, emulator->romfs);
  emulator->program_loaded = true;
  log_info("[emulator] program '%s' loaded; main thread armed at pc=0x%010llx",
           emulator->process.npdm.name, (unsigned long long)emulator->process.entry_point);
  return OK;
}
/* §12 "what load a game actually does", step 1 through 4, with the §1.6
 * encrypted-input diagnosis surfaced before any structural parse. */
Error emulator_load_program(Emulator* emulator, const Byte_Source* nca_source,
                            uint64_t aslr_seed) {
  if (!emulator || !nca_source) {
    return ERR(RESULT_INVALID_ARGUMENT, "emulator_load_program: NULL argument");
  }
  if (emulator->program_loaded) {
    return ERR(RESULT_INVALID_ARGUMENT,
               "emulator_load_program: a program is already loaded; unload it first");
  }

  /* Step 1: NCA -> ExeFS -> npdm. */
  NCA_File nca;
  Error err = nca_open(nca_source, &nca);
  if (!error_is_ok(err)) return err;
  if (nca.header.content_type != NCA_CONTENT_PROGRAM) {
    return ERR(RESULT_INVALID_ARGUMENT, "emulator_load_program: not a PROGRAM NCA");
  }
  const int exefs_index = nca_find_section(&nca, NCA_FS_PARTITION_FS);
  if (exefs_index < 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "emulator_load_program: PROGRAM NCA has no ExeFS");
  }
  err = nca_probe_section(&nca, (uint32_t)exefs_index);
  if (!error_is_ok(err)) return err;

  /* The loader arena lives for this call only: ExeFS directory, the npdm
   * bytes, then NSO staging. Not a hot path (§3). */
  Arena scratch;
  if (!arena_create(&scratch, EMULATOR_LOADER_ARENA_BYTES)) {
    return ERR(RESULT_OUT_OF_MEMORY, "emulator_load_program: loader arena");
  }

  ExeFS exefs;
  NPDM npdm;
  err = exefs_open(nca_section_source(&nca, (uint32_t)exefs_index), &scratch, &exefs);
  if (error_is_ok(err)) {
    const ExeFS_Entry* npdm_entry = exefs_find(&exefs, EXEFS_FILE_NPDM);
    if (!npdm_entry) {
      err = ERR(RESULT_NOT_FOUND, "emulator_load_program: ExeFS has no main.npdm");
    } else if (npdm_entry->size > NPDM_MAX_FILE_BYTES) {
      err = ERR(RESULT_INVALID_ARGUMENT, "emulator_load_program: main.npdm over size cap");
    } else {
      uint8_t* npdm_bytes = ARENA_ALLOC_ARRAY(&scratch, uint8_t, (size_t)npdm_entry->size);
      if (!npdm_bytes) {
        err = ERR(RESULT_OUT_OF_MEMORY, "emulator_load_program: loader arena exhausted");
      } else {
        err = exefs_read(&exefs, npdm_entry, 0, npdm_bytes, npdm_entry->size);
        if (error_is_ok(err)) err = npdm_parse(npdm_bytes, npdm_entry->size, &npdm);
      }
    }
  }

  /* Steps 2-4: the process. */
  if (error_is_ok(err)) {
    const Process_Bootstrap_Params params = {
        .exefs = &exefs,
        .npdm = &npdm,
        .vmm = emulator->vmm,
        .pages = &emulator->pages,
        .scratch = &scratch,
        .aslr_seed = aslr_seed,
    };
    err = process_bootstrap(&params, &emulator->process);
    if (!error_is_ok(err)) {
      /* The bootstrap unmapped its own; the pages it consumed come back
       * with the allocator (nothing else has allocated yet). */
      page_allocator_reset(&emulator->pages);
    }
  }
  arena_destroy(&scratch);
  if (!error_is_ok(err)) return err;

  return finish_load(emulator, nca_source, false);
}

Error emulator_load_nro(Emulator* emulator, const Byte_Source* nro_source, uint64_t aslr_seed) {
  if (!emulator || !nro_source) return ERR(RESULT_INVALID_ARGUMENT, "emulator_load_nro: NULL argument");
  if (emulator->program_loaded) {
    return ERR(RESULT_INVALID_ARGUMENT, "emulator_load_nro: a program is already loaded; unload it first");
  }
  NSO image;
  Error err = nro_open(nro_source, &image);
  if (!error_is_ok(err)) return err;

  /* Homebrew has no main.npdm: the defaults hbloader's own npdm uses. */
  NPDM npdm;
  memset(&npdm, 0, sizeof(npdm));
  npdm.is_64bit_instruction = true;
  npdm.address_space = NPDM_ADDRESS_SPACE_64_BIT_39;
  npdm.main_thread_priority = EMULATOR_HOMEBREW_PRIORITY;
  npdm.main_thread_core_number = 0;
  npdm.main_thread_stack_size = EMULATOR_HOMEBREW_STACK_BYTES;
  memcpy(npdm.name, "homebrew", sizeof("homebrew"));
  npdm.program_id = EMULATOR_HOMEBREW_PROGRAM_ID;

  Arena scratch;
  if (!arena_create(&scratch, EMULATOR_HOMEBREW_ARENA_BYTES)) {
    return ERR(RESULT_OUT_OF_MEMORY, "emulator_load_nro: loader arena");
  }
  const Process_Bootstrap_Params params = {
      .exefs = NULL,
      .single_module = &image,
      .npdm = &npdm,
      .vmm = emulator->vmm,
      .pages = &emulator->pages,
      .scratch = &scratch,
      .aslr_seed = aslr_seed,
  };
  err = process_bootstrap(&params, &emulator->process);
  arena_destroy(&scratch);
  if (!error_is_ok(err)) {
    page_allocator_reset(&emulator->pages);
    return err;
  }
  return finish_load(emulator, nro_source, true);
}

Error emulator_load(Emulator* emulator, const Byte_Source* source, uint64_t aslr_seed) {
  if (!emulator || !source) return ERR(RESULT_INVALID_ARGUMENT, "emulator_load: NULL argument");
  uint8_t magic[4] = {0};
  if (source->size >= NRO_HEADER_END && error_is_ok(byte_source_read(source, NRO_HEADER_OFFSET, magic, sizeof(magic))) &&
      memcmp(magic, "NRO0", sizeof(magic)) == 0) {
    return emulator_load_nro(emulator, source, aslr_seed);
  }
  return emulator_load_program(emulator, source, aslr_seed);
}

void emulator_unload_program(Emulator* emulator) {
  if (!emulator || !emulator->program_loaded) return;
  const Thread_Env env = {emulator->cpu_backend, emulator->vmm, &emulator->process.tls, &emulator->hle};
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread* t = &emulator->scheduler.threads[i];
    if (t->state != THREAD_STATE_FREE && t->owns_cpu_state) thread_destroy(&env, &t->thread);
  }
  scheduler_init(&emulator->scheduler, emulator->cpu_backend);
  /* Transfer memory still lent at exit: the heap gets its pages back
   * read-write, so teardown can release them. */
  for (uint32_t i = 0; i < TRANSFER_MEMORY_POOL_CAPACITY; i++) {
    const Kernel_Transfer_Memory* tmem = &emulator->transfer_memory.objects[i];
    if (tmem->references) (void)vmm_reprotect(emulator->vmm, tmem->address, tmem->size, VMM_PERM_RW);
  }
  process_teardown(&emulator->process, emulator->vmm, &emulator->pages);
  page_allocator_reset(&emulator->pages);
  reset_process_services(emulator); /* shared-memory pages went with the reset */
  ipc_session_pool_init(&emulator->sessions); /* every session belonged to the process */
  emulator->romfs = NULL;
  release_content_arena(emulator);
  memset(&emulator->content, 0, sizeof(emulator->content));
  emulator->content_node = RAMFS_NO_NODE;
  emulator->is_homebrew = false;
  emulator->program_loaded = false;
}

Emulator_Status emulator_run_slice(Emulator* emulator, uint64_t cycle_budget) {
  SWITCH_ASSERT_ALWAYS(emulator != NULL, "emulator_run_slice: emulator is NULL");
  if (!emulator->program_loaded) return EMULATOR_NOT_LOADED;
  CPU_ExitReason reason = CPU_EXIT_CYCLES_ELAPSED;
  /* GPU completions (§13) arrive at scheduler-tick cadence. */
  nvdrv_poll_completions(&emulator->nvdrv, &emulator->hle);
  /* Controllers (§18): the input region into hid's shared memory. */
  const Memory_Layout* layout = layout_get();
  hid_update(&emulator->hid, &emulator->hle, layout ? (const void*)(uintptr_t)layout->input_region_base : NULL,
             emulator->scheduler.ticks);
  /* Display (§13): vsync composites queued buffers into the §6 slots. */
  vi_update(&emulator->vi, &emulator->hle, emulator->scheduler.ticks);
  /* Audio (§14): queued PCM / rendered frames into the ring at 48kHz of
   * virtual time. */
  audout_update(&emulator->audout, &emulator->hle, emulator->scheduler.ticks);
  if (emulator->audren) audren_update(emulator->audren, &emulator->hle, emulator->scheduler.ticks);
  {
    const uint64_t vsync = vi_next_wake(&emulator->vi);
    const uint64_t audio = emulator->audren ? audren_next_wake(emulator->audren) : UINT64_MAX;
    emulator->scheduler.device_wake_at = audio < vsync ? audio : vsync;
  }
  switch (scheduler_tick(&emulator->scheduler, emulator->cpu_backend, cycle_budget, &reason)) {
  case SCHEDULER_RAN: return EMULATOR_RUNNING;
  case SCHEDULER_IDLE: return EMULATOR_IDLE;
  case SCHEDULER_EXITED: return emulator->is_homebrew && chain_load(emulator) ? EMULATOR_RUNNING : EMULATOR_EXITED;
  case SCHEDULER_CRASHED: return EMULATOR_CRASHED;
  default: return EMULATOR_DEADLOCK;
  }
}

CPU_ExitReason emulator_run(Emulator* emulator, uint64_t cycle_budget) {
  SWITCH_ASSERT_ALWAYS(emulator != NULL, "emulator_run: emulator is NULL");
  if (!emulator->program_loaded) return emulator->cpu_backend->run(emulator->cpu_state, cycle_budget);
  CPU_ExitReason reason = CPU_EXIT_HALT;
  if (scheduler_tick(&emulator->scheduler, emulator->cpu_backend, cycle_budget, &reason) != SCHEDULER_RAN) {
    return CPU_EXIT_HALT;
  }
  return reason;
}

static Error sd_result(uint32_t rc, const char *what) {
  return rc ? ERR(rc == FS_RESULT_USABLE_SPACE_NOT_ENOUGH ? RESULT_OUT_OF_MEMORY : RESULT_INVALID_ARGUMENT, what) : OK;
}

/* Host paths (§15): "/..." is the SD card; "save:SS:<128 hex>/..." is
 * the save with space SS and that SaveDataAttribute (created when
 * `create`). `*rest` is the path inside the chosen root. */
#define SAVE_PATH_PREFIX "save:"
#define SAVE_PATH_PREFIX_BYTES 5u
#define SAVE_PATH_HEADER_BYTES (SAVE_PATH_PREFIX_BYTES + 3u + 2u * FS_SAVE_ATTRIBUTE_BYTES) /* "save:SS:" + key */

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parse_hex_bytes(const char* text, uint8_t* out, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const int hi = hex_digit(text[2u * i]), lo = hex_digit(text[2u * i + 1u]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static uint32_t host_root(Emulator* emulator, const char* path, bool create, uint32_t* root, const char** rest) {
  if (strncmp(path, SAVE_PATH_PREFIX, SAVE_PATH_PREFIX_BYTES) != 0) {
    *root = emulator->fs.sd_root;
    *rest = path;
    return 0;
  }
  uint8_t space = 0;
  uint8_t key[FS_SAVE_ATTRIBUTE_BYTES];
  const char* p = path + SAVE_PATH_PREFIX_BYTES;
  if (strlen(path) < SAVE_PATH_HEADER_BYTES || !parse_hex_bytes(p, &space, 1) || p[2] != ':' ||
      !parse_hex_bytes(p + 3, key, sizeof(key)) || (path[SAVE_PATH_HEADER_BYTES] != '/' && path[SAVE_PATH_HEADER_BYTES]))
    return FS_RESULT_PATH_NOT_FOUND;
  *rest = path[SAVE_PATH_HEADER_BYTES] ? path + SAVE_PATH_HEADER_BYTES : "/";
  return fs_save_root(&emulator->fs, space, key, create, root);
}

/* mkdir -p: every prefix ending at a '/', then the whole path. */
static uint32_t create_directories(Emulator* emulator, uint32_t root, const char* path) {
  char partial[FS_MAX_PATH_BYTES];
  const size_t length = strlen(path);
  if (length >= sizeof(partial)) return FS_RESULT_PATH_NOT_FOUND;
  for (size_t i = 1; i <= length; i++) {
    if (i < length && path[i] != '/') continue;
    memcpy(partial, path, i);
    partial[i] = '\0';
    const uint32_t rc = ramfs_create_directory(&emulator->ramfs, root, partial);
    if (rc && rc != FS_RESULT_PATH_ALREADY_EXISTS) return rc;
  }
  return 0;
}

Error emulator_sd_card_create_directory(Emulator* emulator, const char* path) {
  if (!emulator || !path || !emulator->ramfs_ready) return ERR(RESULT_INVALID_ARGUMENT, "sd card unavailable");
  uint32_t root = 0;
  const char* rest = NULL;
  uint32_t rc = host_root(emulator, path, true, &root, &rest);
  if (!rc && strcmp(rest, "/") != 0) rc = create_directories(emulator, root, rest);
  return rc ? sd_result(rc, "sd card: create directory failed") : OK;
}

Error emulator_sd_card_clear(Emulator* emulator) {
  if (!emulator || !emulator->ramfs_ready) return ERR(RESULT_INVALID_ARGUMENT, "sd card unavailable");
  return sd_result(ramfs_delete_directory(&emulator->ramfs, emulator->fs.sd_root, "/", true, true),
                   "sd card: clear failed (a file is open)");
}

Error emulator_sd_card_write_file(Emulator* emulator, const char* path, const void* data, uint64_t size) {
  if (!emulator || !path || !emulator->ramfs_ready) return ERR(RESULT_INVALID_ARGUMENT, "sd card unavailable");
  uint32_t root = 0;
  const char* rest = NULL;
  uint32_t rc = host_root(emulator, path, true, &root, &rest);
  if (rc) return sd_result(rc, "sd card: bad save path");
  const char* slash = strrchr(rest, '/');
  if (slash && slash != rest) {
    char parent[FS_MAX_PATH_BYTES];
    const size_t length = (size_t)(slash - rest);
    if (length >= sizeof(parent)) return ERR(RESULT_INVALID_ARGUMENT, "sd card path too long");
    memcpy(parent, rest, length);
    parent[length] = '\0';
    rc = create_directories(emulator, root, parent);
    if (rc) return sd_result(rc, "sd card: create directory failed");
  }
  (void)ramfs_delete_file(&emulator->ramfs, root, rest);
  rc = ramfs_create_file(&emulator->ramfs, root, rest, 0);
  uint32_t node = 0;
  if (!rc) rc = ramfs_lookup(&emulator->ramfs, root, rest, &node);
  if (!rc) rc = ramfs_write(&emulator->ramfs, node, 0, data, size);
  return sd_result(rc, "sd card: write failed");
}

bool emulator_text_request(const Emulator* emulator, Am_Text_Request* out) {
  if (!emulator) return false;
  const Am_Text_Request* t = am_text_request(&emulator->am);
  if (!t) return false;
  if (out) *out = *t;
  return true;
}

void emulator_text_respond(Emulator* emulator, const char* utf8, bool accepted) {
  if (emulator) am_text_respond(&emulator->am, &emulator->hle, utf8, accepted);
}

uint64_t emulator_sd_card_generation(const Emulator* emulator) {
  return emulator && emulator->ramfs_ready ? emulator->ramfs.generation : 0;
}

#define SD_MANIFEST_DEPTH 64u

/* Appends one manifest line per file under `root`, paths prefixed with
 * `prefix`; `skip` (a full path) is left out. */
static uint64_t manifest_tree(const Ramfs_Pool* pool, uint32_t root, const char* prefix, const char* skip, char* out,
                              uint64_t max, uint64_t need) {
  /* Depth-first with an explicit stack of directories. */
  uint32_t stack[SD_MANIFEST_DEPTH];
  size_t prefix_length[SD_MANIFEST_DEPTH];
  char path[FS_MAX_PATH_BYTES + SAVE_PATH_HEADER_BYTES];
  const size_t start = strlen(prefix);
  if (start >= sizeof(path)) return need;
  memcpy(path, prefix, start + 1u);
  uint32_t depth = 0;
  stack[depth] = pool->nodes[root].first_child;
  prefix_length[depth] = start;
  while (true) {
    const uint32_t node = stack[depth];
    if (node == RAMFS_NO_NODE) {
      if (depth == 0) break;
      depth--;
      path[prefix_length[depth]] = '\0';
      stack[depth] = pool->nodes[stack[depth]].next_sibling;
      continue;
    }
    const Ramfs_Node* n = &pool->nodes[node];
    const size_t base = prefix_length[depth];
    const size_t name_length = strlen(n->name);
    if (base + 1u + name_length >= sizeof(path)) {
      stack[depth] = n->next_sibling;
      continue;
    }
    path[base] = '/';
    memcpy(path + base + 1u, n->name, name_length + 1u);
    if (n->is_dir) {
      if (depth + 1u < SD_MANIFEST_DEPTH) {
        depth++;
        stack[depth] = n->first_child;
        prefix_length[depth] = base + 1u + name_length;
        continue;
      }
    } else if (!skip || strcmp(path, skip) != 0) {
      char line[sizeof(path) + 48];
      const int length = snprintf(line, sizeof(line), "%u %llu %s\n", n->version, (unsigned long long)n->size, path);
      if (length > 0) {
        if (out && need + (uint64_t)length <= max) memcpy(out + need, line, (size_t)length);
        need += (uint64_t)length;
      }
    }
    path[base] = '\0';
    stack[depth] = n->next_sibling;
  }
  return need;
}

uint64_t emulator_sd_card_manifest(const Emulator* emulator, char* out, uint64_t max) {
  if (!emulator || !emulator->ramfs_ready) return 0;
  uint64_t need = manifest_tree(&emulator->ramfs, emulator->fs.sd_root, "", emulator->program_path, out, max, 0);
  for (uint32_t i = 0; i < FS_MAX_SAVES; i++) {
    const Fs_Save* save = &emulator->fs.saves[i];
    if (!save->used) continue;
    char prefix[SAVE_PATH_HEADER_BYTES + 1u];
    int at = snprintf(prefix, sizeof(prefix), SAVE_PATH_PREFIX "%02x:", save->space);
    for (uint32_t b = 0; b < FS_SAVE_ATTRIBUTE_BYTES && at > 0; b++)
      at += snprintf(prefix + at, sizeof(prefix) - (size_t)at, "%02x", save->key[b]);
    need = manifest_tree(&emulator->ramfs, save->root, prefix, NULL, out, max, need);
  }
  return need;
}

int64_t emulator_sd_card_read_file(Emulator* emulator, const char* path, void* out, uint64_t max) {
  if (!emulator || !path || !emulator->ramfs_ready) return -1;
  uint32_t root = 0, node = 0;
  const char* rest = NULL;
  if (host_root(emulator, path, false, &root, &rest) != 0) return -1;
  if (ramfs_lookup(&emulator->ramfs, root, rest, &node) != 0 || emulator->ramfs.nodes[node].is_dir) return -1;
  const uint64_t size = emulator->ramfs.nodes[node].size;
  uint64_t read = 0;
  if (out && max) (void)ramfs_read(&emulator->ramfs, node, 0, out, size < max ? size : max, &read);
  return (int64_t)size;
}

void emulator_set_program_path(Emulator* emulator, const char* sd_path) {
  if (!emulator || !sd_path || !sd_path[0]) return;
  snprintf(emulator->program_path, sizeof(emulator->program_path), "%s%s", sd_path[0] == '/' ? "" : "/", sd_path);
}

void emulator_set_shared_font(Emulator* emulator, const uint8_t* ttf, uint32_t size) {
  if (!emulator) return;
  emulator->shared_font = ttf;
  emulator->shared_font_size = ttf ? size : 0;
  if (!emulator->program_loaded) {
    pl_init(&emulator->pl, &emulator->shared_memory, emulator->shared_font, emulator->shared_font_size);
  }
}

/* The ring is full: wait for the GPU worker to consume (bounded, so a
 * stalled consumer shows up as slowness rather than a hang in one wait). */
#define GPU_STREAM_WAIT_NS 20000000ll
static void gpu_stream_wait(void *user, volatile int32_t *word, int32_t expected) {
  (void)user;
#ifdef __EMSCRIPTEN__
  (void)__builtin_wasm_memory_atomic_wait32((int32_t *)word, expected, GPU_STREAM_WAIT_NS);
#else
  (void)word;
  (void)expected;
#endif
}

void emulator_set_gpu_mode(Emulator *emulator, bool on) {
  if (!emulator) return;
  if (on && !emulator->gpu_stream_ready) {
    const Memory_Layout *layout = layout_get();
    uint8_t *header = (uint8_t *)(uintptr_t)layout->gpu_ring_base;
    gpu_stream_init(&emulator->gpu_stream, header, header + GPU_STREAM_RING_OFFSET,
                    LAYOUT_GPU_RING_SIZE - GPU_STREAM_RING_OFFSET, gpu_stream_wait, NULL);
    emulator->gpu_stream_ready = true;
  }
  raster3d_set_gpu(&emulator->renderer, on ? &emulator->gpu_stream : NULL);
}

void emulator_set_frame_skip(Emulator* emulator, uint32_t n) {
  if (!emulator) return;
  emulator->frame_skip = n > EMULATOR_MAX_FRAME_SKIP ? EMULATOR_MAX_FRAME_SKIP : n;
  emulator->vi.frame_skip = emulator->frame_skip;
  if (!emulator->frame_skip) emulator->renderer.skip_draws = false;
}

void emulator_set_rtc(Emulator* emulator, int64_t unix_seconds) {
  if (!emulator) return;
  emulator->rtc = unix_seconds;
  if (!emulator->program_loaded) emulator->time.rtc_at_boot = unix_seconds;
}

void emulator_set_debug_output(Emulator* emulator, HLE_Debug_Output_Fn fn, void* userdata) {
  if (!emulator) return;
  emulator->hle.debug_output = fn;
  emulator->hle.debug_userdata = userdata;
}

CPU_ExitReason emulator_step(Emulator* emulator) {
  SWITCH_ASSERT_ALWAYS(emulator != NULL, "emulator_step: emulator is NULL");
  return emulator->cpu_backend->step(emulator->cpu_state);
}

const char* emulator_backend_name(const Emulator* emulator) {
  if (!emulator || !emulator->cpu_backend) return "<none>";
  return emulator->cpu_backend->name;
}
