/**
 * Offline networking (§12, §20): bsd:u / bsd:s (BSD sockets) and nifm:u
 * (network interface manager), presenting a console with no network.
 * Titles and homebrew initialize both at boot whether or not they go
 * online, so the services must exist; every socket operation fails with
 * ENETDOWN and nifm reports no connection. Nothing here connects
 * anywhere (CLAUDE.md rule 9) - real multiplayer transport is §20's
 * LDN-over-WebRTC, a separate path.
 *
 *   bsd: 0 RegisterClient (-> pid), 1 StartMonitoring, 2-31 socket calls
 *        -> {ret -1, errno ENETDOWN} (Close -> 0).
 *   nifm: 4/5 CreateGeneralService(Old) -> IGeneralService: 1 GetClientId,
 *        4 CreateRequest -> IRequest (state Free, result NotConnected),
 *        12/15/18 address/status queries -> NotConnected, 17/20/21/22
 *        "enabled/accepted" queries -> false, 23/24 sleep/wake.
 */
#ifndef SWITCH_HLE_SERVICES_NETWORK_NETWORK_H
#define SWITCH_HLE_SERVICES_NETWORK_NETWORK_H

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define NIFM_MODULE 110u
#define NIFM_RESULT_NOT_CONNECTED ((300u << 9) | NIFM_MODULE)
#define BSD_ENETDOWN 50
#define BSD_CLIENT_PID 0x51u

typedef struct Network_State {
  Service_Interface bsd;
  Service_Interface nifm;
  Service_Interface general_service;
  Service_Interface request;
  Kernel_Event *request_event;
} Network_State;

void network_init(Network_State *state);
Error network_register(Network_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_NETWORK_NETWORK_H */
