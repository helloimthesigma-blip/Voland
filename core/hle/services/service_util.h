/**
 * Shared helpers for HLE services (§12): the accepted-and-inert command
 * handlers every service's table uses for its "_stub" entries, and small
 * request/response conveniences. Header-only; each helper is tiny.
 */
#ifndef SWITCH_HLE_SERVICES_SERVICE_UTIL_H
#define SWITCH_HLE_SERVICES_SERVICE_UTIL_H

#include <stdint.h>
#include <string.h>

#include "common/vmm.h"
#include "hle/hle.h"
#include "hle/kernel/event.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/ipc.h"

#define SERVICE_COMMAND_COUNT(table) (sizeof(table) / sizeof((table)[0]))

/* An interface over a command table, with no close hook. */
#define SERVICE_INTERFACE(name, table, pointer_buffer_size, state) \
  ((Service_Interface){(name), (table), SERVICE_COMMAND_COUNT(table), (pointer_buffer_size), (state), NULL})

/* Success, no output. */
static inline HLE_ServiceResult service_cmd_ok(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return HLE_RESULT_SUCCESS;
}

/* Success with a zero (or one) of the given width: bools, counts, ids. */
static inline HLE_ServiceResult service_cmd_out_u8_false(HLE_Context *c, Service_Object *self,
                                                         const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0);
  return HLE_RESULT_SUCCESS;
}

static inline HLE_ServiceResult service_cmd_out_u8_true(HLE_Context *c, Service_Object *self,
                                                        const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 1);
  return HLE_RESULT_SUCCESS;
}

static inline HLE_ServiceResult service_cmd_out_u64_zero(HLE_Context *c, Service_Object *self,
                                                         const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, 0);
  return HLE_RESULT_SUCCESS;
}

static inline HLE_ServiceResult service_cmd_out_zero128(HLE_Context *c, Service_Object *self,
                                                        const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, 0);
  (void)ipc_response_push_u64(res, 0);
  return HLE_RESULT_SUCCESS;
}

/* The request's first output buffer: B (receive) or C (receive list),
 * whichever has a size - libnx's HipcAutoSelect sends the other empty. */
static inline const IPC_Buffer *service_out_buffer(const IPC_Request *req, uint32_t index) {
  if (index < req->receive_count && req->receives[index].size) return &req->receives[index];
  if (index < req->receive_list_count && req->receive_list[index].size) return &req->receive_list[index];
  return NULL;
}

/* The request's input buffer: A (send) or X (static). */
static inline const IPC_Buffer *service_in_buffer(const IPC_Request *req, uint32_t index) {
  if (index < req->send_count && req->sends[index].size) return &req->sends[index];
  if (index < req->static_count && req->statics[index].size) return &req->statics[index];
  return NULL;
}

/* Copies `size` bytes (clamped to the buffer) into an output buffer.
 * Returns the bytes written, 0 if there is no buffer or the write fails. */
static inline uint64_t service_write_out(HLE_Context *c, const IPC_Request *req, uint32_t index, const void *src,
                                         uint64_t size) {
  const IPC_Buffer *buf = service_out_buffer(req, index);
  if (!buf) return 0;
  const uint64_t n = size < buf->size ? size : buf->size;
  return error_is_ok(vmm_write_block(c->vmm, buf->gva, src, n)) ? n : 0;
}

/* Reads up to `max` bytes of an input buffer; returns the bytes read. */
static inline uint64_t service_read_in(HLE_Context *c, const IPC_Request *req, uint32_t index, void *dst,
                                       uint64_t max) {
  const IPC_Buffer *buf = service_in_buffer(req, index);
  if (!buf) return 0;
  const uint64_t n = buf->size < max ? buf->size : max;
  return error_is_ok(vmm_read_block(c->vmm, buf->gva, dst, n)) ? n : 0;
}

/* Hands the guest a new readable handle to `event` (taking a reference). */
static inline HLE_ServiceResult service_push_event(HLE_Context *c, IPC_Response *res, Kernel_Event **slot) {
  uint32_t handle = 0;
  if (!*slot) {
    const uint32_t result = hle_create_event(c, &handle, NULL, slot);
    if (result != HLE_RESULT_SUCCESS) return result;
    event_retain(*slot); /* the service keeps its own reference */
  } else {
    event_retain(*slot);
    if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_EVENT_READABLE, *slot, &handle))) {
      event_release(*slot);
      return HLE_RESULT_OUT_OF_HANDLES;
    }
  }
  (void)ipc_response_push_copy_handle(res, handle);
  return HLE_RESULT_SUCCESS;
}

#endif /* SWITCH_HLE_SERVICES_SERVICE_UTIL_H */
