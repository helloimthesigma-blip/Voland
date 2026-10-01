/**
 * IPC: HIPC transport, CMIF/TIPC framing, sessions, domains and the C
 * service-object pattern (§12 "The call surface: SVC -> HIPC -> CMIF").
 * PROPOSED HEADER - awaiting maintainer review; no implementation exists
 * yet. Phase 1, §25 "Minimal IPC + sm: stub".
 *
 * Wire formats verified against libnx's sf/hipc.h, sf/cmif.h and
 * sf/tipc.h (the SDK every homebrew links - published protocol, not
 * another emulator's implementation; CLAUDE.md rule 2).
 *
 * ------------------------------------------------------------------
 * Data flow for one SendSyncRequest
 * ------------------------------------------------------------------
 *   1. svc_ipc.c reads the caller's 0x100-byte command buffer out of its
 *      TLS block (tpidrro_el0) with ONE vmm_read_block into a host copy.
 *      Nothing below ever holds a pointer into guest RAM.
 *   2. ipc_parse_request() decodes that copy: HIPC header, special header
 *      (PID, copy/move handles), X/A/B/W descriptors, raw data, C list;
 *      then, by HIPC type, the CMIF header (SFCI) - preceded by the
 *      domain header when the session is a domain - or TIPC's bare data.
 *   3. ipc_dispatch() routes: Close, Control (domain conversion etc.), or
 *      a command - looked up by bsearch in the target object's
 *      Service_Interface - and runs the handler with an IPC_Response.
 *   4. ipc_write_response() encodes the reply into a host copy, and
 *      svc_ipc.c writes it back with ONE vmm_write_block.
 * Buffer descriptors stay guest VAs + sizes in IPC_Request; a handler
 * reaches buffer contents only through vmm (§12: "a descriptor is
 * untrusted input, never a raw pointer").
 *
 * ------------------------------------------------------------------
 * Framing is per message, not per session
 * ------------------------------------------------------------------
 * HIPC type 4/6 = CMIF request, 5/7 = CMIF control, 2 = CMIF close,
 * >= 16 = TIPC request with command id (type - 16), 15 = TIPC close.
 * libnx itself mixes CMIF and TIPC on ONE sm: session (RegisterClient is
 * always CMIF; on 12.0.0+ other sm: commands may be TIPC), and
 * Nintendo's SDK uses TIPC for sm: since 12.0.0, so a session accepts
 * both and every response mirrors its request's framing. Types 1/3
 * (legacy) and 0 are rejected.
 *
 * ------------------------------------------------------------------
 * Domains from day one (§12)
 * ------------------------------------------------------------------
 * Every session carries an object table. Before ConvertCurrentObjectToDomain
 * the session's one object is `objects[0]`; conversion assigns it domain
 * object id 1 (Horizon's first id) and from then on requests are routed
 * by the domain header's object id, new objects returned by a handler
 * get the next free id (written after the out data) instead of a new
 * session handle, and a domain Close message frees one id. Non-domain
 * sessions return new objects as move handles of fresh sessions.
 *
 * ------------------------------------------------------------------
 * Deviations from §12, stated for review
 * ------------------------------------------------------------------
 *  (a) Service_Command_Fn takes the HLE_Context as a first parameter.
 *      §12's (self, req, res) shape has no way to reach the session pool
 *      or handle table, which any command returning an object (sm:
 *      GetServiceHandle is the very first) must touch.
 *  (b) Service_Object carries a `state` word instead of being a struct a
 *      service extends. Without malloc, per-object state must live in
 *      the fixed session pool; one 64-bit word (an index into a
 *      service-owned fixed table, or a flag set) covers sm: and every
 *      service on the §12 priority list examined so far. Shared,
 *      service-global state hangs off Service_Interface.service_state.
 *  (c) Unknown command ids answer the sf result UnknownCommandId
 *      (IPC_RESULT_SF_UNKNOWN_COMMAND, 0x1BA0A: module 10, description
 *      221) inside the CMIF/TIPC response rather than a kernel
 *      NotImplemented: it is what a real service returns, so a title's
 *      own error handling sees the value it was written against. The
 *      §12 policy (log + trace + never silent success) is unchanged.
 */
#ifndef SWITCH_HLE_KERNEL_IPC_H
#define SWITCH_HLE_KERNEL_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/result.h"

typedef struct HLE_Context HLE_Context;

/* ------------------------------------------------------------------ */
/* Limits.                                                             */
/* ------------------------------------------------------------------ */

#define IPC_COMMAND_BUFFER_BYTES 0x100u /* TLS_IPC_COMMAND_BUFFER_BYTES (tls.h) */
#define IPC_MAX_HANDLES 15u             /* 4-bit copy/move counts */
#define IPC_MAX_BUFFERS 15u             /* 4-bit X/A/B/W counts */
#define IPC_MAX_RECV_LIST 13u           /* C descriptors; recv_static_mode - 2 */
#define IPC_MAX_IN_OBJECTS 8u           /* domain in-object ids per request */
#define IPC_MAX_OUT_OBJECTS 8u          /* objects a handler may return */
#define IPC_DOMAIN_MAX_OBJECTS 64u      /* per session, including id 1 */
#define IPC_MAX_SESSIONS 256u           /* emulator-wide session pool */

/* ------------------------------------------------------------------ */
/* Wire constants (libnx sf/hipc.h, sf/cmif.h, sf/tipc.h).             */
/* ------------------------------------------------------------------ */

#define IPC_CMIF_IN_MAGIC ((uint32_t)0x49434653u)  /* "SFCI" */
#define IPC_CMIF_OUT_MAGIC ((uint32_t)0x4F434653u) /* "SFCO" */
#define IPC_DATA_ALIGNMENT 16u /* CMIF payload aligned to 16 from buffer start */

typedef enum IPC_Hipc_Type {
  IPC_HIPC_TYPE_INVALID = 0,
  IPC_HIPC_TYPE_LEGACY_REQUEST = 1,
  IPC_HIPC_TYPE_CLOSE = 2,
  IPC_HIPC_TYPE_LEGACY_CONTROL = 3,
  IPC_HIPC_TYPE_REQUEST = 4,
  IPC_HIPC_TYPE_CONTROL = 5,
  IPC_HIPC_TYPE_REQUEST_WITH_CONTEXT = 6,
  IPC_HIPC_TYPE_CONTROL_WITH_CONTEXT = 7,
  IPC_HIPC_TYPE_TIPC_CLOSE = 15,
  IPC_HIPC_TYPE_TIPC_COMMAND_BASE = 16, /* type = 16 + command id */
} IPC_Hipc_Type;

typedef enum IPC_Domain_Request_Type {
  IPC_DOMAIN_SEND_MESSAGE = 1,
  IPC_DOMAIN_CLOSE = 2,
} IPC_Domain_Request_Type;

/* CMIF control commands (HIPC type 5/7). */
typedef enum IPC_Control_Command {
  IPC_CONTROL_CONVERT_CURRENT_OBJECT_TO_DOMAIN = 0,
  IPC_CONTROL_COPY_FROM_CURRENT_DOMAIN = 1, /* Phase 1: not implemented */
  IPC_CONTROL_CLONE_CURRENT_OBJECT = 2,
  IPC_CONTROL_QUERY_POINTER_BUFFER_SIZE = 3,
  IPC_CONTROL_CLONE_CURRENT_OBJECT_EX = 4,
} IPC_Control_Command;

/* sf (module 10) results a service framework returns inside a reply. */
#define IPC_SF_MODULE 10u
#define IPC_RESULT_SF_INVALID_HEADER_SIZE ((uint32_t)((202u << 9) | IPC_SF_MODULE))
#define IPC_RESULT_SF_INVALID_IN_HEADER ((uint32_t)((211u << 9) | IPC_SF_MODULE))
#define IPC_RESULT_SF_UNKNOWN_COMMAND ((uint32_t)((221u << 9) | IPC_SF_MODULE)) /* 0x1BA0A */
#define IPC_RESULT_SF_INVALID_IN_OBJECT ((uint32_t)((239u << 9) | IPC_SF_MODULE))
#define IPC_RESULT_SF_TARGET_NOT_FOUND ((uint32_t)((261u << 9) | IPC_SF_MODULE))
#define IPC_RESULT_SF_OUT_OF_DOMAIN_ENTRIES ((uint32_t)((301u << 9) | IPC_SF_MODULE))

/* ------------------------------------------------------------------ */
/* Parsed request.                                                     */
/* ------------------------------------------------------------------ */

typedef enum IPC_Message_Kind {
  IPC_MESSAGE_CMIF_REQUEST, /* HIPC 4 / 6 */
  IPC_MESSAGE_CMIF_CONTROL, /* HIPC 5 / 7 */
  IPC_MESSAGE_CMIF_CLOSE,   /* HIPC 2 */
  IPC_MESSAGE_TIPC_REQUEST, /* HIPC >= 16 */
  IPC_MESSAGE_TIPC_CLOSE,   /* HIPC 15 */
} IPC_Message_Kind;

/* One X (static), A (send), B (receive), W (exchange) or C (receive
 * list) descriptor, decoded to a full 39-bit+ guest VA. */
typedef struct IPC_Buffer {
  uint64_t gva;
  uint64_t size;
  uint8_t mode;  /* A/B/W: HipcBufferMode; X: unused */
  uint8_t index; /* X: static index the receiver matches against C; else 0 */
} IPC_Buffer;

typedef struct IPC_Request {
  IPC_Message_Kind kind;
  uint16_t hipc_type;
  uint32_t command_id; /* CMIF header / TIPC type-16 / control command */
  uint32_t token;      /* CMIF only; echoed */

  bool has_pid;
  uint64_t pid; /* as sent; the kernel overwrites it - see ipc.c note */

  uint32_t copy_handles[IPC_MAX_HANDLES];
  uint32_t copy_handle_count;
  uint32_t move_handles[IPC_MAX_HANDLES];
  uint32_t move_handle_count;

  IPC_Buffer statics[IPC_MAX_BUFFERS]; /* X */
  uint32_t static_count;
  IPC_Buffer sends[IPC_MAX_BUFFERS];   /* A */
  uint32_t send_count;
  IPC_Buffer receives[IPC_MAX_BUFFERS]; /* B */
  uint32_t receive_count;
  IPC_Buffer exchanges[IPC_MAX_BUFFERS]; /* W */
  uint32_t exchange_count;
  IPC_Buffer receive_list[IPC_MAX_RECV_LIST]; /* C */
  uint32_t receive_list_count;

  /* Domain routing (only when the session is a domain). */
  bool is_domain_message;
  uint8_t domain_request_type; /* IPC_Domain_Request_Type */
  uint32_t domain_object_id;
  uint32_t in_objects[IPC_MAX_IN_OBJECTS];
  uint32_t in_object_count;

  /* Raw arguments after the SFCI header (CMIF) or the HIPC data start
   * (TIPC), as an offset/size into `buffer`. Read with ipc_request_*. */
  uint32_t payload_offset;
  uint32_t payload_size;

  uint8_t buffer[IPC_COMMAND_BUFFER_BYTES]; /* the host copy of the TLS buffer */
} IPC_Request;

/* Decodes `request->buffer` (filled by the caller) in place. Bounds are
 * enforced against IPC_COMMAND_BUFFER_BYTES everywhere - a request is
 * untrusted input.
 *   RESULT_INVALID_ARGUMENT legacy/invalid HIPC type; counts, data size
 *                           or in-object count overrunning the buffer;
 *                           CMIF magic missing; unknown domain request
 * `session_is_domain` selects whether a CMIF request is preceded by a
 * domain header - the wire format cannot say, only the session knows.
 * svc_ipc.c turns a parse failure into a kernel-level failure in W0
 * (HLE_RESULT_INVALID_STATE) without writing a reply. */
Error ipc_parse_request(IPC_Request *request, bool session_is_domain);

/* Bounds-checked payload readers: RESULT_INVALID_ARGUMENT past the end. */
Error ipc_request_read_u32(const IPC_Request *request, uint32_t offset, uint32_t *out);
Error ipc_request_read_u64(const IPC_Request *request, uint32_t offset, uint64_t *out);
Error ipc_request_read_bytes(const IPC_Request *request, uint32_t offset, void *out, uint32_t size);

/* ------------------------------------------------------------------ */
/* Response.                                                           */
/* ------------------------------------------------------------------ */

typedef struct Service_Object Service_Object;

typedef struct IPC_Response {
  uint32_t result; /* the service Result in SFCO / TIPC word 0 */

  uint8_t data[IPC_COMMAND_BUFFER_BYTES];
  uint32_t data_size;

  uint32_t copy_handles[IPC_MAX_HANDLES];
  uint32_t copy_handle_count;
  uint32_t move_handles[IPC_MAX_HANDLES];
  uint32_t move_handle_count;

  /* Objects a handler returns; ipc_dispatch turns each into a domain id
   * or a new session's move handle depending on the session. */
  Service_Object *out_objects[IPC_MAX_OUT_OBJECTS];
  uint32_t out_object_count;
} IPC_Response;

/* Append raw out data / handles. RESULT_INVALID_ARGUMENT on overflow;
 * a handler that overflows is a bug, and ipc_dispatch turns it into
 * IPC_RESULT_SF_INVALID_HEADER_SIZE rather than a truncated reply. */
Error ipc_response_push_u32(IPC_Response *response, uint32_t value);
Error ipc_response_push_u64(IPC_Response *response, uint64_t value);
Error ipc_response_push_bytes(IPC_Response *response, const void *bytes, uint32_t size);
Error ipc_response_push_copy_handle(IPC_Response *response, uint32_t handle);
Error ipc_response_push_move_handle(IPC_Response *response, uint32_t handle);

/* Return a new service object (e.g. sm: GetServiceHandle's session). The
 * response does not own `object`; it must come from
 * ipc_session_pool_new_object() for the duration of the dispatch. */
Error ipc_response_push_object(IPC_Response *response, Service_Object *object);

/* Encodes the reply for `request`'s framing into `out` (one TLS command
 * buffer). Pure: no vmm, no handle table. Writes HIPC header + special
 * header (if handles) + data; for CMIF the SFCO header (with the domain
 * out header and out object ids when `session_is_domain`), for TIPC the
 * result word then data. */
Error ipc_write_response(const IPC_Request *request, const IPC_Response *response,
                         bool session_is_domain, uint8_t out[IPC_COMMAND_BUFFER_BYTES]);

/* ------------------------------------------------------------------ */
/* The C service-object pattern (§12).                                 */
/* ------------------------------------------------------------------ */

typedef uint32_t HLE_ServiceResult; /* a Horizon Result: (description << 9) | module */

typedef HLE_ServiceResult (*Service_Command_Fn)(HLE_Context *context, Service_Object *self,
                                                const IPC_Request *request,
                                                IPC_Response *response);

typedef struct Service_Command {
  uint32_t command_id;
  Service_Command_Fn handler;
  const char *name; /* for trace/coverage output; "_stub" suffix marks an allowlisted stub (§12) */
} Service_Command;

typedef struct Service_Interface {
  const char *name;                /* "sm:", "IFileSystemProxy", ... */
  const Service_Command *commands; /* sorted by command_id ascending; bsearch */
  size_t command_count;
  uint16_t pointer_buffer_size;    /* QueryPointerBufferSize answer */
  void *service_state;             /* service-global, not per object; may be NULL */
} Service_Interface;

/* One live object: an interface plus one word of per-object state. */
struct Service_Object {
  const Service_Interface *interface; /* NULL = free */
  uint64_t state;
};

/* ------------------------------------------------------------------ */
/* Sessions.                                                           */
/* ------------------------------------------------------------------ */

typedef struct IPC_Session {
  bool in_use;
  bool is_domain;
  /* Non-domain: objects[0] is the session's object. Domain: objects[id-1]
   * for domain object id `id` in [1, IPC_DOMAIN_MAX_OBJECTS]. */
  Service_Object objects[IPC_DOMAIN_MAX_OBJECTS];
} IPC_Session;

typedef struct IPC_Session_Pool {
  IPC_Session sessions[IPC_MAX_SESSIONS];
  uint32_t live_count;
} IPC_Session_Pool;

void ipc_session_pool_init(IPC_Session_Pool *pool);

/* A fresh non-domain session holding one object of `interface`. NULL when
 * the pool is full (svc layer: HLE_RESULT_OUT_OF_SESSIONS). */
IPC_Session *ipc_session_pool_open(IPC_Session_Pool *pool, const Service_Interface *interface,
                                   uint64_t state);

/* Frees the session and every object in it. */
void ipc_session_pool_close(IPC_Session_Pool *pool, IPC_Session *session);

/* Scratch object for ipc_response_push_object: a handler fills one in
 * and returns it; ipc_dispatch then copies it into its final home (a
 * domain slot or a new session). Lives in the pool so no handler needs
 * storage of its own. NULL if more than IPC_MAX_OUT_OBJECTS are requested
 * in one dispatch. */
Service_Object *ipc_session_pool_new_object(IPC_Session_Pool *pool,
                                            const Service_Interface *interface, uint64_t state);

/* ------------------------------------------------------------------ */
/* Dispatch.                                                           */
/* ------------------------------------------------------------------ */

/* Runs one parsed request against `session` and fills `response`.
 * Returns the KERNEL result SendSyncRequest reports in W0:
 *   HLE_RESULT_SUCCESS          a reply was produced (the service's own
 *                               Result, success or not, is in
 *                               response->result)
 *   HLE_RESULT_CONNECTION_CLOSED request was a Close (CMIF 2 / TIPC 15):
 *                               no reply; the caller closes the handle
 * Out objects are materialized here: domain -> next free object id,
 * else -> a new session added to the process handle table, its handle
 * pushed as a move handle. Allocation failures become sf results
 * (OUT_OF_DOMAIN_ENTRIES) in the reply, never a partial reply.
 * Unknown command / domain object id: §12 policy - log_warn with the
 * interface name and command id, IPC_RESULT_SF_UNKNOWN_COMMAND or
 * IPC_RESULT_SF_TARGET_NOT_FOUND in the reply. (TRACE_UNIMPL_CMD is
 * emitted once the §23 trace buffer API exists; it does not yet.) */
uint32_t ipc_dispatch(HLE_Context *context, IPC_Session *session, const IPC_Request *request,
                      IPC_Response *response);

/* ------------------------------------------------------------------ */
/* Named ports (ConnectToNamedPort).                                   */
/* ------------------------------------------------------------------ */

#define IPC_PORT_NAME_MAX_BYTES 12u /* including the NUL; Horizon's limit */

/* The interface a named port's sessions start with, or NULL. Phase 1
 * knows exactly one port: "sm:" -> &context->sm->interface (sm.h). The
 * kernel layer reaches the sm: instance only through HLE_Context. */
const Service_Interface *ipc_find_named_port(const HLE_Context *context, const char *name);

#endif /* SWITCH_HLE_KERNEL_IPC_H */
