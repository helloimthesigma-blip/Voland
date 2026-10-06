/**
 * Online-adjacent system services big commercial titles open at start-up
 * (§12 stub tier). Voland is offline (§1.6, §20: no Nintendo servers), so
 * each answers as a console that is signed out and has nothing to report:
 * the titles' SDK initialisation succeeds and the features stay empty.
 *
 *   prepo:u/a/a2/m/s/p  play reports: every Save*Report is accepted and
 *        dropped; GetTransmissionStatus 0 (idle), GetSystemSessionId 0.
 *   friend:u/v/m/s/a    0 CreateFriendService -> IFriendService: no
 *        friends, no blocked users, presence and play-session declarations
 *        accepted, the completion event never fires; 1 CreateNotification-
 *        Service -> INotificationService (event never fires, Pop: nothing).
 *   bcat:u/s/m/a        0 CreateBcatService -> IBcatService (requests
 *        accepted), 1/2 CreateDeliveryCacheStorageService(WithApplicationId)
 *        -> IDeliveryCacheStorageService: no delivery-cache directories;
 *        files and directories fail to open (NotFound).
 *   caps:su / caps:u    album: SetShimLibraryVersion accepted; screenshots
 *        are not saved (SaveScreenShot* report success with a zeroed entry).
 *   fatal:u             0/1/2 ThrowFatal*: the result is logged as an error
 *        and the process stops as crashed (a console shows the fatal screen).
 *   mii:e / mii:u       the Mii database: 0 GetDatabaseService ->
 *        IDatabaseService with no user Miis: IsUpdated / IsFullDatabase
 *        false, GetCount 0, Get* return none, SetInterfaceVersion accepted.
 *   nfp:user / nfp:sys  amiibo: 0 CreateUserInterface -> IUser: Initialize,
 *        ListDevices (none: no NFC reader), state Initialized, attach events
 *        that never fire. Titles report that no amiibo reader is present.
 */
#ifndef SWITCH_HLE_SERVICES_SOCIAL_SOCIAL_H
#define SWITCH_HLE_SERVICES_SOCIAL_SOCIAL_H

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define SOCIAL_PREPO_PORTS 6u
#define SOCIAL_FRIEND_PORTS 5u
#define SOCIAL_BCAT_PORTS 4u
#define SOCIAL_NFP_PORTS 2u
#define SOCIAL_MII_PORTS 2u

#define BCAT_MODULE 122u
#define BCAT_RESULT_NOT_FOUND ((2u << 9) | BCAT_MODULE)
#define FRIENDS_MODULE 121u
#define FRIENDS_RESULT_NO_NOTIFICATION ((303u << 9) | FRIENDS_MODULE)
#define NFP_STATE_INITIALIZED 1u

typedef struct Social_State {
  Service_Interface prepo[SOCIAL_PREPO_PORTS];
  Service_Interface friends[SOCIAL_FRIEND_PORTS];
  Service_Interface friend_service;
  Service_Interface friend_notifications;
  Kernel_Event *friend_event;
  Service_Interface bcat[SOCIAL_BCAT_PORTS];
  Service_Interface bcat_service;
  Service_Interface bcat_storage;
  Service_Interface caps_su;
  Service_Interface caps_u;
  Service_Interface fatal;
  Service_Interface mii[SOCIAL_MII_PORTS];
  Service_Interface mii_database;
  Service_Interface nfp[SOCIAL_NFP_PORTS];
  Service_Interface nfp_user;
  Kernel_Event *nfp_event;
} Social_State;

void social_init(Social_State *state);
Error social_register(Social_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_SOCIAL_SOCIAL_H */
