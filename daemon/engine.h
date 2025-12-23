#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Events that can trigger engine actions
typedef enum { EVENT_HOTKEY_TOGGLE, EVENT_WS_MESSAGE_RECEIVED } DaemonEvent;

// Interface for platform-specific operations
typedef struct {
  void (*quit_cb)(void);
} PlatformAdapter;

// Tab-specific events
typedef enum {
  TAB_EVENT_ACTIVATED,
  TAB_EVENT_UPDATED,
  TAB_EVENT_HIGHLIGHTED,
  TAB_EVENT_ZOOM_CHANGE,
  TAB_EVENT_UNKNOWN
} TabEventType;

typedef enum { LOG_LEVEL_INFO = 0, LOG_LEVEL_ERROR = 1, LOG_LEVEL_WARN = 3, LOG_LEVEL_TRACE = 2 } LogLevel;

struct ServerContext;
struct StatusBarRunContext;
typedef struct EngineContext {
  struct ServerContext* serv_ctx;
  struct StatusBarRunContext* run_ctx;
  PlatformAdapter* adapter;
  int app_pid;  // TODO: internalize this
  int destroyed;
} EngineContext;

// Initializes the daemon engine.
// @param ectx (out) - Will be allocated if engine initialization is successful.
//
// @returns 1 on success and negative value on error.
int engine_init(EngineContext** ectx, PlatformAdapter* adapter);
void engine_run(EngineContext* ectx);
void engine_destroy(EngineContext* ectx);
void engine_set_log_level(LogLevel level);
void engine_handle_event(EngineContext* ectx, DaemonEvent event, void* data);
void engine_handle_tab_event(TabEventType type, const char* json_data);

// Log a message with the specified level
void vlog(LogLevel level, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
