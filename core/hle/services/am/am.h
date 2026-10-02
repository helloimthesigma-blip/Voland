/**
 * am - the applet manager (§12): "appletOE" (applications) and "appletAE"
 * (applets), and every interface their proxies hand out. This is the
 * service whose message loop tells a title it has focus; libnx's
 * appletInitialize and nn::oe both block on it before main runs.
 *
 *   appletOE 0 OpenApplicationProxy / appletAE 100/200/201/300/350
 *     Open*Proxy -> IApplicationProxy-shaped object:
 *       0 ICommonStateGetter   1 ISelfController   2 IWindowController
 *       3 IAudioController     4 IDisplayController
 *       11 ILibraryAppletCreator   20 IApplicationFunctions
 *       1000 IDebugFunctions   (applet-only getters answer the same set)
 *
 * State Voland presents: in focus from the first message (a
 * FocusStateChanged message is queued at open so titles that wait for it
 * see it), handheld operation mode, Normal performance mode, 1280x720.
 * PopLaunchParameter has no data except the preselected user.
 *
 * Library applets: storages a title pushes (PushInData, including
 * transfer-memory storages) are kept by the applet; Start runs it. The
 * software keyboard (0x11) becomes a pending text request the host
 * answers (am_text_request / am_text_respond: the web shell's dialog,
 * the CLI's --swkbd); it then completes with the keyboard's output
 * storage {u32 close result, UTF-16 text}. Controller support (0xC),
 * player select (0x10) and the rest complete at once with the outputs
 * their callers parse (one handheld player, the local user) or none.
 * IStorage / IStorageAccessor are real.
 */
#ifndef SWITCH_HLE_SERVICES_AM_AM_H
#define SWITCH_HLE_SERVICES_AM_AM_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define AM_MODULE 128u
#define AM_RESULT_NO_MESSAGES ((3u << 9) | AM_MODULE)       /* 0x680 */
#define AM_RESULT_NO_DATA_IN_CHANNEL ((2u << 9) | AM_MODULE) /* 0x480 */
#define AM_RESULT_OUT_OF_BOUNDS ((503u << 9) | AM_MODULE)

#define AM_MESSAGE_QUEUE 16u
#define AM_MESSAGE_FOCUS_STATE_CHANGED 15u
#define AM_MESSAGE_OPERATION_MODE_CHANGED 30u
#define AM_MESSAGE_PERFORMANCE_MODE_CHANGED 31u
#define AM_MESSAGE_EXIT_REQUESTED 4u
#define AM_FOCUS_IN_FOCUS 1u
#define AM_OPERATION_MODE_HANDHELD 0u
#define AM_DISPLAY_WIDTH 1280u
#define AM_DISPLAY_HEIGHT 720u
#define AM_APPLET_RESOURCE_USER_ID 0x51u

#define AM_STORAGE_CAPACITY 32u
#define AM_STORAGE_MAX_BYTES 0x8000u /* software keyboard configs are the largest common storage */
#define AM_APPLET_CAPACITY 8u

#define AM_STORAGE_POOL_BYTES ((uint64_t)AM_STORAGE_CAPACITY * AM_STORAGE_MAX_BYTES)

#define AM_APPLET_STORAGES 8u
#define AM_TEXT_BYTES 2048u
#define AM_SWKBD_OUTPUT_BYTES 0x7D8u
#define AM_APPLET_SWKBD 0x11u
#define AM_APPLET_CONTROLLER 0x0Cu
#define AM_APPLET_PLAYER_SELECT 0x10u

typedef struct Am_Storage {
  bool in_use;
  bool retained;          /* held by a library applet: outlives the guest's handle */
  uint32_t size;
  uint64_t tmem_address;  /* transfer-memory storage: bytes live in guest memory */
  uint64_t tmem_size;
  uint8_t *data; /* AM_STORAGE_MAX_BYTES of the pool handed to am_init */
} Am_Storage;

typedef struct Am_Applet {
  bool in_use;
  uint32_t applet_id;
  Kernel_Event *state_event;
  Kernel_Event *out_event;
  Kernel_Event *interactive_out_event;
  bool started;
  bool completed;
  uint32_t result;               /* GetResult */
  uint32_t in[AM_APPLET_STORAGES];   /* storage indices pushed in */
  uint32_t in_count;
  uint32_t out[AM_APPLET_STORAGES];  /* output queue */
  uint32_t out_head, out_count;
} Am_Applet;

/* A software-keyboard request waiting for the host. */
typedef struct Am_Text_Request {
  bool pending;
  uint32_t applet;               /* applet slot */
  char header[AM_TEXT_BYTES / 4u];
  char sub[AM_TEXT_BYTES / 4u];
  char guide[AM_TEXT_BYTES / 2u];
  char initial[AM_TEXT_BYTES];
  uint32_t max_length;           /* 0 = no limit */
  uint32_t min_length;
  bool password;
} Am_Text_Request;

typedef struct Am_State {
  Service_Interface oe;
  Service_Interface ae;
  Service_Interface proxy;
  Service_Interface common_state_getter;
  Service_Interface self_controller;
  Service_Interface window_controller;
  Service_Interface audio_controller;
  Service_Interface display_controller;
  Service_Interface library_applet_creator;
  Service_Interface application_functions;
  Service_Interface debug_functions;
  Service_Interface storage;            /* object state = storage index */
  Service_Interface storage_accessor;   /* object state = storage index */
  Service_Interface library_applet_accessor; /* object state = applet index */

  uint32_t messages[AM_MESSAGE_QUEUE];
  uint32_t message_head;
  uint32_t message_count;
  Kernel_Event *message_event;
  Kernel_Event *launchable_event;
  Kernel_Event *suspended_tick_event;
  Kernel_Event *resolution_event;
  Kernel_Event *gpu_error_event;
  Kernel_Event *friend_invitation_event;
  Kernel_Event *notification_event;
  Kernel_Event *health_warning_event;
  Kernel_Event *event_210; /* IApplicationFunctions 210 (unnamed; never fires) */
  Kernel_Event *sleep_lock_event;
  float master_volume;
  uint32_t idle_time_extension;
  uint64_t next_layer_id;
  bool exit_locked;
  bool launched; /* the first proxy open queued FocusStateChanged */
  bool preselected_user_popped; /* PopLaunchParameter(PreselectedUser) is one-shot */

  Am_Storage storages[AM_STORAGE_CAPACITY];
  Am_Applet applets[AM_APPLET_CAPACITY];
  Am_Text_Request text_request;
  uint64_t player_select_uid[2]; /* the user player select returns */
} Am_State;

/* `storage_pool`: AM_STORAGE_POOL_BYTES the caller owns (outlives state). */
void am_init(Am_State *state, uint8_t *storage_pool);
Error am_register(Am_State *state, SM_Registry *registry);

/* The pending software-keyboard request, or NULL. */
const Am_Text_Request *am_text_request(const Am_State *state);
/* Answers it: `accepted` false is the player cancelling. Completes the
 * keyboard applet (its state event fires; PopOutData returns the text). */
void am_text_respond(Am_State *state, HLE_Context *context, const char *utf8, bool accepted);

/* Queues an applet message and signals the message event. */
void am_push_message(Am_State *state, HLE_Context *context, uint32_t message);

#endif /* SWITCH_HLE_SERVICES_AM_AM_H */
