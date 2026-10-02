#include "hle/services/acc/acc.h"

#include "hle/services/network/network.h"
#include "hle/services/service_util.h"

#define ACC_PROFILE_BASE_BYTES 0x38u
#define ACC_USER_DATA_BYTES 0x80u
#define ACC_NICKNAME_BYTES 0x20u
#define ACC_DIGEST_BYTES 0x10u
#define ACC_ACCOUNT_ID 0x566F6C616E640001ull
#define ACC_LAST_EDIT 1767225600ull
#define ACC_MODULE 124u
#define ACC_RESULT_USER_NOT_FOUND ((100u << 9) | ACC_MODULE)

static Acc_State *state_of(Service_Object *self) { return (Acc_State *)self->interface->service_state; }

void acc_user_uid(uint8_t out[ACC_UID_BYTES]) {
  const uint64_t uid[2] = {ACC_USER_UID_LO, ACC_USER_UID_HI};
  memcpy(out, uid, ACC_UID_BYTES);
}

static bool is_our_user(const IPC_Request *req, uint32_t offset) {
  uint8_t uid[ACC_UID_BYTES], ours[ACC_UID_BYTES];
  if (!error_is_ok(ipc_request_read_bytes(req, offset, uid, sizeof(uid)))) return false;
  acc_user_uid(ours);
  return memcmp(uid, ours, sizeof(uid)) == 0;
}

static void push_uid(IPC_Response *res) {
  uint8_t uid[ACC_UID_BYTES];
  acc_user_uid(uid);
  (void)ipc_response_push_bytes(res, uid, sizeof(uid));
}

static HLE_ServiceResult cmd_user_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 1);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_user_exists(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)self;
  (void)ipc_response_push_u32(res, is_our_user(req, 0) ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

/* The user list: our uid, then zeros to the end of the buffer. */
static HLE_ServiceResult cmd_list_users(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)self;
  (void)res;
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  if (!buf) return HLE_RESULT_SUCCESS;
  uint8_t entry[ACC_UID_BYTES];
  for (uint64_t at = 0; at + ACC_UID_BYTES <= buf->size; at += ACC_UID_BYTES) {
    if (at == 0) acc_user_uid(entry);
    else memset(entry, 0, sizeof(entry));
    if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + at, entry, sizeof(entry)))) return HLE_RESULT_INVALID_POINTER;
  }
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_last_opened(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  push_uid(res);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_profile(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  if (!is_our_user(req, 0)) return ACC_RESULT_USER_NOT_FOUND;
  (void)ipc_response_push_object(res, &state_of(self)->profile, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_profile_digest(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  uint8_t digest[ACC_DIGEST_BYTES];
  memset(digest, 0x5A, sizeof(digest));
  (void)ipc_response_push_bytes(res, digest, sizeof(digest));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_manager(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->manager, 0);
  return HLE_RESULT_SUCCESS;
}

/* IProfile. */
static void profile_base(uint8_t out[ACC_PROFILE_BASE_BYTES]) {
  memset(out, 0, ACC_PROFILE_BASE_BYTES);
  acc_user_uid(out);
  const uint64_t edited = ACC_LAST_EDIT;
  memcpy(out + ACC_UID_BYTES, &edited, sizeof(edited));
  memcpy(out + ACC_UID_BYTES + 8, ACC_NICKNAME, sizeof(ACC_NICKNAME));
}

static HLE_ServiceResult cmd_profile_get(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)self;
  uint8_t base[ACC_PROFILE_BASE_BYTES], user_data[ACC_USER_DATA_BYTES];
  profile_base(base);
  memset(user_data, 0, sizeof(user_data));
  (void)service_write_out(c, req, 0, user_data, sizeof(user_data));
  (void)ipc_response_push_bytes(res, base, sizeof(base));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_profile_base(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  uint8_t base[ACC_PROFILE_BASE_BYTES];
  profile_base(base);
  (void)ipc_response_push_bytes(res, base, sizeof(base));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_image_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, k_acc_profile_icon_size);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_load_image(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)self;
  (void)ipc_response_push_u32(res, (uint32_t)service_write_out(c, req, 0, k_acc_profile_icon, k_acc_profile_icon_size));
  return HLE_RESULT_SUCCESS;
}

/* IManagerForApplication. */
static HLE_ServiceResult cmd_account_id(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, ACC_ACCOUNT_ID);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_offline(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                     IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return NIFM_RESULT_NOT_CONNECTED;
}

static const Service_Command k_acc_commands[] = {
    {0, cmd_user_count, "GetUserCount"},
    {1, cmd_user_exists, "GetUserExistence"},
    {2, cmd_list_users, "ListAllUsers"},
    {3, cmd_list_users, "ListOpenUsers"},
    {4, cmd_last_opened, "GetLastOpenedUser"},
    {5, cmd_get_profile, "GetProfile"},
    {6, cmd_profile_digest, "GetProfileDigest"},
    {50, service_cmd_out_u8_false, "IsUserRegistrationRequestPermitted"},
    {51, cmd_last_opened, "TrySelectUserWithoutInteraction"},
    {60, cmd_list_users, "ListOpenContextStoredUsersOld"},
    {100, service_cmd_ok, "InitializeApplicationInfoV0"},
    {101, cmd_get_manager, "GetBaasAccountManagerForApplication"},
    {102, cmd_offline, "AuthenticateApplicationAsync"},
    {103, cmd_offline, "CheckNetworkServiceAvailabilityAsync"},
    {110, service_cmd_ok, "StoreSaveDataThumbnail"},
    {111, service_cmd_ok, "ClearSaveDataThumbnail"},
    {130, cmd_get_manager, "LoadOpenContext"},
    {131, cmd_list_users, "ListOpenContextStoredUsers"},
    {140, service_cmd_ok, "InitializeApplicationInfo"},
    {141, cmd_list_users, "ListQualifiedUsers"},
    {150, service_cmd_out_u8_false, "IsUserAccountSwitchLocked"},
    {160, service_cmd_ok, "InitializeApplicationInfoV2"},
};

static const Service_Command k_profile_commands[] = {
    {0, cmd_profile_get, "Get"},
    {1, cmd_profile_base, "GetBase"},
    {10, cmd_image_size, "GetImageSize"},
    {11, cmd_load_image, "LoadImage"},
};

static const Service_Command k_manager_commands[] = {
    {0, service_cmd_ok, "CheckAvailability"},
    {1, cmd_account_id, "GetAccountId"},
    {2, cmd_offline, "EnsureIdTokenCacheAsync"},
    {3, cmd_offline, "LoadIdTokenCache"},
    {130, cmd_offline, "GetNintendoAccountUserResourceCacheForApplication"},
    {150, cmd_offline, "CreateAuthorizationRequest"},
    {160, service_cmd_ok, "StoreOpenContext"},
    {170, cmd_offline, "LoadNetworkServiceLicenseKindAsync"},
};

void acc_init(Acc_State *s) {
  memset(s, 0, sizeof(*s));
  s->service = SERVICE_INTERFACE("acc:u0", k_acc_commands, 0x800, s);
  s->profile = SERVICE_INTERFACE("IProfile", k_profile_commands, 0, s);
  s->manager = SERVICE_INTERFACE("IManagerForApplication", k_manager_commands, 0, s);
}

Error acc_register(Acc_State *s, SM_Registry *registry) {
  static const char *const k_names[] = {"acc:u0", "acc:u1", "acc:su", "acc:aa"};
  for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
    const Error err = sm_registry_add(registry, k_names[i], &s->service);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}
