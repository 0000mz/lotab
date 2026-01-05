#include "client.h"
#include <stdatomic.h>
#include "util.h"

#include <assert.h>
#include <cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct ClientContext {
  struct EngClass* cls;  // Logging context - Must be first for vlog
  char* socket_path;
  ClientCallbacks callbacks;
  void* user_data;
  int server_fd;
  int active_client_fd;
  atomic_bool should_stop;
};

static struct EngClass CLIENT_CLS = {.name = "uds_client"};

ClientContext* lotab_client_new(const char* socket_path, ClientCallbacks callbacks, void* user_data) {
  ClientContext* ctx = (ClientContext*)malloc(sizeof(ClientContext));
  if (!ctx)
    return NULL;

  // Copy string
  ctx->socket_path = strdup(socket_path);
  ctx->callbacks = callbacks;
  ctx->user_data = user_data;
  ctx->server_fd = -1;
  ctx->active_client_fd = -1;
  atomic_init(&ctx->should_stop, false);
  ctx->cls = &CLIENT_CLS;

  return ctx;
}

void lotab_client_destroy(ClientContext* ctx) {
  if (!ctx)
    return;

  lotab_client_stop(ctx);

  if (ctx->active_client_fd >= 0) {
    close(ctx->active_client_fd);
  }
  if (ctx->server_fd >= 0) {
    close(ctx->server_fd);
  }
  if (ctx->socket_path) {
    unlink(ctx->socket_path);
    free(ctx->socket_path);
  }
  free(ctx);
}

void lotab_client_stop(ClientContext* ctx) {
  if (!ctx)
    return;

  atomic_store(&ctx->should_stop, true);

  if (ctx->server_fd >= 0) {
    // Shutdown and close to wake up accept()
    // Using shutdown(SHUT_RDWR) ensures accept() returns with an error.
    shutdown(ctx->server_fd, SHUT_RDWR);
    close(ctx->server_fd);
    ctx->server_fd = -1;
  }
  if (ctx->active_client_fd >= 0) {
    shutdown(ctx->active_client_fd, SHUT_RDWR);
    close(ctx->active_client_fd);
    ctx->active_client_fd = -1;
  }
}

// Helper to free tab list
static void free_tab_list(LotabTabList* list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->count; i++) {
    if (list->tabs[i].title)
      free(list->tabs[i].title);
  }
  free(list->tabs);
}

// Helper to free task list
static void free_task_list(LotabTaskList* list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->count; i++) {
    if (list->tasks[i].name)
      free(list->tasks[i].name);
    if (list->tasks[i].color)
      free(list->tasks[i].color);
  }
  free(list->tasks);
}

void lotab_client_process_message(ClientContext* ctx, const char* json_str) {
  cJSON* json = cJSON_Parse(json_str);
  if (!json) {
    vlog(LOG_LEVEL_ERROR, ctx, "Failed to parse JSON message: %s\n", json_str);
    return;
  }

  cJSON* event = cJSON_GetObjectItemCaseSensitive(json, "event");
  if (!cJSON_IsString(event) || !event->valuestring) {
    cJSON_Delete(json);
    return;
  }

  vlog(LOG_LEVEL_TRACE, ctx, "uds-event: %s\n", event->valuestring);

  if (strcmp(event->valuestring, "Daemon::UDS::TabsUpdate") == 0) {
    // Parse tabs
    cJSON* data = cJSON_GetObjectItem(json, "data");
    cJSON* tabs_json = cJSON_GetObjectItem(data, "tabs");

    if (cJSON_IsArray(tabs_json)) {
      int count = cJSON_GetArraySize(tabs_json);
      LotabTabList list = {0};
      list.count = (size_t)count;
      list.tabs = calloc(list.count, sizeof(LotabTab));

      for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(tabs_json, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* title = cJSON_GetObjectItem(item, "title");
        cJSON* active = cJSON_GetObjectItem(item, "active");
        cJSON* task_id = cJSON_GetObjectItem(item, "task_id");

        list.tabs[i].id = cJSON_IsNumber(id) ? (int)id->valuedouble : 0;
        list.tabs[i].title = (cJSON_IsString(title) && title->valuestring) ? strdup(title->valuestring) : strdup("");
        list.tabs[i].active = cJSON_IsBool(active) ? cJSON_IsTrue(active) : false;
        list.tabs[i].task_id = cJSON_IsNumber(task_id) ? (int)task_id->valuedouble : -1;
      }

      if (ctx->callbacks.on_tabs_update) {
        ctx->callbacks.on_tabs_update(ctx->user_data, &list);
      } else {
        vlog(LOG_LEVEL_WARN, ctx, "on_tabs_update callback is NULL\n");
      }

      free_tab_list(&list);
    }
  } else if (strcmp(event->valuestring, "Daemon::UDS::TasksUpdate") == 0) {
    // Parse tasks
    cJSON* data = cJSON_GetObjectItem(json, "data");
    cJSON* tasks_json = cJSON_GetObjectItem(data, "tasks");

    if (cJSON_IsArray(tasks_json)) {
      int count = cJSON_GetArraySize(tasks_json);
      LotabTaskList list = {0};
      list.count = (size_t)count;
      list.tasks = calloc(list.count, sizeof(LotabTask));

      for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(tasks_json, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* color = cJSON_GetObjectItem(item, "color");

        list.tasks[i].id = cJSON_IsNumber(id) ? (int)id->valuedouble : 0;
        list.tasks[i].name = (cJSON_IsString(name) && name->valuestring) ? strdup(name->valuestring) : strdup("");
        list.tasks[i].color = (cJSON_IsString(color) && color->valuestring) ? strdup(color->valuestring) : strdup("grey");
      }

      if (ctx->callbacks.on_tasks_update) {
        ctx->callbacks.on_tasks_update(ctx->user_data, &list);
      } else {
        vlog(LOG_LEVEL_WARN, ctx, "on_tasks_update callback is NULL\n");
      }

      free_task_list(&list);
    }
  } else if (strcmp(event->valuestring, "Daemon::UDS::ToggleGuiRequest") == 0) {
    vlog(LOG_LEVEL_INFO, ctx, "Processing Daemon::UDS::ToggleGuiRequest\n");
    if (ctx->callbacks.on_ui_toggle) {
      ctx->callbacks.on_ui_toggle(ctx->user_data);
    } else {
      vlog(LOG_LEVEL_WARN, ctx, "on_ui_toggle callback is NULL\n");
    }
  } else {
    vlog(LOG_LEVEL_INFO, ctx, "Unknown UDS event: %s\n", event->valuestring);
  }

  cJSON_Delete(json);
}

static void handle_client(ClientContext* ctx, int client_socket) {
  ctx->active_client_fd = client_socket;

  while (1) {
    // 1. Read Header
    uint32_t msg_len;
    ssize_t n = recv(client_socket, &msg_len, sizeof(msg_len), 0);

    if (n == 0) {
      vlog(LOG_LEVEL_INFO, ctx, "UDS connection closed by peer\n");
      break;
    } else if (n < 0) {
      vlog(LOG_LEVEL_ERROR, ctx, "UDS header read error: %s\n", strerror(errno));
      break;
    } else if (n < (ssize_t)sizeof(msg_len)) {
      vlog(LOG_LEVEL_ERROR, ctx, "UDS partial header read\n");
      break;
    }

    // 2. Read Payload
    char* buffer = malloc(msg_len + 1);
    if (!buffer) {
      vlog(LOG_LEVEL_ERROR, ctx, "OOM\n");
      break;  // OOM
    }

    size_t total_read = 0;
    int err = 0;
    while (total_read < msg_len) {
      n = recv(client_socket, buffer + total_read, msg_len - total_read, 0);
      if (n <= 0) {
        err = 1;
        break;
      }
      total_read += n;
    }

    if (err) {
      vlog(LOG_LEVEL_ERROR, ctx, "UDS payload read error\n");
      free(buffer);
      break;
    }

    buffer[msg_len] = '\0';
    vlog(LOG_LEVEL_TRACE, ctx, "uds-read: %s\n", buffer);

    // 3. Process
    lotab_client_process_message(ctx, buffer);
    free(buffer);
  }

  close(client_socket);
  ctx->active_client_fd = -1;
}

void lotab_client_run_loop(ClientContext* ctx) {
  if (ctx->socket_path) {
    unlink(ctx->socket_path);
  }

  ctx->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ctx->server_fd < 0) {
    vlog(LOG_LEVEL_ERROR, ctx, "Failed to create socket: %s\n", strerror(errno));
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (ctx->socket_path) {
    strncpy(addr.sun_path, ctx->socket_path, sizeof(addr.sun_path) - 1);
  }

  if (bind(ctx->server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    vlog(LOG_LEVEL_ERROR, ctx, "Failed to bind socket: %s\n", strerror(errno));
    close(ctx->server_fd);
    return;
  }

  if (listen(ctx->server_fd, 5) != 0) {
    vlog(LOG_LEVEL_ERROR, ctx, "Failed to listen: %s\n", strerror(errno));
    close(ctx->server_fd);
    return;
  }

  vlog(LOG_LEVEL_INFO, ctx, "UDS Client Server started at %s\n", ctx->socket_path);

  // Simple Loop: Accept one client, handle it, then exit logic (or loop if concurrent, but here sequential)
  // For this use case, we just accept logic.
  // NOTE: The previous Swift logic accepted LOOP connection. Here we do one for simplicity as per refactor req?
  // Actually the swift logic had wait loop. Let's do simple loop.

  while (!atomic_load(&ctx->should_stop)) {
    struct sockaddr_un client_addr;
    socklen_t len = sizeof(client_addr);
    int client_socket = accept(ctx->server_fd, (struct sockaddr*)&client_addr, &len);

    if (client_socket >= 0) {
      vlog(LOG_LEVEL_INFO, ctx, "Accepted new UDS connection\n");
      handle_client(ctx, client_socket);
    } else {
      if (atomic_load(&ctx->should_stop)) {
        break;
      }
      if (errno != EINTR) {
        vlog(LOG_LEVEL_ERROR, ctx, "accept error: %s\n", strerror(errno));
        break;
      }
    }
  }
}

// Helpers for sending
static void send_json_message(ClientContext* ctx, cJSON* json) {
  if (ctx->active_client_fd < 0) {
    vlog(LOG_LEVEL_WARN, ctx, "Cannot send message: no active client\n");
    return;
  }

  char* json_str = cJSON_PrintUnformatted(json);
  if (!json_str)
    return;

  size_t len = strlen(json_str);
  size_t frame_len = sizeof(uint32_t) + len;
  char* msg = malloc(frame_len);
  if (msg) {
    uint32_t header = (uint32_t)len;
    memcpy(msg, &header, sizeof(uint32_t));
    memcpy(msg + sizeof(uint32_t), json_str, len);

    ssize_t sent = send(ctx->active_client_fd, msg, frame_len, 0);
    if (sent < 0) {
      vlog(LOG_LEVEL_ERROR, ctx, "Failed to send message: %s\n", strerror(errno));
    } else {
      vlog(LOG_LEVEL_TRACE, ctx, "Sent message: %s\n", json_str);
    }
    free(msg);
  }
  free(json_str);
}

void lotab_client_send_close_tabs(ClientContext* ctx, const int* tab_ids, size_t count) {
  if (!ctx || count == 0)
    return;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "event", "GUI::UDS::CloseTabsRequest");

  cJSON* data = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "data", data);

  cJSON* ids = cJSON_CreateArray();
  cJSON_AddItemToObject(data, "tabIds", ids);

  for (size_t i = 0; i < count; i++) {
    cJSON_AddItemToArray(ids, cJSON_CreateNumber(tab_ids[i]));
  }

  send_json_message(ctx, root);
  cJSON_Delete(root);
}

void lotab_client_send_tab_selected(ClientContext* ctx, int tab_id) {
  if (!ctx)
    return;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "event", "GUI::UDS::TabSelected");

  cJSON* data = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "data", data);

  cJSON_AddNumberToObject(data, "tabId", tab_id);

  send_json_message(ctx, root);
  cJSON_Delete(root);
}

struct ModeContext {
  struct EngClass* cls;  // Logging context - Must be first for vlog
  LmMode mode;
  LmMode prev_mode;
  void* state_priv;
};
static struct EngClass MODE_CLS = {.name = "app_mode"};

void filter_text_add_char(ModeContext* mctx, const char c);
void filter_text_remove_char(ModeContext* mctx);
void filter_text_clear(ModeContext* mctx);

void transition_state_ctx(ModeContext* mctx, LmMode new_mode);

// Helper for special alnum check
int is_special_alnum(const uint8_t c) {
  return isalnum(c) || c == ' ' || c == '_' || c == '-';
}

#define MACOS_FORWARD_SLASH_KEY_CODE 44
#define MACOS_ESC_CODE 53
#define MACOS_SPACE_CODE 49
#define MACOS_DOWN_ARROW_KEY_CODE 125
#define MACOS_UP_ARROW_KEY_CODE 126
#define MACOS_A_KEY_CODE 0
#define MACOS_J_KEY_CODE 38
#define MACOS_K_KEY_CODE 40
#define MACOS_X_KEY_CODE 7
#define MACOS_ENTER_KEY_CODE 36
#define MACOS_BACKSPACE_KEY_CODE 51

#define MODIFIER_FLAG_CMD 1 << 0
#define MODIFIER_FLAG_SHIFT 1 << 1

// --- State Machine Implementation ---

// Forward declarations
void lm_mode_list_normal__init(void* data);
void lm_mode_list_normal__init_from(void* data, LmMode old_mode, void* old_data);
void lm_mode_list_normal__deinit(void* data);
void lm_mode_list_normal__process_key(void* data,
                                      uint16_t key_code,
                                      const uint8_t ascii_code,
                                      const uint32_t mod_flags,
                                      LmModeTransition* out_transition,
                                      LmMode* out_new_mode);

void lm_mode_filter_inflight__init(void* data);
void lm_mode_filter_inflight__init_from(void* data, LmMode old_mode, void* old_data);
void lm_mode_filter_inflight__deinit(void* data);
void lm_mode_filter_inflight__process_key(void* data,
                                          uint16_t key_code,
                                          const uint8_t ascii_code,
                                          const uint32_t mod_flags,
                                          LmModeTransition* out_transition,
                                          LmMode* out_new_mode);

void lm_mode_multiselect__init(void* data);
void lm_mode_multiselect__init_from(void* data, LmMode old_mode, void* old_data);
void lm_mode_multiselect__deinit(void* data);
void lm_mode_multiselect__process_key(void* data,
                                      uint16_t key_code,
                                      const uint8_t ascii_code,
                                      const uint32_t mod_flags,
                                      LmModeTransition* out_transition,
                                      LmMode* out_new_mode);

struct LmState {
  LmMode mode;
  uint32_t state_data_size;
  void (*init)(void* data);
  void (*init_from)(void* data, LmMode old_mode, void* old_data);
  void (*deinit)(void* data);
  void (*process_key)(void* data,
                      uint16_t key_code,
                      const uint8_t ascii_code,
                      const uint32_t mod_flags,
                      LmModeTransition* out_transition,
                      LmMode* out_new_mode);
};

// Specific States
typedef struct LmModeListNormalState {
  char filter_text[1024];
  int filter_text_len;
} LmModeListNormalState;

typedef struct LmModeFilterInflightState {
  char buffer[1024];
  int buffer_len;
} LmModeFilterInflightState;

typedef struct LmModeMultiselectState {
    char filter_text[1024];
    int filter_text_len;
} LmModeMultiselectState;

static struct LmState STATE_MACHINE[] = {
    {
        .mode = LM_MODE_LIST_NORMAL,
        .state_data_size = sizeof(LmModeListNormalState),
        .init = lm_mode_list_normal__init,
        .init_from = lm_mode_list_normal__init_from,
        .deinit = lm_mode_list_normal__deinit,
        .process_key = lm_mode_list_normal__process_key,
    },
    {
        .mode = LM_MODE_LIST_FILTER_INFLIGHT,
        .state_data_size = sizeof(LmModeFilterInflightState),
        .init = lm_mode_filter_inflight__init,
        .init_from = lm_mode_filter_inflight__init_from,
        .deinit = lm_mode_filter_inflight__deinit,
        .process_key = lm_mode_filter_inflight__process_key,
    },
    {
        .mode = LM_MODE_LIST_MULTISELECT,
        .state_data_size = sizeof(LmModeMultiselectState),
        .init = lm_mode_multiselect__init,
        .init_from = lm_mode_multiselect__init_from,
        .deinit = lm_mode_multiselect__deinit,
        .process_key = lm_mode_multiselect__process_key,
    },
};

struct LmState* find_mode_state(const LmMode mode) {
  for (uint32_t i = 0; i < ARRAY_SIZE(STATE_MACHINE); ++i) {
    if (mode == STATE_MACHINE[i].mode) {
      return &STATE_MACHINE[i];
    }
  }
  return NULL;
}

// Helper for buffer manipulation
static void buffer_add_char(char* buf, int* len, int max_len, const char c) {
  if (*len < max_len - 1) {
    buf[(*len)++] = c;
    buf[*len] = '\0';
  }
}

static void buffer_remove_char(char* buf, int* len) {
  if (*len > 0) {
    buf[--(*len)] = '\0';
  }
}

static void buffer_clear(char* buf, int* len) {
  buf[0] = '\0';
  *len = 0;
}

// --- LIST NORMAL ---
void lm_mode_list_normal__init(void* data) {
  LmModeListNormalState* s = (LmModeListNormalState*)data;
  buffer_clear(s->filter_text, &s->filter_text_len);
}

void lm_mode_list_normal__init_from(void* data, LmMode old_mode, void* old_data) {
  LmModeListNormalState* s = (LmModeListNormalState*)data;
  buffer_clear(s->filter_text, &s->filter_text_len);

  if (old_mode == LM_MODE_LIST_FILTER_INFLIGHT && old_data) {
      LmModeFilterInflightState* old_s = (LmModeFilterInflightState*)old_data;
      memcpy(s->filter_text, old_s->buffer, sizeof(s->filter_text));
      s->filter_text_len = old_s->buffer_len;
  } else if (old_mode == LM_MODE_LIST_MULTISELECT && old_data) {
      LmModeMultiselectState* old_s = (LmModeMultiselectState*)old_data;
      memcpy(s->filter_text, old_s->filter_text, sizeof(s->filter_text));
      s->filter_text_len = old_s->filter_text_len;
  }
}

void lm_mode_list_normal__deinit(void* data) {
  // No op
}

void lm_mode_list_normal__process_key(void* data,
                                      uint16_t key_code,
                                      const uint8_t ascii_code,
                                      const uint32_t mod_flags,
                                      LmModeTransition* out_transition,
                                      LmMode* out_new_mode) {
  LmModeListNormalState* s = (LmModeListNormalState*)data;
  *out_transition = LM_MODETS_UNKNOWN;
  *out_new_mode = LM_MODE_LIST_NORMAL;

  switch (key_code) {
    case MACOS_ESC_CODE:
      if (s->filter_text_len > 0) {
        buffer_clear(s->filter_text, &s->filter_text_len);
        *out_transition = LM_MODETS_UPDATE_LIST_FILTER;
      } else {
        *out_transition = LM_MODETS_HIDE_UI;
      }
      break;
    case MACOS_FORWARD_SLASH_KEY_CODE:
      *out_transition = LM_MODETS_ADHERE_TO_MODE;
      *out_new_mode = LM_MODE_LIST_FILTER_INFLIGHT;
      break;
    case MACOS_DOWN_ARROW_KEY_CODE:
    case MACOS_J_KEY_CODE:
      *out_transition = LM_MODETS_NAVIGATE_DOWN;
      break;
    case MACOS_UP_ARROW_KEY_CODE:
    case MACOS_K_KEY_CODE:
      *out_transition = LM_MODETS_NAVIGATE_UP;
      break;
    case MACOS_ENTER_KEY_CODE:
      *out_transition = LM_MODETS_ACTIVATE_TO_TAB;
      break;
    case MACOS_SPACE_CODE:
      *out_transition = LM_MODETS_SELECT_TAB;
      *out_new_mode = LM_MODE_LIST_MULTISELECT;
      break;
    case MACOS_A_KEY_CODE:
      if (mod_flags & MODIFIER_FLAG_CMD) {
        *out_transition = LM_MODETS_SELECT_ALL_TABS;
        *out_new_mode = LM_MODE_LIST_MULTISELECT;
      }
      break;
    case MACOS_X_KEY_CODE:
      *out_transition = LM_MODETS_CLOSE_SELECTED_TABS;
      break;
  }
}

// --- FILTER INFLIGHT ---
void lm_mode_filter_inflight__init(void* data) {
  LmModeFilterInflightState* s = (LmModeFilterInflightState*)data;
  buffer_clear(s->buffer, &s->buffer_len);
}

void lm_mode_filter_inflight__init_from(void* data, LmMode old_mode, void* old_data) {
  LmModeFilterInflightState* s = (LmModeFilterInflightState*)data;
  buffer_clear(s->buffer, &s->buffer_len);
  
  // User request: New search should clear old filter.
  // So we do NOT copy old_s->filter_text here.
  (void)old_mode;
  (void)old_data;
}

void lm_mode_filter_inflight__deinit(void* data) {
}

void lm_mode_filter_inflight__process_key(void* data,
                                          uint16_t key_code,
                                          const uint8_t ascii_code,
                                          const uint32_t mod_flags,
                                          LmModeTransition* out_transition,
                                          LmMode* out_new_mode) {
  LmModeFilterInflightState* s = (LmModeFilterInflightState*)data;
  *out_transition = LM_MODETS_UNKNOWN;
  *out_new_mode = LM_MODE_LIST_FILTER_INFLIGHT;

  if (key_code == MACOS_ESC_CODE) {
    buffer_clear(s->buffer, &s->buffer_len);
    *out_transition = LM_MODETS_ADHERE_TO_MODE;
    *out_new_mode = LM_MODE_LIST_NORMAL;
  } else if (key_code == MACOS_ENTER_KEY_CODE) {
    *out_transition = LM_MODETS_COMMIT_LIST_FILTER;
    *out_new_mode = LM_MODE_LIST_NORMAL;
  } else if (key_code == MACOS_BACKSPACE_KEY_CODE) {
    buffer_remove_char(s->buffer, &s->buffer_len);
    *out_transition = LM_MODETS_UPDATE_LIST_FILTER;
  } else if (is_special_alnum(ascii_code)) {
    buffer_add_char(s->buffer, &s->buffer_len, sizeof(s->buffer), (char)ascii_code);
    *out_transition = LM_MODETS_UPDATE_LIST_FILTER;
  }
}

// --- MULTISELECT ---
void lm_mode_multiselect__init(void* data) {
    LmModeMultiselectState* s = (LmModeMultiselectState*)data;
    buffer_clear(s->filter_text, &s->filter_text_len);
}

void lm_mode_multiselect__init_from(void* data, LmMode old_mode, void* old_data) {
    LmModeMultiselectState* s = (LmModeMultiselectState*)data;
    buffer_clear(s->filter_text, &s->filter_text_len);

    if (old_mode == LM_MODE_LIST_NORMAL && old_data) {
        LmModeListNormalState* old_s = (LmModeListNormalState*)old_data;
        memcpy(s->filter_text, old_s->filter_text, sizeof(s->filter_text));
        s->filter_text_len = old_s->filter_text_len;
    }
}
void lm_mode_multiselect__deinit(void* data) {
    (void)data;
}

void lm_mode_multiselect__process_key(void* data,
                                      uint16_t key_code,
                                      const uint8_t ascii_code,
                                      const uint32_t mod_flags,
                                      LmModeTransition* out_transition,
                                      LmMode* out_new_mode) {
  *out_transition = LM_MODETS_UNKNOWN;
  *out_new_mode = LM_MODE_LIST_MULTISELECT;

  switch (key_code) {
    case MACOS_ESC_CODE:
      *out_transition = LM_MODETS_ADHERE_TO_MODE;
      *out_new_mode = LM_MODE_LIST_NORMAL;
      break;
    case MACOS_DOWN_ARROW_KEY_CODE:
    case MACOS_J_KEY_CODE:
      *out_transition = LM_MODETS_NAVIGATE_DOWN;
      break;
    case MACOS_UP_ARROW_KEY_CODE:
    case MACOS_K_KEY_CODE:
      *out_transition = LM_MODETS_NAVIGATE_UP;
      break;
    case MACOS_SPACE_CODE:
      *out_transition = LM_MODETS_SELECT_TAB;
      break;
    case MACOS_A_KEY_CODE:
      if (mod_flags & MODIFIER_FLAG_CMD) {
        *out_transition = LM_MODETS_SELECT_ALL_TABS;
      }
      break;
    case MACOS_X_KEY_CODE:
      *out_transition = LM_MODETS_CLOSE_SELECTED_TABS;
      break;
  }
}

// --- Driver ---

void lm_process_key_event(ModeContext* mctx,
                          const uint16_t key_code,
                          const uint8_t ascii_code,
                          uint8_t cmd,
                          uint8_t shift,
                          LmModeTransition* out_transition,
                          LmMode* out_old_mode,
                          LmMode* out_new_mode) {
  assert(mctx);
  // Default outputs
  *out_old_mode = mctx->mode;
  *out_new_mode = mctx->mode;
  *out_transition = LM_MODETS_UNKNOWN;

  struct LmState* current_state = find_mode_state(mctx->mode);
  if (!current_state) {
    vlog(LOG_LEVEL_ERROR, mctx, "Unknown state: %d\n", mctx->mode);
    return;
  }

  const uint32_t mod_flags = (!!cmd << 0) | (!!shift << 1);
  LmModeTransition tx = LM_MODETS_UNKNOWN;
  LmMode new_mode = mctx->mode;

  current_state->process_key(mctx->state_priv, key_code, ascii_code, mod_flags, &tx, &new_mode);

  *out_transition = tx;
  *out_new_mode = new_mode;

  // Handle Mode Transition
  if (new_mode != mctx->mode) {
    transition_state_ctx(mctx, new_mode);
  }
}

void transition_state_ctx(ModeContext* mctx, LmMode new_mode) {
  if (mctx->mode == new_mode)
    return;

  LmMode old_mode = mctx->mode;
  void* old_data = mctx->state_priv;
  struct LmState* old_state = find_mode_state(old_mode);

  struct LmState* new_state = find_mode_state(new_mode);
  if (!new_state) {
    // FATAL
    vlog(LOG_LEVEL_ERROR, mctx, "Cannot transition to unknown mode %d\n", new_mode);
    return;
  }

  void* new_data = malloc(new_state->state_data_size);
  memset(new_data, 0, new_state->state_data_size);

  // Init
  if (new_state->init) {
    new_state->init(new_data);
  }
  // Init From (Transfer data)
  if (new_state->init_from) {
    new_state->init_from(new_data, old_mode, old_data);
  }

  // Deinit old
  if (old_state && old_state->deinit && old_data) {
    old_state->deinit(old_data);
  }
  if (old_data) {
    free(old_data);
  }

  mctx->mode = new_mode;
  mctx->state_priv = new_data;
  mctx->prev_mode = old_mode;
}

struct ModeContext* lm_alloc(void) {
  struct ModeContext* ctx = calloc(1, sizeof(struct ModeContext));
  ctx->cls = &MODE_CLS;
  ctx->mode = LM_MODE_UNKNOWN;
  ctx->state_priv = NULL;
  // Initialize to Normal mode
  transition_state_ctx(ctx, LM_MODE_LIST_NORMAL);
  return ctx;
}

void lm_destroy(ModeContext* mctx) {
  if (!mctx)
    return;

  if (mctx->state_priv) {
    struct LmState* state = find_mode_state(mctx->mode);
    if (state && state->deinit) {
      state->deinit(mctx->state_priv);
    }
    free(mctx->state_priv);
  }
  free(mctx);
}



void lm_on_list_len_update(ModeContext* mctx,
                           int list_len,
                           LmModeTransition* out_transition,
                           LmMode* out_old_mode,
                           LmMode* out_new_mode) {
    *out_transition = LM_MODETS_UNKNOWN;
    *out_old_mode = mctx->mode;
    *out_new_mode = mctx->mode;

    if (mctx->mode == LM_MODE_LIST_MULTISELECT && list_len == 0) {
        // Auto-exit multiselect if list becomes empty (e.g. all filtered items closed)
        transition_state_ctx(mctx, LM_MODE_LIST_NORMAL);
        *out_transition = LM_MODETS_ADHERE_TO_MODE;
        *out_new_mode = LM_MODE_LIST_NORMAL;
    }
}

char* lm_get_filter_text(ModeContext* mctx) {
  if (!mctx->state_priv) return NULL;

  if (mctx->mode == LM_MODE_LIST_NORMAL) {
      LmModeListNormalState* s = (LmModeListNormalState*)mctx->state_priv;
      if (s->filter_text_len == 0) return NULL;
      return s->filter_text;
  } else if (mctx->mode == LM_MODE_LIST_FILTER_INFLIGHT) {
      LmModeFilterInflightState* s = (LmModeFilterInflightState*)mctx->state_priv;
      if (s->buffer_len == 0) return NULL;
      return s->buffer;
  } else if (mctx->mode == LM_MODE_LIST_MULTISELECT) {
      LmModeMultiselectState* s = (LmModeMultiselectState*)mctx->state_priv;
      if (s->filter_text_len == 0) return NULL;
      return s->filter_text;
  }
  
  return NULL;
}
