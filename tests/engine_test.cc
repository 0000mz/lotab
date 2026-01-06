#include "engine.h"

#include <cJSON.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <mutex>
#include <string>
#include <thread>

#include "test_util.h"

extern "C" {
#include <libwebsockets.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
}

class EngineTest : public ::testing::Test {
 protected:
  EngineContext* ectx_ = nullptr;
  std::thread engine_thread_;
  uint32_t port_;

  static std::mutex port_mtx_;
  static int next_port_;

  static uint32_t NextPort() {
    port_mtx_.lock();
    int p = next_port_++;
    port_mtx_.unlock();
    return p;
  }

  uint32_t GetPort() {
    return port_;
  }

  std::string config_uds_path_;

  void SetUp() override {
    engine_set_log_level(LOG_LEVEL_TRACE);

    port_ = NextPort();
    EngineCreationInfo create_info = {
        .port = port_,
        .enable_statusbar = 0,
        .app_path = nullptr,
        .uds_path = nullptr,
        .config_path = nullptr,
    };
    if (!config_uds_path_.empty()) {
      create_info.uds_path = config_uds_path_.c_str();
    }

    int ret = engine_init(&ectx_, create_info);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(ectx_, nullptr);
    engine_thread_ = std::thread(engine_run, ectx_);
    sleep(2);
    // If we are using a custom UDS path (mock server), we need to ensure the daemon connected.
    // The daemon tries to connect in setup_uds_client.
  }

  void TearDown() override {
    printf("EngineTest::TearDown\n");
    if (ectx_) {
      printf("engine::destroy\n");
      engine_destroy(ectx_);
      ectx_ = nullptr;
    }
    if (engine_thread_.joinable()) {
      engine_thread_.join();
    }
  }

  void FindActiveTab(EngineContext* ectx, TabInfo** out_tab) {
    ASSERT_NE(ectx, nullptr);
    ASSERT_NE(out_tab, nullptr);
    ASSERT_NE(ectx->tab_state, nullptr);

    *out_tab = nullptr;
    TabInfo* current = ectx->tab_state->tabs;
    while (current) {
      if (current->active) {
        *out_tab = current;
        return;
      }
      current = current->next;
    }
  }
};

class WebsockedAndUdsStreamTest : public EngineTest {
 protected:
  std::string socket_path_;
  std::unique_ptr<TestUDSServer> server_;

  void SetUp() override {
    const testing::TestInfo* const test_info = testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(test_info, nullptr);
    socket_path_ = std::string("/tmp/sockstream_") + test_info->test_suite_name() + ".sock";
    config_uds_path_ = socket_path_;
    server_ = std::unique_ptr<TestUDSServer>(new TestUDSServer(socket_path_));
    EngineTest::SetUp();
  }

  void TearDown() override {
    EngineTest::TearDown();
  }
};

TEST_F(WebsockedAndUdsStreamTest, ConnectsToCustomUDS_And_WS) {
  ASSERT_TRUE(server_->Accept(2)) << "Daemon did not connect to custom UDS path";

  TestWebSocketClient ws_client;
  ws_client.Connect(GetPort());
  ASSERT_TRUE(ws_client.IsConnected()) << "Failed to connect WebSocket";
  ASSERT_TRUE(ws_client.WaitForEvent("Daemon::WS::AllTabsInfoRequest", 2000));

  // 3. Test UDS -> Daemon -> WS Flow
  // Simulate App (via UDS) sending a 'tab_selected' event
  /*
     Event JSON from engine.c:
     { "event": "tab_selected", "data": { "tabId": 999 } }

     Daemon should translate this to WS message:
     { "event": "activate_tab", "data": { "tabId": 999 } }
  */

  const char* uds_evt = R"pb({
                               "event": "GUI::UDS::TabSelected",
                               "data": { "tabId": 999 }
                             })pb";
  ASSERT_TRUE(server_->Send(uds_evt));
  ASSERT_TRUE(ws_client.WaitForEvent("Daemon::WS::ActivateTabRequest", 2000))
      << "Daemon did not forward UDS tab_selected event to WS";

  // 4. Test WS -> Daemon -> UDS Flow
  // Simulate Extension (via WS) sending 'tabs.onCreated'
  // Daemon should forward this to UDS as 'tabs_update' (as per engine_handle_event)
  const char* ws_evt = R"pb({
                              "event": "Extension::WS::TabCreated",
                              "data": { "id": 888, "title": "New Tab via WS", "active": true }
                            })pb";
  ws_client.Send(ws_evt);

  // Verify UDS Server receives 'Daemon::UDS::TabsUpdate'
  // engine.c: send_tabs_update_to_uds sends {"event":"Daemon::UDS::TabsUpdate", ...}
  std::string received_msg;
  ASSERT_TRUE(server_->WaitForEvent("Daemon::UDS::TabsUpdate", 2000, &received_msg))
      << "Daemon did not send tabs_update to UDS";
  EXPECT_NE(received_msg.find("New Tab via WS"), std::string::npos);
}

int EngineTest::next_port_ = 9002;
std::mutex EngineTest::port_mtx_;

TEST_F(EngineTest, EngineInit) {
  ASSERT_TRUE(ectx_ != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  printf("client connected: %s\n", client.IsConnected() ? "true" : "false");
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("Daemon::WS::AllTabsInfoRequest", 2000))
      << "Did not receive Daemon::WS::AllTabsInfoRequest message from daemon";
  printf("[test] Received Daemon::WS::AllTabsInfoRequest request\n");

  // Simulate extension response
  const char* mock_response = R"pb({
                                     "event": "Extension::WS::AllTabsInfoResponse",
                                     "data": {
                                       "tabs":
                                       [ { "id": 101, "title": "Mock Tab 1", "url": "http://example.com" }
                                         , { "id": 102, "title": "Mock Tab 2", "url": "http://google.com" }],
                                       "groups": []
                                     },
                                     "activeTabIds": [ 101 ]
                                   })pb";
  client.Send(mock_response);
  printf("[test] Sent mock response\n");
  sleep(1);

  ASSERT_NE(ectx_->tab_state, nullptr);
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 2);

  TabInfo* active_tab = nullptr;
  FindActiveTab(ectx_, &active_tab);
  ASSERT_NE(active_tab, nullptr);
  EXPECT_EQ(active_tab->id, 101ul);
}

TEST_F(EngineTest, TabRemoved) {
  ASSERT_TRUE(ectx_ != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("Daemon::WS::AllTabsInfoRequest", 2000));

  // 1. Send Initial State
  const char* init_response = R"pb({
                                     "event": "Extension::WS::AllTabsInfoResponse",
                                     "data": {
                                       "tabs":
                                       [ { "id": 201, "title": "Tab To Remove", "url": "http://example.com/1" }
                                         , { "id": 202, "title": "Tab To Keep", "url": "http://example.com/2" }],
                                       "groups": []
                                     },
                                     "activeTabIds": [ 201 ]
                                   })pb";
  client.Send(init_response);
  sleep(1);

  ASSERT_NE(ectx_->tab_state, nullptr);
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 2);

  // 2. Send Removal Event
  const char* remove_event = R"pb({
                                    "event": "Extension::WS::TabRemoved",
                                    "data": {
                                      "tabId": 201,
                                      "removeInfo": { "windowId": 1, "isWindowClosing": false }
                                    }
                                  })pb";
  client.Send(remove_event);
  sleep(1);

  // 3. Verify Removal
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 1);
  TabInfo* current = ectx_->tab_state->tabs;
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->id, 202ul);
}

TEST_F(EngineTest, TabCreated) {
  ASSERT_TRUE(ectx_ != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("Daemon::WS::AllTabsInfoRequest", 2000));

  // Send Created Event
  const char* create_event =
      R"pb({
             "event": "Extension::WS::TabCreated",
             "data": {
               "id": 301,
               "title": "New Created Tab",
               "url": "http://example.com/new",
               "active": true,
               "groupId": -1
             }
           })pb";
  client.Send(create_event);
  sleep(1);

  ASSERT_NE(ectx_->tab_state, nullptr);
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 1);
  TabInfo* tab = ectx_->tab_state->tabs;
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->id, 301ul);
  EXPECT_STREQ(tab->title, "New Created Tab");
}

TEST_F(EngineTest, TabUpdated) {
  ASSERT_TRUE(ectx_ != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("Daemon::WS::AllTabsInfoRequest", 2000));

  // 1. Send Initial State
  const char* init_response = R"pb({
                                     "event": "Extension::WS::AllTabsInfoResponse",
                                     "data":
                                     [ { "id": 401, "title": "Old Title", "url": "http://example.com" }],
                                     "activeTabIds": [ 401 ]
                                   })pb";
  client.Send(init_response);
  sleep(1);

  ASSERT_NE(ectx_->tab_state, nullptr);
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 1);
  TabInfo* tab = ectx_->tab_state->tabs;
  EXPECT_STREQ(tab->title, "Old Title");

  // 2. Send Update Event
  const char* update_event =
      R"pb({
             "event": "Extension::WS::TabUpdated",
             "data": {
               "tabId": 401,
               "changeInfo": { "title": "New Title" },
               "tab": { "id": 401, "title": "New Title", "url": "http://example.com", "active": true }
             }
           })pb";
  client.Send(update_event);
  sleep(1);

  // 3. Verify Update
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 1);
  tab = ectx_->tab_state->tabs;
  EXPECT_STREQ(tab->title, "New Title");
}

TEST_F(EngineTest, TabGroupSync) {
  ASSERT_TRUE(ectx_ != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("Daemon::WS::AllTabsInfoRequest", 2000));

  // Simulate extension response with groups
  const char* mock_response = R"pb({
                                     "event": "Extension::WS::AllTabsInfoResponse",
                                     "data": {
                                       "tabs":
                                       [ { "id": 501, "title": "Grouped Tab", "url": "http://a.com", "groupId": 10 }],
                                       "groups":
                                       [ { "id": 10, "title": "Work Group", "color": "blue", "collapsed": false }]
                                     },
                                     "activeTabIds": [ 501 ]
                                   })pb";
  client.Send(mock_response);
  sleep(1);

  // 1. Verify Task was created for the group
  ASSERT_NE(ectx_->task_state, nullptr);
  TaskInfo* task = task_state_find_by_external_id(ectx_->task_state, 10);
  ASSERT_NE(task, nullptr);
  EXPECT_STREQ(task->task_name, "Work Group");
  EXPECT_EQ(task->external_id, 10);

  // 2. Verify Tab is associated with the correct task_id
  ASSERT_NE(ectx_->tab_state, nullptr);
  TabInfo* tab = tab_state_find_tab(ectx_->tab_state, 501);
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->task_ext_id, (int64_t)task->external_id);
}

TEST_F(EngineTest, ConfigCreated) {
  char tmp_dir[] = "/tmp/lotab_test_XXXXXX";
  ASSERT_NE(mkdtemp(tmp_dir), nullptr);

  EngineCreationInfo cinfo = {
      .port = NextPort(),
      .enable_statusbar = 0,
      .config_path = tmp_dir,
  };

  EngineContext* ec = nullptr;
  ASSERT_EQ(engine_init(&ec, cinfo), 0);

  std::string config_path = std::string(tmp_dir) + "/config.toml";
  struct stat st;
  EXPECT_EQ(stat(config_path.c_str(), &st), 0) << "Config file was not created at " << config_path;

  engine_destroy(ec);

  // Cleanup
  std::string cmd = std::string("rm -rf ") + tmp_dir;
  system(cmd.c_str());
}

TEST_F(EngineTest, ConfigKeybindParsed) {
  char tmp_dir[] = "/tmp/lotab_test_keybind_XXXXXX";
  ASSERT_NE(mkdtemp(tmp_dir), nullptr);

  // Pre-create config with custom keybind
  std::string config_path = std::string(tmp_dir) + "/config.toml";
  FILE* fp = fopen(config_path.c_str(), "w");
  ASSERT_NE(fp, nullptr);
  fprintf(fp, "UiToggleKeybind = \"CMD+SHIFT+K\"\n");
  fclose(fp);

  EngineCreationInfo cinfo = {
      .port = NextPort(),
      .enable_statusbar = 0,
      .app_path = nullptr,
      .uds_path = nullptr,
      .config_path = tmp_dir,
  };

  EngineContext* ec = nullptr;
  ASSERT_EQ(engine_init(&ec, cinfo), 0);

  ASSERT_NE(ec->ui_toggle_keybind, nullptr);
  EXPECT_STREQ(ec->ui_toggle_keybind, "CMD+SHIFT+K");

  engine_destroy(ec);

  // Cleanup
  std::string cmd = std::string("rm -rf ") + tmp_dir;
  system(cmd.c_str());
}

TEST_F(EngineTest, ConfigKeybindInvalid) {
  char tmp_dir[] = "/tmp/lotab_test_keybind_invalid_XXXXXX";
  ASSERT_NE(mkdtemp(tmp_dir), nullptr);

  // Pre-create config with invalid keybind (missing modifiers)
  std::string config_path = std::string(tmp_dir) + "/config.toml";
  FILE* fp = fopen(config_path.c_str(), "w");
  ASSERT_NE(fp, nullptr);
  fprintf(fp, "UiToggleKeybind = \"CTRL+K\"\n");
  fclose(fp);

  EngineCreationInfo cinfo = {
      .port = NextPort(),
      .enable_statusbar = 0,
      .app_path = nullptr,
      .uds_path = nullptr,
      .config_path = tmp_dir,
  };

  EngineContext* ec = nullptr;
  // Should fail
  ASSERT_EQ(engine_init(&ec, cinfo), -1);

  // Cleanup
  std::string cmd = std::string("rm -rf ") + tmp_dir;
  system(cmd.c_str());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
