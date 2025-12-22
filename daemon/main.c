#ifdef SANITIZER_CONFIG_H
#include SANITIZER_CONFIG_H
#else
#include "config.h"
#endif
#include "statusbar.h"
#include <errno.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int interrupted;
static int uds_fd = -1;
static const char *uds_path = "/tmp/tabmanager.sock";
static struct lws_context *lws_ctx = NULL;

// Forward declaration

static pthread_t ws_thread;

// --- WebSocket Callbacks ---

static int callback_minimal(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
  (void)user;
  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED:
    lwsl_user("LWS_CALLBACK_ESTABLISHED (new connection)\n");
    break;
  case LWS_CALLBACK_CLOSED:
    lwsl_user("LWS_CALLBACK_CLOSED (connection lost)\n");
    break;
  case LWS_CALLBACK_RECEIVE:
    lwsl_user("LWS_CALLBACK_RECEIVE (%lu bytes): %s\n", (unsigned long)len,
              (const char *)in);

    if (uds_fd >= 0) {
      if (send(uds_fd, in, len, 0) < 0) {
        lwsl_err("Daemon: Failed to send data to App via UDS: %s\n",
                 strerror(errno));
      } else {
        lwsl_user("Daemon: Forwarded message to App via UDS\n");
      }
    }

    // Echo back for testing
    // lws_write(wsi, in, len, LWS_WRITE_TEXT);
    // Commented out echo to avoid confusion/loops, enable if needed for tests
    break;
  default:
    break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {{.name = "minimal",
                                            .callback = callback_minimal,
                                            .per_session_data_size = 0,
                                            .rx_buffer_size = 0},
                                           LWS_PROTOCOL_LIST_TERM};

static void cleanup_and_exit(void) {
  if (!interrupted) {
    interrupted = 1;
  }

  // Stop LWS loop
  if (lws_ctx) {
    lws_cancel_service(lws_ctx);
  }

  // Join LWS thread
  // Note: We check if we are NOT the ws_thread to avoid deadlock if called from
  // within it (unlikely here)
  if (ws_thread) {
    pthread_join(ws_thread, NULL);
    ws_thread = NULL;
  }

  if (uds_fd >= 0) {
    close(uds_fd);
    uds_fd = -1;
  }

  printf("Daemon: Cleanup complete.\n");
}

void sigint_handler(int sig) {
  (void)sig;
  printf("Daemon: Caught SIGINT\n");
  interrupted = 1;
  // We cannot easily join thread in signal handler, so we just set flag and let
  // lws close? Or we rely on main() falling through if possible. However, since
  // main() is blocked in [App run], SIGINT might need to kill the app or let
  // Cocoa handle it. Ideally Cocoa apps don't use SIGINT handler this way, but
  // for a daemon it's okay. We'll trust the OS to cleanup on immediate exit if
  // we can't join.
  if (lws_ctx) {
    lws_cancel_service(lws_ctx);
  }
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
      fprintf(stderr, "Daemon: Failed to create UDS socket: %s\n",
              strerror(errno));
      return;
    }

    if (connect(uds_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
      printf("Daemon: Connected to App UDS at %s\n", uds_path);
      // Send startup ping
      const char *ping = "{\"event\":\"daemon_startup\",\"data\":\"ping\"}";
      send(uds_fd, ping, strlen(ping), 0);
      return;
    }

    close(uds_fd);
    uds_fd = -1;
    fprintf(stderr, "Daemon: UDS connect failed, retrying in 1s... (%d left)\n",
            retries - 1);
    sleep(1);
    retries--;
  }
  fprintf(stderr,
          "Daemon: Failed to connect to App UDS after multiple attempts\n");
}

// Thread function to run the LWS service
static void *lws_thread_func(void *arg) {
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

  // Signal the Cocoa app to stop, in case we are exiting due to SIGINT or error
  // in LWS
  stop_daemon_cocoa_app();

  return NULL;
}

// Callback implementation
static void send_toggle_event(void) {
  if (uds_fd >= 0) {
    const char *msg =
        "{\"event\":\"ui_visibility_toggle\",\"data\":\"toggle\"}";
    send(uds_fd, msg, strlen(msg), 0);
    printf("Daemon: Sent toggle command to App\n");
  } else {
    printf("Daemon: Warning - Cannot Toggle, UDS not connected.\n");
  }
}

static void on_status_toggle(void) {
  printf("Daemon: Toggle Requested via Hotkey\n");
  send_toggle_event();
}

static void on_status_quit(void) {
  printf("Daemon: Quit Requested via Menu\n");
  cleanup_and_exit();
}

int main(void) {
  signal(SIGINT, sigint_handler);

#ifndef APP_PATH
  fprintf(stderr, "Error: APP_PATH is not defined. Daemon cannot continue.\n");
  return 1;
#endif

  printf("Daemon: Global App Path: %s\n", APP_PATH);

  // Spawn the TabManager App
  pid_t pid;
  char *argv[] = {(char *)APP_PATH, NULL};
  int spawn_status = posix_spawn(&pid, APP_PATH, NULL, NULL, argv, environ);
  if (spawn_status == 0) {
    printf("Daemon: Successfully spawned TabManager (PID: %d)\n", pid);
    sleep(1);
    init_uds_client();
  } else {
    fprintf(stderr, "Daemon: Failed to spawn TabManager: %s\n",
            strerror(spawn_status));
    init_uds_client();
  }

  // Start LWS in background thread
  if (pthread_create(&ws_thread, NULL, lws_thread_func, NULL) != 0) {
    fprintf(stderr, "Failed to start WebSocket thread\n");
    return 1;
  }

  // Run Cocoa App (Main Thread)
  // This blocks until termination
  printf("Daemon: Starting Cocoa Event Loop\n");
  run_daemon_cocoa_app(on_status_toggle, on_status_quit);

  printf("Daemon: Exiting (Normally shouldn't reach here if terminated via "
         "Menu)\n");

  // Just in case we fall through (e.g. invalid Quit logic in future)
  cleanup_and_exit();

  return 0;
}
