#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OUT __attribute__((annotate("out")))

// Events that can trigger engine actions
typedef enum { EVENT_HOTKEY_TOGGLE, EVENT_WS_MESSAGE_RECEIVED } DaemonEvent;

// Tab-specific events
typedef enum {
  TAB_EVENT_ACTIVATED,
  TAB_EVENT_UPDATED,
  TAB_EVENT_CREATED,
  TAB_EVENT_HIGHLIGHTED,
  TAB_EVENT_ZOOM_CHANGE,
  TAB_EVENT_ALL_TABS,
  TAB_EVENT_TAB_REMOVED,
  TAB_EVENT_UNKNOWN
} TabEventType;

typedef enum { LOG_LEVEL_WARN = 0, LOG_LEVEL_ERROR = 1, LOG_LEVEL_INFO = 2, LOG_LEVEL_TRACE = 3 } LogLevel;

struct ServerContext;
struct StatusBarRunContext;
typedef struct TabInfo {
  uint64_t id;
  char* title;
  int active;
  struct TabInfo* next;
} TabInfo;

typedef struct TabState {
  int nb_tabs;
  TabInfo* tabs;
} TabState;

typedef struct TaskInfo {
  uint64_t task_id;
  char* task_name;
  struct TaskInfo* next;
} TaskInfo;

typedef struct TaskState {
  int nb_tasks;
  TaskInfo* tasks;
} TaskState;

typedef struct EngineContext {
  struct ServerContext* serv_ctx;
  struct StatusBarRunContext* run_ctx;
  struct TabState* tab_state;
  struct TaskState* task_state;
  int app_pid;  // TODO: internalize this
  int destroyed;
  int init_statusline;
} EngineContext;

typedef struct EngineCreationInfo {
  uint32_t port;
  int enable_statusbar;
} EngineCreationInfo;

// Initializes the daemon engine.
// @param ectx (out) - Will be allocated if engine initialization is successful.
//
// @returns 1 on success and negative value on error.
int engine_init(OUT EngineContext** ectx, EngineCreationInfo cinfo);
void engine_run(EngineContext* ectx);
void engine_destroy(EngineContext* ectx);
void engine_set_log_level(LogLevel level);
void engine_handle_event(EngineContext* ectx, DaemonEvent event, void* data);

// Log a message with the specified level
void vlog(LogLevel level, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
