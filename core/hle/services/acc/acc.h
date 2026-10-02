/**
 * acc - user accounts (§12). Voland presents one local user, "Player",
 * with a generated profile icon and no network service account (§1.6:
 * nothing online). Registered as acc:u0 (applications), acc:u1 and acc:su.
 *
 *   0 GetUserCount, 1 GetUserExistence, 2/3 ListAllUsers/ListOpenUsers,
 *   4 GetLastOpenedUser, 5 GetProfile -> IProfile, 6 GetProfileDigest,
 *   50 IsUserRegistrationRequestPermitted, 51 TrySelectUserWithoutInter-
 *   action, 100/140 InitializeApplicationInfo(V0), 101 GetBaasAccount-
 *   ManagerForApplication / 130 LoadOpenContext -> IManagerForApplication,
 *   110/111 Store/ClearSaveDataThumbnail, 131/141 List*Users,
 *   150 IsUserAccountSwitchLocked.
 *   IProfile: 0 Get, 1 GetBase, 10 GetImageSize, 11 LoadImage.
 *   IManagerForApplication: 0 CheckAvailability, 1 GetAccountId; the
 *   network-account operations fail with "not connected".
 *
 * The same user is the application's preselected user: am's
 * PopLaunchParameter(PreselectedUser) hands it out (am.c).
 */
#ifndef SWITCH_HLE_SERVICES_ACC_ACC_H
#define SWITCH_HLE_SERVICES_ACC_ACC_H

#include <stdint.h>

#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define ACC_UID_BYTES 16u
#define ACC_USER_UID_LO 0x564F4C414E440001ull /* "VOLAND" + 1: any non-zero id works */
#define ACC_USER_UID_HI 0x0000000000000001ull
#define ACC_NICKNAME "Player"

extern const uint8_t k_acc_profile_icon[];
extern const uint32_t k_acc_profile_icon_size;

typedef struct Acc_State {
  Service_Interface service;
  Service_Interface profile;
  Service_Interface manager;
} Acc_State;

void acc_init(Acc_State *state);
Error acc_register(Acc_State *state, SM_Registry *registry);

/* The user's 16-byte id as it appears on the wire. */
void acc_user_uid(uint8_t out[ACC_UID_BYTES]);

#endif /* SWITCH_HLE_SERVICES_ACC_ACC_H */
