#include "engine.h"
#include <stddef.h>

static PlatformAdapter *g_adapter = NULL;

void engine_init(PlatformAdapter *adapter) {
  g_adapter = adapter;
  if (g_adapter && g_adapter->log) {
    g_adapter->log("Engine: Initialized");
  }
}

void engine_handle_event(DaemonEvent event, void *data) {
  if (!g_adapter)
    return;

  switch (event) {
  case EVENT_APP_STARTED:
    if (g_adapter->log)
      g_adapter->log("Engine: App Started");
    break;

  case EVENT_HOTKEY_TOGGLE:
    if (g_adapter->log)
      g_adapter->log("Engine: Toggle Requested");
    if (g_adapter->send_uds) {
      const char *msg =
          "{\"event\":\"ui_visibility_toggle\",\"data\":\"toggle\"}";
      g_adapter->send_uds(msg);
    }
    break;

  case EVENT_MENU_QUIT:
    if (g_adapter->log)
      g_adapter->log("Engine: Quit Requested");
    if (g_adapter->kill_gui)
      g_adapter->kill_gui();
    if (g_adapter->quit_app)
      g_adapter->quit_app();
    break;

  case EVENT_WS_MESSAGE_RECEIVED:
    if (g_adapter->log)
      g_adapter->log("Engine: WS Message Received");
    if (g_adapter->send_uds && data) {
      g_adapter->send_uds((const char *)data);
    }
    break;
  }
}
