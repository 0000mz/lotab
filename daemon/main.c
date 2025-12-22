#include <Carbon/Carbon.h>
#include <errno.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Explicitly declare these as they are sometimes missing from modern headers
// in certain build environments (e.g. when building as a pure C binary).
extern void RunApplicationEventLoop(void);
extern void QuitApplicationEventLoop(void);

static int interrupted;
static int uds_fd = -1;
static const char *uds_path = "/tmp/tabmanager.sock";

// --- Hotkey Logic ---

static OSStatus HotKeyHandler(EventHandlerCallRef nextHandler,
                              EventRef theEvent, void *userData) {
  (void)nextHandler;
  (void)userData;

  EventHotKeyID hkID;
  GetEventParameter(theEvent, kEventParamDirectObject, typeEventHotKeyID, NULL,
                    sizeof(hkID), NULL, &hkID);

  lwsl_user("Daemon: Hotkey event detected (ID: %d)\n", hkID.id);

  if (hkID.id == 1) {
    lwsl_user("Daemon: Global Hotkey Triggered!\n");
    if (uds_fd >= 0) {
      const char *msg =
          "{\"event\":\"hotkey_triggered\",\"data\":\"Cmd+Shift+J\"}";
      send(uds_fd, msg, strlen(msg), 0);
      lwsl_user("Daemon: Forwarded hotkey event to App via UDS\n");
    } else {
      lwsl_warn("Daemon: Hotkey triggered but UDS is not connected\n");
    }
  }

  return noErr;
}

static void *hotkey_thread_func(void *arg) {
  (void)arg;

  // Transform process to allow UI-less background app capabilities
  ProcessSerialNumber psn = {0, kCurrentProcess};
  TransformProcessType(&psn, kProcessTransformToUIElementApplication);

  EventHotKeyRef gMyHotKeyRef;
  EventHotKeyID gMyHotKeyID;
  EventTypeSpec eventType;

  eventType.eventClass = kEventClassKeyboard;
  eventType.eventKind = kEventHotKeyPressed;

  InstallApplicationEventHandler(NewEventHandlerUPP(HotKeyHandler), 1,
                                 &eventType, NULL, NULL);

  gMyHotKeyID.signature = 'htk1';
  gMyHotKeyID.id = 1;

  // Register Cmd+Shift+J
  // kVK_ANSI_J is 38
  OSStatus status =
      RegisterEventHotKey(38, cmdKey | shiftKey, gMyHotKeyID,
                          GetApplicationEventTarget(), 0, &gMyHotKeyRef);

  if (status != noErr) {
    lwsl_err("Daemon: Failed to register hotkey: %d\n", (int)status);
    return NULL;
  }

  lwsl_user("Daemon: Hotkey thread started (Cmd+Shift+J)\n");

  // Run the Carbon event loop
  RunApplicationEventLoop();

  lwsl_user("Daemon: Hotkey thread exiting\n");
  return NULL;
}

// --- WebSocket Callbacks ---

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
  QuitApplicationEventLoop();
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

  // Start Hotkey Thread
  pthread_t hotkey_thread;
  if (pthread_create(&hotkey_thread, NULL, hotkey_thread_func, NULL) != 0) {
    lwsl_err("Daemon: Failed to start hotkey thread\n");
  }

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

  if (uds_fd >= 0) {
    close(uds_fd);
  }
  lws_context_destroy(ctx);
  return 0;
}
