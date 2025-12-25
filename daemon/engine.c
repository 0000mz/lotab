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
#include <stdio.h>
#include <string.h>
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
  pthread_t ws_thread;
  int uds_fd;
  int ws_thread_exit;  // TODO: this should be protected by mutex.
} ServerContext;

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

static int callback_minimal(struct lws* wsi, enum lws_callback_reasons reason, void* _user, void* in, size_t len) {
  EngineContext* ec = (EngineContext*)lws_context_user(lws_get_context(wsi));
  (void)_user;

  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
      lwsl_user("LWS_CALLBACK_ESTABLISHED (new connection)\n");
      break;
    case LWS_CALLBACK_CLOSED:
      lwsl_user("LWS_CALLBACK_CLOSED (connection lost)\n");
      break;
    case LWS_CALLBACK_RECEIVE: {
      char* msg = malloc(len + 1);
      if (msg) {
        memcpy(msg, in, len);
        msg[len] = '\0';
        engine_handle_event(ec, EVENT_WS_MESSAGE_RECEIVED, msg);
        free(msg);
      }
    } break;
    default:
      break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {
    {.name = "minimal", .callback = callback_minimal, .per_session_data_size = 0, .rx_buffer_size = 0},
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

  ec->serv_ctx = sc;
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

// Helper to determine event type from JSON string
static TabEventType parse_event_type(const char* json_str) {
  cJSON* json = cJSON_Parse(json_str);
  if (!json) {
    return TAB_EVENT_UNKNOWN;
  }

  TabEventType type = TAB_EVENT_UNKNOWN;
  cJSON* event = cJSON_GetObjectItemCaseSensitive(json, "event");

  if (cJSON_IsString(event) && (event->valuestring != NULL)) {
    if (strcmp(event->valuestring, "tabs.onActivated") == 0) {
      type = TAB_EVENT_ACTIVATED;
    } else if (strcmp(event->valuestring, "tabs.onUpdated") == 0) {
      type = TAB_EVENT_UPDATED;
    } else if (strcmp(event->valuestring, "tabs.onHighlighted") == 0) {
      type = TAB_EVENT_HIGHLIGHTED;
    } else if (strcmp(event->valuestring, "tabs.onZoomChange") == 0) {
      type = TAB_EVENT_ZOOM_CHANGE;
    }
  }

  cJSON_Delete(json);
  return type;
}

void engine_handle_tab_event(TabEventType type, const char* json_data) {
  (void)json_data;  // Unused for now

  switch (type) {
    case TAB_EVENT_ACTIVATED:
      vlog(LOG_LEVEL_INFO, "Engine: Tab Activated\n");
      break;
    case TAB_EVENT_UPDATED:
      vlog(LOG_LEVEL_INFO, "Engine: Tab Updated\n");
      break;
    case TAB_EVENT_HIGHLIGHTED:
      vlog(LOG_LEVEL_INFO, "Engine: Tab Highlighted\n");
      break;
    case TAB_EVENT_ZOOM_CHANGE:
      vlog(LOG_LEVEL_INFO, "Engine: Tab Zoom Change\n");
      break;
    case TAB_EVENT_UNKNOWN:
    default:
      vlog(LOG_LEVEL_INFO, "Engine: Unknown Tab Event\n");
      break;
  }
}

void engine_handle_event(EngineContext* ectx, DaemonEvent event, void* data) {
  assert(ectx != NULL);

  switch (event) {
    case EVENT_HOTKEY_TOGGLE:
      vlog(LOG_LEVEL_INFO, "Engine: Toggle Requestedh\n");
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
        TabEventType type = parse_event_type(json_msg);
        if (type != TAB_EVENT_UNKNOWN) {
          engine_handle_tab_event(type, json_msg);
        }
      }
      break;
  }
}
