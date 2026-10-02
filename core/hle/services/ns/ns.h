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
 *     GetApplicationVersionInterface, 7994 GetFactoryResetInterface,
 *     7995 GetAccountProxyInterface, 7996 GetApplicationManagerInterface,
 *     7997 GetDownloadTaskInterface, 7998 GetContentManagementInterface,
 *     7999 GetDocumentInterface.
 *   IContentManagementInterface: 11 CalculateApplicationOccupiedSize
 *     (zero), 43 CheckSdCardMountStatus, 47/48 Get{Total,Free}SpaceSize,
 *     600/601/605 content meta (none), 607 IsAnyApplicationRunning.
 *   IApplicationManagerInterface: 0 ListApplicationRecord, 1 Generate-
 *     ApplicationRecordCount, 2 GetApplicationRecordUpdateSystemEvent,
 *     400 GetApplicationControlData, 403 GetMaxApplicationControlCache-
 *     Count, 1701 GetApplicationView (and inert maintenance commands).
 *   IReadOnlyApplicationControlDataInterface: 0 GetApplicationControlData.
 *   IReadOnlyApplicationRecordInterface: 0 HasApplicationRecord.
 *
 * ncm - the content manager, same story: every storage (built-in system /
 * user, SD card, game card) opens and is empty. IContentManager 0-13;
 * IContentStorage counts/lists nothing, has nothing, reports free space;
 * IContentMetaDatabase lists nothing and finds nothing.
 *
 * aoc:u - add-on content: none installed (count 0, empty lists, the
 * derived base id, an event that never fires, no purchases).
 *
 * es - the ticket service: there are no tickets (Voland handles no keys or
 * rights data, §1.6); counts are zero, lists empty, lookups fail.
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
#define NS_OCCUPIED_SIZE_BYTES 0x80u
#define NS_AOC_ID_MASK 0xfffull
#define NS_AOC_BASE_OFFSET 0x1000ull
#define NS_RESULT_AOC_NO_PURCHASED_PRODUCT ((900u << 9) | 166u) /* nim: NoPurchasedProductInfoAvailable */

#define NCM_MODULE 5u
#define NCM_RESULT_CONTENT_NOT_FOUND ((7u << 9) | NCM_MODULE)
#define NCM_RESULT_CONTENT_META_NOT_FOUND ((8u << 9) | NCM_MODULE)

#define ES_MODULE 5u /* reported through ncm's module, as "not found" */

typedef struct Ns_State {
  Service_Interface es;
  Service_Interface ncm;
  Service_Interface ncm_storage;
  Service_Interface ncm_database;
  Service_Interface getters[NS_PORT_COUNT];
  Service_Interface application_manager;
  Service_Interface control_data;
  Service_Interface record;
  Service_Interface inert;
  Service_Interface content_management;
  Service_Interface aoc;
  Service_Interface aoc_purchase;
  Kernel_Event *aoc_event;
  Kernel_Event *record_event;
  Kernel_Event *sd_event;  /* SD card mount status: never changes */
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
