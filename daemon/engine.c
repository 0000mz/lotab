#include "engine.h"
#include <stddef.h>

static PlatformAdapter *g_adapter = NULL;

void engine_init(PlatformAdapter *adapter) {
  g_adapter = adapter;
  if (g_adapter && g_adapter->log) {
    g_adapter->log("Engine: Initialized");
  }
}

#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

// Helper to determine event type from JSON string
static TabEventType parse_event_type(const char *json_str) {
  cJSON *json = cJSON_Parse(json_str);
  if (!json) {
    return TAB_EVENT_UNKNOWN;
  }

  TabEventType type = TAB_EVENT_UNKNOWN;
  cJSON *event = cJSON_GetObjectItemCaseSensitive(json, "event");

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

void engine_handle_tab_event(TabEventType type, const char *json_data) {
  (void)json_data; // Unused for now
  if (!g_adapter || !g_adapter->log)
    return;

  switch (type) {
  case TAB_EVENT_ACTIVATED:
    g_adapter->log("Engine: Tab Activated");
    break;
  case TAB_EVENT_UPDATED:
    g_adapter->log("Engine: Tab Updated");
    break;
  case TAB_EVENT_HIGHLIGHTED:
    g_adapter->log("Engine: Tab Highlighted");
    break;
  case TAB_EVENT_ZOOM_CHANGE:
    g_adapter->log("Engine: Tab Zoom Change");
    break;
  case TAB_EVENT_UNKNOWN:
  default:
    g_adapter->log("Engine: Unknown Tab Event");
    break;
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
    if (data) {
      const char *json_msg = (const char *)data;
      TabEventType type = parse_event_type(json_msg);
      if (type != TAB_EVENT_UNKNOWN) {
        engine_handle_tab_event(type, json_msg);
      }

      // Forward to UDS as before
      if (g_adapter->send_uds) {
        g_adapter->send_uds((const char *)data);
      }
    }
    break;
  }
}
