#include "engine.h"
#include <toml.h>

#include <assert.h>
#include <cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "statusbar.h"
#include "util.h"

#define NGERROR(x) (-x)

extern char** environ;  // Necessary global for inheriting env in subprocess.

typedef struct ServerContext {
  struct EngClass* cls;
  struct lws_context* lws_ctx;
  struct lws* client_wsi;
  pthread_t ws_thread;
  int uds_fd;
  atomic_bool ws_thread_exit;
  atomic_bool send_tab_request;
  pthread_t uds_read_thread;
  atomic_bool uds_read_exit;
  char* pending_ws_msg;
  atomic_bool send_pending_msg;
  pthread_mutex_t pending_msg_mutex;
  char* uds_path;
} ServerContext;

typedef struct PerSessionData {
  char* msg;
  size_t len;
} PerSessionData;

static struct EngClass TAB_STATE_CLASS = {
    .name = "tab",
};
static struct EngClass TASK_STATE_CLASS = {
    .name = "task",
};
static struct EngClass ENGINE_CONTEXT_CLASS = {
    .name = "engine",
};
static struct EngClass SERVER_CONTEXT_CLASS = {
    .name = "server",
};

void task_state_add(TaskState* ts, const char* task_name, int64_t external_id);
void tab_state_update_active(TabState* ts, const cJSON* json_data);

static void tab_state_free(TabState* ts) {
  if (!ts)
    return;
  TabInfo* current = ts->tabs;
  while (current) {
    TabInfo* next = current->next;
    if (current->title)
      free(current->title);
    free(current);
    current = next;
  }
  free(ts);
}

static void task_state_free(TaskState* ts) {
  if (!ts)
    return;
  TaskInfo* current = ts->tasks;
  while (current) {
    TaskInfo* next = current->next;
    if (current->task_name)
      free(current->task_name);
    free(current);
    current = next;
  }
  free(ts);
}

static void handle_gui_msg(ServerContext* sc, const char* msg) {
  cJSON* json = cJSON_Parse(msg);
  if (!json) {
    vlog(LOG_LEVEL_ERROR, sc, "Failed to parse GUI message: %s\n", msg);
    return;
  }

  cJSON* event = cJSON_GetObjectItem(json, "event");
  if (cJSON_IsString(event) && event->valuestring) {
    if (strcmp(event->valuestring, "GUI::UDS::TabSelected") == 0) {
      cJSON* data = cJSON_GetObjectItem(json, "data");
      cJSON* tab_id = cJSON_GetObjectItem(data, "tabId");
      if (cJSON_IsNumber(tab_id)) {
        vlog(LOG_LEVEL_INFO, sc, "gui-evt: tab_selected - id=%llu]\n", (uint64_t)tab_id->valuedouble);

        // Queue WS message to extension
        if (sc->client_wsi) {
          vlog(LOG_LEVEL_TRACE, sc, "queueing message to websocket\n");
          cJSON* ws_payload = cJSON_CreateObject();
          cJSON_AddStringToObject(ws_payload, "event", "Daemon::WS::ActivateTabRequest");
          cJSON* ws_data = cJSON_CreateObject();
          cJSON_AddNumberToObject(ws_data, "tabId", tab_id->valuedouble);
          cJSON_AddItemToObject(ws_payload, "data", ws_data);

          char* ws_str = cJSON_PrintUnformatted(ws_payload);

          pthread_mutex_lock(&sc->pending_msg_mutex);
          if (sc->pending_ws_msg) {
            free(sc->pending_ws_msg);
          }
          sc->pending_ws_msg = ws_str;
          atomic_store(&sc->send_pending_msg, true);
          pthread_mutex_unlock(&sc->pending_msg_mutex);
          cJSON_Delete(ws_payload);

          // Wake up the WebSocket thread safely
          lws_cancel_service(sc->lws_ctx);
        }
      } else {
        vlog(LOG_LEVEL_ERROR, sc, "gui-evt: No tab id found for tab_selected\n");
      }
    } else if (strcmp(event->valuestring, "GUI::UDS::CloseTabsRequest") == 0) {
      cJSON* data = cJSON_GetObjectItem(json, "data");
      cJSON* tab_ids = cJSON_GetObjectItem(data, "tabIds");
      if (cJSON_IsArray(tab_ids)) {
        vlog(LOG_LEVEL_INFO, sc, "gui-evt: close_tabs - count=%d\n", cJSON_GetArraySize(tab_ids));

        if (sc->client_wsi) {
          cJSON* ws_payload = cJSON_CreateObject();
          cJSON_AddStringToObject(ws_payload, "event", "Daemon::WS::CloseTabsRequest");
          cJSON* ws_data = cJSON_CreateObject();
          cJSON_AddItemToObject(ws_data, "tabIds", cJSON_Duplicate(tab_ids, 1));
          cJSON_AddItemToObject(ws_payload, "data", ws_data);

          char* ws_str = cJSON_PrintUnformatted(ws_payload);

          pthread_mutex_lock(&sc->pending_msg_mutex);
          if (sc->pending_ws_msg) {
            free(sc->pending_ws_msg);
          }
          sc->pending_ws_msg = ws_str;
          atomic_store(&sc->send_pending_msg, true);
          pthread_mutex_unlock(&sc->pending_msg_mutex);
          cJSON_Delete(ws_payload);

          lws_cancel_service(sc->lws_ctx);
        }
      }
    } else {
      vlog(LOG_LEVEL_INFO, sc, "Received GUI Event: %s\n", event->valuestring);
    }
  }
  cJSON_Delete(json);
}

static void* uds_read_thread_run(void* arg) {
  ServerContext* sc = (ServerContext*)arg;
  uint32_t msg_len = 0;

// Reuse buffer for payload to avoid constant malloc for small messages
#define MAX_UDS_MSG_SIZE 65536
  char* buffer = malloc(MAX_UDS_MSG_SIZE);
  if (!buffer) {
    vlog(LOG_LEVEL_ERROR, sc, "Failed to allocate UDS buffer\n");
    return NULL;
  }

  vlog(LOG_LEVEL_INFO, sc, "Starting UDS read loop (Framed)\n");

  while (!atomic_load(&sc->uds_read_exit)) {
    // 1. Read Length (4 bytes)
    ssize_t n = recv(sc->uds_fd, &msg_len, sizeof(msg_len), 0);

    if (n == 0) {
      vlog(LOG_LEVEL_INFO, sc, "UDS connection closed by GUI\n");
      break;
    } else if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(100000);
        continue;
      }
      vlog(LOG_LEVEL_ERROR, sc, "UDS recv header error: %s\n", strerror(errno));
      break;
    } else if (n != sizeof(msg_len)) {
      vlog(LOG_LEVEL_WARN, sc, "UDS recv partial header: %zd bytes\n", n);
      continue;
    }

    // 2. Read Payload
    // Check max size
    if (msg_len > MAX_UDS_MSG_SIZE - 1) {
      vlog(LOG_LEVEL_ERROR, sc, "UDS message too large: %u\n", msg_len);
      break;
    }

    size_t total_read = 0;
    while (total_read < msg_len) {
      n = recv(sc->uds_fd, buffer + total_read, msg_len - total_read, 0);
      if (n <= 0) {
        break;
      }
      total_read += n;
    }

    if (total_read == msg_len) {
      buffer[msg_len] = '\0';
      handle_gui_msg(sc, buffer);
    } else {
      vlog(LOG_LEVEL_ERROR, sc, "UDS recv payload incomplete. Expected %u, got %zu\n", msg_len, total_read);
      break;
    }
  }

  free(buffer);
  return NULL;
}

static int setup_uds_client(ServerContext* sctx) {
  int retries = 5;
  int uds_fd;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sctx->uds_path, sizeof(addr.sun_path) - 1);

  while (retries > 0) {
    uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) {
      vlog(LOG_LEVEL_ERROR, sctx, "Failed to create UDS socket: %s\n", strerror(errno));
      return -1;  // TODO: Use some error code
    }

    if (connect(uds_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      vlog(LOG_LEVEL_INFO, sctx, "Connected to App UDS at %s\n", sctx->uds_path);

      const char* ping_json = "{\"event\":\"Daemon::UDS::Ping\",\"data\":\"ping\"}";
      size_t len = strlen(ping_json);
      size_t frame_len = sizeof(uint32_t) + len;
      char* msg = malloc(frame_len);
      if (msg) {
        uint32_t header = (uint32_t)len;
        memcpy(msg, &header, sizeof(uint32_t));
        memcpy(msg + sizeof(uint32_t), ping_json, len);
        send(uds_fd, msg, frame_len, 0);
        free(msg);
      }
      sctx->uds_fd = uds_fd;
      atomic_store(&sctx->uds_read_exit, 0);
      pthread_create(&sctx->uds_read_thread, NULL, uds_read_thread_run, sctx);
      return 0;
    }

    close(uds_fd);
    uds_fd = -1;
    fprintf(stderr, "UDS connect failed, retrying in 1s... (%d left)\n", retries - 1);
    sleep(1);
    retries--;
  }
  vlog(LOG_LEVEL_ERROR, sctx, "Failed to connect to App UDS after multiple attempts\n");
  return -1;
}

static void send_uds(const int uds_fd, const cJSON* json_data) {
  if (uds_fd >= 0) {
    char* json_str = cJSON_PrintUnformatted(json_data);
    if (json_str) {
      size_t len = strlen(json_str);
      // Frame: [Length: 4 bytes (LE)] [Payload]
      size_t frame_len = sizeof(uint32_t) + len;
      char* msg = malloc(frame_len);
      if (msg) {
        // Assume LE host (macOS M1/Intel are LE)
        uint32_t header = (uint32_t)len;
        memcpy(msg, &header, sizeof(uint32_t));
        memcpy(msg + sizeof(uint32_t), json_str, len);

        if (send(uds_fd, msg, frame_len, 0) < 0) {
          vlog(LOG_LEVEL_ERROR, NULL, "Failed to send data to App via UDS: %s\n", strerror(errno));
        } else {
          vlog(LOG_LEVEL_TRACE, NULL, "uds-send: %s (len: %zu)\n", json_str, len);
        }
        free(msg);
      }
      free(json_str);
    }
  } else {
    vlog(LOG_LEVEL_WARN, NULL, "Warning - Cannot send UDS, not connected.\n");
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
      atomic_store(&sc->send_tab_request, 1);
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
      if (pss && pss->msg) {
        pss_clear_message(pss, 1);
      }
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      if (sc && sc->client_wsi) {
        pthread_mutex_lock(&sc->pending_msg_mutex);
        if (atomic_load(&sc->send_pending_msg)) {
          lws_callback_on_writable(sc->client_wsi);
        }
        pthread_mutex_unlock(&sc->pending_msg_mutex);
      }
      break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
      vlog(LOG_LEVEL_TRACE, sc, "lws-server-writeable\n");
      if (atomic_load(&sc->send_tab_request)) {
        const char* msg = "{\"event\":\"Daemon::WS::AllTabsInfoRequest\"}";
        size_t msg_len = strlen(msg);
        unsigned char buf[LWS_PRE + 256];
        memset(buf, 0, sizeof(buf));
        memcpy(&buf[LWS_PRE], msg, msg_len);
        lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);
        atomic_store(&sc->send_tab_request, 0);
        vlog(LOG_LEVEL_INFO, sc, "Sent request_tab_info to extension\n");
      }
      pthread_mutex_lock(&sc->pending_msg_mutex);
      if (atomic_load(&sc->send_pending_msg) && sc->pending_ws_msg) {
        vlog(LOG_LEVEL_TRACE, sc, "Sending pending message to websocket.\n");
        size_t msg_len = strlen(sc->pending_ws_msg);
        // Allocate buffer with LWS_PRE padding
        unsigned char* buf = malloc(LWS_PRE + msg_len + 1);
        if (buf) {
          memcpy(&buf[LWS_PRE], sc->pending_ws_msg, msg_len);
          buf[LWS_PRE + msg_len] = '\0';  // Just in case
          lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);
          free(buf);
        }
        free(sc->pending_ws_msg);
        sc->pending_ws_msg = NULL;
        atomic_store(&sc->send_pending_msg, 0);
      }
      pthread_mutex_unlock(&sc->pending_msg_mutex);
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
      // TODO: pass some context here
      vlog(LOG_LEVEL_INFO, NULL, "Process terminated.\n");
    } else {
      vlog(LOG_LEVEL_ERROR, NULL, "Failed to kill child process.\n");
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
  } while (n >= 0 && !atomic_load(&sc->ws_thread_exit));
  return NULL;
}

static void lws_log_emit_cb(int level, const char* line) {
  LogLevel vlevel = LOG_LEVEL_INFO;
  switch (level) {
    case LLL_ERR:
      vlevel = LOG_LEVEL_ERROR;
      break;
    case LLL_WARN:
      vlevel = LOG_LEVEL_WARN;
      break;
    case LLL_NOTICE:
    case LLL_INFO:
      vlevel = LOG_LEVEL_INFO;
      break;
    default:
      vlevel = LOG_LEVEL_TRACE;
      break;
  }
  static struct EngClass LWS_LOG_CLASS = {
      .name = "lws",
  };
  static struct EngClass* ec = &LWS_LOG_CLASS;
  vlog(vlevel, &ec, "%s", line);
}

// @returns 0 on failure.
static int setup_app_config_dir(const EngineCreationInfo* cinfo, OUT char** out_keyboard_toggle) {
  char config_dir[512];
  char* keybind_val = NULL;
  static const char* DEFAULT_UI_TOGGLE_KEYBIND = "CMD+SHIFT+J";

  assert(out_keyboard_toggle);

  if (cinfo->config_path && strlen(cinfo->config_path) > 0) {
    strncpy(config_dir, cinfo->config_path, sizeof(config_dir) - 1);
  } else {
    const char* home_dir = getenv("HOME");
    if (home_dir) {
      snprintf(config_dir, sizeof(config_dir), "%s/.lotab", home_dir);
    } else {
      config_dir[0] = '\0';
    }
  }

  if (strlen(config_dir) > 0) {
    struct stat st = {0};
    if (stat(config_dir, &st) == -1) {
      if (mkdir(config_dir, 0755) != 0) {
        vlog(LOG_LEVEL_ERROR, NULL, "Failed to create config directory: %s (err: %s)\n", config_dir, strerror(errno));
        goto fail;
      } else {
        vlog(LOG_LEVEL_INFO, NULL, "Created config directory: %s\n", config_dir);
      }
    }

    char config_file[512];
    snprintf(config_file, sizeof(config_file), "%s/config.toml", config_dir);
    if (stat(config_file, &st) == -1) {
      FILE* fp = fopen(config_file, "w");
      if (fp) {
        fprintf(fp, "# Lotab Configuration\n");
        fprintf(fp, "UiToggleKeybind = \"%s\"\n", DEFAULT_UI_TOGGLE_KEYBIND);
        fclose(fp);
        vlog(LOG_LEVEL_INFO, NULL, "Created config file: %s\n", config_file);
      } else {
        vlog(LOG_LEVEL_ERROR, NULL, "Failed to create config file: %s\n", strerror(errno));
      }
    }

    FILE* fp = fopen(config_file, "r");
    if (fp) {
      char errbuf[200];
      toml_table_t* conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
      fclose(fp);

      if (conf) {
        toml_datum_t bind = toml_string_in(conf, "UiToggleKeybind");
        if (bind.ok) {
          keybind_val = strdup(bind.u.s);
          free(bind.u.s);
          vlog(LOG_LEVEL_INFO, NULL, "Loaded keybind: %s\n", keybind_val);
        }
        toml_free(conf);
      } else {
        vlog(LOG_LEVEL_ERROR, NULL, "Failed to parse config file: %s\n", errbuf);
      }
    } else {
      vlog(LOG_LEVEL_ERROR, NULL, "Failed to open config file for reading: %s\n", config_file);
    }
  }

  if (!keybind_val) {
    keybind_val = strdup(DEFAULT_UI_TOGGLE_KEYBIND);
  }

  // Validate Keybind
  char* upper = strdup(keybind_val);
  for (int i = 0; upper[i]; i++) {
    upper[i] = toupper(upper[i]);
  }
  int has_cmd = (strstr(upper, "CMD") != NULL) || (strstr(upper, "COMMAND") != NULL);
  int has_shift = (strstr(upper, "SHIFT") != NULL);
  free(upper);

  if (!has_cmd || !has_shift) {
    vlog(LOG_LEVEL_ERROR, NULL, "Invalid UiToggleKeybind: '%s'. Must contain CMD and SHIFT.\n", keybind_val);
    goto fail;
  }

  *out_keyboard_toggle = keybind_val;
  return 1;

fail:
  if (keybind_val)
    free(keybind_val);
  return 0;
}

int engine_init(EngineContext** ectx, EngineCreationInfo cinfo) {
  assert(ectx != NULL);
  assert(*ectx == NULL);

  EngineContext* ec = NULL;
  ServerContext* sc = NULL;
  struct lws_context_creation_info info;
  int ret;

  ec = (EngineContext*)malloc(sizeof(EngineContext));
  if (!ec) {
    vlog(LOG_LEVEL_ERROR, NULL, "Failed to allocate engine context.\n");
    ret = NGERROR(ENOMEM);
    goto fail;
  }
  memset(ec, 0, sizeof(EngineContext));
  ec->cls = &ENGINE_CONTEXT_CLASS;
  ec->app_pid = -1;
  ec->tab_state = calloc(1, sizeof(TabState));
  ec->tab_state->cls = &TAB_STATE_CLASS;
  ec->task_state = calloc(1, sizeof(TaskState));
  ec->task_state->cls = &TASK_STATE_CLASS;
  ec->init_statusline = cinfo.enable_statusbar != 0;

  if (setup_app_config_dir(&cinfo, &ec->ui_toggle_keybind) == 0) {
    ret = -1;
    goto fail;
  }

  // Setup websocket server
  vlog(LOG_LEVEL_INFO, ec, "Setting up websocket server.\n");
  sc = (ServerContext*)malloc(sizeof(ServerContext));
  if (!sc) {
    vlog(LOG_LEVEL_ERROR, ec, "Failed to allocate server context.\n");
    ret = NGERROR(ENOMEM);
    goto fail;
  }
  memset(sc, 0, sizeof(ServerContext));
  sc->cls = &SERVER_CONTEXT_CLASS;
  sc->uds_fd = -1;
  pthread_mutex_init(&sc->pending_msg_mutex, NULL);
  atomic_init(&sc->ws_thread_exit, 0);
  atomic_init(&sc->send_tab_request, 0);
  atomic_init(&sc->uds_read_exit, 0);
  atomic_init(&sc->send_pending_msg, 0);

  lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO, lws_log_emit_cb);

  if (cinfo.uds_path && strlen(cinfo.uds_path) > 0) {
    sc->uds_path = strdup(cinfo.uds_path);
  } else {
    sc->uds_path = strdup("/tmp/lotab.sock");
  }

  ec->serv_ctx = sc;
  memset(&info, 0, sizeof info);
  info.port = cinfo.port;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.user = ec;
  vlog(LOG_LEVEL_INFO, ec, "Starting Daemon WebSocket server on port %d\n", info.port);
  sc->lws_ctx = lws_create_context(&info);
  if (!sc->lws_ctx) {
    ret = -1;  // TODO: use some defined error code.
    goto fail;
  }

  if (pthread_create(&sc->ws_thread, NULL, ws_thread_run, (void*)sc) != 0) {
    vlog(LOG_LEVEL_ERROR, ec, "Failed to start websocket server.\n");
    ret = -1;  // TODO: use some defined error code.
    goto fail;
  }

// Initialize gui client and uds protocol
#ifndef APP_PATH
  vlog(LOG_LEVEL_ERROR, ec, "APP_PATH is not defined. Daemon cannot continue.\n");
  ret = -1;  // TODO: use some defined error code.
  goto fail;
#endif
  const char* target_app_path = (cinfo.app_path && strlen(cinfo.app_path) > 0) ? cinfo.app_path : APP_PATH;

  char log_level_arg[16] = {0};
  snprintf(log_level_arg, sizeof(log_level_arg), "%d", engine_get_log_level());
  char* spawn_args[] = {(char*)target_app_path, "--log-level", log_level_arg, NULL};
  int spawn_status = posix_spawn(&ec->app_pid, target_app_path, NULL, NULL, spawn_args, environ);
  if (spawn_status == 0) {
    vlog(LOG_LEVEL_INFO, ec, "Successfully spawned Lotab (PID: %d)\n", ec->app_pid);
    sleep(1);
    setup_uds_client(sc);
  } else {
    vlog(LOG_LEVEL_ERROR, ec, "Failed to spawn Lotab: %s\n", strerror(spawn_status));
    ret = -1;
    goto fail;
  }
  vlog(LOG_LEVEL_INFO, ec, "Engine initialized.\n");
  *ectx = ec;
  return 0;

fail:
  if (ec && ec->app_pid >= 0) {
    kill_process(ec->app_pid);
    ec->app_pid = -1;
  }
  if (ec && ec->ui_toggle_keybind) {
    free(ec->ui_toggle_keybind);
  }
  if (sc && sc->lws_ctx) {
    lws_context_destroy(sc->lws_ctx);
  }
  if (sc) {
    free(sc);
  }
  if (ec->tab_state) {
    tab_state_free(ec->tab_state);
  }
  if (ec->task_state) {
    task_state_free(ec->task_state);
  }
  if (ec) {
    free(ec);
  }
  return ret;
}

static void sigint_handler(int sig) {
  (void)sig;
  printf("Daemon: Caught SIGINT\n");
  // TODO: This should only be called if the statusline initialized, which depends on the engine context.
  stop_daemon_cocoa_app();
}

void engine_run(EngineContext* ectx) {
  signal(SIGINT, sigint_handler);
  ectx->run_ctx = malloc(sizeof(StatusBarRunContext));
  if (!ectx->run_ctx) {
    vlog(LOG_LEVEL_ERROR, ectx, "Failed to allocate run context.\n");
    return;
  }
  ectx->run_ctx->on_toggle = on_status_toggle;
  ectx->run_ctx->on_quit = on_status_quit;
  ectx->run_ctx->keybind = ectx->ui_toggle_keybind;
  ectx->run_ctx->privdata = ectx;
  if (ectx->init_statusline) {
    vlog(LOG_LEVEL_INFO, ectx, "Starting cocoa event loop\n");
    run_daemon_cocoa_app(ectx->run_ctx);
  }
}

void engine_destroy(EngineContext* ectx) {
  assert(ectx);
  if (__atomic_exchange_n(&ectx->destroyed, 1, __ATOMIC_SEQ_CST))
    return;
  if (ectx->app_pid >= 0) {
    kill_process(ectx->app_pid);
    ectx->app_pid = -1;
  }
  if (ectx->serv_ctx) {
    if (ectx->serv_ctx->lws_ctx) {
      lws_cancel_service(ectx->serv_ctx->lws_ctx);
      if (ectx->serv_ctx->ws_thread) {
        atomic_store(&ectx->serv_ctx->ws_thread_exit, 1);
        pthread_join(ectx->serv_ctx->ws_thread, NULL);
      }
      lws_context_destroy(ectx->serv_ctx->lws_ctx);
      if (ectx->init_statusline)
        stop_daemon_cocoa_app();
    }
    if (ectx->serv_ctx->uds_read_thread) {
      atomic_store(&ectx->serv_ctx->uds_read_exit, 1);
      if (ectx->serv_ctx->uds_fd >= 0) {
        shutdown(ectx->serv_ctx->uds_fd, SHUT_RDWR);
        close(ectx->serv_ctx->uds_fd);
        ectx->serv_ctx->uds_fd = -1;
        unlink(ectx->serv_ctx->uds_path);
      }
      pthread_join(ectx->serv_ctx->uds_read_thread, NULL);
    }
    if (ectx->serv_ctx->uds_path) {
      free(ectx->serv_ctx->uds_path);
    }
    pthread_mutex_destroy(&ectx->serv_ctx->pending_msg_mutex);
    free(ectx->serv_ctx);
    ectx->serv_ctx = NULL;
  }
  if (ectx->run_ctx) {
    free(ectx->run_ctx);
    ectx->run_ctx = NULL;
  }
  if (ectx->tab_state) {
    tab_state_free(ectx->tab_state);
    ectx->tab_state = NULL;
  }
  if (ectx->task_state) {
    task_state_free(ectx->task_state);
    ectx->task_state = NULL;
  }
  if (ectx->ui_toggle_keybind) {
    free(ectx->ui_toggle_keybind);
    ectx->ui_toggle_keybind = NULL;
  }
  ectx->destroyed = 1;
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

void tab_state_update_tab(TabState* ts, const char* title, const uint64_t id, int64_t task_id) {
  TabInfo* ti = tab_state_find_tab(ts, id);
  if (ti) {
    if (ti->title && strcmp(title, ti->title) != 0) {
      free(ti->title);
      ti->title = strdup(title);
    }
    ti->task_id = task_id;
  }
}

void tab_state_add_tab(TabState* ts, const char* title, const uint64_t id, int64_t task_id) {
  TabInfo* new_tab = malloc(sizeof(TabInfo));
  if (new_tab) {
    new_tab->id = id;
    new_tab->title = strdup(title);
    new_tab->active = 0;
    new_tab->task_id = task_id;
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

TaskInfo* task_state_find_by_external_id(TaskState* ts, int64_t external_id) {
  assert(ts);
  if (external_id < 0)
    return NULL;
  TaskInfo* cur = ts->tasks;
  while (cur) {
    if (cur->external_id == external_id)
      return cur;
    cur = cur->next;
  }
  return NULL;
}

void task_state_add(TaskState* ts, const char* task_name, int64_t external_id) {
  TaskInfo* new_task = malloc(sizeof(TaskInfo));
  if (new_task) {
    new_task->task_id = ts->nb_tasks++;
    new_task->task_name = strdup(task_name ? task_name : "Unknown Task");
    new_task->external_id = external_id;
    new_task->next = ts->tasks;
    ts->tasks = new_task;
  }
}

void tab_event__handle_all_tabs(EngineContext* ec, const cJSON* json_data) {
  TabState* ts = ec->tab_state;
  TaskState* tks = ec->task_state;
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (!data) {
    vlog(LOG_LEVEL_WARN, ts, "onAllTabs: 'data' key missing.\n");
    return;
  }

  cJSON* tabs_json = NULL;
  cJSON* groups_json = NULL;

  if (cJSON_IsArray(data)) {
    // Legacy format: 'data' is the array of tabs
    tabs_json = data;
  } else if (cJSON_IsObject(data)) {
    tabs_json = cJSON_GetObjectItem(data, "tabs");
    groups_json = cJSON_GetObjectItem(data, "groups");
  }

  if (groups_json && cJSON_IsArray(groups_json)) {
    int group_count = cJSON_GetArraySize(groups_json);
    for (int i = 0; i < group_count; i++) {
      cJSON* g = cJSON_GetArrayItem(groups_json, i);
      cJSON* gid_json = cJSON_GetObjectItemCaseSensitive(g, "id");
      cJSON* gtitle_json = cJSON_GetObjectItemCaseSensitive(g, "title");

      int64_t external_id = -1;
      if (cJSON_IsNumber(gid_json)) {
        external_id = (int64_t)gid_json->valuedouble;
      }
      char* group_title = "Browser Group";
      if (cJSON_IsString(gtitle_json) && gtitle_json->valuestring && strlen(gtitle_json->valuestring) > 0) {
        group_title = gtitle_json->valuestring;
      }

      if (task_state_find_by_external_id(tks, external_id) == NULL) {
        task_state_add(tks, group_title, external_id);
      }
    }
  }

  if (tabs_json && cJSON_IsArray(tabs_json)) {
    int count = cJSON_GetArraySize(tabs_json);
    vlog(LOG_LEVEL_INFO, ts, "Received %d tabs (current state: %d)\n", count, ts->nb_tabs);
    int tabs_added = 0, tabs_updated = 0;

    for (int i = 0; i < count; i++) {
      cJSON* item = cJSON_GetArrayItem(tabs_json, i);
      cJSON* title_json = cJSON_GetObjectItemCaseSensitive(item, "title");
      cJSON* id_json = cJSON_GetObjectItemCaseSensitive(item, "id");
      cJSON* gid_json = cJSON_GetObjectItemCaseSensitive(item, "groupId");

      char* tab_title = NULL;
      uint64_t tab_id = 0;
      int64_t task_id = -1;

      if (cJSON_IsString(title_json) && (title_json->valuestring != NULL)) {
        tab_title = title_json->valuestring;
      }
      if (cJSON_IsNumber(id_json)) {
        tab_id = (uint64_t)id_json->valuedouble;
      }
      if (cJSON_IsNumber(gid_json)) {
        int64_t external_id = (int64_t)gid_json->valuedouble;
        TaskInfo* task = task_state_find_by_external_id(tks, external_id);
        if (task) {
          task_id = task->task_id;
        }
      }

      if (tab_state_find_tab(ts, tab_id) != NULL) {
        tab_state_update_tab(ts, tab_title ? tab_title : "Unknown", tab_id, task_id);
        ++tabs_updated;
      } else {
        tab_state_add_tab(ts, tab_title ? tab_title : "Unknown", tab_id, task_id);
        ++tabs_added;
      }
    }
    vlog(LOG_LEVEL_INFO, ts, "Tab State Synced: %d updated, %d added. Total: %d\n", tabs_updated, tabs_added,
         ts->nb_tabs);
  } else {
    vlog(LOG_LEVEL_WARN, ts, "onAllTabs: 'tabs' data missing or not an array.\n");
  }
  tab_state_update_active(ts, json_data);
}

void tab_state_update_active(TabState* ts, const cJSON* json_data) {
  cJSON* active_tabs_json = cJSON_GetObjectItem(json_data, "activeTabIds");
  if (!active_tabs_json || !cJSON_IsArray(active_tabs_json))
    return;

#define MAX_ACTIVE_TAB_IDS_SIZE 64
  uint64_t active_tab_ids[MAX_ACTIVE_TAB_IDS_SIZE];
  int active_count = 0;
  int array_size = cJSON_GetArraySize(active_tabs_json);

  for (int i = 0; i < array_size && i < MAX_ACTIVE_TAB_IDS_SIZE; i++) {
    cJSON* item = cJSON_GetArrayItem(active_tabs_json, i);
    if (cJSON_IsNumber(item)) {
      active_tab_ids[active_count++] = (uint64_t)item->valuedouble;
    }
  }
  TabInfo* current = ts->tabs;
  while (current) {
    current->active = 0;
    for (int i = 0; i < active_count; i++) {
      if (current->id == active_tab_ids[i]) {
        current->active = 1;
        break;
      }
    }
    current = current->next;
  }
}

void tab_event__handle_remove_tab(EngineContext* ec, const cJSON* json_data) {
  TabState* ts = ec->tab_state;
  // {"event": "tabs.onRemoved", "data": {"tabId": 123, "removeInfo": {...}}}
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (data) {
    cJSON* tabIdJson = cJSON_GetObjectItem(data, "tabId");
    if (cJSON_IsNumber(tabIdJson)) {
      uint64_t id = (uint64_t)tabIdJson->valuedouble;
      tab_state_remove_tab(ts, id);
      vlog(LOG_LEVEL_INFO, ts, "Tab Removed: %llu. Remaining: %d\n", id, ts->nb_tabs);
    } else {
      vlog(LOG_LEVEL_WARN, ts, "onRemoved: tabId missing or invalid\n");
    }
  }
  tab_state_update_active(ts, json_data);
}

void tab_event__handle_activated(EngineContext* ec, const cJSON* json_data) {
  TabState* ts = ec->tab_state;
  // {"event": "tabs.onActivated", "data": {"tabId": 123, "windowId": 456}}
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (data) {
    cJSON* tabIdJson = cJSON_GetObjectItem(data, "tabId");
    if (cJSON_IsNumber(tabIdJson)) {
      uint64_t id = (uint64_t)tabIdJson->valuedouble;
      vlog(LOG_LEVEL_INFO, ts, "Tab Activated: %llu\n", id);
    } else {
      vlog(LOG_LEVEL_WARN, ts, "onActivated: tabId missing or invalid\n");
    }
  }
  tab_state_update_active(ts, json_data);
}

void tab_event__handle_created(EngineContext* ec, const cJSON* json_data) {
  TabState* ts = ec->tab_state;
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
      tab_state_add_tab(ts, title, id, -1);
      vlog(LOG_LEVEL_INFO, ts, "Tab Created: %llu, Title: %s\n", id, title);
    } else {
      vlog(LOG_LEVEL_WARN, ts, "onCreated: id missing or invalid\n");
    }
  }
  tab_state_update_active(ts, json_data);
}

void tab_event__do_nothing(EngineContext* ec, const cJSON* json_data) {
  (void)ec;
  (void)json_data;
  vlog(LOG_LEVEL_TRACE, ec, "tab_event -- do nothing\n");
}

void tab_event__handle_updated(EngineContext* ec, const cJSON* json_data) {
  TabState* ts = ec->tab_state;
  // {"event": "tabs.onUpdated", "data": {"tabId": 123, "changeInfo": {...}, "tab": {"id": 123, "title": "..."}}}
  cJSON* data = cJSON_GetObjectItem(json_data, "data");
  if (!data)
    return;
  cJSON* tab_json = cJSON_GetObjectItem(data, "tab");
  if (!tab_json)
    return;
  cJSON* id_json = cJSON_GetObjectItem(tab_json, "id");
  cJSON* title_json = cJSON_GetObjectItem(tab_json, "title");

  if (!cJSON_IsNumber(id_json))
    return;
  uint64_t id = (uint64_t)id_json->valuedouble;
  const char* title = "Unknown";
  if (cJSON_IsString(title_json) && title_json->valuestring) {
    title = title_json->valuestring;
  }
  if (tab_state_find_tab(ts, id)) {
    tab_state_update_tab(ts, title, id, -1);
    vlog(LOG_LEVEL_INFO, ts, "Tab Updated: %llu, Title: %s\n", id, title);
  } else {
    tab_state_add_tab(ts, title, id, -1);
    vlog(LOG_LEVEL_INFO, ts, "Tab Updated (New): %llu, Title: %s\n", id, title);
  }
  tab_state_update_active(ts, json_data);
}

struct TabEventMapEntry {
  const char* event_name;
  TabEventType type;
};
// clang-format off
static const struct TabEventMapEntry TAB_EVENT_MAP[] = {
    {"Extension::WS::TabActivated", TAB_EVENT_ACTIVATED},
    {"Extension::WS::TabUpdated", TAB_EVENT_UPDATED},
    {"Extension::WS::TabCreated", TAB_EVENT_CREATED},
    {"Extension::WS::TabHighlighted", TAB_EVENT_HIGHLIGHTED},
    {"Extension::WS::TabZoomChanged", TAB_EVENT_ZOOM_CHANGE},
    {"Extension::WS::AllTabsInfoResponse", TAB_EVENT_ALL_TABS},
    {"Extension::WS::TabRemoved", TAB_EVENT_TAB_REMOVED},
    {NULL, TAB_EVENT_UNKNOWN},
};

static struct {
  TabEventType type;
  void (*event_handler)(EngineContext* ec, const cJSON* json_data);
} TAB_EVENT_HANDLERS[] = {
    {TAB_EVENT_ALL_TABS, tab_event__handle_all_tabs},
    {TAB_EVENT_TAB_REMOVED, tab_event__handle_remove_tab},
    {TAB_EVENT_ACTIVATED, tab_event__handle_activated},
    {TAB_EVENT_CREATED, tab_event__handle_created},
    {TAB_EVENT_UPDATED, tab_event__handle_updated},
    {TAB_EVENT_HIGHLIGHTED, tab_event__do_nothing},
    {TAB_EVENT_ZOOM_CHANGE, tab_event__do_nothing},
    {TAB_EVENT_UNKNOWN, NULL},
};
// clang-format on

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
      vlog(LOG_LEVEL_WARN, NULL, "Unknown tab event: %s\n", event->valuestring);
    }
  }
  return type;
}

void tab_event_handle(EngineContext* ec, TabEventType type, cJSON* json_data) {
  int event_handled = 0;
  for (int i = 0; TAB_EVENT_HANDLERS[i].type != TAB_EVENT_UNKNOWN; ++i) {
    if (TAB_EVENT_HANDLERS[i].type == type) {
      if (TAB_EVENT_HANDLERS[i].event_handler) {
        TAB_EVENT_HANDLERS[i].event_handler(ec, json_data);
      }
      event_handled = 1;
      break;
    }
  }
  if (!event_handled) {
    vlog(LOG_LEVEL_WARN, ec->tab_state, "Unhandled tab event type: %d\n", type);
  }
}

static void send_tabs_update_to_uds(EngineContext* ectx) {
  if (!ectx->serv_ctx || ectx->serv_ctx->uds_fd < 0)
    return;

  cJSON* tab_update_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(tab_update_msg, "event", "Daemon::UDS::TabsUpdate");

  cJSON* event_data = cJSON_CreateObject();
  cJSON_AddItemToObject(tab_update_msg, "data", event_data);

  cJSON* tabs_array = cJSON_CreateArray();
  cJSON_AddItemToObject(event_data, "tabs", tabs_array);

  if (ectx->tab_state) {
    TabInfo* current = ectx->tab_state->tabs;
    while (current) {
      cJSON* tab_obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(tab_obj, "id", (double)current->id);
      cJSON_AddStringToObject(tab_obj, "title", current->title ? current->title : "Unknown");
      cJSON_AddBoolToObject(tab_obj, "active", current->active);
      cJSON_AddNumberToObject(tab_obj, "task_id", (double)current->task_id);
      cJSON_AddItemToArray(tabs_array, tab_obj);
      current = current->next;
    }
  }
  send_uds(ectx->serv_ctx->uds_fd, tab_update_msg);
  cJSON_Delete(tab_update_msg);
}

static void send_tasks_update_to_uds(EngineContext* ectx) {
  if (!ectx->serv_ctx || ectx->serv_ctx->uds_fd < 0)
    return;

  cJSON* task_update_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(task_update_msg, "event", "Daemon::UDS::TasksUpdate");

  cJSON* task_data = cJSON_CreateObject();
  cJSON_AddItemToObject(task_update_msg, "data", task_data);

  cJSON* tasks_array = cJSON_CreateArray();
  cJSON_AddItemToObject(task_data, "tasks", tasks_array);

  if (ectx->task_state) {
    TaskInfo* current = ectx->task_state->tasks;
    while (current) {
      cJSON* task_obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(task_obj, "id", (double)current->task_id);
      cJSON_AddStringToObject(task_obj, "name", current->task_name ? current->task_name : "Unknown");
      cJSON_AddItemToArray(tasks_array, task_obj);
      current = current->next;
    }
  }
  send_uds(ectx->serv_ctx->uds_fd, task_update_msg);
  cJSON_Delete(task_update_msg);
}

void engine_handle_event(EngineContext* ectx, DaemonEvent event, void* data) {
  assert(ectx != NULL);

  switch (event) {
    case EVENT_HOTKEY_TOGGLE: {
      send_tabs_update_to_uds(ectx);
      send_tasks_update_to_uds(ectx);

      // 3. Send Toggle
      cJSON* toggle_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(toggle_msg, "event", "Daemon::UDS::ToggleGuiRequest");
      cJSON_AddStringToObject(toggle_msg, "data", "toggle");
      send_uds(ectx->serv_ctx->uds_fd, toggle_msg);
      cJSON_Delete(toggle_msg);
    } break;

    case EVENT_WS_MESSAGE_RECEIVED:
      if (data) {
        vlog(LOG_LEVEL_TRACE, ectx->serv_ctx, "raw message: %s\n", (const char*)data);
        const char* json_msg = (const char*)data;
        cJSON* json = cJSON_Parse(json_msg);
        if (json) {
          vlog(LOG_LEVEL_TRACE, ectx->serv_ctx, "json parsed message: %s\n", cJSON_Print(json));
          TabEventType type = parse_event_type(json);
          if (type != TAB_EVENT_UNKNOWN) {
            tab_event_handle(ectx, type, json);
            switch (type) {
              case TAB_EVENT_ACTIVATED:
              case TAB_EVENT_ALL_TABS:
              case TAB_EVENT_TAB_REMOVED:
              case TAB_EVENT_CREATED:
              case TAB_EVENT_UPDATED:
                send_tabs_update_to_uds(ectx);
                send_tasks_update_to_uds(ectx);
                break;
              default:
                vlog(LOG_LEVEL_TRACE, ectx->serv_ctx, "ignoring tab event type: %d\n", type);
            }
          }
          cJSON_Delete(json);
        } else {
          vlog(LOG_LEVEL_ERROR, ectx, "Failed to parse json from websocket message.\n");
        }
      }
      break;
  }
}
