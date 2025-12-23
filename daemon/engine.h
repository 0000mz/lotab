#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Events that can trigger engine actions
typedef enum {
  EVENT_APP_STARTED,
  EVENT_HOTKEY_TOGGLE,
  EVENT_MENU_QUIT,
  EVENT_WS_MESSAGE_RECEIVED
} DaemonEvent;

// Interface for platform-specific operations
typedef struct {
  void (*log)(const char *msg);
  void (*send_uds)(const char *data);
  void (*spawn_gui)(void);
  void (*kill_gui)(void);
  void (*quit_app)(void);
} PlatformAdapter;

// Initialize the engine with the platform adapter
void engine_init(PlatformAdapter *adapter);

// Handle an incoming event
void engine_handle_event(DaemonEvent event, void *data);

#ifdef __cplusplus
}
#endif
