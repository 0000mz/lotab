#include "../daemon/client.h"
#include <gtest/gtest.h>

// Mock Callback Data
struct MockData {
  int tabs_count;
  char* last_tab_title;
  int tasks_count;
  char* last_task_name;
  bool ui_toggled;
};

void on_tabs_update(void* user_data, const LotabTabList* tabs) {
  MockData* data = (MockData*)user_data;
  data->tabs_count = (int)tabs->count;
  if (tabs->count > 0 && tabs->tabs[0].title) {
    if (data->last_tab_title)
      free(data->last_tab_title);
    data->last_tab_title = strdup(tabs->tabs[0].title);
  }
}

void on_tasks_update(void* user_data, const LotabTaskList* tasks) {
  MockData* data = (MockData*)user_data;
  data->tasks_count = (int)tasks->count;
  if (tasks->count > 0 && tasks->tasks[0].name) {
    if (data->last_task_name)
      free(data->last_task_name);
    data->last_task_name = strdup(tasks->tasks[0].name);
  }
}

void on_ui_toggle(void* user_data) {
  MockData* data = (MockData*)user_data;
  data->ui_toggled = true;
}

class ClientTest : public ::testing::Test {
 protected:
  ClientContext* ctx;
  MockData data;

  void SetUp() override {
    ClientCallbacks cbs = {0};
    cbs.on_tabs_update = on_tabs_update;
    cbs.on_tasks_update = on_tasks_update;
    cbs.on_ui_toggle = on_ui_toggle;

    memset(&data, 0, sizeof(data));
    ctx = lotab_client_new("/tmp/test.sock", cbs, &data);
  }

  void TearDown() override {
    lotab_client_destroy(ctx);
    if (data.last_tab_title)
      free(data.last_tab_title);
    if (data.last_task_name)
      free(data.last_task_name);
  }
};

TEST_F(ClientTest, ParseTabsUpdate) {
  const char* json = R"json({
        "event": "Daemon::UDS::TabsUpdate",
                            "data": {
                              "tabs":
                              [ { "id": 1, "title": "Google", "active": true }]
                            }
                          })json";
  lotab_client_process_message(ctx, json);

  EXPECT_EQ(data.tabs_count, 1);
  EXPECT_STREQ(data.last_tab_title, "Google");
}

TEST_F(ClientTest, ParseTasksUpdate) {
  const char* json = R"json({
        "event": "Daemon::UDS::TasksUpdate",
                            "data": {
                              "tasks":
                              [ { "id": 101, "name": "Buy Milk" }]
                            }
                          })json";
  lotab_client_process_message(ctx, json);

  EXPECT_EQ(data.tasks_count, 1);
  EXPECT_STREQ(data.last_task_name, "Buy Milk");
}

TEST_F(ClientTest, ParseUIToggle) {
  const char* json = R"json({
        "event": "Daemon::UDS::ToggleGuiRequest"
    })json";
  lotab_client_process_message(ctx, json);

  EXPECT_TRUE(data.ui_toggled);
}

TEST_F(ClientTest, ParseInvalidJson) {
  const char* json = "{invalid}";
  lotab_client_process_message(ctx, json);
  // Should not crash
  EXPECT_EQ(data.tabs_count, 0);
}
