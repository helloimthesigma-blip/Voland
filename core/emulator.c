#include "emulator.h"

#include "common/arena.h"
#include "common/assert.h"
#include "common/log.h"
#include "hle/loader/exefs.h"
#include "hle/loader/nca_parse.h"
#include "hle/loader/npdm.h"
#include "hle/loader/nro.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/thread.h"

#include <string.h>

#define EMULATOR_HOMEBREW_PRIORITY 44u
#define EMULATOR_HOMEBREW_STACK_BYTES 0x100000u
#define EMULATOR_HOMEBREW_ARENA_BYTES ((size_t)1024 * 1024)

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
  event_pool_init(&out->events);
  out->hle.events = &out->events;
  /* Services (§12) register with sm: once; their state resets per process. */
  nvdrv_init(&out->nvdrv);
  shared_memory_pool_init(&out->shared_memory, &out->pages);
  out->hle.shared_memory = &out->shared_memory;
  hid_init(&out->hid, &out->shared_memory);
  Error service_err = nvdrv_register(&out->nvdrv, &out->sm);
  if (error_is_ok(service_err)) service_err = hid_register(&out->hid, &out->sm);
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
  vmm_destroy(emulator->vmm);
  layout_destroy();
  memset(emulator, 0, sizeof(*emulator));
}

/* Shared tail of every load: arm the main thread (Horizon entry ABI) and
 * adopt it into the scheduler. On failure the process is torn down. */
static Error finish_load(Emulator* emulator) {
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

  return finish_load(emulator);
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
  return finish_load(emulator);
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
  event_pool_init(&emulator->events);
  nvdrv_init(&emulator->nvdrv); /* fds, nvmap handles and syncpoints die with the process */
  process_teardown(&emulator->process, emulator->vmm, &emulator->pages);
  page_allocator_reset(&emulator->pages);
  shared_memory_pool_init(&emulator->shared_memory, &emulator->pages); /* pages went with the reset */
  hid_init(&emulator->hid, &emulator->shared_memory);
  ipc_session_pool_init(&emulator->sessions); /* every session belonged to the process */
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
  switch (scheduler_tick(&emulator->scheduler, emulator->cpu_backend, cycle_budget, &reason)) {
  case SCHEDULER_RAN: return EMULATOR_RUNNING;
  case SCHEDULER_IDLE: return EMULATOR_IDLE;
  case SCHEDULER_EXITED: return EMULATOR_EXITED;
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
