#include "hle/kernel/handle_table.h"

#include <stddef.h>
#include <string.h>

static uint32_t encode_handle(uint16_t linear_id, uint32_t index) {
  return ((uint32_t)linear_id << HANDLE_INDEX_BITS) | index;
}

/* Returns the slot for a live handle, or NULL. Pseudo-handles are never
 * stored (handle_table.h) and are rejected explicitly. */
static const Handle_Entry *lookup(const Handle_Table *table, uint32_t handle) {
  if (!table || handle == HANDLE_INVALID) return NULL;
  if (handle == HANDLE_PSEUDO_CURRENT_THREAD || handle == HANDLE_PSEUDO_CURRENT_PROCESS) return NULL;
  const uint32_t index = handle & HANDLE_INDEX_MASK;
  const uint32_t linear_id = handle >> HANDLE_INDEX_BITS;
  if (index >= HANDLE_TABLE_CAPACITY || linear_id == 0 || linear_id > HANDLE_LINEAR_ID_MASK) return NULL;
  const Handle_Entry *entry = &table->entries[index];
  if (entry->type == KERNEL_OBJECT_NONE || entry->linear_id != linear_id) return NULL;
  return entry;
}

Error handle_table_init(Handle_Table *table) {
  if (!table) return ERR(RESULT_INVALID_ARGUMENT, "handle_table_init: NULL table");
  memset(table, 0, sizeof(*table));
  table->next_linear_id = 1;
  return OK;
}

Error handle_table_add(Handle_Table *table, Kernel_Object_Type type, void *object,
                       uint32_t *out_handle) {
  if (!table || !out_handle || !object || type == KERNEL_OBJECT_NONE) {
    return ERR(RESULT_INVALID_ARGUMENT, "handle_table_add: bad argument");
  }
  for (uint32_t index = 0; index < HANDLE_TABLE_CAPACITY; index++) {
    Handle_Entry *entry = &table->entries[index];
    if (entry->type != KERNEL_OBJECT_NONE) continue;
    const uint16_t linear_id = table->next_linear_id;
    table->next_linear_id = (uint16_t)((linear_id & HANDLE_LINEAR_ID_MASK) + 1u);
    if (table->next_linear_id > HANDLE_LINEAR_ID_MASK) table->next_linear_id = 1;
    entry->type = type;
    entry->linear_id = linear_id;
    entry->object = object;
    table->count++;
    *out_handle = encode_handle(linear_id, index);
    return OK;
  }
  return ERR(RESULT_OUT_OF_MEMORY, "handle_table_add: table full");
}

void *handle_table_get(const Handle_Table *table, uint32_t handle, Kernel_Object_Type expected) {
  const Handle_Entry *entry = lookup(table, handle);
  if (!entry || entry->type != expected) return NULL;
  return entry->object;
}

Kernel_Object_Type handle_table_type_of(const Handle_Table *table, uint32_t handle) {
  const Handle_Entry *entry = lookup(table, handle);
  return entry ? entry->type : KERNEL_OBJECT_NONE;
}

Error handle_table_remove(Handle_Table *table, uint32_t handle, Kernel_Object_Type *out_type,
                          void **out_object) {
  const Handle_Entry *found = lookup(table, handle);
  if (!found) return ERR(RESULT_NOT_FOUND, "handle_table_remove: handle is not live");
  Handle_Entry *entry = &table->entries[handle & HANDLE_INDEX_MASK];
  if (out_type) *out_type = entry->type;
  if (out_object) *out_object = entry->object;
  memset(entry, 0, sizeof(*entry));
  table->count--;
  return OK;
}
