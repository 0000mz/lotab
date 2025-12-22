#include <libwebsockets.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static int interrupted;

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
    lwsl_user("LWS_CALLBACK_ESTABLISHED\n");
    break;
  case LWS_CALLBACK_RECEIVE:
    lwsl_user("LWS_CALLBACK_RECEIVE: %s\n", (const char *)in);
    lws_write(wsi, in, len, LWS_WRITE_TEXT);
    break;
  default:
    break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {{.name = "http",
                                            .callback = callback_http,
                                            .per_session_data_size = 0,
                                            .rx_buffer_size = 0},
                                           {.name = "minimal",
                                            .callback = callback_minimal,
                                            .per_session_data_size = 0,
                                            .rx_buffer_size = 0},
                                           LWS_PROTOCOL_LIST_TERM};

void sigint_handler(int sig) {
  (void)sig;
  interrupted = 1;
}

int main(void) {
  struct lws_context_creation_info info;
  struct lws_context *ctx;
  int n = 0;

  signal(SIGINT, sigint_handler);

  memset(&info, 0, sizeof info);
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

  lws_context_destroy(ctx);

  return 0;
}
