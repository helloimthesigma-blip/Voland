#include "hle/services/ns/ns.h"

#include <string.h>

#include "hle/services/service_util.h"

static const char *const k_ports[NS_PORT_COUNT] = {"ns:am2", "ns:ec", "ns:rid", "ns:rt", "ns:web", "ns:ro"};

#define RECORD_TYPE_INSTALLED 3u
#define CONTROL_SOURCE_STORAGE 0u
#define MAX_CONTROL_CACHE 0x10u
#define APPLICATION_VIEW_BYTES 0x50u
#define VIEW_FLAGS_RUNNABLE 0x00001F02u

static Ns_State *state_of(Service_Object *self) { return (Ns_State *)self->interface->service_state; }

/* ---- IServiceGetterInterface --------------------------------------- */

static HLE_ServiceResult cmd_get_manager(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->application_manager, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_control_data(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->control_data, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_record(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->record, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_inert(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->inert, 0);
  return HLE_RESULT_SUCCESS;
}

/* ---- application records and control data -------------------------- */

static HLE_ServiceResult cmd_list_records(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  const Ns_State *s = state_of(self);
  uint32_t offset = 0;
  (void)ipc_request_read_u32(req, 0, &offset);
  uint32_t count = 0;
  if (s->has_title && offset == 0) {
    /* ApplicationRecord {u64 application_id, u8 type, u8 unk, pad[6],
     * u64 timestamp-ish}. */
    uint8_t record[NS_RECORD_BYTES];
    memset(record, 0, sizeof(record));
    memcpy(record, &s->title_id, 8);
    record[8] = RECORD_TYPE_INSTALLED;
    if (service_write_out(c, req, 0, record, sizeof(record)) == sizeof(record)) count = 1;
  }
  (void)ipc_response_push_u32(res, count);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_record_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, state_of(self)->has_title ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_record_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->record_event);
}

/* {u8 source, pad, u64 application_id} -> u32 size + NsApplicationControlData
 * (NACP 0x4000 bytes, then the JPEG icon). */
static HLE_ServiceResult cmd_control_data(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  const Ns_State *s = state_of(self);
  uint64_t id = 0;
  (void)ipc_request_read_u64(req, 8, &id);
  if (!s->has_title || id != s->title_id || !s->nacp) return NS_RESULT_APPLICATION_NOT_FOUND;
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  if (!buf) return NS_RESULT_APPLICATION_NOT_FOUND;
  uint64_t size = NS_CONTROL_NACP_BYTES;
  (void)service_write_out(c, req, 0, s->nacp, NS_CONTROL_NACP_BYTES);
  if (s->icon && s->icon_size && buf->size >= NS_CONTROL_NACP_BYTES + s->icon_size) {
    (void)vmm_write_block(c->vmm, buf->gva + NS_CONTROL_NACP_BYTES, s->icon, s->icon_size);
    size += s->icon_size;
  }
  (void)ipc_response_push_u32(res, (uint32_t)size);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_max_cache(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, MAX_CONTROL_CACHE);
  return HLE_RESULT_SUCCESS;
}

/* buffer<u64 ids> in -> buffer<ApplicationView 0x50 each> out. */
static HLE_ServiceResult cmd_application_view(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)res;
  const Ns_State *s = state_of(self);
  uint64_t ids[16];
  const uint64_t n = service_read_in(c, req, 0, ids, sizeof(ids)) / 8u;
  const IPC_Buffer *out = service_out_buffer(req, 0);
  for (uint64_t i = 0; out && i < n && (i + 1u) * APPLICATION_VIEW_BYTES <= out->size; i++) {
    if (!s->has_title || ids[i] != s->title_id) return NS_RESULT_APPLICATION_NOT_FOUND;
    uint8_t view[APPLICATION_VIEW_BYTES];
    memset(view, 0, sizeof(view));
    memcpy(view, &ids[i], 8);
    const uint32_t flags = VIEW_FLAGS_RUNNABLE;
    memcpy(view + 0x0C, &flags, 4);
    (void)vmm_write_block(c->vmm, out->gva + i * APPLICATION_VIEW_BYTES, view, sizeof(view));
  }
  return HLE_RESULT_SUCCESS;
}

/* {u64 application_id} -> bool. */
static HLE_ServiceResult cmd_has_record(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  const Ns_State *s = state_of(self);
  uint64_t id = 0;
  (void)ipc_request_read_u64(req, 0, &id);
  (void)ipc_response_push_u32(res, s->has_title && id == s->title_id ? 1u : 0u);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_getter_commands[] = {
    {7988, cmd_get_inert, "GetDynamicRightsInterface"},
    {7989, cmd_get_control_data, "GetReadOnlyApplicationControlDataInterface"},
    {7991, cmd_get_record, "GetReadOnlyApplicationRecordInterface"},
    {7992, cmd_get_inert, "GetECommerceInterface"},
    {7993, cmd_get_inert, "GetApplicationVersionInterface"},
    {7994, cmd_get_inert, "GetDocumentInterface"},
    {7995, cmd_get_inert, "GetDownloadTaskInterface"},
    {7996, cmd_get_manager, "GetApplicationManagerInterface"},
    {7997, cmd_get_inert, "GetContentManagementInterface"},
};

static const Service_Command k_manager_commands[] = {
    {0, cmd_list_records, "ListApplicationRecord"},
    {1, cmd_record_count, "GenerateApplicationRecordCount"},
    {2, cmd_record_event, "GetApplicationRecordUpdateSystemEvent"},
    {44, service_cmd_out_u64_zero, "GetTotalSpaceSize_stub"},
    {47, service_cmd_out_u64_zero, "GetFreeSpaceSize_stub"},
    {400, cmd_control_data, "GetApplicationControlData"},
    {401, service_cmd_ok, "InvalidateAllApplicationControlCache"},
    {403, cmd_max_cache, "GetMaxApplicationControlCacheCount"},
    {404, service_cmd_ok, "InvalidateApplicationControlCache"},
    {1701, cmd_application_view, "GetApplicationView"},
};

static const Service_Command k_control_commands[] = {
    {0, cmd_control_data, "GetApplicationControlData"},
};

static const Service_Command k_record_commands[] = {
    {0, cmd_has_record, "HasApplicationRecord"},
    {1, service_cmd_ok, "NotifyApplicationFailure"},
    {2, service_cmd_out_u8_false, "IsDataCorruptedResult"},
};

static const Service_Command k_inert_commands[] = {
    {0, service_cmd_ok, "Unspecified_stub"},
};

/* ---- ncm ------------------------------------------------------------ */

static HLE_ServiceResult cmd_open_storage(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->ncm_storage, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_open_database(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->ncm_database, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_two_zero_counts(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0);
  (void)ipc_response_push_u32(res, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_content_not_found(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return NCM_RESULT_CONTENT_NOT_FOUND;
}

static HLE_ServiceResult cmd_meta_not_found(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return NCM_RESULT_CONTENT_META_NOT_FOUND;
}

#define NCM_REPORTED_SPACE (32ull * 1024u * 1024u * 1024u)

static HLE_ServiceResult cmd_space(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                   IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, NCM_REPORTED_SPACE);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_ncm_commands[] = {
    {0, service_cmd_ok, "CreateContentStorage"},
    {1, service_cmd_ok, "CreateContentMetaDatabase"},
    {2, service_cmd_ok, "VerifyContentStorage"},
    {3, service_cmd_ok, "VerifyContentMetaDatabase"},
    {4, cmd_open_storage, "OpenContentStorage"},
    {5, cmd_open_database, "OpenContentMetaDatabase"},
    {6, service_cmd_ok, "CloseContentStorageForcibly"},
    {7, service_cmd_ok, "CloseContentMetaDatabaseForcibly"},
    {8, service_cmd_ok, "CleanupContentMetaDatabase"},
    {9, service_cmd_ok, "ActivateContentStorage"},
    {10, service_cmd_ok, "InactivateContentStorage"},
    {11, service_cmd_ok, "ActivateContentMetaDatabase"},
    {12, service_cmd_ok, "InactivateContentMetaDatabase"},
    {13, service_cmd_ok, "InvalidateRightsIdCache"},
};

static const Service_Command k_ncm_storage_commands[] = {
    {0, service_cmd_out_zero128, "GeneratePlaceHolderId"},
    {1, service_cmd_ok, "CreatePlaceHolder"},
    {2, service_cmd_ok, "DeletePlaceHolder"},
    {3, service_cmd_out_u8_false, "HasPlaceHolder"},
    {4, service_cmd_ok, "WritePlaceHolder"},
    {5, service_cmd_ok, "Register"},
    {6, cmd_content_not_found, "Delete"},
    {7, service_cmd_out_u8_false, "Has"},
    {8, cmd_content_not_found, "GetPath"},
    {9, cmd_content_not_found, "GetPlaceHolderPath"},
    {10, service_cmd_ok, "CleanupAllPlaceHolder"},
    {11, service_cmd_out_u8_false, "ListPlaceHolder"},
    {12, service_cmd_out_u8_false, "GetContentCount"},
    {13, service_cmd_out_u8_false, "ListContentId"},
    {14, cmd_content_not_found, "GetSizeFromContentId"},
    {15, service_cmd_ok, "DisableForcibly"},
    {16, service_cmd_ok, "RevertToPlaceHolder"},
    {17, service_cmd_ok, "SetPlaceHolderSize"},
    {18, cmd_content_not_found, "ReadContentIdFile"},
    {19, cmd_content_not_found, "GetRightsIdFromPlaceHolderId"},
    {20, cmd_content_not_found, "GetRightsIdFromContentId"},
    {21, service_cmd_ok, "WriteContentForDebug"},
    {22, cmd_space, "GetFreeSpaceSize"},
    {23, cmd_space, "GetTotalSpaceSize"},
    {24, service_cmd_ok, "FlushPlaceHolder"},
};

static const Service_Command k_ncm_database_commands[] = {
    {0, service_cmd_ok, "Set"},
    {1, cmd_meta_not_found, "Get"},
    {2, cmd_meta_not_found, "Remove"},
    {3, cmd_meta_not_found, "GetContentIdByType"},
    {4, service_cmd_out_u8_false, "ListContentInfo"},
    {5, cmd_two_zero_counts, "List"},
    {6, cmd_meta_not_found, "GetLatestContentMetaKey"},
    {7, cmd_two_zero_counts, "ListApplication"},
    {8, service_cmd_out_u8_false, "Has"},
    {9, service_cmd_out_u8_false, "HasAll"},
    {10, cmd_meta_not_found, "GetSize"},
    {11, cmd_meta_not_found, "GetRequiredSystemVersion"},
    {12, cmd_meta_not_found, "GetPatchId"},
    {13, service_cmd_ok, "DisableForcibly"},
    {14, service_cmd_ok, "LookupOrphanContent"},
    {15, service_cmd_ok, "Commit"},
    {16, service_cmd_out_u8_false, "HasContent"},
    {17, service_cmd_out_u8_false, "ListContentMetaInfo"},
    {18, cmd_meta_not_found, "GetAttributes"},
    {19, cmd_meta_not_found, "GetRequiredApplicationVersion"},
    {20, cmd_meta_not_found, "GetContentIdByTypeAndIdOffset"},
};

static const Service_Command k_es_commands[] = {
    {1, service_cmd_ok, "ImportTicket"},
    {2, service_cmd_ok, "ImportTicketCertificateSet"},
    {3, service_cmd_ok, "DeleteTicket"},
    {4, service_cmd_ok, "DeletePersonalizedTicket"},
    {5, service_cmd_ok, "DeleteAllCommonTicket"},
    {6, service_cmd_ok, "DeleteAllPersonalizedTicket"},
    {7, service_cmd_ok, "DeleteAllPersonalizedTicketEx"},
    {8, cmd_content_not_found, "GetTitleKey"},
    {9, service_cmd_out_u8_false, "CountCommonTicket"},
    {10, service_cmd_out_u8_false, "CountPersonalizedTicket"},
    {11, service_cmd_out_u8_false, "ListCommonTicket"},
    {12, service_cmd_out_u8_false, "ListPersonalizedTicket"},
    {13, service_cmd_out_u8_false, "ListMissingPersonalizedTicket"},
    {14, cmd_content_not_found, "GetCommonTicketSize"},
    {15, cmd_content_not_found, "GetPersonalizedTicketSize"},
    {16, cmd_content_not_found, "GetCommonTicketData"},
    {17, cmd_content_not_found, "GetPersonalizedTicketData"},
    {18, service_cmd_ok, "OwnTicket"},
    {19, cmd_content_not_found, "GetTicketInfo"},
    {20, service_cmd_out_u8_false, "ListLightTicketInfo"},
    {23, service_cmd_out_u8_false, "GetCommonTicketAndCertificateSize"},
    {25, service_cmd_out_u8_false, "ListOwnedTicketRightsIds"},
};

void ns_init(Ns_State *s) {
  const bool has_title = s->has_title;
  const uint64_t title_id = s->title_id;
  const uint8_t *nacp = s->nacp, *icon = s->icon;
  const uint32_t icon_size = s->icon_size;
  memset(s, 0, sizeof(*s));
  for (uint32_t i = 0; i < NS_PORT_COUNT; i++) s->getters[i] = SERVICE_INTERFACE(k_ports[i], k_getter_commands, 0, s);
  s->application_manager = SERVICE_INTERFACE("IApplicationManagerInterface", k_manager_commands, 0, s);
  s->control_data = SERVICE_INTERFACE("IReadOnlyApplicationControlDataInterface", k_control_commands, 0, s);
  s->record = SERVICE_INTERFACE("IReadOnlyApplicationRecordInterface", k_record_commands, 0, s);
  s->inert = SERVICE_INTERFACE("INsInterface", k_inert_commands, 0, s);
  s->ncm = SERVICE_INTERFACE("ncm", k_ncm_commands, 0, s);
  s->es = SERVICE_INTERFACE("es", k_es_commands, 0, s);
  s->ncm_storage = SERVICE_INTERFACE("IContentStorage", k_ncm_storage_commands, 0, s);
  s->ncm_database = SERVICE_INTERFACE("IContentMetaDatabase", k_ncm_database_commands, 0, s);
  /* The loaded title survives a process reset (it is set at load). */
  s->has_title = has_title;
  s->title_id = title_id;
  s->nacp = nacp;
  s->icon = icon;
  s->icon_size = icon_size;
}

Error ns_register(Ns_State *s, SM_Registry *registry) {
  const Error ncm = sm_registry_add(registry, "ncm", &s->ncm);
  if (!error_is_ok(ncm)) return ncm;
  const Error es = sm_registry_add(registry, "es", &s->es);
  if (!error_is_ok(es)) return es;
  for (uint32_t i = 0; i < NS_PORT_COUNT; i++) {
    const Error err = sm_registry_add(registry, k_ports[i], &s->getters[i]);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}

void ns_set_loaded_title(Ns_State *s, uint64_t title_id, const uint8_t *nacp, const uint8_t *icon, uint32_t icon_size) {
  s->has_title = title_id != 0;
  s->title_id = title_id;
  s->nacp = nacp;
  s->icon = icon;
  s->icon_size = icon_size;
}
