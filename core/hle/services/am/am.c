/**
 * am: appletOE/appletAE and the proxy's interfaces. Command ids per the
 * public libnx applet.c / switchbrew AM documentation. See am.h.
 */
#include "hle/services/am/am.h"

#include "common/log.h"
#include "hle/kernel/scheduler.h"
#include "hle/services/service_util.h"
#include "hle/services/set/set.h"

#define AM_DISPLAY_VERSION_BYTES 0x10u
#define AM_PSEUDO_DEVICE_ID_BYTES 0x10u
#define AM_NO_STORAGE UINT64_MAX

static Am_State *state_of(Service_Object *self) { return (Am_State *)self->interface->service_state; }

/* ------------------------------------------------------------------ */
/* Messages.                                                           */
/* ------------------------------------------------------------------ */

void am_push_message(Am_State *s, HLE_Context *c, uint32_t message) {
  if (s->message_count == AM_MESSAGE_QUEUE) return; /* full: drop, as the sysmodule does */
  s->messages[(s->message_head + s->message_count) % AM_MESSAGE_QUEUE] = message;
  s->message_count++;
  if (s->message_event) hle_signal_event(c, s->message_event);
}

static HLE_ServiceResult cmd_get_event_handle(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)req;
  Am_State *s = state_of(self);
  const HLE_ServiceResult result = service_push_event(c, res, &s->message_event);
  if (result == HLE_RESULT_SUCCESS && s->message_count) hle_signal_event(c, s->message_event);
  return result;
}

static HLE_ServiceResult cmd_receive_message(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)req;
  Am_State *s = state_of(self);
  if (!s->message_count) return AM_RESULT_NO_MESSAGES;
  const uint32_t message = s->messages[s->message_head];
  s->message_head = (s->message_head + 1u) % AM_MESSAGE_QUEUE;
  s->message_count--;
  if (!s->message_count && s->message_event) s->message_event->signaled = false;
  (void)ipc_response_push_u32(res, message);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Small value commands.                                               */
/* ------------------------------------------------------------------ */

#define AM_VALUE_COMMAND(fn, push, value)                                                              \
  static HLE_ServiceResult fn(HLE_Context *c, Service_Object *self, const IPC_Request *req,          \
                              IPC_Response *res) {                                                   \
    (void)c;                                                                                         \
    (void)self;                                                                                      \
    (void)req;                                                                                       \
    (void)push(res, value);                                                                          \
    return HLE_RESULT_SUCCESS;                                                                       \
  }

AM_VALUE_COMMAND(cmd_get_operation_mode, ipc_response_push_u32, AM_OPERATION_MODE_HANDHELD)
AM_VALUE_COMMAND(cmd_get_focus_state, ipc_response_push_u32, AM_FOCUS_IN_FOCUS)
AM_VALUE_COMMAND(cmd_get_aruid, ipc_response_push_u64, AM_APPLET_RESOURCE_USER_ID)
AM_VALUE_COMMAND(cmd_get_settings_platform_region, ipc_response_push_u32, 1u) /* Global */
AM_VALUE_COMMAND(cmd_get_previous_program_index, ipc_response_push_u32, UINT32_MAX) /* -1: none */
AM_VALUE_COMMAND(cmd_get_desired_language, ipc_response_push_u64, set_language_code(SET_LANGUAGE_EN_US))
AM_VALUE_COMMAND(cmd_get_this_applet_kind, ipc_response_push_u64, 0) /* application */

static HLE_ServiceResult cmd_get_default_display_resolution(HLE_Context *c, Service_Object *self,
                                                            const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, AM_DISPLAY_WIDTH);
  (void)ipc_response_push_u32(res, AM_DISPLAY_HEIGHT);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_volume(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)req;
  uint32_t bits = 0;
  memcpy(&bits, &state_of(self)->master_volume, sizeof(bits));
  (void)ipc_response_push_u32(res, bits);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_volume(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)res;
  uint32_t bits = 0;
  if (error_is_ok(ipc_request_read_u32(req, 0, &bits))) memcpy(&state_of(self)->master_volume, &bits, sizeof(bits));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_illuminance(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  const float lux = 300.0f;
  uint32_t bits = 0;
  memcpy(&bits, &lux, sizeof(bits));
  (void)ipc_response_push_u32(res, bits);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_idle_extension(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)c;
  (void)res;
  (void)ipc_request_read_u32(req, 0, &state_of(self)->idle_time_extension);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_idle_extension(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, state_of(self)->idle_time_extension);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Events handed out by getters.                                       */
/* ------------------------------------------------------------------ */

#define AM_EVENT_COMMAND(fn, field, signaled)                                                          \
  static HLE_ServiceResult fn(HLE_Context *c, Service_Object *self, const IPC_Request *req,          \
                              IPC_Response *res) {                                                   \
    (void)req;                                                                                       \
    Am_State *s = state_of(self);                                                                    \
    const HLE_ServiceResult result = service_push_event(c, res, &s->field);                          \
    if (result == HLE_RESULT_SUCCESS && (signaled)) hle_signal_event(c, s->field);                   \
    return result;                                                                                   \
  }

AM_EVENT_COMMAND(cmd_launchable_event, launchable_event, true)
AM_EVENT_COMMAND(cmd_suspended_tick_event, suspended_tick_event, false)
AM_EVENT_COMMAND(cmd_resolution_event, resolution_event, false)
AM_EVENT_COMMAND(cmd_gpu_error_event, gpu_error_event, false)
AM_EVENT_COMMAND(cmd_friend_invitation_event, friend_invitation_event, false)
AM_EVENT_COMMAND(cmd_notification_event, notification_event, false)
AM_EVENT_COMMAND(cmd_health_warning_event, health_warning_event, false)
AM_EVENT_COMMAND(cmd_sleep_lock_event, sleep_lock_event, true)

/* ------------------------------------------------------------------ */
/* ISelfController.                                                    */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_exit(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)self;
  (void)req;
  (void)res;
  log_info("[am] ISelfController::Exit");
  if (c->scheduler) c->scheduler->process_exited = true;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_lock_exit(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)req;
  (void)res;
  state_of(self)->exit_locked = true;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_unlock_exit(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  (void)res;
  state_of(self)->exit_locked = false;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_layer(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, state_of(self)->next_layer_id++);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_create_separable_layer(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)req;
  Am_State *s = state_of(self);
  (void)ipc_response_push_u64(res, s->next_layer_id++);
  (void)ipc_response_push_u64(res, s->next_layer_id++);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IApplicationFunctions.                                              */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_pop_launch_parameter(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return AM_RESULT_NO_DATA_IN_CHANNEL;
}

static HLE_ServiceResult cmd_get_display_version(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  char version[AM_DISPLAY_VERSION_BYTES];
  memset(version, 0, sizeof(version));
  memcpy(version, "1.0.0", sizeof("1.0.0"));
  (void)ipc_response_push_bytes(res, version, sizeof(version));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_pseudo_device_id(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  uint8_t id[AM_PSEUDO_DEVICE_ID_BYTES];
  memset(id, 0x56, sizeof(id));
  (void)ipc_response_push_bytes(res, id, sizeof(id));
  return HLE_RESULT_SUCCESS;
}

/* EnsureSaveData -> u64 required size (0: nothing to do). */
static HLE_ServiceResult cmd_ensure_save_data(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  return service_cmd_out_u64_zero(c, self, req, res);
}

/* ------------------------------------------------------------------ */
/* Storages.                                                           */
/* ------------------------------------------------------------------ */

static Am_Storage *storage_of(Am_State *s, uint64_t index) {
  if (index >= AM_STORAGE_CAPACITY || !s->storages[index].in_use) return NULL;
  return &s->storages[index];
}

static uint64_t storage_alloc(Am_State *s, uint64_t size) {
  if (size > AM_STORAGE_MAX_BYTES) return AM_NO_STORAGE;
  for (uint32_t i = 0; i < AM_STORAGE_CAPACITY; i++) {
    if (s->storages[i].in_use || !s->storages[i].data) continue;
    s->storages[i].in_use = true;
    s->storages[i].size = (uint32_t)size;
    memset(s->storages[i].data, 0, AM_STORAGE_MAX_BYTES);
    return i;
  }
  return AM_NO_STORAGE;
}

/* An IStorage and its accessors share one backing slot; the slot frees
 * when the IStorage closes (accessors close first in practice). */
static void storage_on_close(void *service_state, uint64_t object_state) {
  Am_State *s = (Am_State *)service_state;
  if (object_state < AM_STORAGE_CAPACITY) s->storages[object_state].in_use = false;
}

static HLE_ServiceResult cmd_create_storage(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  Am_State *s = state_of(self);
  uint64_t size = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &size))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  const uint64_t index = storage_alloc(s, size);
  if (index == AM_NO_STORAGE) {
    log_warn("[am] CreateStorage(0x%llx): no storage slot", (unsigned long long)size);
    return HLE_RESULT_OUT_OF_MEMORY;
  }
  (void)ipc_response_push_object(res, &s->storage, index);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_storage_open(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  Am_State *s = state_of(self);
  if (!storage_of(s, self->state)) return HLE_RESULT_INVALID_STATE;
  (void)ipc_response_push_object(res, &s->storage_accessor, self->state);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_storage_get_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)req;
  const Am_Storage *storage = storage_of(state_of(self), self->state);
  if (!storage) return HLE_RESULT_INVALID_STATE;
  (void)ipc_response_push_u64(res, storage->size);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult storage_io(HLE_Context *c, Service_Object *self, const IPC_Request *req, bool write) {
  Am_Storage *storage = storage_of(state_of(self), self->state);
  if (!storage) return HLE_RESULT_INVALID_STATE;
  uint64_t offset = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &offset))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  const IPC_Buffer *buf = write ? service_in_buffer(req, 0) : service_out_buffer(req, 0);
  if (!buf) return HLE_RESULT_SUCCESS;
  if (offset > storage->size || buf->size > storage->size - offset) return AM_RESULT_OUT_OF_BOUNDS;
  const Error err = write ? vmm_read_block(c->vmm, buf->gva, storage->data + offset, buf->size)
                          : vmm_write_block(c->vmm, buf->gva, storage->data + offset, buf->size);
  return error_is_ok(err) ? HLE_RESULT_SUCCESS : HLE_RESULT_INVALID_POINTER;
}

static HLE_ServiceResult cmd_storage_write(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)res;
  return storage_io(c, self, req, true);
}

static HLE_ServiceResult cmd_storage_read(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)res;
  return storage_io(c, self, req, false);
}

/* ------------------------------------------------------------------ */
/* Library applets: start, complete at once, no output.                */
/* ------------------------------------------------------------------ */

static Am_Applet *applet_of(Am_State *s, uint64_t index) {
  if (index >= AM_APPLET_CAPACITY || !s->applets[index].in_use) return NULL;
  return &s->applets[index];
}

static void applet_on_close(void *service_state, uint64_t object_state) {
  Am_State *s = (Am_State *)service_state;
  Am_Applet *applet = applet_of(s, object_state);
  if (!applet) return;
  event_release(applet->state_event);
  memset(applet, 0, sizeof(*applet));
}

static HLE_ServiceResult cmd_create_library_applet(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                   IPC_Response *res) {
  (void)c;
  Am_State *s = state_of(self);
  uint32_t applet_id = 0;
  (void)ipc_request_read_u32(req, 0, &applet_id);
  for (uint32_t i = 0; i < AM_APPLET_CAPACITY; i++) {
    if (s->applets[i].in_use) continue;
    memset(&s->applets[i], 0, sizeof(s->applets[i]));
    s->applets[i].in_use = true;
    s->applets[i].applet_id = applet_id;
    log_info("[am] CreateLibraryApplet(0x%x): completes immediately (Phase 6 stub)", applet_id);
    (void)ipc_response_push_object(res, &s->library_applet_accessor, i);
    return HLE_RESULT_SUCCESS;
  }
  return HLE_RESULT_OUT_OF_MEMORY;
}

static HLE_ServiceResult cmd_applet_state_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)req;
  Am_Applet *applet = applet_of(state_of(self), self->state);
  if (!applet) return HLE_RESULT_INVALID_STATE;
  const HLE_ServiceResult result = service_push_event(c, res, &applet->state_event);
  if (result == HLE_RESULT_SUCCESS && applet->started) hle_signal_event(c, applet->state_event);
  return result;
}

static HLE_ServiceResult cmd_applet_start(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)req;
  (void)res;
  Am_Applet *applet = applet_of(state_of(self), self->state);
  if (!applet) return HLE_RESULT_INVALID_STATE;
  applet->started = true;
  if (applet->state_event) hle_signal_event(c, applet->state_event);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_applet_is_completed(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)c;
  (void)req;
  const Am_Applet *applet = applet_of(state_of(self), self->state);
  (void)ipc_response_push_u32(res, applet && applet->started ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_no_data(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                     IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return AM_RESULT_NO_DATA_IN_CHANNEL;
}

/* ------------------------------------------------------------------ */
/* Proxies.                                                            */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_open_proxy(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)req;
  Am_State *s = state_of(self);
  /* The first open is the title's launch: it starts in focus, and the
   * FocusStateChanged message is waiting for it. */
  if (!s->launched) {
    s->launched = true;
    am_push_message(s, c, AM_MESSAGE_FOCUS_STATE_CHANGED);
  }
  (void)ipc_response_push_object(res, &s->proxy, 0);
  return HLE_RESULT_SUCCESS;
}

#define AM_GETTER(fn, field)                                                                           \
  static HLE_ServiceResult fn(HLE_Context *c, Service_Object *self, const IPC_Request *req,          \
                              IPC_Response *res) {                                                   \
    (void)c;                                                                                         \
    (void)req;                                                                                       \
    (void)ipc_response_push_object(res, &state_of(self)->field, 0);                                  \
    return HLE_RESULT_SUCCESS;                                                                       \
  }

AM_GETTER(cmd_get_common_state_getter, common_state_getter)
AM_GETTER(cmd_get_self_controller, self_controller)
AM_GETTER(cmd_get_window_controller, window_controller)
AM_GETTER(cmd_get_audio_controller, audio_controller)
AM_GETTER(cmd_get_display_controller, display_controller)
AM_GETTER(cmd_get_library_applet_creator, library_applet_creator)
AM_GETTER(cmd_get_application_functions, application_functions)
AM_GETTER(cmd_get_debug_functions, debug_functions)

/* ------------------------------------------------------------------ */
/* Command tables (sorted).                                            */
/* ------------------------------------------------------------------ */

static const Service_Command k_oe_commands[] = {
    {0, cmd_open_proxy, "OpenApplicationProxy"},
};

static const Service_Command k_ae_commands[] = {
    {100, cmd_open_proxy, "OpenSystemAppletProxy"},
    {200, cmd_open_proxy, "OpenLibraryAppletProxyOld"},
    {201, cmd_open_proxy, "OpenLibraryAppletProxy"},
    {300, cmd_open_proxy, "OpenOverlayAppletProxy"},
    {350, cmd_open_proxy, "OpenSystemApplicationProxy"},
};

static const Service_Command k_proxy_commands[] = {
    {0, cmd_get_common_state_getter, "GetCommonStateGetter"},
    {1, cmd_get_self_controller, "GetSelfController"},
    {2, cmd_get_window_controller, "GetWindowController"},
    {3, cmd_get_audio_controller, "GetAudioController"},
    {4, cmd_get_display_controller, "GetDisplayController"},
    {11, cmd_get_library_applet_creator, "GetLibraryAppletCreator"},
    {20, cmd_get_application_functions, "GetApplicationFunctions"},
    {1000, cmd_get_debug_functions, "GetDebugFunctions"},
};

static const Service_Command k_common_state_getter_commands[] = {
    {0, cmd_get_event_handle, "GetEventHandle"},
    {1, cmd_receive_message, "ReceiveMessage"},
    {2, cmd_get_this_applet_kind, "GetThisAppletKind"},
    {3, service_cmd_ok, "AllowToEnterSleep_stub"},
    {4, service_cmd_ok, "DisallowToEnterSleep_stub"},
    {5, cmd_get_operation_mode, "GetOperationMode"},
    {6, service_cmd_out_u8_false, "GetPerformanceMode"}, /* u32 0: Normal */
    {7, service_cmd_out_u8_false, "GetCradleStatus"},
    {8, service_cmd_out_u8_false, "GetBootMode"},
    {9, cmd_get_focus_state, "GetCurrentFocusState"},
    {10, service_cmd_ok, "RequestToAcquireSleepLock_stub"},
    {11, service_cmd_ok, "ReleaseSleepLock_stub"},
    {12, service_cmd_ok, "ReleaseSleepLockTransiently_stub"},
    {13, cmd_sleep_lock_event, "GetAcquiredSleepLockEvent"},
    {50, service_cmd_out_u8_false, "IsVrModeEnabled"},
    {51, service_cmd_ok, "SetVrModeEnabled_stub"},
    {52, service_cmd_ok, "SetLcdBacklighOffEnabled_stub"},
    {53, service_cmd_ok, "BeginVrModeEx_stub"},
    {54, service_cmd_ok, "EndVrModeEx_stub"},
    {55, service_cmd_out_u8_false, "IsInControllerFirmwareUpdateSection"},
    {60, cmd_get_default_display_resolution, "GetDefaultDisplayResolution"},
    {61, cmd_resolution_event, "GetDefaultDisplayResolutionChangeEvent"},
    {62, service_cmd_out_u8_false, "GetHdcpAuthenticationState"},
    {66, service_cmd_ok, "SetCpuBoostMode_stub"},
    {67, service_cmd_ok, "CancelCpuBoostMode_stub"},
    {68, service_cmd_out_u8_false, "GetBuiltInDisplayType"},
    {90, service_cmd_ok, "SetPerformanceConfigurationChangedNotification_stub"},
    {91, service_cmd_out_u8_false, "GetCurrentPerformanceConfiguration"},
    {100, service_cmd_ok, "SetHandlingHomeButtonShortPressedEnabled_stub"},
    {300, cmd_get_settings_platform_region, "GetSettingsPlatformRegion"},
    {400, service_cmd_ok, "ActivateMigrationService_stub"},
    {401, service_cmd_ok, "DeactivateMigrationService_stub"},
    {502, service_cmd_out_u8_true, "IsSleepEnabled"},
};

static const Service_Command k_self_controller_commands[] = {
    {0, cmd_exit, "Exit"},
    {1, cmd_lock_exit, "LockExit"},
    {2, cmd_unlock_exit, "UnlockExit"},
    {3, service_cmd_ok, "EnterFatalSection_stub"},
    {4, service_cmd_ok, "LeaveFatalSection_stub"},
    {9, cmd_launchable_event, "GetLibraryAppletLaunchableEvent"},
    {10, service_cmd_ok, "SetScreenShotPermission_stub"},
    {11, service_cmd_ok, "SetOperationModeChangedNotification_stub"},
    {12, service_cmd_ok, "SetPerformanceModeChangedNotification_stub"},
    {13, service_cmd_ok, "SetFocusHandlingMode_stub"},
    {14, service_cmd_ok, "SetRestartMessageEnabled_stub"},
    {15, service_cmd_ok, "SetScreenShotAppletIdentityInfo_stub"},
    {16, service_cmd_ok, "SetOutOfFocusSuspendingEnabled_stub"},
    {17, service_cmd_ok, "SetControllerFirmwareUpdateSection_stub"},
    {18, service_cmd_ok, "SetRequiresCaptureButtonShortPressedMessage_stub"},
    {19, service_cmd_ok, "SetAlbumImageOrientation_stub"},
    {20, service_cmd_ok, "SetDesirableKeyboardLayout_stub"},
    {40, cmd_create_layer, "CreateManagedDisplayLayer"},
    {41, service_cmd_out_u8_false, "IsSystemBufferSharingEnabled"},
    {44, cmd_create_separable_layer, "CreateManagedDisplaySeparableLayer"},
    {45, service_cmd_ok, "SetManagedDisplayLayerSeparationMode_stub"},
    {46, service_cmd_ok, "SetRecordingLayerCompositionEnabled_stub"},
    {50, service_cmd_ok, "SetHandlesRequestToDisplay_stub"},
    {51, service_cmd_ok, "ApproveToDisplay_stub"},
    {60, service_cmd_ok, "OverrideAutoSleepTimeAndDimmingTime_stub"},
    {61, service_cmd_ok, "SetMediaPlaybackState_stub"},
    {62, cmd_set_idle_extension, "SetIdleTimeDetectionExtension"},
    {63, cmd_get_idle_extension, "GetIdleTimeDetectionExtension"},
    {64, service_cmd_ok, "SetInputDetectionSourceSet_stub"},
    {65, service_cmd_ok, "ReportUserIsActive_stub"},
    {66, cmd_get_illuminance, "GetCurrentIlluminance"},
    {67, service_cmd_out_u8_false, "IsIlluminanceAvailable"},
    {68, service_cmd_ok, "SetAutoSleepDisabled_stub"},
    {69, service_cmd_out_u8_false, "IsAutoSleepDisabled"},
    {70, service_cmd_ok, "ReportMultimediaPlaybackState_stub"},
    {72, service_cmd_ok, "SetInputDetectionPolicy_stub"},
    {80, service_cmd_ok, "SetWirelessPriorityMode_stub"},
    {90, service_cmd_out_u64_zero, "GetAccumulatedSuspendedTickValue"},
    {91, cmd_suspended_tick_event, "GetAccumulatedSuspendedTickChangedEvent"},
    {100, service_cmd_ok, "SetAlbumImageTakenNotificationEnabled_stub"},
    {110, service_cmd_ok, "SetApplicationAlbumUserData_stub"},
    {130, service_cmd_ok, "SetRecordVolumeMuted_stub"},
};

static const Service_Command k_window_controller_commands[] = {
    {1, cmd_get_aruid, "GetAppletResourceUserId"},
    {2, cmd_get_aruid, "GetAppletResourceUserIdOfCallerApplet"},
    {10, service_cmd_ok, "AcquireForegroundRights"},
    {11, service_cmd_ok, "ReleaseForegroundRights"},
    {12, service_cmd_ok, "RejectToChangeIntoBackground_stub"},
    {20, service_cmd_ok, "SetAppletWindowVisibility_stub"},
    {21, service_cmd_ok, "SetAppletGpuTimeSlice_stub"},
};

static const Service_Command k_audio_controller_commands[] = {
    {0, cmd_set_volume, "SetExpectedMasterVolume"},
    {1, cmd_get_volume, "GetMainAppletExpectedMasterVolume"},
    {2, cmd_get_volume, "GetLibraryAppletExpectedMasterVolume"},
    {3, service_cmd_ok, "ChangeMainAppletMasterVolume_stub"},
    {4, service_cmd_ok, "SetTransparentVolumeRate_stub"},
};

static const Service_Command k_display_controller_commands[] = {
    {1, service_cmd_ok, "UpdateLastForegroundCaptureImage_stub"},
    {4, service_cmd_ok, "UpdateCallerAppletCaptureImage_stub"},
    {20, service_cmd_ok, "ClearCaptureBuffer_stub"},
};

static const Service_Command k_library_applet_creator_commands[] = {
    {0, cmd_create_library_applet, "CreateLibraryApplet"},
    {10, cmd_create_storage, "CreateStorage"},
};

static const Service_Command k_application_functions_commands[] = {
    {1, cmd_pop_launch_parameter, "PopLaunchParameter"},
    {20, cmd_ensure_save_data, "EnsureSaveData"},
    {21, cmd_get_desired_language, "GetDesiredLanguage"},
    {22, service_cmd_ok, "SetTerminateResult_stub"},
    {23, cmd_get_display_version, "GetDisplayVersion"},
    {25, service_cmd_out_zero128, "ExtendSaveData"},
    {26, service_cmd_out_zero128, "GetSaveDataSize"},
    {30, service_cmd_ok, "BeginBlockingHomeButtonShortAndLongPressed_stub"},
    {31, service_cmd_ok, "EndBlockingHomeButtonShortAndLongPressed_stub"},
    {32, service_cmd_ok, "BeginBlockingHomeButton_stub"},
    {33, service_cmd_ok, "EndBlockingHomeButton_stub"},
    {40, service_cmd_out_u8_false, "NotifyRunning"},
    {50, cmd_get_pseudo_device_id, "GetPseudoDeviceId"},
    {60, service_cmd_ok, "SetMediaPlaybackStateForApplication_stub"},
    {65, service_cmd_out_u8_false, "IsGamePlayRecordingSupported"},
    {66, service_cmd_ok, "InitializeGamePlayRecording_stub"},
    {67, service_cmd_ok, "SetGamePlayRecordingState_stub"},
    {90, service_cmd_ok, "EnableApplicationCrashReport_stub"},
    {100, service_cmd_ok, "InitializeApplicationCopyrightFrameBuffer_stub"},
    {101, service_cmd_ok, "SetApplicationCopyrightImage_stub"},
    {102, service_cmd_ok, "SetApplicationCopyrightVisibility_stub"},
    {110, service_cmd_out_u8_false, "QueryApplicationPlayStatistics_stub"},
    {111, service_cmd_out_u8_false, "QueryApplicationPlayStatisticsByUid_stub"},
    {121, service_cmd_ok, "ClearUserChannel_stub"},
    {122, service_cmd_ok, "UnpopToUserChannel_stub"},
    {123, cmd_get_previous_program_index, "GetPreviousProgramIndex"},
    {124, service_cmd_ok, "EnableApplicationAllThreadDumpOnCrash_stub"},
    {130, cmd_gpu_error_event, "GetGpuErrorDetectedSystemEvent"},
    {131, service_cmd_ok, "SetDelayTimeToAbortOnGpuError_stub"},
    {140, cmd_friend_invitation_event, "GetFriendInvitationStorageChannelEvent"},
    {141, cmd_no_data, "TryPopFromFriendInvitationStorageChannel"},
    {150, cmd_notification_event, "GetNotificationStorageChannelEvent"},
    {151, cmd_no_data, "TryPopFromNotificationStorageChannel"},
    {160, cmd_health_warning_event, "GetHealthWarningDisappearedSystemEvent"},
    {170, service_cmd_ok, "SetHdcpAuthenticationActivated_stub"},
};

static const Service_Command k_debug_functions_commands[] = {
    {0, service_cmd_ok, "NotifyMessageToHomeMenuForDebug_stub"},
};

static const Service_Command k_storage_commands[] = {
    {0, cmd_storage_open, "Open"},
};

static const Service_Command k_storage_accessor_commands[] = {
    {0, cmd_storage_get_size, "GetSize"},
    {10, cmd_storage_write, "Write"},
    {11, cmd_storage_read, "Read"},
};

static const Service_Command k_library_applet_accessor_commands[] = {
    {0, cmd_applet_state_event, "GetAppletStateChangedEvent"},
    {1, cmd_applet_is_completed, "IsCompleted"},
    {10, cmd_applet_start, "Start"},
    {20, service_cmd_ok, "RequestExit"},
    {25, service_cmd_ok, "Terminate"},
    {30, service_cmd_ok, "GetResult"},
    {50, service_cmd_ok, "SetOutOfFocusApplicationSuspendingEnabled_stub"},
    {60, service_cmd_ok, "PresetLibraryAppletGpuTimeSliceZero_stub"},
    {100, service_cmd_ok, "PushInData"},
    {101, cmd_no_data, "PopOutData"},
    {102, service_cmd_ok, "PushExtraStorage"},
    {103, service_cmd_ok, "PushInteractiveInData"},
    {104, cmd_no_data, "PopInteractiveOutData"},
    {110, service_cmd_out_u8_false, "NeedsToExitProcess"},
};

#define AM_INTERFACE(name, table, state) SERVICE_INTERFACE(name, table, 0, state)

void am_init(Am_State *s, uint8_t *storage_pool) {
  memset(s, 0, sizeof(*s));
  for (uint32_t i = 0; i < AM_STORAGE_CAPACITY; i++) {
    s->storages[i].data = storage_pool ? storage_pool + (uint64_t)i * AM_STORAGE_MAX_BYTES : NULL;
  }
  s->master_volume = 1.0f;
  s->next_layer_id = 1;
  s->oe = AM_INTERFACE("appletOE", k_oe_commands, s);
  s->ae = AM_INTERFACE("appletAE", k_ae_commands, s);
  s->proxy = AM_INTERFACE("IApplicationProxy", k_proxy_commands, s);
  s->common_state_getter = AM_INTERFACE("ICommonStateGetter", k_common_state_getter_commands, s);
  s->self_controller = AM_INTERFACE("ISelfController", k_self_controller_commands, s);
  s->window_controller = AM_INTERFACE("IWindowController", k_window_controller_commands, s);
  s->audio_controller = AM_INTERFACE("IAudioController", k_audio_controller_commands, s);
  s->display_controller = AM_INTERFACE("IDisplayController", k_display_controller_commands, s);
  s->library_applet_creator = AM_INTERFACE("ILibraryAppletCreator", k_library_applet_creator_commands, s);
  s->application_functions = AM_INTERFACE("IApplicationFunctions", k_application_functions_commands, s);
  s->debug_functions = AM_INTERFACE("IDebugFunctions", k_debug_functions_commands, s);
  s->storage = AM_INTERFACE("IStorage", k_storage_commands, s);
  s->storage.on_close = storage_on_close;
  s->storage_accessor = AM_INTERFACE("IStorageAccessor", k_storage_accessor_commands, s);
  s->library_applet_accessor = AM_INTERFACE("ILibraryAppletAccessor", k_library_applet_accessor_commands, s);
  s->library_applet_accessor.on_close = applet_on_close;
}

Error am_register(Am_State *s, SM_Registry *registry) {
  Error err = sm_registry_add(registry, "appletOE", &s->oe);
  if (error_is_ok(err)) err = sm_registry_add(registry, "appletAE", &s->ae);
  return err;
}
