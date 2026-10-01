/**
 * sm: - the service manager (§12 priority list item 1). Phase 1, §25
 * "Minimal IPC + sm: stub".
 *
 * Every other service is reached through sm: GetServiceHandle, so this is
 * the first Service_Interface (ipc.h) and the template the
 * /new-hle-service scaffolding (§12) will be cut from.
 *
 * Commands, numbered per libnx's services/sm.c (CMIF) - the same ids are
 * valid over TIPC (HIPC type 16 + id), and one session may mix both
 * framings (ipc.h):
 *   0 RegisterClient     in: PID (send-pid flag; the value itself is
 *                        replaced by the kernel)        -> marks the
 *                        session's object registered (state bit 0)
 *   1 GetServiceHandle   in: u64 name (8 ASCII bytes, NUL-padded)
 *                        out: move handle to a new session on the
 *                        registered HLE interface
 *   2 RegisterService    sysmodules only - absent from the table, so the
 *   3 UnregisterService  unknown-command policy logs them
 * (4 DetachClient, 11.0.0+, is not in the table: unknown-command policy.)
 *
 * Results, module 21 (Atmosphère's sm_results.hpp; the numbering
 * Nintendo's sm uses): see SM_RESULT_* below. Behavior:
 *   - GetServiceHandle before RegisterClient -> SM_RESULT_INVALID_CLIENT.
 *   - A name that is empty or has a non-NUL after a NUL ->
 *     SM_RESULT_INVALID_SERVICE_NAME (Atmosphère's sm validation; no
 *     character-class check).
 *   - A well-formed name with no registered HLE interface ->
 *     SM_RESULT_NOT_REGISTERED, logged with the name. DEVIATION, stated:
 *     real sm defers the reply until the service registers (the caller
 *     blocks). Under HLE every service that will ever exist is registered
 *     at boot, so waiting can never end; answering NotRegistered
 *     immediately turns a silent hang into a logged, visible failure.
 *   - No access-control check against main.npdm's service_access_control
 *     (SM_RESULT_NOT_ALLOWED is defined but not yet returned).
 *     DEVIATION, stated: enforcing it guards against a hostile title,
 *     which a user's own dump is not (§12 loader note's reasoning); wire
 *     it when a title is found that probes it.
 *
 * Registry. Fixed-size name -> interface table owned by the emulator;
 * services register themselves at emulator_create time. In Phase 1 the
 * table is empty in shipping builds (no service beyond sm: exists yet),
 * so every GetServiceHandle answers NotRegistered - which is exactly the
 * observable state a Phase 2 homebrew needs to see before fsp-srv & co.
 * land. Tests register a dummy interface to prove the session path.
 */
#ifndef SWITCH_HLE_SERVICES_SM_SM_H
#define SWITCH_HLE_SERVICES_SM_SM_H

#include <stdint.h>

#include "common/result.h"
#include "hle/kernel/ipc.h"

#define SM_MODULE 21u
#define SM_RESULT_INVALID_CLIENT ((uint32_t)((2u << 9) | SM_MODULE))
#define SM_RESULT_INVALID_SERVICE_NAME ((uint32_t)((6u << 9) | SM_MODULE))
#define SM_RESULT_NOT_REGISTERED ((uint32_t)((7u << 9) | SM_MODULE))
#define SM_RESULT_NOT_ALLOWED ((uint32_t)((8u << 9) | SM_MODULE))

#define SM_PORT_NAME "sm:"
#define SM_SERVICE_NAME_BYTES 8u
#define SM_REGISTRY_CAPACITY 128u

typedef enum SM_Command {
  SM_COMMAND_REGISTER_CLIENT = 0,
  SM_COMMAND_GET_SERVICE_HANDLE = 1,
  SM_COMMAND_REGISTER_SERVICE = 2,
  SM_COMMAND_UNREGISTER_SERVICE = 3,
} SM_Command;

/* Bit in Service_Object.state for an sm: session object. */
#define SM_OBJECT_STATE_CLIENT_REGISTERED ((uint64_t)1)

typedef struct SM_Registry_Entry {
  uint64_t name; /* the 8-byte wire form, little-endian, NUL-padded */
  const Service_Interface *interface;
} SM_Registry_Entry;

typedef struct SM_Registry {
  SM_Registry_Entry entries[SM_REGISTRY_CAPACITY];
  uint32_t count;
  /* The sm: interface itself: the static command table with
   * service_state pointing back at this registry. No global state - two
   * Emulators (tests) get two independent sm: instances. */
  Service_Interface interface;
} SM_Registry;

/* Empties the registry and initializes `registry->interface`. */
void sm_registry_init(SM_Registry *registry);

/* Register an HLE service under `name` (1-8 chars).
 *   RESULT_INVALID_ARGUMENT bad name / NULL
 *   RESULT_OUT_OF_MEMORY    registry full
 *   (re-registering a name replaces nothing: RESULT_INVALID_ARGUMENT) */
Error sm_registry_add(SM_Registry *registry, const char *name, const Service_Interface *interface);

/* The interface for a wire-form name, or NULL. */
const Service_Interface *sm_registry_find(const SM_Registry *registry, uint64_t name);

/* Name validation exactly as GetServiceHandle applies it. Exposed for
 * tests and for sm_registry_add. */
bool sm_service_name_is_valid(uint64_t name);

#endif /* SWITCH_HLE_SERVICES_SM_SM_H */
