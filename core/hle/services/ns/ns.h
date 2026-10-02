/**
 * ns - application management (§12), the read-only half homebrew save
 * managers and launchers use. Every ns:* port (am2, ec, rid, rt, web,
 * ro) serves IServiceGetterInterface, whose getters hand out the
 * interfaces below. Voland installs nothing (no NAND, §1.6), so the
 * application record list is empty unless a title is loaded from an NCA:
 * then that one title is listed, with the control data (NACP + icon) its
 * own content supplies (ns_set_loaded_title).
 *
 *   IServiceGetterInterface: 7988 GetDynamicRightsInterface, 7989
 *     GetReadOnlyApplicationControlDataInterface, 7991 GetReadOnly-
 *     ApplicationRecordInterface, 7992 GetECommerceInterface, 7993
 *     GetApplicationVersionInterface, 7994 GetDocumentInterface, 7995
 *     GetDownloadTaskInterface, 7996 GetApplicationManagerInterface,
 *     7997 GetContentManagementInterface.
 *   IApplicationManagerInterface: 0 ListApplicationRecord, 1 Generate-
 *     ApplicationRecordCount, 2 GetApplicationRecordUpdateSystemEvent,
 *     400 GetApplicationControlData, 403 GetMaxApplicationControlCache-
 *     Count, 1701 GetApplicationView (and inert maintenance commands).
 *   IReadOnlyApplicationControlDataInterface: 0 GetApplicationControlData.
 *   IReadOnlyApplicationRecordInterface: 0 HasApplicationRecord.
 */
#ifndef SWITCH_HLE_SERVICES_NS_NS_H
#define SWITCH_HLE_SERVICES_NS_NS_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define NS_MODULE 16u
#define NS_RESULT_APPLICATION_NOT_FOUND ((300u << 9) | NS_MODULE)
#define NS_CONTROL_NACP_BYTES 0x4000u
#define NS_CONTROL_MAX_ICON_BYTES 0x20000u
#define NS_RECORD_BYTES 0x18u
#define NS_PORT_COUNT 6u

typedef struct Ns_State {
  Service_Interface getters[NS_PORT_COUNT];
  Service_Interface application_manager;
  Service_Interface control_data;
  Service_Interface record;
  Service_Interface inert;
  Kernel_Event *record_event;
  /* The title loaded from an NCA, if any. */
  bool has_title;
  uint64_t title_id;
  const uint8_t *nacp;      /* NS_CONTROL_NACP_BYTES, or NULL */
  const uint8_t *icon;      /* JPEG, or NULL */
  uint32_t icon_size;
} Ns_State;

void ns_init(Ns_State *state);
Error ns_register(Ns_State *state, SM_Registry *registry);
/* Lists `title_id` as installed; `nacp`/`icon` stay owned by the caller
 * and must outlive the process. */
void ns_set_loaded_title(Ns_State *state, uint64_t title_id, const uint8_t *nacp, const uint8_t *icon,
                         uint32_t icon_size);

#endif /* SWITCH_HLE_SERVICES_NS_NS_H */
