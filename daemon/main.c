#include <argparse.h>
#include <errno.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "engine.h"
#include "statusbar.h"

extern char** environ;

static int interrupted;
static int uds_fd = -1;
static const char* uds_path = "/tmp/tabmanager.sock";
static struct lws_context* lws_ctx = NULL;
static pid_t app_pid = -1;  // Global PID for spawned app
static pthread_t ws_thread;

// --- Adapter Implementations ---

static void adapter_log(const char* msg) {
  printf("Daemon: %s\n", msg);
}

static void adapter_send_uds(const char* data) {
  if (uds_fd >= 0) {
    if (send(uds_fd, data, strlen(data), 0) < 0) {
      fprintf(stderr, "Daemon: Failed to send data to App via UDS: %s\n", strerror(errno));
    } else {
      // lwsl_user is acceptable here too if we want LWS logs, but standard
      // printf matches previous behavior better for adapter actually previous
      // code used lwsl_user in callback_minimal, but printf in toggle. Let's
      // stick to simple printf for the adapter log or lwsl_user if context
      // exists? "adapter_log" above uses printf. For UDS send success, previous
      // code logged "Daemon: Forwarded message..."
      printf("Daemon: Sent UDS message: %s\n", data);  // Simplified log
    }
  } else {
    printf("Daemon: Warning - Cannot send UDS, not connected.\n");
  }
}

static void adapter_spawn_gui(void) {
  // Currently spawn is done in main() init, not requested by engine yet.
  // If engine asks for it, we could move logic here.
  // For now, no-op or implementation if we move logic.
}

static void adapter_kill_gui(void) {
  if (app_pid > 0) {
    printf("Daemon: Terminating child process %d...\n", app_pid);
    if (kill(app_pid, SIGTERM) == 0) {
      int status;
      waitpid(app_pid, &status, 0);
      printf("Daemon: Child process terminated.\n");
    } else {
      perror("Daemon: Failed to kill child process");
    }
    app_pid = -1;
  }
}

static void adapter_quit_app(void) {
  // Stop LWS loop
  if (lws_ctx) {
    lws_cancel_service(lws_ctx);
  }
  interrupted = 1;

  // We can't easily join ws_thread if we are called FROM ws_thread.
  // engine_handle_event might be called from WS callback.
  // So we just set interrupted = 1 and cancel service. main() loop will handle
  // cleanup. But if we are called from Menu (main thread), we might want to
  // exit main loop.

  // The Cocoa app run loop is blocking main(). We need to stop it.
  stop_daemon_cocoa_app();
}

static void cleanup_and_exit(void) {
  if (!interrupted) {
    interrupted = 1;
  }

  // Stop LWS loop
  if (lws_ctx) {
    lws_cancel_service(lws_ctx);
  }

  // Join LWS thread
  if (ws_thread) {
    pthread_join(ws_thread, NULL);
    ws_thread = NULL;
  }

  if (uds_fd >= 0) {
    close(uds_fd);
    uds_fd = -1;
  }

  // Helper to kill GUI directly if not done by engine
  adapter_kill_gui();

  printf("Daemon: Cleanup complete.\n");
}

// --- WebSocket Callbacks ---

static int callback_minimal(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
  (void)user;
  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
      lwsl_user("LWS_CALLBACK_ESTABLISHED (new connection)\n");
      break;
    case LWS_CALLBACK_CLOSED:
      lwsl_user("LWS_CALLBACK_CLOSED (connection lost)\n");
      break;
    case LWS_CALLBACK_RECEIVE:
      // Ensure null termination for engine string handling if needed,
      // though 'in' is not guaranteed null terminated by LWS, usually text.
      // We'll trust source is text or handle it carefully.
      // wrapper for safety:
      {
        char* msg = malloc(len + 1);
        if (msg) {
          memcpy(msg, in, len);
          msg[len] = '\0';
          engine_handle_event(EVENT_WS_MESSAGE_RECEIVED, msg);
          free(msg);
        }
      }
      break;
    default:
      break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {
    {.name = "minimal", .callback = callback_minimal, .per_session_data_size = 0, .rx_buffer_size = 0},
    LWS_PROTOCOL_LIST_TERM};

void sigint_handler(int sig) {
  (void)sig;
  printf("Daemon: Caught SIGINT\n");
  adapter_quit_app();
}

static void init_uds_client(void) {
  int retries = 5;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);

  while (retries > 0) {
    uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) {
      fprintf(stderr, "Daemon: Failed to create UDS socket: %s\n", strerror(errno));
      return;
    }

    if (connect(uds_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      printf("Daemon: Connected to App UDS at %s\n", uds_path);
      // Send startup ping - maybe engine should do this on EVENT_APP_STARTED?
      // For now, keep it here or send EVENT_APP_STARTED.
      const char* ping = "{\"event\":\"daemon_startup\",\"data\":\"ping\"}";
      send(uds_fd, ping, strlen(ping), 0);
      return;
    }

    close(uds_fd);
    uds_fd = -1;
    fprintf(stderr, "Daemon: UDS connect failed, retrying in 1s... (%d left)\n", retries - 1);
    sleep(1);
    retries--;
  }
  fprintf(stderr, "Daemon: Failed to connect to App UDS after multiple attempts\n");
}

static void* lws_thread_func(void* arg) {
  (void)arg;
  struct lws_context_creation_info info;
  int n = 0;

  memset(&info, 0, sizeof info);
  info.port = 9001;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;

  lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
  lwsl_user("Starting Daemon WebSocket server on port %d\n", info.port);

  lws_ctx = lws_create_context(&info);
  if (!lws_ctx) {
    lwsl_err("lws init failed\n");
    return NULL;
  }

  while (n >= 0 && !interrupted) {
    n = lws_service(lws_ctx, 100);
  }

  lws_context_destroy(lws_ctx);
  lws_ctx = NULL;

  // Ensure app quits if thread exits
  stop_daemon_cocoa_app();

  return NULL;
}

// GUI Callbacks
static void on_status_toggle(void) {
  engine_handle_event(EVENT_HOTKEY_TOGGLE, NULL);
}

static void on_status_quit(void) {
  engine_handle_event(EVENT_MENU_QUIT, NULL);
}

int main(int argc, const char** argv) {
  signal(SIGINT, sigint_handler);

#ifndef APP_PATH
  fprintf(stderr, "Error: APP_PATH is not defined. Daemon cannot continue.\n");
  return 1;
#endif

  // Arg Parse
  const char* loglevel_str = NULL;

  struct argparse_option options[] = {
      OPT_HELP(),
      OPT_STRING('l', "loglevel", &loglevel_str, "info(default)|trace", NULL, 0, 0),
      OPT_END(),
  };

  struct argparse argparse;
  static const char* const usages[] = {
      "daemon [options]",
      NULL,
  };

  argparse_init(&argparse, options, usages, 0);
  argparse_describe(&argparse, "\nTabManager Daemon", "\nControls the TabManager backend.");
  argc = argparse_parse(&argparse, argc, argv);

  // Init Engine
  PlatformAdapter adapter = {.log = adapter_log,
                             .send_uds = adapter_send_uds,
                             .spawn_gui = adapter_spawn_gui,
                             .kill_gui = adapter_kill_gui,
                             .quit_app = adapter_quit_app};
  engine_init(&adapter);

  LogLevel loglevel = LOG_LEVEL_INFO;
  if (loglevel_str && strcmp(loglevel_str, "trace") == 0) {
    loglevel = LOG_LEVEL_TRACE;
  }
  engine_set_log_level(loglevel);

  printf("Daemon: Global App Path: %s\n", APP_PATH);

  // Spawn the TabManager App
  char* spawn_args[] = {(char*)APP_PATH, NULL};
  int spawn_status = posix_spawn(&app_pid, APP_PATH, NULL, NULL, spawn_args, environ);
  if (spawn_status == 0) {
    printf("Daemon: Successfully spawned TabManager (PID: %d)\n", app_pid);
    sleep(1);
    init_uds_client();
    engine_handle_event(EVENT_APP_STARTED, NULL);
  } else {
    fprintf(stderr, "Daemon: Failed to spawn TabManager: %s\n", strerror(spawn_status));
    init_uds_client();  // Try to connect anyway?
  }

  if (pthread_create(&ws_thread, NULL, lws_thread_func, NULL) != 0) {
    fprintf(stderr, "Failed to start WebSocket thread\n");
    return 1;
  }

  printf("Daemon: Starting Cocoa Event Loop\n");
  run_daemon_cocoa_app(on_status_toggle, on_status_quit);

  printf("Daemon: Exiting\n");
  cleanup_and_exit();

  return 0;
}
