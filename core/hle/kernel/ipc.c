/**
 * IPC: HIPC/CMIF/TIPC parse and encode, sessions, domains, dispatch.
 * See ipc.h for the data flow and the wire-format sources. Everything
 * here operates on host copies of the TLS command buffer; guest memory is
 * svc_ipc.c's business (through vmm).
 */
#include "hle/kernel/ipc.h"

#include "common/log.h"
#include "hle/hle.h"
#include "hle/kernel/handle_table.h"

#include <stddef.h>
#include <string.h>

/* HIPC header field layout (libnx sf/hipc.h HipcHeader/HipcSpecialHeader
 * bitfields, restated as shifts). */
#define HIPC_HEADER_BYTES 8u
#define HIPC_TYPE_MASK 0xFFFFu
#define HIPC_COUNT_MASK 0xFu
#define HIPC_SEND_STATICS_SHIFT 16u
#define HIPC_SEND_BUFFERS_SHIFT 20u
#define HIPC_RECV_BUFFERS_SHIFT 24u
#define HIPC_EXCH_BUFFERS_SHIFT 28u
#define HIPC_DATA_WORDS_MASK 0x3FFu
#define HIPC_RECV_STATIC_MODE_SHIFT 10u
#define HIPC_HAS_SPECIAL_HEADER_BIT (1u << 31)
#define HIPC_SPECIAL_HEADER_BYTES 4u
#define HIPC_SPECIAL_SEND_PID_BIT 1u
#define HIPC_SPECIAL_COPY_SHIFT 1u
#define HIPC_SPECIAL_MOVE_SHIFT 5u
#define HIPC_PID_BYTES 8u
#define HIPC_HANDLE_BYTES 4u
#define HIPC_STATIC_DESCRIPTOR_BYTES 8u
#define HIPC_BUFFER_DESCRIPTOR_BYTES 12u
#define HIPC_RECV_LIST_ENTRY_BYTES 8u
#define HIPC_RECV_STATIC_MODE_AUTO 2u /* one entry, "use the pointer buffer" */
#define HIPC_RESPONSE_TYPE 0u

#define HIPC_ADDRESS_MID_SHIFT 32u
#define HIPC_ADDRESS_HIGH_SHIFT 36u
#define HIPC_STATIC_INDEX_MASK 0x3Fu
#define HIPC_STATIC_ADDRESS_HIGH_SHIFT 6u
#define HIPC_STATIC_ADDRESS_HIGH_MASK 0x3Fu
#define HIPC_STATIC_ADDRESS_MID_SHIFT 12u
#define HIPC_STATIC_SIZE_SHIFT 16u
#define HIPC_BUFFER_MODE_MASK 0x3u
#define HIPC_BUFFER_ADDRESS_HIGH_SHIFT 2u
#define HIPC_BUFFER_ADDRESS_HIGH_MASK 0x3FFFFFu
#define HIPC_BUFFER_SIZE_HIGH_SHIFT 24u
#define HIPC_BUFFER_ADDRESS_MID_SHIFT 28u
#define HIPC_RECV_ADDRESS_HIGH_MASK 0xFFFFu
#define HIPC_RECV_SIZE_SHIFT 16u
#define HIPC_NIBBLE_MASK 0xFu

/* CMIF headers (libnx sf/cmif.h). */
#define CMIF_HEADER_BYTES 16u
#define CMIF_DOMAIN_HEADER_BYTES 16u
#define CMIF_OBJECT_ID_BYTES 4u

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

/* ------------------------------------------------------------------ */
/* Request parsing.                                                    */
/* ------------------------------------------------------------------ */

static void decode_buffer_descriptor(const uint8_t *p, IPC_Buffer *out) {
  const uint32_t size_low = rd32(p);
  const uint32_t address_low = rd32(p + 4);
  const uint32_t packed = rd32(p + 8);
  out->mode = (uint8_t)(packed & HIPC_BUFFER_MODE_MASK);
  out->index = 0;
  out->gva = (uint64_t)address_low |
             ((uint64_t)((packed >> HIPC_BUFFER_ADDRESS_MID_SHIFT) & HIPC_NIBBLE_MASK)
              << HIPC_ADDRESS_MID_SHIFT) |
             ((uint64_t)((packed >> HIPC_BUFFER_ADDRESS_HIGH_SHIFT) & HIPC_BUFFER_ADDRESS_HIGH_MASK)
              << HIPC_ADDRESS_HIGH_SHIFT);
  out->size = (uint64_t)size_low |
              ((uint64_t)((packed >> HIPC_BUFFER_SIZE_HIGH_SHIFT) & HIPC_NIBBLE_MASK) << 32);
}

static void decode_static_descriptor(const uint8_t *p, IPC_Buffer *out) {
  const uint32_t packed = rd32(p);
  const uint32_t address_low = rd32(p + 4);
  out->mode = 0;
  out->index = (uint8_t)(packed & HIPC_STATIC_INDEX_MASK);
  out->size = packed >> HIPC_STATIC_SIZE_SHIFT;
  out->gva = (uint64_t)address_low |
             ((uint64_t)((packed >> HIPC_STATIC_ADDRESS_MID_SHIFT) & HIPC_NIBBLE_MASK)
              << HIPC_ADDRESS_MID_SHIFT) |
             ((uint64_t)((packed >> HIPC_STATIC_ADDRESS_HIGH_SHIFT) & HIPC_STATIC_ADDRESS_HIGH_MASK)
              << HIPC_ADDRESS_HIGH_SHIFT);
}

static void decode_recv_list_entry(const uint8_t *p, IPC_Buffer *out) {
  const uint32_t address_low = rd32(p);
  const uint32_t packed = rd32(p + 4);
  out->mode = 0;
  out->index = 0;
  out->gva = (uint64_t)address_low | ((uint64_t)(packed & HIPC_RECV_ADDRESS_HIGH_MASK) << 32);
  out->size = packed >> HIPC_RECV_SIZE_SHIFT;
}

static bool classify(uint16_t type, IPC_Request *r) {
  switch (type) {
  case IPC_HIPC_TYPE_CLOSE:
    r->kind = IPC_MESSAGE_CMIF_CLOSE;
    return true;
  case IPC_HIPC_TYPE_REQUEST:
  case IPC_HIPC_TYPE_REQUEST_WITH_CONTEXT:
    r->kind = IPC_MESSAGE_CMIF_REQUEST;
    return true;
  case IPC_HIPC_TYPE_CONTROL:
  case IPC_HIPC_TYPE_CONTROL_WITH_CONTEXT:
    r->kind = IPC_MESSAGE_CMIF_CONTROL;
    return true;
  case IPC_HIPC_TYPE_TIPC_CLOSE:
    r->kind = IPC_MESSAGE_TIPC_CLOSE;
    return true;
  default:
    if (type >= IPC_HIPC_TYPE_TIPC_COMMAND_BASE) {
      r->kind = IPC_MESSAGE_TIPC_REQUEST;
      r->command_id = (uint32_t)type - IPC_HIPC_TYPE_TIPC_COMMAND_BASE;
      return true;
    }
    return false; /* 0 invalid, 1/3 legacy, 8-14 undefined */
  }
}

/* CMIF layer over data words [data_offset, data_end). */
static void parse_cmif(IPC_Request *r, uint32_t data_offset, uint32_t data_end,
                       bool session_is_domain) {
  const uint8_t *b = r->buffer;
  uint32_t header = align_up(data_offset, IPC_DATA_ALIGNMENT);
  uint32_t payload_end = data_end;

  if (r->kind == IPC_MESSAGE_CMIF_REQUEST && session_is_domain) {
    if (header + CMIF_DOMAIN_HEADER_BYTES > data_end) {
      r->framing_error = IPC_RESULT_SF_INVALID_HEADER_SIZE;
      return;
    }
    const uint8_t *domain = b + header;
    r->is_domain_message = true;
    r->domain_request_type = domain[0];
    r->in_object_count = domain[1];
    const uint32_t data_size = rd16(domain + 2);
    r->domain_object_id = rd32(domain + 4);
    header += CMIF_DOMAIN_HEADER_BYTES;
    if (r->domain_request_type == IPC_DOMAIN_CLOSE) return;
    if (r->domain_request_type != IPC_DOMAIN_SEND_MESSAGE) {
      r->framing_error = IPC_RESULT_SF_INVALID_IN_HEADER;
      return;
    }
    const uint32_t objects_offset = header + data_size;
    if (r->in_object_count > IPC_MAX_IN_OBJECTS ||
        objects_offset + r->in_object_count * CMIF_OBJECT_ID_BYTES > data_end) {
      r->framing_error = IPC_RESULT_SF_INVALID_IN_OBJECT;
      return;
    }
    for (uint32_t i = 0; i < r->in_object_count; i++) {
      r->in_objects[i] = rd32(b + objects_offset + i * CMIF_OBJECT_ID_BYTES);
    }
    payload_end = objects_offset;
  }

  if (header + CMIF_HEADER_BYTES > payload_end) {
    r->framing_error = IPC_RESULT_SF_INVALID_HEADER_SIZE;
    return;
  }
  if (rd32(b + header) != IPC_CMIF_IN_MAGIC) {
    r->framing_error = IPC_RESULT_SF_INVALID_IN_HEADER;
    return;
  }
  r->command_id = rd32(b + header + 8);
  r->token = rd32(b + header + 12);
  r->payload_offset = header + CMIF_HEADER_BYTES;
  r->payload_size = payload_end - r->payload_offset;
}

Error ipc_parse_request(IPC_Request *r, bool session_is_domain) {
  if (!r) return ERR(RESULT_INVALID_ARGUMENT, "ipc_parse_request: NULL request");
  memset(r, 0, offsetof(IPC_Request, buffer));
  const uint8_t *b = r->buffer;

#define IPC_NEED(bytes)                                                         \
  do {                                                                          \
    if ((uint64_t)offset + (uint64_t)(bytes) > IPC_COMMAND_BUFFER_BYTES)        \
      return ERR(RESULT_INVALID_ARGUMENT, "ipc_parse_request: message overruns the buffer"); \
  } while (0)

  const uint32_t w0 = rd32(b);
  const uint32_t w1 = rd32(b + 4);
  r->hipc_type = (uint16_t)(w0 & HIPC_TYPE_MASK);
  if (!classify(r->hipc_type, r)) {
    return ERR(RESULT_INVALID_ARGUMENT, "ipc_parse_request: invalid or legacy HIPC type");
  }
  r->static_count = (w0 >> HIPC_SEND_STATICS_SHIFT) & HIPC_COUNT_MASK;
  r->send_count = (w0 >> HIPC_SEND_BUFFERS_SHIFT) & HIPC_COUNT_MASK;
  r->receive_count = (w0 >> HIPC_RECV_BUFFERS_SHIFT) & HIPC_COUNT_MASK;
  r->exchange_count = (w0 >> HIPC_EXCH_BUFFERS_SHIFT) & HIPC_COUNT_MASK;
  const uint32_t data_bytes = (w1 & HIPC_DATA_WORDS_MASK) * 4u;
  const uint32_t recv_static_mode = (w1 >> HIPC_RECV_STATIC_MODE_SHIFT) & HIPC_COUNT_MASK;

  uint32_t offset = HIPC_HEADER_BYTES;
  if (w1 & HIPC_HAS_SPECIAL_HEADER_BIT) {
    IPC_NEED(HIPC_SPECIAL_HEADER_BYTES);
    const uint32_t special = rd32(b + offset);
    offset += HIPC_SPECIAL_HEADER_BYTES;
    r->has_pid = (special & HIPC_SPECIAL_SEND_PID_BIT) != 0;
    r->copy_handle_count = (special >> HIPC_SPECIAL_COPY_SHIFT) & HIPC_COUNT_MASK;
    r->move_handle_count = (special >> HIPC_SPECIAL_MOVE_SHIFT) & HIPC_COUNT_MASK;
    if (r->has_pid) {
      IPC_NEED(HIPC_PID_BYTES);
      r->pid = rd64(b + offset);
      offset += HIPC_PID_BYTES;
    }
    IPC_NEED((r->copy_handle_count + r->move_handle_count) * HIPC_HANDLE_BYTES);
    for (uint32_t i = 0; i < r->copy_handle_count; i++, offset += HIPC_HANDLE_BYTES) {
      r->copy_handles[i] = rd32(b + offset);
    }
    for (uint32_t i = 0; i < r->move_handle_count; i++, offset += HIPC_HANDLE_BYTES) {
      r->move_handles[i] = rd32(b + offset);
    }
  }

  IPC_NEED(r->static_count * HIPC_STATIC_DESCRIPTOR_BYTES);
  for (uint32_t i = 0; i < r->static_count; i++, offset += HIPC_STATIC_DESCRIPTOR_BYTES) {
    decode_static_descriptor(b + offset, &r->statics[i]);
  }
  IPC_NEED((r->send_count + r->receive_count + r->exchange_count) * HIPC_BUFFER_DESCRIPTOR_BYTES);
  for (uint32_t i = 0; i < r->send_count; i++, offset += HIPC_BUFFER_DESCRIPTOR_BYTES) {
    decode_buffer_descriptor(b + offset, &r->sends[i]);
  }
  for (uint32_t i = 0; i < r->receive_count; i++, offset += HIPC_BUFFER_DESCRIPTOR_BYTES) {
    decode_buffer_descriptor(b + offset, &r->receives[i]);
  }
  for (uint32_t i = 0; i < r->exchange_count; i++, offset += HIPC_BUFFER_DESCRIPTOR_BYTES) {
    decode_buffer_descriptor(b + offset, &r->exchanges[i]);
  }

  IPC_NEED(data_bytes);
  const uint32_t data_offset = offset;
  offset += data_bytes;

  if (recv_static_mode == HIPC_RECV_STATIC_MODE_AUTO) {
    r->receive_list_count = 1;
  } else if (recv_static_mode > HIPC_RECV_STATIC_MODE_AUTO) {
    r->receive_list_count = recv_static_mode - HIPC_RECV_STATIC_MODE_AUTO;
  }
  IPC_NEED(r->receive_list_count * HIPC_RECV_LIST_ENTRY_BYTES);
  for (uint32_t i = 0; i < r->receive_list_count; i++, offset += HIPC_RECV_LIST_ENTRY_BYTES) {
    decode_recv_list_entry(b + offset, &r->receive_list[i]);
  }
#undef IPC_NEED

  switch (r->kind) {
  case IPC_MESSAGE_TIPC_REQUEST:
    r->payload_offset = data_offset;
    r->payload_size = data_bytes;
    break;
  case IPC_MESSAGE_CMIF_REQUEST:
  case IPC_MESSAGE_CMIF_CONTROL:
    parse_cmif(r, data_offset, data_offset + data_bytes, session_is_domain);
    break;
  case IPC_MESSAGE_CMIF_CLOSE:
  case IPC_MESSAGE_TIPC_CLOSE:
    break;
  }
  return OK;
}

Error ipc_request_read_bytes(const IPC_Request *request, uint32_t offset, void *out, uint32_t size) {
  if (!request || !out || (uint64_t)offset + size > request->payload_size) {
    return ERR(RESULT_INVALID_ARGUMENT, "ipc_request_read: past the end of the payload");
  }
  memcpy(out, request->buffer + request->payload_offset + offset, size);
  return OK;
}

Error ipc_request_read_u32(const IPC_Request *request, uint32_t offset, uint32_t *out) {
  uint8_t bytes[4];
  const Error err = ipc_request_read_bytes(request, offset, bytes, sizeof(bytes));
  if (error_is_ok(err) && out) *out = rd32(bytes);
  return err;
}

Error ipc_request_read_u64(const IPC_Request *request, uint32_t offset, uint64_t *out) {
  uint8_t bytes[8];
  const Error err = ipc_request_read_bytes(request, offset, bytes, sizeof(bytes));
  if (error_is_ok(err) && out) *out = rd64(bytes);
  return err;
}

/* ------------------------------------------------------------------ */
/* Response building and encoding.                                     */
/* ------------------------------------------------------------------ */

static Error overflow(IPC_Response *response, const char *message) {
  if (response) response->overflowed = true;
  return ERR(RESULT_INVALID_ARGUMENT, message);
}

Error ipc_response_push_bytes(IPC_Response *response, const void *bytes, uint32_t size) {
  if (!response || (!bytes && size)) return ERR(RESULT_INVALID_ARGUMENT, "ipc_response_push: NULL");
  if ((uint64_t)response->data_size + size > sizeof(response->data)) {
    return overflow(response, "ipc_response_push: out data overflow");
  }
  if (size) memcpy(response->data + response->data_size, bytes, size);
  response->data_size += size;
  return OK;
}

Error ipc_response_push_u32(IPC_Response *response, uint32_t value) {
  uint8_t bytes[4];
  wr32(bytes, value);
  return ipc_response_push_bytes(response, bytes, sizeof(bytes));
}

Error ipc_response_push_u64(IPC_Response *response, uint64_t value) {
  uint8_t bytes[8];
  wr32(bytes, (uint32_t)value);
  wr32(bytes + 4, (uint32_t)(value >> 32));
  return ipc_response_push_bytes(response, bytes, sizeof(bytes));
}

Error ipc_response_push_copy_handle(IPC_Response *response, uint32_t handle) {
  if (!response) return ERR(RESULT_INVALID_ARGUMENT, "ipc_response_push: NULL");
  if (response->copy_handle_count >= IPC_MAX_HANDLES) return overflow(response, "copy handle overflow");
  response->copy_handles[response->copy_handle_count++] = handle;
  return OK;
}

Error ipc_response_push_move_handle(IPC_Response *response, uint32_t handle) {
  if (!response) return ERR(RESULT_INVALID_ARGUMENT, "ipc_response_push: NULL");
  if (response->move_handle_count >= IPC_MAX_HANDLES) return overflow(response, "move handle overflow");
  response->move_handles[response->move_handle_count++] = handle;
  return OK;
}

Error ipc_response_push_object(IPC_Response *response, const Service_Interface *interface,
                               uint64_t state) {
  if (!response || !interface) return ERR(RESULT_INVALID_ARGUMENT, "ipc_response_push_object: NULL");
  if (response->out_object_count >= IPC_MAX_OUT_OBJECTS) return overflow(response, "out object overflow");
  response->out_objects[response->out_object_count].interface = interface;
  response->out_objects[response->out_object_count].state = state;
  response->out_object_count++;
  return OK;
}

Error ipc_write_response(const IPC_Request *request, const IPC_Response *response,
                         bool session_is_domain, uint8_t out[IPC_COMMAND_BUFFER_BYTES]) {
  if (!request || !response || !out) return ERR(RESULT_INVALID_ARGUMENT, "ipc_write_response: NULL");
  memset(out, 0, IPC_COMMAND_BUFFER_BYTES);

  const bool has_special = response->copy_handle_count + response->move_handle_count > 0;
  uint32_t offset = HIPC_HEADER_BYTES;
  if (has_special) {
    const uint32_t handles = response->copy_handle_count + response->move_handle_count;
    if (offset + HIPC_SPECIAL_HEADER_BYTES + handles * HIPC_HANDLE_BYTES > IPC_COMMAND_BUFFER_BYTES) {
      return ERR(RESULT_INVALID_ARGUMENT, "ipc_write_response: too many handles");
    }
    wr32(out + offset, (response->copy_handle_count << HIPC_SPECIAL_COPY_SHIFT) |
                           (response->move_handle_count << HIPC_SPECIAL_MOVE_SHIFT));
    offset += HIPC_SPECIAL_HEADER_BYTES;
    for (uint32_t i = 0; i < response->copy_handle_count; i++, offset += HIPC_HANDLE_BYTES) {
      wr32(out + offset, response->copy_handles[i]);
    }
    for (uint32_t i = 0; i < response->move_handle_count; i++, offset += HIPC_HANDLE_BYTES) {
      wr32(out + offset, response->move_handles[i]);
    }
  }

  const uint32_t data_offset = offset;
  uint32_t end;
  if (request->kind == IPC_MESSAGE_TIPC_REQUEST) {
    end = data_offset + 4u + response->data_size;
    if (end > IPC_COMMAND_BUFFER_BYTES) return ERR(RESULT_INVALID_ARGUMENT, "ipc_write_response: too large");
    wr32(out + data_offset, response->result);
    memcpy(out + data_offset + 4u, response->data, response->data_size);
  } else {
    const bool domain_frame = session_is_domain && request->is_domain_message;
    const uint32_t object_count = domain_frame ? response->out_object_count : 0u;
    uint32_t p = align_up(data_offset, IPC_DATA_ALIGNMENT);
    end = p + (domain_frame ? CMIF_DOMAIN_HEADER_BYTES : 0u) + CMIF_HEADER_BYTES +
          response->data_size + object_count * CMIF_OBJECT_ID_BYTES;
    if (end > IPC_COMMAND_BUFFER_BYTES) return ERR(RESULT_INVALID_ARGUMENT, "ipc_write_response: too large");
    if (domain_frame) {
      wr32(out + p, object_count);
      p += CMIF_DOMAIN_HEADER_BYTES;
    }
    wr32(out + p, IPC_CMIF_OUT_MAGIC);
    wr32(out + p + 4, 0); /* version */
    wr32(out + p + 8, response->result);
    wr32(out + p + 12, request->token);
    p += CMIF_HEADER_BYTES;
    memcpy(out + p, response->data, response->data_size);
    p += response->data_size;
    for (uint32_t i = 0; i < object_count; i++, p += CMIF_OBJECT_ID_BYTES) {
      wr32(out + p, response->out_object_ids[i]);
    }
  }

  const uint32_t data_words = (end - data_offset + 3u) / 4u;
  wr32(out, HIPC_RESPONSE_TYPE);
  wr32(out + 4, data_words | (has_special ? HIPC_HAS_SPECIAL_HEADER_BIT : 0u));
  return OK;
}

/* ------------------------------------------------------------------ */
/* Sessions.                                                           */
/* ------------------------------------------------------------------ */

void ipc_session_pool_init(IPC_Session_Pool *pool) {
  if (pool) memset(pool, 0, sizeof(*pool));
}

IPC_Session *ipc_session_pool_open(IPC_Session_Pool *pool, const Service_Interface *interface,
                                   uint64_t state) {
  if (!pool || !interface) return NULL;
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) {
    IPC_Session *session = &pool->sessions[i];
    if (session->in_use) continue;
    memset(session, 0, sizeof(*session));
    session->in_use = true;
    session->objects[0].interface = interface;
    session->objects[0].state = state;
    pool->live_count++;
    return session;
  }
  return NULL;
}

static void close_object(Service_Object *object) {
  const Service_Interface *interface = object->interface;
  if (interface && interface->on_close) interface->on_close(interface->service_state, object->state);
  memset(object, 0, sizeof(*object));
}

void ipc_session_pool_close(IPC_Session_Pool *pool, IPC_Session *session) {
  if (!pool || !session || !session->in_use) return;
  for (uint32_t i = 0; i < IPC_DOMAIN_MAX_OBJECTS; i++) {
    if (session->objects[i].interface) close_object(&session->objects[i]);
  }
  memset(session, 0, sizeof(*session));
  pool->live_count--;
}

void ipc_session_close_server(IPC_Session *session) {
  if (!session || !session->in_use) return;
  for (uint32_t i = 0; i < IPC_DOMAIN_MAX_OBJECTS; i++) {
    if (session->objects[i].interface) close_object(&session->objects[i]);
  }
  session->server_closed = true;
}

uint32_t ipc_open_session_handle(HLE_Context *context, const Service_Interface *interface,
                                 uint64_t state, uint32_t *out_handle) {
  IPC_Session *session = ipc_session_pool_open(context->sessions, interface, state);
  if (!session) return HLE_RESULT_OUT_OF_SESSIONS;
  const Error err = handle_table_add(&context->process->handles, KERNEL_OBJECT_SESSION, session,
                                     out_handle);
  if (!error_is_ok(err)) {
    ipc_session_pool_close(context->sessions, session);
    return HLE_RESULT_OUT_OF_HANDLES;
  }
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Dispatch.                                                           */
/* ------------------------------------------------------------------ */

static const Service_Command *find_command(const Service_Interface *interface, uint32_t id) {
  size_t low = 0;
  size_t high = interface->command_count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2u;
    const uint32_t mid_id = interface->commands[mid].command_id;
    if (mid_id == id) return &interface->commands[mid];
    if (mid_id < id) low = mid + 1u;
    else high = mid;
  }
  return NULL;
}

static void clear_outputs(IPC_Response *response) {
  response->data_size = 0;
  response->copy_handle_count = 0;
  response->move_handle_count = 0;
  response->out_object_count = 0;
}

/* Undo handles a failed reply will never deliver. Move handles a handler
 * pushed were created for this reply, so the reply owned them. */
static void close_handles(HLE_Context *context, const uint32_t *handles, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    Kernel_Object_Type type = KERNEL_OBJECT_NONE;
    void *object = NULL;
    if (!error_is_ok(handle_table_remove(&context->process->handles, handles[i], &type, &object))) {
      continue;
    }
    if (type == KERNEL_OBJECT_SESSION) ipc_session_pool_close(context->sessions, (IPC_Session *)object);
  }
}

/* Out objects [from, count) that never became guest-visible: their
 * interfaces release whatever the handler allocated for them. */
static void drop_unplaced_objects(const IPC_Response *response, uint32_t from) {
  for (uint32_t i = from; i < response->out_object_count; i++) {
    const Service_Interface *interface = response->out_objects[i].interface;
    if (interface && interface->on_close) interface->on_close(interface->service_state, response->out_objects[i].state);
  }
}

/* Turns handler-returned objects into domain ids or session handles.
 * All-or-nothing: on failure every object made so far is undone. */
static void materialize_objects(HLE_Context *context, IPC_Session *session, bool domain,
                                IPC_Response *response) {
  const uint32_t count = response->out_object_count;
  if (count == 0) return;

  if (domain) {
    uint32_t assigned = 0;
    for (uint32_t slot = 1; slot < IPC_DOMAIN_MAX_OBJECTS && assigned < count; slot++) {
      if (session->objects[slot].interface) continue;
      session->objects[slot].interface = response->out_objects[assigned].interface;
      session->objects[slot].state = response->out_objects[assigned].state;
      response->out_object_ids[assigned] = slot + 1u;
      assigned++;
    }
    if (assigned < count) {
      for (uint32_t i = 0; i < assigned; i++) {
        close_object(&session->objects[response->out_object_ids[i] - 1u]);
      }
      drop_unplaced_objects(response, assigned);
      clear_outputs(response);
      response->result = IPC_RESULT_SF_OUT_OF_DOMAIN_ENTRIES;
    }
    return;
  }

  if (count + response->move_handle_count > IPC_MAX_HANDLES) {
    drop_unplaced_objects(response, 0);
    clear_outputs(response);
    response->result = IPC_RESULT_SF_INVALID_OUT_RAW_SIZE;
    return;
  }
  uint32_t object_handles[IPC_MAX_OUT_OBJECTS];
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t result = ipc_open_session_handle(context, response->out_objects[i].interface,
                                                    response->out_objects[i].state,
                                                    &object_handles[i]);
    if (result != HLE_RESULT_SUCCESS) {
      close_handles(context, object_handles, i);
      drop_unplaced_objects(response, i);
      clear_outputs(response);
      response->result = result;
      return;
    }
  }
  /* Objects first, then the handler's own move handles (libnx order). */
  memmove(response->move_handles + count, response->move_handles,
          response->move_handle_count * sizeof(uint32_t));
  memcpy(response->move_handles, object_handles, count * sizeof(uint32_t));
  response->move_handle_count += count;
}

static void dispatch_control(HLE_Context *context, IPC_Session *session,
                             const IPC_Request *request, IPC_Response *response) {
  switch (request->command_id) {
  case IPC_CONTROL_CONVERT_CURRENT_OBJECT_TO_DOMAIN:
    /* objects[0] is already domain object id 1 in both layouts. */
    session->is_domain = true;
    (void)ipc_response_push_u32(response, 1u);
    return;
  case IPC_CONTROL_QUERY_POINTER_BUFFER_SIZE: {
    /* The Nintendo SDK sends pointer (X/C) buffers only when the server
     * has a pointer buffer large enough; a 0 answer makes its fixed-X
     * commands fail client-side (hid's SetSupportedNpadIdType). Handlers
     * accept pointer and mapped buffers alike (service_util.h). */
    const uint16_t declared = session->objects[0].interface->pointer_buffer_size;
    const uint16_t size = declared ? declared : IPC_DEFAULT_POINTER_BUFFER_SIZE;
    const uint8_t bytes[2] = {(uint8_t)size, (uint8_t)(size >> 8)};
    (void)ipc_response_push_bytes(response, bytes, sizeof(bytes));
    return;
  }
  case IPC_CONTROL_CLONE_CURRENT_OBJECT:
  case IPC_CONTROL_CLONE_CURRENT_OBJECT_EX: {
    uint32_t handle = HANDLE_INVALID;
    const uint32_t result = ipc_open_session_handle(context, session->objects[0].interface,
                                                    session->objects[0].state, &handle);
    if (result != HLE_RESULT_SUCCESS) {
      response->result = result;
      return;
    }
    /* A clone of a domain session shares the domain on hardware; here
     * the clone starts as a copy of the object table. Nothing in Phase 1
     * clones a domain session. */
    IPC_Session *clone = (IPC_Session *)handle_table_get(&context->process->handles, handle,
                                                         KERNEL_OBJECT_SESSION);
    clone->is_domain = session->is_domain;
    memcpy(clone->objects, session->objects, sizeof(clone->objects));
    (void)ipc_response_push_move_handle(response, handle);
    return;
  }
  default:
    log_warn("[ipc] %s: unimplemented control command %u", session->objects[0].interface->name,
             (unsigned)request->command_id);
    response->result = IPC_RESULT_SF_UNKNOWN_COMMAND;
    return;
  }
}

uint32_t ipc_dispatch(HLE_Context *context, IPC_Session *session, const IPC_Request *request,
                      IPC_Response *response) {
  memset(response, 0, sizeof(*response));

  if (request->kind == IPC_MESSAGE_CMIF_CLOSE || request->kind == IPC_MESSAGE_TIPC_CLOSE) {
    return HLE_RESULT_CONNECTION_CLOSED;
  }
  if (request->framing_error) {
    response->result = request->framing_error;
    return HLE_RESULT_SUCCESS;
  }
  if (request->kind == IPC_MESSAGE_CMIF_CONTROL) {
    dispatch_control(context, session, request, response);
    if (response->result != HLE_RESULT_SUCCESS) clear_outputs(response);
    return HLE_RESULT_SUCCESS;
  }

  Service_Object *target = &session->objects[0];
  if (request->is_domain_message) {
    const uint32_t id = request->domain_object_id;
    if (id == 0 || id > IPC_DOMAIN_MAX_OBJECTS || !session->objects[id - 1u].interface) {
      response->result = IPC_RESULT_SF_TARGET_NOT_FOUND;
      return HLE_RESULT_SUCCESS;
    }
    target = &session->objects[id - 1u];
    if (request->domain_request_type == IPC_DOMAIN_CLOSE) {
      close_object(target);
      return HLE_RESULT_SUCCESS;
    }
  }

  const Service_Interface *interface = target->interface;
  const Service_Command *command = find_command(interface, request->command_id);
  if (!command) {
    /* §12 unimplemented-surface policy: log, never silent success. */
    log_warn("[ipc] %s: unimplemented command %u", interface->name, (unsigned)request->command_id);
    response->result = IPC_RESULT_SF_UNKNOWN_COMMAND;
    return HLE_RESULT_SUCCESS;
  }

  log_debug("[ipc] %s:%s", interface->name, command->name);
  response->result = command->handler(context, target, request, response);
  if (response->overflowed) {
    log_error("[ipc] %s:%s overflowed its reply", interface->name, command->name);
    close_handles(context, response->move_handles, response->move_handle_count);
    drop_unplaced_objects(response, 0);
    clear_outputs(response);
    response->result = IPC_RESULT_SF_INVALID_OUT_RAW_SIZE;
    return HLE_RESULT_SUCCESS;
  }
  if (response->result != HLE_RESULT_SUCCESS) {
    close_handles(context, response->move_handles, response->move_handle_count);
    drop_unplaced_objects(response, 0);
    clear_outputs(response);
    return HLE_RESULT_SUCCESS;
  }
  materialize_objects(context, session, session->is_domain && request->is_domain_message, response);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Named ports.                                                        */
/* ------------------------------------------------------------------ */

const Service_Interface *ipc_find_named_port(const HLE_Context *context, const char *name) {
  if (!context || !name || !context->sm) return NULL;
  if (strcmp(name, SM_PORT_NAME) == 0) return &context->sm->interface;
  return NULL;
}

Service_Object *ipc_request_in_object(Service_Object *self, const IPC_Request *request, uint32_t index) {
  if (!self || !request || !request->is_domain_message || index >= request->in_object_count) return NULL;
  const uint32_t self_id = request->domain_object_id, id = request->in_objects[index];
  if (self_id == 0 || self_id > IPC_DOMAIN_MAX_OBJECTS || id == 0 || id > IPC_DOMAIN_MAX_OBJECTS) return NULL;
  Service_Object *object = self - (self_id - 1u) + (id - 1u);
  return object->interface ? object : NULL;
}
