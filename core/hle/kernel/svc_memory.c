/**
 * Memory SVCs. See svc_memory.h for the register ABI (verified against
 * libnx) and the two flagged simplifications (UnmapMemory exact-range-only,
 * QueryMemory's partial MemoryType/MemoryAttribute fidelity).
 */
#include "hle/kernel/svc_memory.h"

#include "common/assert.h"
#include "common/layout.h"
#include "common/log.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/shared_memory.h"
#include "hle/kernel/transfer_memory.h"
#include "hle/loader/nro.h"

#include <stddef.h>
#include <string.h>

static bool ranges_overlap(uint64_t a_base, uint64_t a_size, uint64_t b_base, uint64_t b_size) {
  return a_base < b_base + b_size && b_base < a_base + a_size;
}

/* True if `gva` falls inside the source or destination half of a
 * currently-live svcMapMemory borrow. Used by classify_type() to tell a
 * plain stack/heap page apart from one that is a borrow's other half -
 * something vmm's own perm bits cannot do alone (see below). */
static const Memory_Borrow *find_borrow_by_dst(const Process *process, uint64_t gva) {
  for (uint32_t i = 0; i < process->heap_borrow_count; i++) {
    const Memory_Borrow *b = &process->heap_borrows[i];
    const Address_Region dst_region = {b->dst_base, b->size};
    if (address_region_contains(&dst_region, gva, 1)) return b;
  }
  return NULL;
}

static bool borrow_overlaps_src(const Process *process, uint64_t base, uint64_t size) {
  for (uint32_t i = 0; i < process->heap_borrow_count; i++) {
    const Memory_Borrow *b = &process->heap_borrows[i];
    if (ranges_overlap(base, size, b->src_base, b->size)) return true;
  }
  return false;
}

/* NOTE ON PARTIAL FIDELITY (see svc_memory.h's top comment for the full
 * reasoning): Horizon's real MemoryType has ~30 values; the ones missing
 * here all describe kernel objects (transfer memory, IPC buffers, module
 * code, ...) that no SVC implemented before Phase 5+ can create, so no
 * Phase 1 title can construct a gva that would need one of them - this
 * is a closure argument over what is reachable right now, not a shortcut
 * with a known-broken case. The heap/stack split below is the one place
 * vmm's own state (is_mapped + perms) is NOT enough to answer correctly:
 * a MapMemory dst alias and a plain stack page are both mapped RW, and a
 * MapMemory'd-away heap source and a plain heap page would both read as
 * "mapped" if perms were VMM_PERM_NONE for some other reason (they are
 * not, today, but the borrow table is the honest source of truth rather
 * than perms as a proxy for it). */
static const Ro_Module *ro_module_at(const Process *process, uint64_t gva) {
  for (uint32_t i = 0; i < PROCESS_MAX_RO_MODULES; i++) {
    const Ro_Module *m = &process->ro_modules[i];
    if (m->base && gva >= m->base && gva - m->base < m->nro_size + m->bss_size) return m;
  }
  return NULL;
}

static uint32_t classify_type(const Process *process, uint64_t gva, bool is_mapped, uint32_t perms) {
  if (!is_mapped) return HLE_MEMTYPE_UNMAPPED;
  /* nn::ro modules: code static below .data, mutable from it. */
  const Ro_Module *ro = ro_module_at(process, gva);
  if (ro) return gva - ro->base >= ro->writable ? HLE_MEMTYPE_CODE_MUTABLE : HLE_MEMTYPE_CODE_STATIC;
  for (uint32_t i = 0; i < process->shared_mapping_count; i++) {
    const Address_Region view = {process->shared_mappings[i].base, process->shared_mappings[i].size};
    if (address_region_contains(&view, gva, 1)) return HLE_MEMTYPE_SHARED;
  }
  const Address_Space *as = &process->address_space;
  /* Modules: .text/.rodata CodeStatic, writable .data/.bss CodeMutable
   * (nn::ro's module walk checks exactly this pairing). A page borrowed
   * out by MapMemory (perm none) stays CodeMutable. */
  if (address_region_contains(&as->code, gva, 1)) {
    if ((perms & VMM_PERM_W) || (perms == VMM_PERM_NONE && borrow_overlaps_src(process, gva, 1)))
      return HLE_MEMTYPE_CODE_MUTABLE;
    return HLE_MEMTYPE_CODE_STATIC;
  }
  if (address_region_contains(&as->heap, gva, 1)) {
    return borrow_overlaps_src(process, gva, 1) ? HLE_MEMTYPE_WEIRD_MAPPED_MEM : HLE_MEMTYPE_HEAP;
  }
  if (address_region_contains(&as->stack, gva, 1)) {
    return find_borrow_by_dst(process, gva) ? HLE_MEMTYPE_MAPPED_MEMORY : HLE_MEMTYPE_NORMAL;
  }
  if (address_region_contains(&as->tls_io, gva, 1)) return HLE_MEMTYPE_THREAD_LOCAL;
  /* svcMapPhysicalMemory backing (KMemoryState Normal). */
  if (address_region_contains(&as->alias, gva, 1)) return HLE_MEMTYPE_NORMAL;
  return HLE_MEMTYPE_UNMAPPED;
}

static bool vmm_run_covers(const VMM_Region_Info *info, uint64_t gva, uint64_t size) {
  const Address_Region run = {info->base_gva, info->size};
  return address_region_contains(&run, gva, size);
}

/* One page's guest physical address. Its own borrow scope, not the
 * caller's: vmm's debug borrow cap (VMM_DEBUG_MAX_BORROWS=64, vmm.c)
 * exists to catch a handler that LEAKS borrows, not to size a loop that
 * calls this hundreds of times - scoping tightly here keeps at most one
 * borrow live at a time no matter how many pages the caller walks. */
static uint64_t page_pa_of(VMM_Context *vmm, uint64_t gva) {
  vmm_borrow_scope_begin(vmm);
  void *host_ptr = NULL;
  const Error got = vmm_guest_to_host(vmm, gva, VMM_PAGE_SIZE, VMM_PERM_R, &host_ptr);
  vmm_borrow_scope_end(vmm);
  if (!error_is_ok(got)) log_error("heap release: page 0x%llx unreadable: %s", (unsigned long long)gva, got.message);
  SWITCH_ASSERT_ALWAYS(error_is_ok(got), "heap release: heap page not readable");
  return (uint64_t)(uintptr_t)host_ptr - layout_get()->guest_ram_base;
}

static void free_run(Page_Allocator *pages, uint64_t pa, uint64_t page_count) {
  const Error freed = page_allocator_free(pages, pa, page_count);
  if (!error_is_ok(freed)) {
    /* Leaking the pages is survivable; stopping the game is not. */
    log_error("heap release: page_allocator_free(pa=0x%llx, %llu pages) failed: %s - pages leaked",
              (unsigned long long)pa, (unsigned long long)page_count, freed.message);
  }
}

/* Releases the physical pages backing [gva, gva + size), then unmaps the
 * whole range in one call. Every page must currently be mapped and
 * readable (both SetHeapSize's shrink path and process_teardown's heap
 * release only ever call this after confirming no live borrow overlaps
 * the range, so no page in it can be VMM_PERM_NONE); a violation is an
 * internal bug, not a bad guest argument, hence the asserts rather than
 * a Result.
 *
 * Fast path: try the whole range as ONE physical run first - the common
 * case, since a single SetHeapSize grow maps one page_allocator_allocate()
 * run in one vmm_map() call, so the range this releases is usually
 * exactly that one run. Only a range whose backing got fragmented across
 * a shrink-then-regrow (page_allocator.h's freelist) falls back to one
 * page at a time.
 *
 * Duplicated (not shared) between here and process.c's process_teardown,
 * which needs the identical logic: factoring a two-call-site helper out
 * into a new header felt like more ceremony than the ~20 lines of
 * duplication it would remove. Revisit if a third caller shows up. */
static void free_and_unmap_heap_range(VMM_Context *vmm, Page_Allocator *pages,
                                      uint64_t gva, uint64_t size) {
  vmm_borrow_scope_begin(vmm);
  void *host_ptr = NULL;
  const Error got = vmm_guest_to_host(vmm, gva, size, VMM_PERM_R, &host_ptr);
  vmm_borrow_scope_end(vmm);
  if (error_is_ok(got)) {
    free_run(pages, (uint64_t)(uintptr_t)host_ptr - layout_get()->guest_ram_base, size >> VMM_PAGE_BITS);
  } else {
    /* Fragmented backing: release maximal physically contiguous runs. */
    uint64_t run_pa = 0, run_pages = 0;
    for (uint64_t offset = 0; offset < size; offset += VMM_PAGE_SIZE) {
      const uint64_t pa = page_pa_of(vmm, gva + offset);
      if (run_pages && pa == run_pa + (run_pages << VMM_PAGE_BITS)) {
        run_pages++;
        continue;
      }
      if (run_pages) free_run(pages, run_pa, run_pages);
      run_pa = pa;
      run_pages = 1;
    }
    if (run_pages) free_run(pages, run_pa, run_pages);
  }
  const Error unmapped = vmm_unmap(vmm, gva, size);
  SWITCH_ASSERT_ALWAYS(error_is_ok(unmapped), "free_and_unmap_heap_range: vmm_unmap failed");
}

void hle_svc_set_heap_size(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  const Address_Region *heap = &process->address_space.heap;

  /* Verified ABI (svc_memory.h): the requested size arrives in X1, not
   * X0 - libnx's stub never lets the kernel see X0 for this SVC. */
  const uint64_t requested = regs->x[1];

  if (requested % PROCESS_HEAP_SIZE_GRANULE != 0) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (requested > heap->size) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }

  if (requested > process->heap_size) {
    const uint64_t grow_bytes = requested - process->heap_size;
    uint64_t pa = 0;
    Error err = page_allocator_allocate(context->pages, grow_bytes >> VMM_PAGE_BITS, &pa);
    if (!error_is_ok(err)) {
      regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
      return;
    }
    err = vmm_map(context->vmm, heap->base + process->heap_size, pa, grow_bytes, VMM_PERM_RW);
    if (!error_is_ok(err)) {
      /* vmm_map validates its whole range before mutating anything, so
       * nothing to unwind there; just give the pages back. */
      const Error freed = page_allocator_free(context->pages, pa, grow_bytes >> VMM_PAGE_BITS);
      SWITCH_ASSERT_ALWAYS(error_is_ok(freed), "hle_svc_set_heap_size: rollback free failed");
      regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
      return;
    }
    process->heap_size = requested;
  } else if (requested < process->heap_size) {
    const uint64_t shrink_base = heap->base + requested;
    const uint64_t shrink_bytes = process->heap_size - requested;
    if (borrow_overlaps_src(process, shrink_base, shrink_bytes)) {
      regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
      return;
    }
    free_and_unmap_heap_range(context->vmm, context->pages, shrink_base, shrink_bytes);
    process->heap_size = requested;
  }

  regs->x[0] = HLE_RESULT_SUCCESS;
  regs->x[1] = heap->base; /* fixed by address_space_init at bootstrap - no per-call ASLR to do */
}

/* Maps fresh zeroed pages over [gva, gva + size): one contiguous run when
 * the allocator has it, else page by page. */
static uint32_t map_fresh_pages(HLE_Context *context, uint64_t gva, uint64_t size) {
  uint64_t pa = 0;
  if (error_is_ok(page_allocator_allocate(context->pages, size >> VMM_PAGE_BITS, &pa))) {
    if (error_is_ok(vmm_map(context->vmm, gva, pa, size, VMM_PERM_RW))) return HLE_RESULT_SUCCESS;
    (void)page_allocator_free(context->pages, pa, size >> VMM_PAGE_BITS);
    return HLE_RESULT_OUT_OF_MEMORY;
  }
  for (uint64_t offset = 0; offset < size; offset += VMM_PAGE_SIZE) {
    if (!error_is_ok(page_allocator_allocate(context->pages, 1, &pa))) return HLE_RESULT_OUT_OF_MEMORY;
    if (!error_is_ok(vmm_map(context->vmm, gva + offset, pa, VMM_PAGE_SIZE, VMM_PERM_RW))) {
      (void)page_allocator_free(context->pages, pa, 1);
      return HLE_RESULT_OUT_OF_MEMORY;
    }
  }
  return HLE_RESULT_SUCCESS;
}

/* svcMapPhysicalMemory(addr X0, size X1): backs every unmapped page of
 * the range (inside the alias region) with new RW memory; pages already
 * mapped stay as they are. */
void hle_svc_map_physical_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint64_t base = regs->x[0], size = regs->x[1];
  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK) != 0 || (base & VMM_PAGE_OFFSET_MASK) != 0 || base + size < base) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (!address_region_contains(&context->process->address_space.alias, base, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }
  for (uint64_t at = base; at < base + size;) {
    VMM_Region_Info info;
    if (!error_is_ok(vmm_query(context->vmm, at, &info))) {
      regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
      return;
    }
    const uint64_t end = info.base_gva + info.size < base + size ? info.base_gva + info.size : base + size;
    if (!info.is_mapped) {
      const uint32_t rc = map_fresh_pages(context, at, end - at);
      if (rc != HLE_RESULT_SUCCESS) {
        regs->x[0] = rc;
        return;
      }
    }
    at = end;
  }
  regs->x[0] = HLE_RESULT_SUCCESS;
}

/* svcUnmapPhysicalMemory(addr X0, size X1): releases what MapPhysical-
 * Memory mapped there. */
void hle_svc_unmap_physical_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint64_t base = regs->x[0], size = regs->x[1];
  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK) != 0 || (base & VMM_PAGE_OFFSET_MASK) != 0 || base + size < base) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (!address_region_contains(&context->process->address_space.alias, base, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }
  /* Release the backing in maximal physically contiguous runs (page by
   * page, scattered single frees exhaust the allocator's freelist). */
  uint64_t run_pa = 0, run_pages = 0;
  for (uint64_t offset = 0; offset < size; offset += VMM_PAGE_SIZE) {
    VMM_Region_Info info;
    if (!error_is_ok(vmm_query(context->vmm, base + offset, &info)) || !info.is_mapped || !(info.perms & VMM_PERM_R))
      continue;
    const uint64_t pa = page_pa_of(context->vmm, base + offset);
    (void)vmm_unmap(context->vmm, base + offset, VMM_PAGE_SIZE);
    if (run_pages && pa == run_pa + (run_pages << VMM_PAGE_BITS)) {
      run_pages++;
      continue;
    }
    if (run_pages) free_run(context->pages, run_pa, run_pages);
    run_pa = pa;
    run_pages = 1;
  }
  if (run_pages) free_run(context->pages, run_pa, run_pages);
  regs->x[0] = HLE_RESULT_SUCCESS;
}

/* Every page of [gva, gva + size) mapped read-write. */
static bool range_is_rw(VMM_Context *vmm, uint64_t gva, uint64_t size) {
  for (uint64_t at = gva; at < gva + size;) {
    VMM_Region_Info info;
    if (!error_is_ok(vmm_query(vmm, at, &info)) || !info.is_mapped || (info.perms & VMM_PERM_RW) != VMM_PERM_RW)
      return false;
    at = info.base_gva + info.size;
  }
  return true;
}

void hle_svc_map_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;

  const uint64_t dst = regs->x[0];
  const uint64_t src = regs->x[1];
  const uint64_t size = regs->x[2];

  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK) != 0 || (dst & VMM_PAGE_OFFSET_MASK) != 0 ||
      (src & VMM_PAGE_OFFSET_MASK) != 0) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  /* The source may be anything the kernel lets alias: heap, physical
   * memory (alias region), or a module's writable data (the Nintendo SDK
   * maps pages of its .bss as thread stacks). */
  const Address_Space *as = &process->address_space;
  const bool src_ok = address_region_contains(&as->heap, src, size) ||
                      address_region_contains(&as->alias, src, size) ||
                      (address_region_contains(&as->code, src, size) && range_is_rw(vmm, src, size));
  if (!address_region_contains(&as->stack, dst, size) || !src_ok) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }
  if (process->heap_borrow_count >= PROCESS_MAX_HEAP_BORROWS) {
    regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
    return;
  }

  VMM_Region_Info dst_info;
  Error err = vmm_query(vmm, dst, &dst_info);
  if (!error_is_ok(err) || dst_info.is_mapped || !vmm_run_covers(&dst_info, dst, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }

  /* Recovers src's backing guest_pa via the host pointer vmm hands back
   * for a validated, host-contiguous range (svc_memory.h's ABI note: vmm
   * has no direct gva->guest_pa accessor, and this reuses vmm_guest_to_host
   * plus layout_get() rather than adding one). The pointer itself is never
   * kept past this call - only the derived guest_pa integer is - matching
   * vmm.h's borrow-scope contract. Scoped tightly around just this call
   * (not the whole handler, and not by hle_on_svc - see hle.c). */
  vmm_borrow_scope_begin(vmm);
  void *host_ptr = NULL;
  err = vmm_guest_to_host(vmm, src, size, VMM_PERM_R, &host_ptr);
  vmm_borrow_scope_end(vmm);
  if (!error_is_ok(err)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }
  const uint64_t guest_pa = (uint64_t)(uintptr_t)host_ptr - layout_get()->guest_ram_base;

  err = vmm_reprotect(vmm, src, size, VMM_PERM_NONE);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_map_memory: reprotect of validated src failed");
  err = vmm_map(vmm, dst, guest_pa, size, VMM_PERM_RW);
  if (!error_is_ok(err)) {
    const Error undo = vmm_reprotect(vmm, src, size, VMM_PERM_RW);
    SWITCH_ASSERT_ALWAYS(error_is_ok(undo), "hle_svc_map_memory: rollback reprotect failed");
    regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
    return;
  }

  process->heap_borrows[process->heap_borrow_count++] =
      (Memory_Borrow){.dst_base = dst, .src_base = src, .size = size};
  regs->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_unmap_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;

  const uint64_t dst = regs->x[0];
  const uint64_t src = regs->x[1];
  const uint64_t size = regs->x[2];

  int32_t index = -1;
  for (uint32_t i = 0; i < process->heap_borrow_count; i++) {
    const Memory_Borrow *b = &process->heap_borrows[i];
    if (b->dst_base == dst && b->src_base == src && b->size == size) {
      index = (int32_t)i;
      break;
    }
  }
  if (index < 0) {
    /* Voland requires an exact match against a recorded MapMemory call -
     * real Horizon allows a partial unmap, but libnx's thread.c (the only
     * real-world caller in the reference SDK) always unmaps the exact
     * triple threadCreate mapped, and no title is known to need more.
     * See svc_memory.h. */
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }

  Error err = vmm_unmap(vmm, dst, size);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_unmap_memory: unmap of a tracked alias failed");
  err = vmm_reprotect(vmm, src, size, VMM_PERM_RW);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_unmap_memory: reprotect of a tracked borrow failed");

  /* Swap-remove: heap_borrows has no meaningful order. */
  process->heap_borrows[(uint32_t)index] = process->heap_borrows[--process->heap_borrow_count];
  regs->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_query_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;

  /* Verified ABI (svc_memory.h): X0 is the GUEST address to write the
   * MemoryInfo struct into, X2 is the address to query - X1 is never
   * read by the kernel for this SVC either (same reasoning as
   * SetHeapSize's X0: libnx's stub only uses it locally, afterward, to
   * store the kernel's returned pageinfo word). */
  const uint64_t out_gva = regs->x[0];
  const uint64_t query_addr = regs->x[2];

  /* Past the end of the address space the kernel answers with one
   * inaccessible region reaching 2^64, never an error: rtld walks memory
   * with QueryMemory until base + size wraps to 0, and halts on a failure. */
  VMM_Region_Info info;
  uint32_t type = HLE_MEMTYPE_INACCESSIBLE;
  if (query_addr >= VMM_ADDRESS_SPACE_SIZE) {
    info.base_gva = VMM_ADDRESS_SPACE_SIZE;
    info.size = 0u - VMM_ADDRESS_SPACE_SIZE;
    info.is_mapped = false;
    info.perms = VMM_PERM_NONE;
  } else {
    const Error err = vmm_query(vmm, query_addr, &info);
    if (!error_is_ok(err)) {
      regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
      return;
    }
    type = classify_type(process, query_addr, info.is_mapped, info.perms);
  }

  const HLE_Memory_Info out = {
      .addr = info.base_gva,
      .size = info.size,
      .type = type,
      .attr = 0, /* MemAttr_IsBorrowed not modeled - see the note above classify_type() */
      .perm = info.perms,
      .ipc_refcount = 0,
      .device_refcount = 0,
      .padding = 0,
  };
  const Error wrote = vmm_write_block(vmm, out_gva, &out, sizeof(out));
  if (!error_is_ok(wrote)) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }

  regs->x[0] = HLE_RESULT_SUCCESS;
  regs->x[1] = 0; /* pageinfo: reserved, always 0 on real hardware too */
}

/* ------------------------------------------------------------------ */
/* Shared memory (shared_memory.h).                                    */
/* ------------------------------------------------------------------ */

static int find_shared_mapping(const Process *process, uint64_t base, uint64_t size, const void *object) {
  for (uint32_t i = 0; i < process->shared_mapping_count; i++) {
    const Shared_Mapping *m = &process->shared_mappings[i];
    if (m->base == base && m->size == size && m->object == object) return (int)i;
  }
  return -1;
}

static bool overlaps_region(const Address_Region *region, uint64_t base, uint64_t size) {
  return region->size && ranges_overlap(region->base, region->size, base, size);
}

/* The common checks of Map/UnmapSharedMemory: a shared-memory handle, a
 * page-aligned range of exactly the object's size. Writes the failing
 * result into W0 and returns NULL. */
static Kernel_Shared_Memory *shared_memory_args(HLE_Context *context, CPU_Register_File *regs) {
  const uint32_t handle = (uint32_t)regs->x[0];
  const uint64_t addr = regs->x[1];
  const uint64_t size = regs->x[2];
  Kernel_Shared_Memory *object =
      (Kernel_Shared_Memory *)handle_table_get(&context->process->handles, handle, KERNEL_OBJECT_SHARED_MEMORY);
  if (!object) {
    regs->x[0] = HLE_RESULT_INVALID_HANDLE;
    return NULL;
  }
  if (addr & VMM_PAGE_OFFSET_MASK) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return NULL;
  }
  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK) || size != object->size) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return NULL;
  }
  return object;
}

void hle_svc_map_shared_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  const uint64_t addr = regs->x[1];
  const uint64_t size = regs->x[2];
  const uint32_t perm = (uint32_t)regs->x[3];
  Kernel_Shared_Memory *object = shared_memory_args(context, regs);
  if (!object) return;

  if (perm != SHARED_MEMORY_PERM_R && perm != SHARED_MEMORY_PERM_RW) {
    regs->x[0] = HLE_RESULT_INVALID_NEW_MEMORY_PERMISSION;
    return;
  }
  if (object->remote_perm != SHARED_MEMORY_PERM_DONT_CARE && perm != object->remote_perm) {
    regs->x[0] = HLE_RESULT_INVALID_NEW_MEMORY_PERMISSION;
    return;
  }
  const Address_Space *as = &process->address_space;
  if (!address_region_contains(&as->aslr, addr, size) || overlaps_region(&as->heap, addr, size) ||
      overlaps_region(&as->alias, addr, size) || overlaps_region(&as->stack, addr, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }
  if (object->mapped_gva || process->shared_mapping_count >= PROCESS_MAX_SHARED_MAPPINGS) {
    regs->x[0] = HLE_RESULT_INVALID_STATE;
    return;
  }
  const uint32_t vmm_perms = perm == SHARED_MEMORY_PERM_RW ? (uint32_t)VMM_PERM_RW : (uint32_t)VMM_PERM_R;
  if (!error_is_ok(vmm_map(context->vmm, addr, object->guest_pa, size, vmm_perms))) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE; /* something is mapped there */
    return;
  }
  object->mapped_gva = addr;
  process->shared_mappings[process->shared_mapping_count++] = (Shared_Mapping){addr, size, object};
  regs->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_unmap_shared_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  const uint64_t addr = regs->x[1];
  const uint64_t size = regs->x[2];
  Kernel_Shared_Memory *object = shared_memory_args(context, regs);
  if (!object) return;
  const int index = find_shared_mapping(process, addr, size, object);
  if (index < 0) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }
  const Error err = vmm_unmap(context->vmm, addr, size);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_unmap_shared_memory: tracked view was not mapped");
  object->mapped_gva = 0;
  process->shared_mappings[(uint32_t)index] = process->shared_mappings[--process->shared_mapping_count];
  regs->x[0] = HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SetMemoryPermission.                                                */
/* ------------------------------------------------------------------ */

static bool range_in_module(const Process *process, uint64_t base, uint64_t size) {
  for (uint32_t m = 0; m < process->module_count; m++) {
    const Address_Region image = {process->modules[m].base_gva, process->modules[m].image_size};
    if (address_region_contains(&image, base, size)) return true;
  }
  const Ro_Module *ro = ro_module_at(process, base);
  if (ro) {
    const Address_Region image = {ro->base, ro->nro_size + ro->bss_size};
    if (address_region_contains(&image, base, size)) return true;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* nn::ro modules (ldr:ro).                                             */
/* ------------------------------------------------------------------ */

typedef struct Guest_Source {
  VMM_Context *vmm;
  uint64_t base;
} Guest_Source;

static Error guest_source_read(void *user, uint64_t offset, void *out, uint64_t size) {
  const Guest_Source *g = (const Guest_Source *)user;
  return vmm_read_block(g->vmm, g->base + offset, out, size);
}

/* Every page of [gva, gva + size) mapped readable and writable. */
static bool range_mapped_rw(VMM_Context *vmm, uint64_t gva, uint64_t size) {
  for (uint64_t at = gva; at < gva + size;) {
    VMM_Region_Info info;
    if (!error_is_ok(vmm_query(vmm, at, &info)) || !info.is_mapped || (info.perms & VMM_PERM_RW) != VMM_PERM_RW) return false;
    at = info.base_gva + info.size;
  }
  return true;
}

/* The first unmapped run of `size` bytes at or above `from` (2MB-aligned
 * starts, as Horizon places modules), below the address-space end. */
static uint64_t find_free_run(VMM_Context *vmm, uint64_t from, uint64_t end, uint64_t size) {
  uint64_t at = (from + ADDRESS_SPACE_ASLR_GRANULE - 1u) & ~(ADDRESS_SPACE_ASLR_GRANULE - 1u);
  while (at + size <= end) {
    VMM_Region_Info info;
    if (!error_is_ok(vmm_query(vmm, at, &info))) return 0;
    if (!info.is_mapped && info.base_gva + info.size >= at + size) return at;
    at = (info.base_gva + info.size + ADDRESS_SPACE_ASLR_GRANULE - 1u) & ~(ADDRESS_SPACE_ASLR_GRANULE - 1u);
  }
  return 0;
}

/* Aliases each page of [src, src + size) at dst with `perms`. */
static bool alias_pages(VMM_Context *vmm, uint64_t dst, uint64_t src, uint64_t size, uint32_t perms) {
  for (uint64_t off = 0; off < size; off += VMM_PAGE_SIZE) {
    if (!error_is_ok(vmm_map(vmm, dst + off, page_pa_of(vmm, src + off), VMM_PAGE_SIZE, perms))) return false;
  }
  return true;
}

uint32_t hle_ro_map_module(HLE_Context *context, uint64_t nro, uint64_t nro_size, uint64_t bss, uint64_t bss_size,
                           uint64_t *out) {
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;
  if (!nro_size || ((nro | nro_size | bss | bss_size) & VMM_PAGE_OFFSET_MASK) || (bss_size && !bss))
    return HLE_RESULT_INVALID_SIZE;
  if (!range_mapped_rw(vmm, nro, nro_size) || (bss_size && !range_mapped_rw(vmm, bss, bss_size)))
    return HLE_RESULT_INVALID_MEMORY_STATE;
  Guest_Source guest = {vmm, nro};
  const Byte_Source source = {&guest, nro_size, guest_source_read};
  NSO image;
  const Error parsed = nro_open(&source, &image);
  if (!error_is_ok(parsed)) {
    log_warn("[ro] LoadModule: %s", parsed.message);
    return HLE_RESULT_INVALID_MEMORY_STATE;
  }
  const NSO_Segment *text = &image.segments[NSO_SEGMENT_TEXT];
  const NSO_Segment *rodata = &image.segments[NSO_SEGMENT_RODATA];
  const NSO_Segment *data = &image.segments[NSO_SEGMENT_DATA];
  if ((uint64_t)data->memory_offset + data->memory_size > nro_size || image.bss_size > bss_size + (nro_size - data->memory_offset - data->memory_size))
    return HLE_RESULT_INVALID_SIZE;
  Ro_Module *slot = NULL;
  for (uint32_t i = 0; i < PROCESS_MAX_RO_MODULES && !slot; i++)
    if (!process->ro_modules[i].base) slot = &process->ro_modules[i];
  if (!slot) return HLE_RESULT_OUT_OF_MEMORY;
  const Address_Space *as = &process->address_space;
  const uint64_t base = find_free_run(vmm, as->tls_io.base + as->tls_io.size, as->aslr.base + as->aslr.size,
                                      nro_size + bss_size);
  if (!base) return HLE_RESULT_OUT_OF_MEMORY;
  const uint64_t text_end = (uint64_t)text->memory_offset + text->memory_size;
  const uint64_t ro_end = (uint64_t)rodata->memory_offset + rodata->memory_size;
  bool ok = alias_pages(vmm, base, nro, text_end, VMM_PERM_RX) &&
            alias_pages(vmm, base + text_end, nro + text_end, ro_end - text_end, VMM_PERM_R) &&
            alias_pages(vmm, base + ro_end, nro + ro_end, nro_size - ro_end, VMM_PERM_RW) &&
            (!bss_size || alias_pages(vmm, base + nro_size, bss, bss_size, VMM_PERM_RW));
  if (!ok) {
    (void)vmm_unmap(vmm, base, nro_size + bss_size);
    return HLE_RESULT_OUT_OF_MEMORY;
  }
  /* The buffers belong to the module now. */
  (void)vmm_reprotect(vmm, nro, nro_size, VMM_PERM_NONE);
  if (bss_size) (void)vmm_reprotect(vmm, bss, bss_size, VMM_PERM_NONE);
  *slot = (Ro_Module){base, nro, nro_size, bss, bss_size, ro_end};
  log_info("[ro] module mapped at 0x%llx (.text 0x%llx, .rodata 0x%llx, .data+.bss 0x%llx)", (unsigned long long)base,
           (unsigned long long)text_end, (unsigned long long)(ro_end - text_end),
           (unsigned long long)(nro_size - ro_end + bss_size));
  *out = base;
  return HLE_RESULT_SUCCESS;
}

uint32_t hle_ro_unmap_module(HLE_Context *context, uint64_t base) {
  Process *process = context->process;
  for (uint32_t i = 0; i < PROCESS_MAX_RO_MODULES; i++) {
    Ro_Module *m = &process->ro_modules[i];
    if (!base || m->base != base) continue;
    (void)vmm_unmap(context->vmm, m->base, m->nro_size + m->bss_size);
    (void)vmm_reprotect(context->vmm, m->nro_src, m->nro_size, VMM_PERM_RW);
    if (m->bss_size) (void)vmm_reprotect(context->vmm, m->bss_src, m->bss_size, VMM_PERM_RW);
    memset(m, 0, sizeof(*m));
    return HLE_RESULT_SUCCESS;
  }
  return HLE_RESULT_INVALID_MEMORY_STATE;
}

void hle_svc_set_memory_permission(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const Process *process = context->process;
  const uint64_t addr = regs->x[0];
  const uint64_t size = regs->x[1];
  const uint32_t perm = (uint32_t)regs->x[2];
  if (addr & VMM_PAGE_OFFSET_MASK) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }
  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK)) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (perm != VMM_PERM_NONE && perm != VMM_PERM_R && perm != VMM_PERM_RW) {
    regs->x[0] = HLE_RESULT_INVALID_NEW_MEMORY_PERMISSION;
    return;
  }
  /* Reprotectable memory: the heap (outside MapMemory borrows) and the
   * loaded images (libnx's crt0 makes .data.rel.ro read-only after
   * relocating). */
  const bool in_heap = process->heap_size &&
                       address_region_contains(&(Address_Region){process->address_space.heap.base, process->heap_size},
                                               addr, size) &&
                       !borrow_overlaps_src(process, addr, size);
  if (!in_heap && !range_in_module(process, addr, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }
  if (!error_is_ok(vmm_reprotect(context->vmm, addr, size, perm))) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }
  regs->x[0] = HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Transfer memory.                                                    */
/* ------------------------------------------------------------------ */

void hle_svc_create_transfer_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const Process *process = context->process;
  const uint64_t addr = regs->x[1];
  const uint64_t size = regs->x[2];
  const uint32_t perm = (uint32_t)regs->x[3];
  if (addr & VMM_PAGE_OFFSET_MASK) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }
  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK)) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (perm != VMM_PERM_NONE && perm != VMM_PERM_R && perm != VMM_PERM_RW) {
    regs->x[0] = HLE_RESULT_INVALID_NEW_MEMORY_PERMISSION;
    return;
  }
  /* Heap, physical memory the process mapped (alias region), or a
   * module's writable .data/.bss (code-data memory can be transferred:
   * titles hand services static buffers, e.g. Super Smash Bros. Ultimate
   * at start-up). */
  const Address_Region heap = {process->address_space.heap.base, process->heap_size};
  const bool in_heap = address_region_contains(&heap, addr, size);
  const bool in_alias = address_region_contains(&process->address_space.alias, addr, size) &&
                        range_is_rw(context->vmm, addr, size);
  const bool in_module_data = range_in_module(process, addr, size) && range_is_rw(context->vmm, addr, size);
  if ((!in_heap && !in_alias && !in_module_data) || borrow_overlaps_src(process, addr, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }
  Transfer_Memory_Pool *pool = context->transfer_memory;
  Kernel_Transfer_Memory *object = NULL;
  for (uint32_t i = 0; pool && i < TRANSFER_MEMORY_POOL_CAPACITY && !object; i++) {
    if (!pool->objects[i].references) object = &pool->objects[i];
  }
  if (!object) {
    regs->x[0] = HLE_RESULT_RESOURCE_EXHAUSTED;
    return;
  }
  uint32_t handle = 0;
  if (!error_is_ok(handle_table_add(&context->process->handles, KERNEL_OBJECT_TRANSFER_MEMORY, object, &handle))) {
    regs->x[0] = HLE_RESULT_OUT_OF_HANDLES;
    return;
  }
  if (!error_is_ok(vmm_reprotect(context->vmm, addr, size, perm))) {
    (void)handle_table_remove(&context->process->handles, handle, NULL, NULL);
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }
  *object = (Kernel_Transfer_Memory){1, addr, size, perm};
  regs->x[0] = HLE_RESULT_SUCCESS;
  regs->x[1] = handle;
}

void hle_transfer_memory_release(HLE_Context *context, void *object) {
  Kernel_Transfer_Memory *tmem = (Kernel_Transfer_Memory *)object;
  if (!tmem || !tmem->references || --tmem->references) return;
  (void)vmm_reprotect(context->vmm, tmem->address, tmem->size, VMM_PERM_RW);
  memset(tmem, 0, sizeof(*tmem));
}
