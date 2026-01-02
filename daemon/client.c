#include "client.h"
#include <stdatomic.h>
#include "util.h"

#include <cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct ClientContext {
  struct EngClass* cls;  // Logging context - Must be first for vlog
  char* socket_path;
  ClientCallbacks callbacks;
  void* user_data;
  int server_fd;
  int active_client_fd;
  atomic_bool should_stop;
};

static struct EngClass CLIENT_CLS = {.name = "client"};

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

        list.tasks[i].id = cJSON_IsNumber(id) ? (int)id->valuedouble : 0;
        list.tasks[i].name = (cJSON_IsString(name) && name->valuestring) ? strdup(name->valuestring) : strdup("");
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
