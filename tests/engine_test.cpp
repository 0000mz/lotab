#include "engine.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

// Mock Adapter capturing calls
struct MockAdapter {
  std::vector<std::string> logs;
  std::vector<std::string> uds_messages;
  int gui_kill_count = 0;
  int app_quit_count = 0;

  static void log(const char *msg) {
    if (GlobalMock)
      GlobalMock->logs.push_back(msg);
  }

  static void send_uds(const char *data) {
    if (GlobalMock)
      GlobalMock->uds_messages.push_back(data);
  }

  static void kill_gui() {
    if (GlobalMock)
      GlobalMock->gui_kill_count++;
  }

  static void quit_app() {
    if (GlobalMock)
      GlobalMock->app_quit_count++;
  }

  static MockAdapter *GlobalMock;
};

MockAdapter *MockAdapter::GlobalMock = nullptr;

class EngineTest : public ::testing::Test {
protected:
  MockAdapter mock;
  PlatformAdapter adapter;

  void SetUp() override {
    MockAdapter::GlobalMock = &mock;

    adapter.log = MockAdapter::log;
    adapter.send_uds = MockAdapter::send_uds;
    adapter.spawn_gui = nullptr; // Not used yet
    adapter.kill_gui = MockAdapter::kill_gui;
    adapter.quit_app = MockAdapter::quit_app;

    engine_init(&adapter);
  }

  void TearDown() override { MockAdapter::GlobalMock = nullptr; }
};

TEST_F(EngineTest, AppStartedLog) {
  engine_handle_event(EVENT_APP_STARTED, nullptr);
  ASSERT_FALSE(mock.logs.empty());
  EXPECT_EQ(mock.logs.back(), "Engine: App Started");
}

TEST_F(EngineTest, HotkeyToggleSendsUDS) {
  engine_handle_event(EVENT_HOTKEY_TOGGLE, nullptr);
  ASSERT_FALSE(mock.uds_messages.empty());
  EXPECT_NE(mock.uds_messages.back().find("ui_visibility_toggle"),
            std::string::npos);
}

TEST_F(EngineTest, MenuQuitTerminatesApp) {
  engine_handle_event(EVENT_MENU_QUIT, nullptr);
  EXPECT_EQ(mock.gui_kill_count, 1);
  EXPECT_EQ(mock.app_quit_count, 1);
}

TEST_F(EngineTest, WSMessageForwarded) {
  const char *msg = "test_message";
  engine_handle_event(EVENT_WS_MESSAGE_RECEIVED, (void *)msg);
  ASSERT_FALSE(mock.uds_messages.empty());
  EXPECT_EQ(mock.uds_messages.back(), "test_message");
}

TEST_F(EngineTest, ParseTabActivated) {
  const char *json = R"json({"event":"tabs.onActivated","data":{}})json";
  engine_handle_event(EVENT_WS_MESSAGE_RECEIVED, (void *)json);
  ASSERT_FALSE(mock.logs.empty());
  // Should log "WS Message Received" AND "Tab Activated"
  bool found_activation = false;
  for (const auto &log : mock.logs) {
    if (log == "Engine: Tab Activated")
      found_activation = true;
  }
  EXPECT_TRUE(found_activation);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
