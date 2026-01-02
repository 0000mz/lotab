#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <algorithm>  // For std::sort
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "daemon/client.h"
#include "daemon/engine.h"

// Helper to make callbacks testable
struct TabData {
  int id;
  std::string title;
  int task_id;
  bool operator==(const TabData& other) const {
    return id == other.id && title == other.title && task_id == other.task_id;
  }
};

TabData MakeTab(int id, std::string title, int task_id = -1) {
  return {id, title, task_id};
}

struct CallbackData {
  // Tabs Update
  int tabs_count = -1;
  char* last_tab_title = nullptr;
  char* active_tab_title = nullptr;
  std::vector<TabData> tabs;

  // Tasks Update
  int tasks_count = -1;
  char* last_task_name = nullptr;

  // UI Toggle
  bool ui_toggled = false;
};

class TestableClientDriver {
 public:
  TestableClientDriver(std::string socket_path) : socket_path_(socket_path) {
    memset(&data_, 0, sizeof(data_));

    ClientCallbacks cbs{};
    cbs.on_tabs_update = [](void* user_data, const LotabTabList* tabs) {
      auto* driver = static_cast<TestableClientDriver*>(user_data);
      std::lock_guard<std::mutex> lock(driver->mutex_);
      driver->data_.tabs_count = (int)tabs->count;
      if (driver->data_.last_tab_title)
        free(driver->data_.last_tab_title);
      if (driver->data_.active_tab_title)
        free(driver->data_.active_tab_title);
      driver->data_.last_tab_title = (tabs->count > 0 && tabs->tabs[0].title) ? strdup(tabs->tabs[0].title) : nullptr;

      driver->data_.tabs.clear();
      for (size_t i = 0; i < tabs->count; ++i) {
        std::string title = tabs->tabs[i].title ? tabs->tabs[i].title : "";
        driver->data_.tabs.push_back({tabs->tabs[i].id, title, tabs->tabs[i].task_id});
      }

      driver->data_.active_tab_title = nullptr;
      for (size_t i = 0; i < tabs->count; ++i) {
        if (tabs->tabs[i].active && tabs->tabs[i].title) {
          driver->data_.active_tab_title = strdup(tabs->tabs[i].title);
          break;
        }
      }
      driver->cv_.notify_all();
    };

    cbs.on_tasks_update = [](void* user_data, const LotabTaskList* tasks) {
      auto* driver = static_cast<TestableClientDriver*>(user_data);
      std::lock_guard<std::mutex> lock(driver->mutex_);
      driver->data_.tasks_count = (int)tasks->count;
      if (driver->data_.last_task_name)
        free(driver->data_.last_task_name);
      driver->data_.last_task_name =
          (tasks->count > 0 && tasks->tasks[0].name) ? strdup(tasks->tasks[0].name) : nullptr;
      driver->cv_.notify_all();
    };

    cbs.on_ui_toggle = [](void* user_data) {
      auto* driver = static_cast<TestableClientDriver*>(user_data);
      std::lock_guard<std::mutex> lock(driver->mutex_);
      driver->data_.ui_toggled = true;
      driver->cv_.notify_all();
    };

    ctx_ = lotab_client_new(socket_path_.c_str(), cbs, this);
  }

  ~TestableClientDriver() {
    Stop();
    lotab_client_destroy(ctx_);
    if (data_.last_tab_title)
      free(data_.last_tab_title);
    if (data_.active_tab_title)
      free(data_.active_tab_title);
    if (data_.last_task_name)
      free(data_.last_task_name);
  }

  void Start() {
    thread_ = std::thread([this]() { lotab_client_run_loop(ctx_); });
    // Give time for socket to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  void Stop() {
    if (ctx_) {
      lotab_client_stop(ctx_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool WaitForTabsUpdate(int expected_count, std::string expected_title, int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
      bool title_match = true;
      if (expected_count > 0) {
        title_match = (data_.last_tab_title && expected_title == data_.last_tab_title);
      }
      return data_.tabs_count == expected_count && title_match;
    });
  }

  bool WaitForTabs(std::vector<TabData> expected_tabs, int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
      if (data_.tabs_count != (int)expected_tabs.size())
        return false;

      auto current = data_.tabs;
      auto expected = expected_tabs;

      auto sort_fn = [](const TabData& a, const TabData& b) { return a.id < b.id; };
      std::sort(current.begin(), current.end(), sort_fn);
      std::sort(expected.begin(), expected.end(), sort_fn);

      return current == expected;
    });
  }

  bool WaitForActiveTab(std::string expected_title, int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [&]() { return (data_.active_tab_title && expected_title == data_.active_tab_title); });
  }

  bool WaitForTasksUpdate(int expected_count, std::string expected_name, int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
      bool name_match = (data_.last_task_name && expected_name == data_.last_task_name);
      return data_.tasks_count == expected_count && name_match;
    });
  }

  bool WaitForUIToggle(int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() { return data_.ui_toggled; });
  }

  bool IsUIToggled() const {
    return data_.ui_toggled;
  }

 private:
  std::string socket_path_;
  ClientContext* ctx_;
  std::thread thread_;
  CallbackData data_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

class IntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Unique socket path based on test name
    const ::testing::TestInfo* const test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string test_name = test_info->name();
    socket_path_ = "/tmp/lotab_test_" + test_name + ".sock";

    // 1. Start Client
    client_driver_ = new TestableClientDriver(socket_path_);
    client_driver_->Start();

    // 2. Start Engine
    EngineCreationInfo info{};
    info.port = 0;  // Disable WS server port binding for test if possible, or use random
    info.enable_statusbar = 0;
    info.app_path = "/usr/bin/true";  // Dummy app
    info.uds_path = socket_path_.c_str();

    // Use a random high port to avoid conflicts
    info.port = 9990 + (::testing::UnitTest::GetInstance()->random_seed() % 100);

    ASSERT_EQ(engine_init(&ec_, info), 0);
  }

  void TearDown() override {
    if (ec_)
      engine_destroy(ec_);
    if (client_driver_)
      delete client_driver_;
  }

  std::string socket_path_;
  TestableClientDriver* client_driver_ = nullptr;
  EngineContext* ec_ = nullptr;
};

TEST_F(IntegrationTest, ExtensionsTabUpdatePropagatesToClient) {
  const char* ws_msg = R"json({
        "event": "Extension::WS::TabUpdated",
        "data": {
            "tab": {
                "id": 123,
                "title": "Test Tab"
            }
        }
    })json";

  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)ws_msg);
  ASSERT_TRUE(client_driver_->WaitForTabsUpdate(1, "Test Tab"));
}

TEST_F(IntegrationTest, HotkeyTogglePropagatesToClient) {
  ASSERT_FALSE(client_driver_->IsUIToggled());
  engine_handle_event(ec_, EVENT_HOTKEY_TOGGLE, nullptr);
  ASSERT_TRUE(client_driver_->WaitForUIToggle());
  EXPECT_TRUE(client_driver_->IsUIToggled());

  // 3. Client should ALSO receive Tabs and Tasks updates per engine logic
  // (engine sends tabs, tasks, then toggle)
  // We can verify tabs too if we want, assuming state is empty/default or what we set.
  // By default engine starts with 0 tabs.
  // ASSERT_TRUE(client_driver_->WaitForTabsUpdate(0, ""));
}

TEST_F(IntegrationTest, ExtensionsTabCreatedPropagatesToClient) {
  const char* ws_msg = R"json({
        "event": "Extension::WS::TabCreated",
        "data": {
            "id": 999,
            "title": "Created Tab"
        }
    })json";

  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)ws_msg);
  ASSERT_TRUE(client_driver_->WaitForTabs({MakeTab(999, "Created Tab")}));
}

TEST_F(IntegrationTest, ExtensionsAllTabsPropagatesToClient) {
  const char* ws_msg_full = R"json({
        "event": "Extension::WS::AllTabsInfoResponse",
        "data": {
            "tabs": [
                { "id": 1, "title": "Tab One", "active": false, "groupId": -1 },
                { "id": 2, "title": "Tab Two", "active": true, "groupId": -1 }
            ],
            "groups": []
        },
        "activeTabIds": [2]
    })json";
  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)ws_msg_full);
  ASSERT_TRUE(client_driver_->WaitForTabsUpdate(2, "Tab Two"));
  ASSERT_TRUE(client_driver_->WaitForTabs({MakeTab(1, "Tab One", -1), MakeTab(2, "Tab Two", -1)}));
}

TEST_F(IntegrationTest, TabGroupsPropagateToClient) {
  const char* ws_msg = R"json({
        "event": "Extension::WS::AllTabsInfoResponse",
        "data": {
            "tabs": [
                { "id": 100, "title": "Grouped Tab", "groupId": 77 }
            ],
            "groups": [
                { "id": 77, "title": "My Group" }
            ]
        }
    })json";

  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)ws_msg);

  // 1. Verify Task was created
  // Engine starts with 1 placeholder task, so now we should have 2.
  ASSERT_TRUE(client_driver_->WaitForTasksUpdate(2, "My Group"));

  // 2. Verify Tab has the correct task_id
  // The first task created will have task_id 1 (since 0 is the placeholder).
  // Wait, in engine.c: task_state_add uses ts->nb_tasks++
  // Engine starts with "Placeholder Task" at task_id 0.
  // So "My Group" will be task_id 1.
  ASSERT_TRUE(client_driver_->WaitForTabs({MakeTab(100, "Grouped Tab", 1)}));
}

TEST_F(IntegrationTest, ExtensionsTabActivatedPropagatesToClient) {
  // Setup: Create 2 tabs first
  const char* setup_msg = R"json({
        "event": "Extension::WS::AllTabsInfoResponse",
        "data": [
            { "id": 10, "title": "Tab Ten", "active": false },
            { "id": 20, "title": "Tab Twenty", "active": true }
        ],
        "activeTabIds": [20]
    })json";
  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)setup_msg);
  ASSERT_TRUE(client_driver_->WaitForTabsUpdate(2, "Tab Twenty"));

  // Now activate Tab 10
  const char* activate_msg = R"json({
        "event": "Extension::WS::TabActivated",
        "data": { "tabId": 10 },
        "activeTabIds": [10]
    })json";

  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)activate_msg);
  ASSERT_TRUE(client_driver_->WaitForActiveTab("Tab Ten"));
}

TEST_F(IntegrationTest, ExtensionsTabRemovedPropagatesToClient) {
  // Setup: Create 1 tab
  const char* setup_msg = R"json({
        "event": "Extension::WS::TabCreated",
        "data": { "id": 55, "title": "To Be Removed" }
    })json";
  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)setup_msg);
  ASSERT_TRUE(client_driver_->WaitForTabsUpdate(1, "To Be Removed"));

  // Remove it
  const char* remove_msg = R"json({
        "event": "Extension::WS::TabRemoved",
        "data": { "tabId": 55 }
    })json";
  engine_handle_event(ec_, EVENT_WS_MESSAGE_RECEIVED, (void*)remove_msg);

  // Should be empty list
  ASSERT_TRUE(client_driver_->WaitForTabs({}));
}
