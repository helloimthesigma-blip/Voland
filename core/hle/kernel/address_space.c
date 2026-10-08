/**
 * Process address-space layout. See address_space.h.
 */
#include "hle/kernel/address_space.h"

#include "common/vmm.h"

#include <string.h>

static uint64_t align_up_page(uint64_t value) {
  return (value + VMM_PAGE_OFFSET_MASK) & ~VMM_PAGE_OFFSET_MASK;
}

/* xorshift64: enough to spread a caller-supplied seed over the ASLR
 * window reproducibly. Not a security boundary - ASLR on a single-
 * process emulator is about matching hardware, not defending it. */
static uint64_t mix_seed(uint64_t seed) {
  uint64_t x = seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

Error address_space_init(NPDM_Address_Space type, uint64_t code_size,
                         uint64_t aslr_seed, Address_Space *out) {
  if (!out) {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: out is NULL");
  }
  if (code_size == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: code size is zero");
  }
  /* Region sizes per layout; the placement below is shared. */
  uint64_t start, end, alias_size, heap_size, stack_size, tls_io_size;
  if (type == NPDM_ADDRESS_SPACE_64_BIT_39) {
    start = ADDRESS_SPACE_39_START;
    end = ADDRESS_SPACE_39_END;
    alias_size = ADDRESS_SPACE_39_ALIAS_SIZE;
    heap_size = ADDRESS_SPACE_39_HEAP_SIZE;
    stack_size = ADDRESS_SPACE_39_STACK_SIZE;
    tls_io_size = ADDRESS_SPACE_39_TLS_IO_SIZE;
  } else if (type == NPDM_ADDRESS_SPACE_64_BIT_36) {
    start = ADDRESS_SPACE_36_START;
    end = ADDRESS_SPACE_36_END;
    alias_size = ADDRESS_SPACE_36_ALIAS_SIZE;
    heap_size = ADDRESS_SPACE_36_HEAP_SIZE;
    stack_size = ADDRESS_SPACE_36_STACK_SIZE;
    tls_io_size = ADDRESS_SPACE_36_TLS_IO_SIZE;
  } else if (type == NPDM_ADDRESS_SPACE_32_BIT || type == NPDM_ADDRESS_SPACE_32_BIT_NO_RESERVED) {
    const bool reserved = type == NPDM_ADDRESS_SPACE_32_BIT;
    start = ADDRESS_SPACE_32_START;
    end = ADDRESS_SPACE_32_END;
    alias_size = reserved ? ADDRESS_SPACE_32_ALIAS_SIZE : ADDRESS_SPACE_32_NO_RESERVED_ALIAS_SIZE;
    heap_size = reserved ? ADDRESS_SPACE_32_HEAP_SIZE : ADDRESS_SPACE_32_NO_RESERVED_HEAP_SIZE;
    stack_size = ADDRESS_SPACE_32_STACK_SIZE;
    tls_io_size = ADDRESS_SPACE_32_TLS_IO_SIZE;
  } else {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: unknown address-space type");
  }
  memset(out, 0, sizeof(*out));

  if (type == NPDM_ADDRESS_SPACE_32_BIT || type == NPDM_ADDRESS_SPACE_32_BIT_NO_RESERVED) {
    const uint64_t code_bytes = align_up_page(code_size);
    if (code_bytes < code_size || code_bytes > ADDRESS_SPACE_32_CODE_END - start - stack_size - tls_io_size)
      return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: code region does not fit");
    out->type = type;
    out->aslr.base = start;
    out->aslr.size = end - start;
    out->code.base = start;
    out->code.size = code_bytes;
    out->tls_io.base = ADDRESS_SPACE_32_CODE_END - tls_io_size;
    out->tls_io.size = tls_io_size;
    out->stack.base = start + code_bytes;
    out->stack.size = out->tls_io.base - out->stack.base;
    out->alias.base = ADDRESS_SPACE_32_CODE_END;
    out->alias.size = alias_size;
    out->heap.base = ADDRESS_SPACE_32_CODE_END + alias_size;
    out->heap.size = heap_size;
    (void)aslr_seed; /* the modules keep the region's base, as 32-bit Horizon titles see them */
    return OK;
  }

  const uint64_t span = end - start;
  const uint64_t fixed_regions = alias_size + heap_size + stack_size + tls_io_size;
  const uint64_t code_bytes = align_up_page(code_size);
  if (code_bytes < code_size || code_bytes > span - fixed_regions) {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: code region does not fit");
  }

  /* Everything after the code region is placed back to back, so the
   * slack is the only room ASLR has to shift the whole train. */
  const uint64_t slack = span - fixed_regions - code_bytes;
  uint64_t aslr_offset = 0;
  if (aslr_seed != 0) {
    const uint64_t granules = slack / ADDRESS_SPACE_ASLR_GRANULE;
    if (granules > 0) {
      aslr_offset = (mix_seed(aslr_seed) % (granules + 1)) * ADDRESS_SPACE_ASLR_GRANULE;
    }
  }

  out->type = type;
  out->aslr.base = start;
  out->aslr.size = span;

  uint64_t cursor = start + aslr_offset;
  out->code.base = cursor;
  out->code.size = code_bytes;
  cursor += code_bytes;
  out->alias.base = cursor;
  out->alias.size = alias_size;
  cursor += alias_size;
  out->heap.base = cursor;
  out->heap.size = heap_size;
  cursor += heap_size;
  out->stack.base = cursor;
  out->stack.size = stack_size;
  cursor += stack_size;
  out->tls_io.base = cursor;
  out->tls_io.size = tls_io_size;
  return OK;
}

bool address_region_contains(const Address_Region *region, uint64_t gva, uint64_t size) {
  if (!region) return false;
  if (gva < region->base) return false;
  const uint64_t offset = gva - region->base;
  if (offset > region->size) return false;
  return size <= region->size - offset;
}
