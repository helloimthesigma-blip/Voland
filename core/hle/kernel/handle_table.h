/**
 * Per-process kernel handle table (§12 "Handle table"). PROPOSED HEADER -
 * awaiting maintainer review; no implementation exists yet.
 *
 * Every kernel object a guest names by integer - threads, IPC sessions,
 * and later events, shared memory and transfer memory - lives behind
 * this table. Fixed size (Horizon caps a process at 1024 handles), no
 * allocation, generation-checked so a stale handle faults instead of
 * aliasing whatever reused its slot.
 *
 * Encoding. A handle is `(linear_id << 15) | index` - Horizon's own
 * shape, which PROCESS_MAIN_THREAD_HANDLE (0x8000: linear id 1, index 0)
 * already commits to. `linear_id` is a 15-bit counter that starts at 1,
 * increments on every add, and skips 0 on wrap, so 0 is never a valid
 * handle (HANDLE_INVALID). Lookup checks both that the slot's stored
 * linear id equals the handle's and that the stored type matches the
 * caller's expectation; either mismatch is HLE_RESULT_INVALID_HANDLE.
 *
 * Determinism. Free slots are reused lowest-index-first, and the very
 * first add on a fresh table yields 0x8000 - process_bootstrap() adds
 * the main thread first so the X1 the entry ABI hands the guest is a
 * real table entry, not just a constant.
 *
 * Pseudo-handles 0xFFFF8000 (current thread) and 0xFFFF8001 (current
 * process) are never stored: handle_table_get() reports them as
 * RESULT_NOT_FOUND and callers that accept them resolve them before
 * lookup (svc handlers know the current thread/process; this table
 * does not).
 *
 * The table stores a type tag and an untyped object pointer it does not
 * own. Reference counting is out of scope for Phase 1: an object is
 * referenced by at most one handle until CloneCurrentObject/duplicate
 * handles land, and the IPC session pool (ipc.h) frees a session when its
 * only handle is closed.
 */
#ifndef SWITCH_HLE_KERNEL_HANDLE_TABLE_H
#define SWITCH_HLE_KERNEL_HANDLE_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "common/result.h"

#define HANDLE_TABLE_CAPACITY 1024u
#define HANDLE_INVALID ((uint32_t)0)
#define HANDLE_PSEUDO_CURRENT_THREAD ((uint32_t)0xFFFF8000u)
#define HANDLE_PSEUDO_CURRENT_PROCESS ((uint32_t)0xFFFF8001u)

/* Handle bit fields: index in [0, 15), linear id in [15, 30). */
#define HANDLE_INDEX_BITS 15u
#define HANDLE_INDEX_MASK ((uint32_t)0x7FFFu)
#define HANDLE_LINEAR_ID_MASK ((uint32_t)0x7FFFu)

typedef enum Kernel_Object_Type {
  KERNEL_OBJECT_NONE = 0, /* free slot */
  KERNEL_OBJECT_THREAD,   /* Guest_Thread (thread.h) or, until Phase 2, the main thread marker */
  KERNEL_OBJECT_SESSION,  /* IPC_Session (ipc.h) - client end */
  /* Events, shared memory, transfer memory, ports: added with their SVCs. */
} Kernel_Object_Type;

typedef struct Handle_Entry {
  Kernel_Object_Type type; /* KERNEL_OBJECT_NONE when the slot is free */
  uint16_t linear_id;      /* the issuing generation; 0 only when free */
  void *object;            /* not owned */
} Handle_Entry;

typedef struct Handle_Table {
  Handle_Entry entries[HANDLE_TABLE_CAPACITY];
  uint16_t next_linear_id; /* never 0 */
  uint32_t count;          /* live entries */
} Handle_Table;

/* Empties the table and resets the generation counter to 1, so the first
 * add issues 0x8000. RESULT_INVALID_ARGUMENT on NULL. */
Error handle_table_init(Handle_Table *table);

/* Stores (type, object) in the lowest free slot and returns its handle.
 *   RESULT_INVALID_ARGUMENT NULL table/out_handle, type NONE, object NULL
 *   RESULT_OUT_OF_MEMORY    table full (svc layer maps this to
 *                           HLE_RESULT_OUT_OF_HANDLES) */
Error handle_table_add(Handle_Table *table, Kernel_Object_Type type, void *object,
                       uint32_t *out_handle);

/* The object behind `handle` if it is live and of `expected` type, else
 * NULL. Pseudo-handles, HANDLE_INVALID, a stale generation, an index out
 * of range and a type mismatch all yield NULL - callers turn that into
 * HLE_RESULT_INVALID_HANDLE. */
void *handle_table_get(const Handle_Table *table, uint32_t handle, Kernel_Object_Type expected);

/* The type of a live handle, or KERNEL_OBJECT_NONE. For CloseHandle,
 * which must dispatch on type before it knows what it is closing. */
Kernel_Object_Type handle_table_type_of(const Handle_Table *table, uint32_t handle);

/* Frees the slot. Optional outs report what was stored so the caller can
 * release the object. RESULT_NOT_FOUND if the handle is not live. */
Error handle_table_remove(Handle_Table *table, uint32_t handle, Kernel_Object_Type *out_type,
                          void **out_object);

#endif /* SWITCH_HLE_KERNEL_HANDLE_TABLE_H */
