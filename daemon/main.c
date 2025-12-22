#include <errno.h>
#include <libwebsockets.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int interrupted;
static int uds_fd = -1;
static const char *uds_path = "/tmp/tabmanager.sock";

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {
  (void)wsi;
  (void)reason;
  (void)user;
  (void)in;
  (void)len;
  return 0;
}

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
    lws_write(wsi, in, len, LWS_WRITE_TEXT);
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
                                           {.name = "http",
                                            .callback = callback_http,
                                            .per_session_data_size = 0,
                                            .rx_buffer_size = 0},
                                           LWS_PROTOCOL_LIST_TERM};

void sigint_handler(int sig) {
  (void)sig;
  interrupted = 1;
}

static void init_uds_client(void) {
  uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (uds_fd < 0) {
    lwsl_err("Daemon: Failed to create UDS socket: %s\n", strerror(errno));
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);

  if (connect(uds_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    lwsl_err("Daemon: Failed to connect to App UDS at %s: %s\n", uds_path,
             strerror(errno));
    close(uds_fd);
    uds_fd = -1;
    return;
  }

  lwsl_user("Daemon: Connected to App UDS at %s\n", uds_path);

  // Send startup ping
  const char *ping = "{\"event\":\"daemon_startup\",\"data\":\"ping\"}";
  send(uds_fd, ping, strlen(ping), 0);
}

int main(void) {
  struct lws_context_creation_info info;
  struct lws_context *ctx;
  int n = 0;

  signal(SIGINT, sigint_handler);

  // Initialize UDS Connection to the App
  init_uds_client();

  memset(&info, 0, sizeof info);
  // TODO: Put the websocket port in some configuratble location so that the
  // extension and the daemon are consistently using the same settings.
  info.port = 9001;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;

  lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
  lwsl_user("Starting Daemon WebSocket server on port 9001\n");

  ctx = lws_create_context(&info);
  if (!ctx) {
    lwsl_err("lws init failed\n");
    return 1;
  }

  while (n >= 0 && !interrupted) {
    n = lws_service(ctx, 0);
  }

  if (uds_fd >= 0) {
    close(uds_fd);
  }
  lws_context_destroy(ctx);
  return 0;
}
