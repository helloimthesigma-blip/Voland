#include "hle/services/network/network.h"

#include "hle/services/service_util.h"

#define BSD_FIRST_SOCKET_CALL 2u
#define BSD_LAST_SOCKET_CALL 31u
#define BSD_CLOSE 26u
#define NIFM_REQUEST_STATE_FREE 1u
#define NIFM_CLIENT_ID 1u

static Network_State *state_of(Service_Object *self) { return (Network_State *)self->interface->service_state; }

/* ------------------------------------------------------------------ */
/* bsd.                                                                */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_register_client(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, BSD_CLIENT_PID);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_socket_call(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)self;
  if (req->command_id == BSD_CLOSE) {
    (void)ipc_response_push_u32(res, 0);
    (void)ipc_response_push_u32(res, 0);
    return HLE_RESULT_SUCCESS;
  }
  (void)ipc_response_push_u32(res, (uint32_t)-1);
  (void)ipc_response_push_u32(res, BSD_ENETDOWN);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* nifm.                                                               */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_create_general_service(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->general_service, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_client_id(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)self;
  (void)res;
  const uint32_t id = NIFM_CLIENT_ID;
  (void)service_write_out(c, req, 0, &id, sizeof(id));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_request(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->request, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_not_connected(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return NIFM_RESULT_NOT_CONNECTED;
}

static HLE_ServiceResult cmd_request_state(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, NIFM_REQUEST_STATE_FREE);
  return HLE_RESULT_SUCCESS;
}

/* Two readable handles to the same (signalled) event: the request is
 * already "done" - it failed. */
static HLE_ServiceResult cmd_request_events(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)req;
  Network_State *s = state_of(self);
  for (int i = 0; i < 2; i++) {
    const HLE_ServiceResult result = service_push_event(c, res, &s->request_event);
    if (result != HLE_RESULT_SUCCESS) return result;
  }
  hle_signal_event(c, s->request_event);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_bsd_commands[] = {
    {0, cmd_register_client, "RegisterClient"},
    {1, service_cmd_ok, "StartMonitoring"},
    {2, cmd_socket_call, "Socket"},
    {3, cmd_socket_call, "SocketExempt"},
    {4, cmd_socket_call, "Open"},
    {5, cmd_socket_call, "Select"},
    {6, cmd_socket_call, "Poll"},
    {7, cmd_socket_call, "Sysctl"},
    {8, cmd_socket_call, "Recv"},
    {9, cmd_socket_call, "RecvFrom"},
    {10, cmd_socket_call, "Send"},
    {11, cmd_socket_call, "SendTo"},
    {12, cmd_socket_call, "Accept"},
    {13, cmd_socket_call, "Bind"},
    {14, cmd_socket_call, "Connect"},
    {15, cmd_socket_call, "GetPeerName"},
    {16, cmd_socket_call, "GetSockName"},
    {17, cmd_socket_call, "GetSockOpt"},
    {18, cmd_socket_call, "Listen"},
    {19, cmd_socket_call, "Ioctl"},
    {20, cmd_socket_call, "Fcntl"},
    {21, cmd_socket_call, "SetSockOpt"},
    {22, cmd_socket_call, "Shutdown"},
    {23, cmd_socket_call, "ShutdownAllSockets"},
    {24, cmd_socket_call, "Write"},
    {25, cmd_socket_call, "Read"},
    {26, cmd_socket_call, "Close"},
    {27, cmd_socket_call, "DuplicateSocket"},
    {28, cmd_socket_call, "GetResourceStatistics"},
    {29, cmd_socket_call, "RecvMMsg"},
    {30, cmd_socket_call, "SendMMsg"},
    {31, cmd_socket_call, "EventFd"},
};

static const Service_Command k_nifm_commands[] = {
    {4, cmd_create_general_service, "CreateGeneralServiceOld"},
    {5, cmd_create_general_service, "CreateGeneralService"},
};

static const Service_Command k_general_service_commands[] = {
    {1, cmd_get_client_id, "GetClientId"},
    {4, cmd_create_request, "CreateRequest"},
    {5, cmd_not_connected, "GetCurrentNetworkProfile"},
    {12, cmd_not_connected, "GetCurrentIpAddress"},
    {15, cmd_not_connected, "GetCurrentIpConfigInfo"},
    {16, service_cmd_ok, "SetWirelessCommunicationEnabled_stub"},
    {17, service_cmd_out_u8_false, "IsWirelessCommunicationEnabled"},
    {18, cmd_not_connected, "GetInternetConnectionStatus"},
    {20, service_cmd_out_u8_false, "IsEthernetCommunicationEnabled"},
    {21, service_cmd_out_u8_false, "IsAnyInternetRequestAccepted"},
    {22, service_cmd_out_u8_false, "IsAnyForegroundRequestAccepted"},
    {23, service_cmd_ok, "PutToSleep"},
    {24, service_cmd_ok, "WakeUp"},
};

static const Service_Command k_request_commands[] = {
    {0, cmd_request_state, "GetRequestState"},
    {1, cmd_not_connected, "GetResult"},
    {2, cmd_request_events, "GetSystemEventReadableHandles"},
    {3, service_cmd_ok, "Cancel"},
    {4, service_cmd_ok, "Submit"},
    {6, service_cmd_ok, "SetRequirementPreset_stub"},
    {8, service_cmd_ok, "SetPriority_stub"},
    {9, service_cmd_ok, "SetNetworkProfileId_stub"},
    {11, service_cmd_ok, "SetConnectionConfirmationOption_stub"},
    {12, service_cmd_ok, "SetPersistent_stub"},
    {13, service_cmd_ok, "SetInstant_stub"},
    {14, service_cmd_ok, "SetSustainable_stub"},
    {15, service_cmd_ok, "SetRawPriority_stub"},
    {17, service_cmd_ok, "SetGreedy_stub"},
    {18, service_cmd_ok, "SetSharable_stub"},
    {19, service_cmd_ok, "SetRequirementByRevision_stub"},
};

void network_init(Network_State *s) {
  memset(s, 0, sizeof(*s));
  s->bsd = SERVICE_INTERFACE("bsd:u", k_bsd_commands, 0, s);
  s->nifm = SERVICE_INTERFACE("nifm:u", k_nifm_commands, 0, s);
  s->general_service = SERVICE_INTERFACE("IGeneralService", k_general_service_commands, 0, s);
  s->request = SERVICE_INTERFACE("IRequest", k_request_commands, 0, s);
}

Error network_register(Network_State *s, SM_Registry *registry) {
  static const char *const k_bsd[] = {"bsd:u", "bsd:s"};
  static const char *const k_nifm[] = {"nifm:u", "nifm:s", "nifm:a"};
  for (size_t i = 0; i < sizeof(k_bsd) / sizeof(k_bsd[0]); i++) {
    const Error err = sm_registry_add(registry, k_bsd[i], &s->bsd);
    if (!error_is_ok(err)) return err;
  }
  for (size_t i = 0; i < sizeof(k_nifm) / sizeof(k_nifm[0]); i++) {
    const Error err = sm_registry_add(registry, k_nifm[i], &s->nifm);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}
