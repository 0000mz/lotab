#ifndef DAEMON_CLIENT_H_
#define DAEMON_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque context
typedef struct ClientContext ClientContext;

// Data Structures
typedef struct LotabTab {
  int id;
  char* title;
  bool active;
} LotabTab;

typedef struct LotabTask {
  int id;
  char* name;
} LotabTask;

typedef struct LotabTabList {
  size_t count;
  LotabTab* tabs;
} LotabTabList;

typedef struct LotabTaskList {
  size_t count;
  LotabTask* tasks;
} LotabTaskList;

// Callbacks
// Note: Pointers invalid after callback returns (lifetimes managed by client)
typedef void (*lotab_on_tabs_update_cb)(void* user_data, const LotabTabList* tabs);
typedef void (*lotab_on_tasks_update_cb)(void* user_data, const LotabTaskList* tasks);
typedef void (*lotab_on_ui_toggle_cb)(void* user_data);

typedef struct ClientCallbacks {
  lotab_on_tabs_update_cb on_tabs_update;
  lotab_on_tasks_update_cb on_tasks_update;
  lotab_on_ui_toggle_cb on_ui_toggle;
} ClientCallbacks;

// API
ClientContext* lotab_client_new(const char* socket_path, ClientCallbacks callbacks, void* user_data);
void lotab_client_destroy(ClientContext* ctx);

// Runs the accepting loop. Blocking. Call in a thread.
void lotab_client_run_loop(ClientContext* ctx);

// Send Actions
void lotab_client_send_close_tabs(ClientContext* ctx, const int* tab_ids, size_t count);
void lotab_client_send_tab_selected(ClientContext* ctx, int tab_id);

// Exposed for testing purposes
void lotab_client_process_message(ClientContext* ctx, const char* json_str);

#ifdef __cplusplus
}
#endif

#endif  // DAEMON_CLIENT_H_
