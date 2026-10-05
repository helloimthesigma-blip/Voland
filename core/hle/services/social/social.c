#include "hle/services/social/social.h"

#include "common/log.h"
#include "hle/kernel/scheduler.h"
#include "hle/services/service_util.h"

#include <string.h>

static Social_State *state_of(Service_Object *self) { return (Social_State *)self->interface->service_state; }

static HLE_ServiceResult cmd_zero_u32(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* prepo                                                               */
/* ------------------------------------------------------------------ */

static const Service_Command k_prepo_commands[] = {
    {10100, service_cmd_ok, "SaveReportOld"},
    {10101, service_cmd_ok, "SaveReportWithUserOld"},
    {10102, service_cmd_ok, "SaveReportOld2"},
    {10103, service_cmd_ok, "SaveReportWithUserOld2"},
    {10104, service_cmd_ok, "SaveReport"},
    {10105, service_cmd_ok, "SaveReportWithUser"},
    {10200, service_cmd_ok, "RequestImmediateTransmission"},
    {10300, cmd_zero_u32, "GetTransmissionStatus"},
    {10400, service_cmd_out_u64_zero, "GetSystemSessionId"},
    {20100, service_cmd_ok, "SaveSystemReport"},
    {20101, service_cmd_ok, "SaveSystemReportWithUser"},
    {30100, service_cmd_ok, "ClearStorage"},
    {40100, service_cmd_out_u8_false, "IsUserAgreementCheckEnabled"},
    {40101, service_cmd_ok, "SetUserAgreementCheckEnabled"},
};

/* ------------------------------------------------------------------ */
/* friends                                                             */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_friend_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->friend_event);
}

static HLE_ServiceResult cmd_create_friend_service(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                   IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->friend_service, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_friend_notifications(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                         IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->friend_notifications, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_no_notification(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return FRIENDS_RESULT_NO_NOTIFICATION;
}

static const Service_Command k_friend_commands[] = {
    {0, cmd_create_friend_service, "CreateFriendService"},
    {1, cmd_create_friend_notifications, "CreateNotificationService"},
    {2, service_cmd_ok, "CreateDaemonSuspendSessionService"},
};

static const Service_Command k_friend_service_commands[] = {
    {0, cmd_friend_event, "GetCompletionEvent"},
    {1, service_cmd_ok, "Cancel"},
    {10100, cmd_zero_u32, "GetFriendListIds"},
    {10101, cmd_zero_u32, "GetFriendList"},
    {10102, service_cmd_ok, "UpdateFriendInfo"},
    {10110, service_cmd_ok, "GetFriendProfileImage"},
    {10120, service_cmd_out_u8_false, "IsFriendListCacheAvailable"},
    {10121, service_cmd_ok, "EnsureFriendListAvailable"},
    {10200, service_cmd_ok, "SendFriendRequestForApplication"},
    {10211, service_cmd_ok, "AddFacedFriendRequestForApplication"},
    {10400, cmd_zero_u32, "GetBlockedUserListIds"},
    {10420, service_cmd_out_u8_false, "IsBlockedUserListCacheAvailable"},
    {10421, service_cmd_ok, "EnsureBlockedUserListAvailable"},
    {10500, service_cmd_ok, "GetProfileList"},
    {10600, service_cmd_ok, "DeclareOpenOnlinePlaySession"},
    {10601, service_cmd_ok, "DeclareCloseOnlinePlaySession"},
    {10610, service_cmd_ok, "UpdateUserPresence"},
    {10700, service_cmd_ok, "GetPlayHistoryRegistrationKey"},
    {10701, service_cmd_ok, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"},
    {10702, service_cmd_ok, "AddPlayHistory"},
    {11000, service_cmd_ok, "GetProfileImageUrl"},
    {20100, cmd_zero_u32, "GetFriendCount"},
    {20101, cmd_zero_u32, "GetNewlyFriendCount"},
    {20102, service_cmd_ok, "GetFriendDetailedInfo"},
    {20103, service_cmd_ok, "SyncFriendList"},
    {20104, service_cmd_ok, "RequestSyncFriendList"},
    {20110, service_cmd_ok, "LoadFriendSetting"},
    {20200, cmd_zero_u32, "GetReceivedFriendRequestCount"},
    {20201, cmd_zero_u32, "GetFriendRequestList"},
    {20300, cmd_zero_u32, "GetFriendCandidateList"},
    {20400, cmd_zero_u32, "GetBlockedUserList"},
    {20401, service_cmd_ok, "SyncBlockedUserList"},
    {20500, service_cmd_ok, "GetProfileExtraList"},
    {20600, service_cmd_ok, "GetUserPresenceView"},
    {20700, cmd_zero_u32, "GetPlayHistoryList"},
    {20701, cmd_zero_u32, "GetPlayHistoryStatistics"},
    {20800, service_cmd_ok, "LoadUserSetting"},
    {20801, service_cmd_ok, "SyncUserSetting"},
    {20900, service_cmd_ok, "RequestListSummaryOverlayNotification"},
    {21000, service_cmd_ok, "GetExternalApplicationCatalog"},
    {30830, service_cmd_ok, "ClearPlayLog"},
};

static const Service_Command k_friend_notification_commands[] = {
    {0, cmd_friend_event, "GetEvent"},
    {1, service_cmd_ok, "Clear"},
    {2, cmd_no_notification, "Pop"},
};

/* ------------------------------------------------------------------ */
/* bcat                                                                */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_create_bcat_service(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->bcat_service, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_bcat_storage(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->bcat_storage, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_bcat_not_found(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return BCAT_RESULT_NOT_FOUND;
}

static const Service_Command k_bcat_commands[] = {
    {0, cmd_create_bcat_service, "CreateBcatService"},
    {1, cmd_create_bcat_storage, "CreateDeliveryCacheStorageService"},
    {2, cmd_create_bcat_storage, "CreateDeliveryCacheStorageServiceWithApplicationId"},
};

static const Service_Command k_bcat_service_commands[] = {
    {10100, service_cmd_ok, "RequestSyncDeliveryCache"},
    {10101, service_cmd_ok, "RequestSyncDeliveryCacheWithDirectoryName"},
    {10200, service_cmd_ok, "CancelSyncDeliveryCacheRequest"},
    {20100, service_cmd_ok, "RequestSyncDeliveryCacheWithApplicationId"},
    {30100, service_cmd_ok, "SetPassphrase"},
    {30200, service_cmd_ok, "RegisterBackgroundDeliveryTask"},
    {30201, service_cmd_ok, "UnregisterBackgroundDeliveryTask"},
    {30202, service_cmd_ok, "BlockDeliveryTask"},
    {30203, service_cmd_ok, "UnblockDeliveryTask"},
    {30210, service_cmd_ok, "SetDeliveryTaskTimer"},
    {90100, cmd_zero_u32, "EnumerateBackgroundDeliveryTask"},
};

/* No delivery cache: nothing to open, no directories to list. */
static const Service_Command k_bcat_storage_commands[] = {
    {0, cmd_bcat_not_found, "CreateFileService"},
    {1, cmd_bcat_not_found, "CreateDirectoryService"},
    {10, cmd_zero_u32, "EnumerateDeliveryCacheDirectory"},
};

/* ------------------------------------------------------------------ */
/* caps (album)                                                        */
/* ------------------------------------------------------------------ */

#define CAPS_ALBUM_ENTRY_BYTES 0x20u

static HLE_ServiceResult cmd_caps_entry(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  for (uint32_t i = 0; i < CAPS_ALBUM_ENTRY_BYTES / 8u; i++) (void)ipc_response_push_u64(res, 0);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_caps_su_commands[] = {
    {32, service_cmd_ok, "SetShimLibraryVersion"},
    {201, cmd_caps_entry, "SaveScreenShot"},
    {203, cmd_caps_entry, "SaveScreenShotEx0"},
    {205, cmd_caps_entry, "SaveScreenShotEx1"},
    {210, cmd_caps_entry, "SaveScreenShotEx2"},
};

static const Service_Command k_caps_u_commands[] = {
    {32, service_cmd_ok, "SetShimLibraryVersion"},
    {102, cmd_zero_u32, "GetAlbumContentsFileListForApplication"},
    {142, cmd_zero_u32, "GetAlbumFileList3AaeAruid"},
    {143, cmd_zero_u32, "GetAlbumFileList4AaeUidAruid"},
};

/* ------------------------------------------------------------------ */
/* fatal                                                               */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_fatal(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)self;
  (void)res;
  uint32_t result = 0;
  (void)ipc_request_read_u32(req, 0, &result);
  log_error("[fatal] the title threw a fatal error: result 0x%x (module %u, description %u)", result, result & 0x1FFu,
            result >> 9);
  Sched_Thread *thread = scheduler_current(c->scheduler);
  c->scheduler->process_crashed = true;
  if (thread && thread->thread.cpu_state) c->scheduler->crash_pc = c->cpu_backend->get_pc(thread->thread.cpu_state);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_fatal_commands[] = {
    {0, cmd_fatal, "ThrowFatal"},
    {1, cmd_fatal, "ThrowFatalWithPolicy"},
    {2, cmd_fatal, "ThrowFatalWithCpuContext"},
};

/* ------------------------------------------------------------------ */
/* nfp (amiibo)                                                        */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_create_nfp_user(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->nfp_user, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_nfp_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->nfp_event);
}

static HLE_ServiceResult cmd_nfp_state(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, NFP_STATE_INITIALIZED);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_nfp_commands[] = {
    {0, cmd_create_nfp_user, "CreateUserInterface"}, /* nfp:sys: CreateSystemInterface */
};

static const Service_Command k_nfp_user_commands[] = {
    {0, service_cmd_ok, "Initialize"},
    {1, service_cmd_ok, "Finalize"},
    {2, cmd_zero_u32, "ListDevices"},
    {17, cmd_nfp_event, "AttachActivateEvent"},
    {18, cmd_nfp_event, "AttachDeactivateEvent"},
    {19, cmd_nfp_state, "GetState"},
    {20, cmd_zero_u32, "GetDeviceState"},
    {23, cmd_nfp_event, "AttachAvailabilityChangeEvent"},
};

/* ------------------------------------------------------------------ */

void social_init(Social_State *s) {
  memset(s, 0, sizeof(*s));
  static const char *const k_prepo[SOCIAL_PREPO_PORTS] = {"prepo:u", "prepo:a", "prepo:a2", "prepo:m", "prepo:s", "prepo:p"};
  for (uint32_t i = 0; i < SOCIAL_PREPO_PORTS; i++) s->prepo[i] = SERVICE_INTERFACE(k_prepo[i], k_prepo_commands, 0, s);
  static const char *const k_friend[SOCIAL_FRIEND_PORTS] = {"friend:u", "friend:v", "friend:m", "friend:s", "friend:a"};
  for (uint32_t i = 0; i < SOCIAL_FRIEND_PORTS; i++) s->friends[i] = SERVICE_INTERFACE(k_friend[i], k_friend_commands, 0, s);
  s->friend_service = SERVICE_INTERFACE("IFriendService", k_friend_service_commands, 0, s);
  s->friend_notifications = SERVICE_INTERFACE("INotificationService", k_friend_notification_commands, 0, s);
  static const char *const k_bcat[SOCIAL_BCAT_PORTS] = {"bcat:u", "bcat:s", "bcat:m", "bcat:a"};
  for (uint32_t i = 0; i < SOCIAL_BCAT_PORTS; i++) s->bcat[i] = SERVICE_INTERFACE(k_bcat[i], k_bcat_commands, 0, s);
  s->bcat_service = SERVICE_INTERFACE("IBcatService", k_bcat_service_commands, 0, s);
  s->bcat_storage = SERVICE_INTERFACE("IDeliveryCacheStorageService", k_bcat_storage_commands, 0, s);
  s->caps_su = SERVICE_INTERFACE("caps:su", k_caps_su_commands, 0, s);
  s->caps_u = SERVICE_INTERFACE("caps:u", k_caps_u_commands, 0, s);
  s->fatal = SERVICE_INTERFACE("fatal:u", k_fatal_commands, 0, s);
  static const char *const k_nfp[SOCIAL_NFP_PORTS] = {"nfp:user", "nfp:sys"};
  for (uint32_t i = 0; i < SOCIAL_NFP_PORTS; i++) s->nfp[i] = SERVICE_INTERFACE(k_nfp[i], k_nfp_commands, 0, s);
  s->nfp_user = SERVICE_INTERFACE("IUser", k_nfp_user_commands, 0, s);
}

Error social_register(Social_State *s, SM_Registry *registry) {
  Error err = OK;
  for (uint32_t i = 0; i < SOCIAL_PREPO_PORTS && error_is_ok(err); i++) err = sm_registry_add(registry, s->prepo[i].name, &s->prepo[i]);
  for (uint32_t i = 0; i < SOCIAL_FRIEND_PORTS && error_is_ok(err); i++) err = sm_registry_add(registry, s->friends[i].name, &s->friends[i]);
  for (uint32_t i = 0; i < SOCIAL_BCAT_PORTS && error_is_ok(err); i++) err = sm_registry_add(registry, s->bcat[i].name, &s->bcat[i]);
  if (error_is_ok(err)) err = sm_registry_add(registry, "caps:su", &s->caps_su);
  if (error_is_ok(err)) err = sm_registry_add(registry, "caps:u", &s->caps_u);
  if (error_is_ok(err)) err = sm_registry_add(registry, "fatal:u", &s->fatal);
  for (uint32_t i = 0; i < SOCIAL_NFP_PORTS && error_is_ok(err); i++) err = sm_registry_add(registry, s->nfp[i].name, &s->nfp[i]);
  return err;
}
