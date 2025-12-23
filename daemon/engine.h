#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Events that can trigger engine actions
typedef enum { EVENT_APP_STARTED, EVENT_HOTKEY_TOGGLE, EVENT_MENU_QUIT, EVENT_WS_MESSAGE_RECEIVED } DaemonEvent;

// Interface for platform-specific operations
typedef struct {
  void (*log)(const char *msg);
  void (*send_uds)(const char *data);
  void (*spawn_gui)(void);
  void (*kill_gui)(void);
  void (*quit_app)(void);
} PlatformAdapter;

// Tab-specific events
typedef enum {
  TAB_EVENT_ACTIVATED,
  TAB_EVENT_UPDATED,
  TAB_EVENT_HIGHLIGHTED,
  TAB_EVENT_ZOOM_CHANGE,
  TAB_EVENT_UNKNOWN
} TabEventType;

// Log Levels
typedef enum { LOG_LEVEL_INFO = 0, LOG_LEVEL_TRACE = 1 } LogLevel;

// Initialize the engine with the platform adapter
void engine_init(PlatformAdapter *adapter);

// Set the log level
void engine_set_log_level(LogLevel level);

// Log a message with the specified level
void vlog(LogLevel level, const char *fmt, ...);

// Handle an incoming event
void engine_handle_event(DaemonEvent event, void *data);

// Handle a parsed tab event
void engine_handle_tab_event(TabEventType type, const char *json_data);

#ifdef __cplusplus
}
#endif
