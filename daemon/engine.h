#pragma once

#ifndef DAEMON_ENGINE_H_
#define DAEMON_ENGINE_H_

#include <stdint.h>

#include "util.h"

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
  TAB_EVENT_GROUP_UPDATED,
  TAB_EVENT_GROUP_CREATED,
  TAB_EVENT_GROUP_REMOVED,
  TAB_EVENT_UNKNOWN
} TabEventType;

struct ServerContext;
struct StatusBarRunContext;
typedef struct TabInfo {
  uint64_t id;
  char* title;
  int active;
  int64_t task_ext_id;
  struct TabInfo* next;
} TabInfo;

typedef struct TabState {
  struct EngClass* cls;
  int nb_tabs;
  TabInfo* tabs;
} TabState;

typedef struct TaskInfo {
  // task_id removed. external_id is the source of truth.
  // We keep external_id as int64_t.
  // Locally created tasks will have negative IDs.
  char* task_name;
  char* color;
  int64_t external_id;
  struct TaskInfo* next;
} TaskInfo;

typedef struct TaskState {
  struct EngClass* cls;
  int nb_tasks;
  TaskInfo* tasks;
} TaskState;

typedef struct EngineContext {
  struct EngClass* cls;
  struct ServerContext* serv_ctx;
  struct StatusBarRunContext* run_ctx;
  struct TabState* tab_state;
  struct TaskState* task_state;
  int app_pid;  // TODO: internalize this
  int destroyed;
  int init_statusline;
  char* ui_toggle_keybind;
  char* daemon_manifest_path;
  char* gui_manifest_path;
} EngineContext;

typedef struct EngineCreationInfo {
  uint32_t port;
  int enable_statusbar;
  const char* app_path;
  const char* uds_path;
  const char* config_path;
  const char* daemon_manifest_path;
  const char* gui_manifest_path;
} EngineCreationInfo;

// Initializes the daemon engine.
// @param ectx (out) - Will be allocated if engine initialization is successful.
//
// @returns 1 on success and negative value on error.
int engine_init(OUT EngineContext** ectx, EngineCreationInfo cinfo);
void engine_run(EngineContext* ectx);
void engine_destroy(EngineContext* ectx);
void engine_handle_event(EngineContext* ectx, DaemonEvent event, void* data);

// State helpers (exposed for testing)
TabInfo* tab_state_find_tab(TabState* ts, const uint64_t id);
TaskInfo* task_state_find_by_external_id(TaskState* ts, int64_t external_id);
TaskInfo* task_state_find_by_external_id(TaskState* ts, int64_t external_id);
int64_t task_state_incorporate_external_group(TaskState* ts, int64_t external_id, const char* title, const char* color);
void task_state_update(TaskState* ts, int64_t external_id, const char* name, const char* color);
void task_state_remove(TaskState* ts, int64_t external_id);

#ifdef __cplusplus
}
#endif

#endif  // DAEMON_ENGINE_H_
