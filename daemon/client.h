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
  int task_id;
} LotabTab;

typedef struct LotabTask {
  int id;
  char* name;
  char* color;
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
void lotab_client_stop(ClientContext* ctx);

// Runs the accepting loop. Blocking. Call in a thread.
void lotab_client_run_loop(ClientContext* ctx);

// Send Actions
void lotab_client_send_close_tabs(ClientContext* ctx, const int* tab_ids, size_t count);
void lotab_client_send_tab_selected(ClientContext* ctx, int tab_id);

// Exposed for testing purposes
void lotab_client_process_message(ClientContext* ctx, const char* json_str);

/// MODE API
// Manages the mode transitions and state for the lotab gui application.
typedef enum LmMode {
  LM_MODE_UNKNOWN,
  // In this mode, a list of tabs are shown and are selectable in a
  // multi-select list.
  LM_MODE_LIST_NORMAL,
  // User is actively setting the filter.
  LM_MODE_LIST_FILTER_INFLIGHT,
  // User is viewing a list with filters applied.
  LM_MODE_LIST_FILTER_COMMITTED,
  LM_MODE_LIST_MULTISELECT,
} LmMode;

// This communicates how the GUI should respond to events.
typedef enum LmModeTransition {
  LM_MODETS_UNKNOWN,
  LM_MODETS_HIDE_UI,
  LM_MODETS_SELECT_TAB,
  LM_MODETS_SELECT_ALL_TABS,
  LM_MODETS_NAVIGATE_UP,
  LM_MODETS_NAVIGATE_DOWN,

  // Applies current filtering to the list and transitions to
  // normal LM_MODE_LIST_NORMAL.
  LM_MODETS_COMMIT_LIST_FILTER,
  LM_MODETS_UPDATE_LIST_FILTER,

  LM_MODETS_ACTIVATE_TO_TAB,
  LM_MODETS_CLOSE_SELECTED_TABS,

  // LM_MODETS_ADHERE_TO_MODE describes a transition that requires the
  // UI to adhere to the current application mode.
  LM_MODETS_ADHERE_TO_MODE,
} LmModeTransition;

// Opaque context
typedef struct ModeContext ModeContext;

ModeContext* lm_alloc(void);
void lm_destroy(ModeContext* mctx);

// NOTE: For now, the event code uses macOS key code mapping. When
// implementing this for other operating systems, some key map will need
// to be preloaded to understand the different OS mappings, since they
// are not the same.
void lm_process_key_event(ModeContext* mctx,                 //
                          uint16_t key_code,                 //
                          uint8_t ascii_code,                //
                          uint8_t cmd,                       //
                          uint8_t shift,                     //
                          LmModeTransition* out_transition,  //
                          LmMode* out_old_mode,              //
                          LmMode* out_new_mode);

void lm_on_list_len_update(ModeContext* mctx,
                           int list_len,
                           LmModeTransition* out_transition,
                           LmMode* out_old_mode,
                           LmMode* out_new_mode);

char* lm_get_filter_text(ModeContext* mctx);

#ifdef __cplusplus
}
#endif

#endif  // DAEMON_CLIENT_H_
