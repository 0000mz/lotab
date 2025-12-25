#include "engine.h"

#include <assert.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/_types/_va_list.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "statusbar.h"

static LogLevel g_log_level = LOG_LEVEL_INFO;

#define NGERROR(x) (-x)

extern char** environ;                                 // Necessary global for inheriting env in subprocess.
static const char* uds_path = "/tmp/tabmanager.sock";  // TODO: Do not use globals.

typedef struct ServerContext {
  struct lws_context* lws_ctx;
  struct lws* client_wsi;
  pthread_t ws_thread;
  int uds_fd;
  int ws_thread_exit;  // TODO: this should be protected by mutex.
  int send_tab_request;
} ServerContext;

typedef struct PerSessionData {
  char* msg;
  size_t len;
} PerSessionData;

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

static int setup_uds_client(ServerContext* sctx) {
  int retries = 5;
  int uds_fd;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);

  while (retries > 0) {
    uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) {
      vlog(LOG_LEVEL_ERROR, "Daemon: Failed to create UDS socket: %s\n", strerror(errno));
      return -1;  // TODO: Use some error code
    }

    if (connect(uds_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      vlog(LOG_LEVEL_INFO, "Daemon: Connected to App UDS at %s\n", uds_path);
      const char* ping = "{\"event\":\"daemon_startup\",\"data\":\"ping\"}";
      send(uds_fd, ping, strlen(ping), 0);
      sctx->uds_fd = uds_fd;
      return 0;
    }

    close(uds_fd);
    uds_fd = -1;
    fprintf(stderr, "Daemon: UDS connect failed, retrying in 1s... (%d left)\n", retries - 1);
    sleep(1);
    retries--;
  }
  vlog(LOG_LEVEL_ERROR, "Daemon: Failed to connect to App UDS after multiple attempts\n");
  return -1;
}

static void send_uds(const int uds_fd, const cJSON* json_data) {
  if (uds_fd >= 0) {
    char* data = cJSON_Print(json_data);
    if (send(uds_fd, data, strlen(data), 0) < 0) {
      vlog(LOG_LEVEL_ERROR, "Daemon: Failed to send data to App via UDS: %s\n", strerror(errno));
    } else {
      vlog(LOG_LEVEL_INFO, "Daemon: Sent UDS message: %s\n", data);  // Simplified log
    }
  } else {
    vlog(LOG_LEVEL_WARN, "Daemon: Warning - Cannot send UDS, not connected.\n");
  }
}

static void pss_clear_message(PerSessionData* pss, int should_free) {
  assert(pss);
  if (should_free && pss->msg) {
    free(pss->msg);
  }
  pss->msg = NULL;
  pss->len = 0;
}

static int callback_minimal(struct lws* wsi, enum lws_callback_reasons reason, void* _user, void* in, size_t len) {
  EngineContext* ec = (EngineContext*)lws_context_user(lws_get_context(wsi));
  ServerContext* sc = (ServerContext*)ec->serv_ctx;
  PerSessionData* pss = (PerSessionData*)_user;

  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
      lwsl_user("LWS_CALLBACK_ESTABLISHED (new connection)\n");
      sc->client_wsi = wsi;
      sc->send_tab_request = 1;
      if (pss) {
        pss_clear_message(pss, 0);
      }
      lws_callback_on_writable(wsi);
      break;

    case LWS_CALLBACK_CLOSED:
      lwsl_user("LWS_CALLBACK_CLOSED (connection lost)\n");
      if (sc->client_wsi == wsi) {
        sc->client_wsi = NULL;
      }
      if (pss && pss->msg) {
        pss_clear_message(pss, 1);
      }
      break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
      if (sc->send_tab_request) {
        const char* msg = "{\"event\":\"request_tab_info\"}";
        size_t msg_len = strlen(msg);
        unsigned char buf[LWS_PRE + 256];
        memset(buf, 0, sizeof(buf));
        memcpy(&buf[LWS_PRE], msg, msg_len);
        lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);
        sc->send_tab_request = 0;
        vlog(LOG_LEVEL_INFO, "Daemon: Sent request_tab_info to extension\n");
      }
      break;

    case LWS_CALLBACK_RECEIVE: {
      const size_t remaining = lws_remaining_packet_payload(wsi);
      int is_final = lws_is_final_fragment(wsi);

      if (!pss->msg) {
        pss_clear_message(pss, 0);
        pss->msg = malloc(len + remaining + 1);
        if (!pss->msg) {
          lwsl_err("OOM: dropping\n");
          return -1;
        }
      } else {
        char* new_msg = realloc(pss->msg, pss->len + len + remaining + 1);
        if (!new_msg) {
          lwsl_err("OOM: dropping\n");
          pss_clear_message(pss, 1);
          return -1;
        }
        pss->msg = new_msg;
      }

      memcpy(pss->msg + pss->len, in, len);
      pss->len += len;

      if (is_final && remaining == 0) {
        pss->msg[pss->len] = '\0';
        engine_handle_event(ec, EVENT_WS_MESSAGE_RECEIVED, pss->msg);
        pss_clear_message(pss, 1);
      }
    } break;
    default:
      break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {{.name = "minimal",
                                            .callback = callback_minimal,
                                            .per_session_data_size = sizeof(PerSessionData),
                                            .rx_buffer_size = 0},
                                           LWS_PROTOCOL_LIST_TERM};

static void kill_process(const int pid) {
  if (pid >= 0) {
    if (kill(pid, SIGTERM) == 0) {
      int status;
      waitpid(pid, &status, 0);
      vlog(LOG_LEVEL_INFO, "Process terminated.\n");
    } else {
      vlog(LOG_LEVEL_ERROR, "Failed to kill child process.\n");
    }
  }
}

// GUI Callbacks
static void on_status_toggle(void* priv) {
  EngineContext* ectx = (EngineContext*)priv;
  engine_handle_event(ectx, EVENT_HOTKEY_TOGGLE, NULL);
}

static void on_status_quit(void* priv) {
  EngineContext* ectx = (EngineContext*)priv;
  if (ectx != NULL) {
    engine_destroy(ectx);
  }
}

static void* ws_thread_run(void* arg) {
  int n;

  ServerContext* sc = (ServerContext*)arg;
  do {
    n = lws_service(sc->lws_ctx, 100);
  } while (n >= 0 && !sc->ws_thread_exit);
  return NULL;
}

int engine_init(EngineContext** ectx) {
  assert(ectx != NULL);
  assert(*ectx == NULL);

  EngineContext* ec = NULL;
  ServerContext* sc = NULL;
  struct lws_context_creation_info info;
  int ret;

  ec = (EngineContext*)malloc(sizeof(EngineContext));
  if (!ec) {
    vlog(LOG_LEVEL_ERROR, "Failed to allocate engine context.\n");
    ret = NGERROR(ENOMEM);
    goto fail;
  }
  memset(ec, 0, sizeof(EngineContext));
  ec->app_pid = -1;
  ec->tab_state = malloc(sizeof(TabState));
  if (ec->tab_state) {
    memset(ec->tab_state, 0, sizeof(TabState));
  }

  // Setup websocket server
  vlog(LOG_LEVEL_INFO, "Setting up websocket server.\n");
  sc = (ServerContext*)malloc(sizeof(ServerContext));
  if (!sc) {
    vlog(LOG_LEVEL_ERROR, "Failed to allocate server context.\n");
    ret = NGERROR(ENOMEM);
    goto fail;
  }
  memset(sc, 0, sizeof(ServerContext));
  lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);

  ec->serv_ctx = sc;
  memset(&info, 0, sizeof info);
  info.port = 9001;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.user = ec;
  vlog(LOG_LEVEL_INFO, "Starting Daemon WebSocket server on port %d\n", info.port);
  sc->lws_ctx = lws_create_context(&info);
  if (!sc->lws_ctx) {
    ret = -1;  // TODO: use some defined error code.
    goto fail;
  }

  if (pthread_create(&sc->ws_thread, NULL, ws_thread_run, (void*)sc) != 0) {
    vlog(LOG_LEVEL_ERROR, "Failed to start websocket server.\n");
    ret = -1;  // TODO: use some defined error code.
    goto fail;
  }

// Initialize gui client and uds protocol
#ifndef APP_PATH
  vlog(LOG_LEVEL_ERROR, "APP_PATH is not defined. Daemon cannot continue.\n");
  ret = -1;  // TODO: use some defined error code.
  goto fail;
#endif
  char* spawn_args[] = {(char*)APP_PATH, NULL};
  int spawn_status = posix_spawn(&ec->app_pid, APP_PATH, NULL, NULL, spawn_args, environ);
  if (spawn_status == 0) {
    vlog(LOG_LEVEL_INFO, "Daemon: Successfully spawned TabManager (PID: %d)\n", ec->app_pid);
    sleep(1);
    setup_uds_client(sc);
  } else {
    vlog(LOG_LEVEL_ERROR, "Daemon: Failed to spawn TabManager: %s\n", strerror(spawn_status));
    ret = -1;
    goto fail;
  }
  vlog(LOG_LEVEL_INFO, "Engine initialized.\n");
  *ectx = ec;
  return 0;

fail:
  if (ec && ec->app_pid >= 0) {
    kill_process(ec->app_pid);
    ec->app_pid = -1;
  }
  if (sc && sc->lws_ctx) {
    lws_context_destroy(sc->lws_ctx);
  }
  if (sc) {
    free(sc);
  }
  if (ec->tab_state) {
    free(ec->tab_state);
  }
  if (ec) {
    free(ec);
  }
  return ret;
}

static void sigint_handler(int sig) {
  (void)sig;
  printf("Daemon: Caught SIGINT\n");
  stop_daemon_cocoa_app();
}

void engine_run(EngineContext* ectx) {
  signal(SIGINT, sigint_handler);
  printf("Daemon: Starting Cocoa Event Loop\n");
  ectx->run_ctx = malloc(sizeof(StatusBarRunContext));
  if (!ectx->run_ctx) {
    vlog(LOG_LEVEL_ERROR, "Failed to allocate run context.\n");
    return;
  }
  ectx->run_ctx->on_toggle = on_status_toggle;
  ectx->run_ctx->on_quit = on_status_quit;
  ectx->run_ctx->privdata = ectx;
  run_daemon_cocoa_app(ectx->run_ctx);
}

void engine_destroy(EngineContext* ectx) {
  assert(ectx);
  if (ectx->destroyed)
    return;
  if (ectx->app_pid >= 0) {
    kill_process(ectx->app_pid);
    ectx->app_pid = -1;
  }
  if (ectx->serv_ctx) {
    if (ectx->serv_ctx->lws_ctx) {
      lws_cancel_service(ectx->serv_ctx->lws_ctx);
      if (ectx->serv_ctx->ws_thread) {
        ectx->serv_ctx->ws_thread_exit = 1;
        pthread_join(ectx->serv_ctx->ws_thread, NULL);
      }
      lws_context_destroy(ectx->serv_ctx->lws_ctx);
      stop_daemon_cocoa_app();
    }
    free(ectx->serv_ctx);
    ectx->serv_ctx = NULL;
  }
  if (ectx->run_ctx) {
    free(ectx->run_ctx);
    ectx->run_ctx = NULL;
  }
  if (ectx->tab_state) {
    TabInfo* current = ectx->tab_state->tabs;
    while (current) {
      TabInfo* next = current->next;
      if (current->title)
        free(current->title);
      free(current);
      current = next;
    }
    free(ectx->tab_state);
    ectx->tab_state = NULL;
  }
  ectx->destroyed = 1;
}

void engine_set_log_level(LogLevel level) {
  g_log_level = level;
}

void vlog(LogLevel level, const char* fmt, ...) {
  if (level > g_log_level) {
    return;
  }
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  FILE* f = stdout;
  if (level == LOG_LEVEL_ERROR)
    f = stderr;
  fprintf(f, "%s", buf);
}

TabInfo* tab_state_find_tab(TabState* ts, const uint64_t id) {
  assert(ts);
  TabInfo* current = ts->tabs;
  while (current) {
    if (current->id == id)
      return current;
    current = current->next;
  }
  return NULL;
}

void tab_state_update_tab(TabState* ts, const char* title, const uint64_t id) {
  TabInfo* ti = tab_state_find_tab(ts, id);
  if (ti && ti->title && strcmp(title, ti->title) != 0) {
    free(ti->title);
    ti->title = strdup(title);
  }
}

void tab_state_add_tab(TabState* ts, const char* title, const uint64_t id) {
  TabInfo* new_tab = malloc(sizeof(TabInfo));
  if (new_tab) {
    new_tab->id = id;
    new_tab->title = strdup(title);
    new_tab->active = 0;
    new_tab->next = ts->tabs;
    ts->tabs = new_tab;
    ts->nb_tabs++;
  }
}

void tab_state_remove_tab(TabState* ts, const uint64_t id) {
  TabInfo* current = ts->tabs;
  TabInfo* prev = NULL;
  while (current) {
    if (current->id == id) {
      if (prev) {
        prev->next = current->next;
      } else {
        ts->tabs = current->next;
      }
      if (current->title)
        free(current->title);
      free(current);
      ts->nb_tabs--;
      return;
    }
    prev = current;
    current = current->next;
  }
}

void tab_state_set_active(TabState* ts, const uint64_t id) {
  TabInfo* current = ts->tabs;
  while (current) {
    if (current->id == id) {
      current->active = 1;
    } else {
      current->active = 0;
    }
    current = current->next;
  }
}

void tab_event__handle_all_tabs(TabState* ts, const cJSON* json_data) {
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (data && cJSON_IsArray(data)) {
    int count = cJSON_GetArraySize(data);
    vlog(LOG_LEVEL_INFO, "Received %d tabs (current state: %d)\n", count, ts->nb_tabs);
    int tabs_added = 0, tabs_updated = 0;

    for (int i = 0; i < count; i++) {
      cJSON* item = cJSON_GetArrayItem(data, i);
      cJSON* title_json = cJSON_GetObjectItemCaseSensitive(item, "title");
      cJSON* id_json = cJSON_GetObjectItemCaseSensitive(item, "id");

      char* tab_title = NULL;
      uint64_t tab_id = 0;

      if (cJSON_IsString(title_json) && (title_json->valuestring != NULL)) {
        tab_title = title_json->valuestring;
      }
      if (cJSON_IsNumber(id_json)) {
        tab_id = (uint64_t)id_json->valuedouble;
      }

      if (tab_state_find_tab(ts, tab_id) != NULL) {
        tab_state_update_tab(ts, tab_title ? tab_title : "Unknown", tab_id);
        ++tabs_updated;
      } else {
        tab_state_add_tab(ts, tab_title ? tab_title : "Unknown", tab_id);
        ++tabs_added;
      }
    }
    vlog(LOG_LEVEL_INFO, "Tab State Synced: %d updated, %d added. Total: %d\n", tabs_updated, tabs_added, ts->nb_tabs);
  } else {
    vlog(LOG_LEVEL_WARN, "onAllTabs: 'data' key missing or not an array.\n");
  }
}

void tab_event__handle_remove_tab(TabState* ts, const cJSON* json_data) {
  // {"event": "tabs.onRemoved", "data": {"tabId": 123, "removeInfo": {...}}}
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (data) {
    cJSON* tabIdJson = cJSON_GetObjectItem(data, "tabId");
    if (cJSON_IsNumber(tabIdJson)) {
      uint64_t id = (uint64_t)tabIdJson->valuedouble;
      tab_state_remove_tab(ts, id);
      vlog(LOG_LEVEL_INFO, "Tab Removed: %llu. Remaining: %d\n", id, ts->nb_tabs);
    } else {
      vlog(LOG_LEVEL_WARN, "onRemoved: tabId missing or invalid\n");
    }
  }
}

void tab_event__handle_activated(TabState* ts, const cJSON* json_data) {
  // {"event": "tabs.onActivated", "data": {"tabId": 123, "windowId": 456}}
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (data) {
    cJSON* tabIdJson = cJSON_GetObjectItem(data, "tabId");
    if (cJSON_IsNumber(tabIdJson)) {
      uint64_t id = (uint64_t)tabIdJson->valuedouble;
      tab_state_set_active(ts, id);
      vlog(LOG_LEVEL_INFO, "Tab Activated: %llu\n", id);
    } else {
      vlog(LOG_LEVEL_WARN, "onActivated: tabId missing or invalid\n");
    }
  }
}

void tab_event__handle_created(TabState* ts, const cJSON* json_data) {
  // {"event": "tabs.onCreated", "data": {"id": 123, "title": "New Tab", ...}}
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (data) {
    cJSON* id_json = cJSON_GetObjectItem(data, "id");
    cJSON* title_json = cJSON_GetObjectItem(data, "title");

    if (cJSON_IsNumber(id_json)) {
      uint64_t id = (uint64_t)id_json->valuedouble;
      const char* title = "New Tab";
      if (cJSON_IsString(title_json) && title_json->valuestring) {
        title = title_json->valuestring;
      }
      tab_state_add_tab(ts, title, id);
      vlog(LOG_LEVEL_INFO, "Tab Created: %llu, Title: %s\n", id, title);
    } else {
      vlog(LOG_LEVEL_WARN, "onCreated: id missing or invalid\n");
    }
  }
}

void tab_event__do_nothing(TabState* ts, const cJSON* json_data) {
  (void)ts;
  (void)json_data;
  vlog(LOG_LEVEL_TRACE, "tab_event -- do nothing\n");
}

struct TabEventMapEntry {
  const char* event_name;
  TabEventType type;
};
static const struct TabEventMapEntry TAB_EVENT_MAP[] = {
    {"tabs.onActivated", TAB_EVENT_ACTIVATED},    {"tabs.onUpdated", TAB_EVENT_UPDATED},
    {"tabs.onCreated", TAB_EVENT_CREATED},        {"tabs.onHighlighted", TAB_EVENT_HIGHLIGHTED},
    {"tabs.onZoomChange", TAB_EVENT_ZOOM_CHANGE}, {"tabs.onAllTabs", TAB_EVENT_ALL_TABS},
    {"tabs.onRemoved", TAB_EVENT_TAB_REMOVED},    {NULL, TAB_EVENT_UNKNOWN},
};

static struct {
  TabEventType type;
  void (*event_handler)(TabState* ts, const cJSON* json_data);
} TAB_EVENT_HANDLERS[] = {
    {TAB_EVENT_ALL_TABS, tab_event__handle_all_tabs},
    {TAB_EVENT_TAB_REMOVED, tab_event__handle_remove_tab},
    {TAB_EVENT_ACTIVATED, tab_event__handle_activated},
    {TAB_EVENT_CREATED, tab_event__handle_created},
    {TAB_EVENT_HIGHLIGHTED, tab_event__do_nothing},
    {TAB_EVENT_ZOOM_CHANGE, tab_event__do_nothing},
    {TAB_EVENT_UNKNOWN, NULL},
};

// Helper to determine event type from JSON string
static TabEventType parse_event_type(cJSON* json) {
  TabEventType type = TAB_EVENT_UNKNOWN;
  cJSON* event = cJSON_GetObjectItemCaseSensitive(json, "event");

  if (cJSON_IsString(event) && (event->valuestring != NULL)) {
    int found = 0;
    for (int i = 0; TAB_EVENT_MAP[i].event_name != NULL; ++i) {
      if (strcmp(event->valuestring, TAB_EVENT_MAP[i].event_name) == 0) {
        type = TAB_EVENT_MAP[i].type;
        found = 1;
        break;
      }
    }
    if (!found) {
      vlog(LOG_LEVEL_WARN, "Unknown tab event: %s\n", event->valuestring);
    }
  }
  return type;
}

void tab_event_handle(TabState* ts, TabEventType type, cJSON* json_data) {
  int event_handled = 0;
  for (int i = 0; TAB_EVENT_HANDLERS[i].type != TAB_EVENT_UNKNOWN; ++i) {
    if (TAB_EVENT_HANDLERS[i].type == type) {
      TAB_EVENT_HANDLERS[i].event_handler(ts, json_data);
      event_handled = 1;
      break;
    }
  }
  if (!event_handled) {
    vlog(LOG_LEVEL_WARN, "Unhandled tab event type: %d\n", type);
  }
}

void engine_handle_event(EngineContext* ectx, DaemonEvent event, void* data) {
  assert(ectx != NULL);

  switch (event) {
    case EVENT_HOTKEY_TOGGLE:
      vlog(LOG_LEVEL_INFO, "Engine: Toggle Requested\n");
      cJSON* message = cJSON_CreateObject();
      cJSON_AddStringToObject(message, "event", "ui_visibility_toggle");
      cJSON_AddStringToObject(message, "data", "toggle");
      send_uds(ectx->serv_ctx->uds_fd, message);
      cJSON_Delete(message);
      break;

    case EVENT_WS_MESSAGE_RECEIVED:
      if (data) {
        vlog(LOG_LEVEL_TRACE, "Engine: WS Message Received: %s\n", (const char*)data);
        const char* json_msg = (const char*)data;
        cJSON* json = cJSON_Parse(json_msg);
        if (json) {
          TabEventType type = parse_event_type(json);
          if (type != TAB_EVENT_UNKNOWN) {
            tab_event_handle(ectx->tab_state, type, json);
          }
          cJSON_Delete(json);
        } else {
          vlog(LOG_LEVEL_ERROR, "Failed to parse json from websocket message.\n");
        }
      }
      break;
  }
}
