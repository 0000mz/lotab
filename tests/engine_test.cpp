#include "engine.h"

#include <cJSON.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libwebsockets.h>
}

class TestWebSocketClient {
 public:
  TestWebSocketClient() {
  }

  ~TestWebSocketClient() {
    Disconnect();
  }

  void Connect(const uint32_t port) {
    if (running_)
      return;
    this->port_ = port;
    running_ = true;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;

    // We need to pass the client pointer to protocols or context
    // Here using context user data
    info.user = this;

    static struct lws_protocols protocols[] = {
        {.name = "minimal", .callback = Callback, .per_session_data_size = 0, .rx_buffer_size = 0},
        LWS_PROTOCOL_LIST_TERM};
    info.protocols = protocols;

    context_ = lws_create_context(&info);
    if (!context_)
      return;

    struct lws_client_connect_info i;
    memset(&i, 0, sizeof i);
    i.context = context_;
    i.address = "localhost";
    i.port = port;
    i.path = "/";
    i.host = i.address;
    i.origin = i.address;
    i.protocol = "minimal";
    i.userdata = this;

    ASSERT_NE(lws_client_connect_via_info(&i), nullptr) << "Error connecting to server.";
    service_thread_ = std::thread(&TestWebSocketClient::ServiceLoop, this);

    // Wait for connection AND writeable
    int retries = 50;
    while (retries-- > 0 && (!connected_ || !writeable_)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void Disconnect() {
    if (!running_)
      return;
    running_ = false;
    if (service_thread_.joinable()) {
      service_thread_.join();
    }
    connected_ = false;
  }

  void Send(const std::string& msg) {
    send_mutex_.lock();
    send_queue_.push(msg);
    send_mutex_.unlock();
    if (wsi_)
      lws_callback_on_writable(wsi_);
  }

  std::vector<std::string> GetReceivedMessages() {
    std::lock_guard<std::mutex> lock(recv_mutex_);
    std::vector<std::string> msgs = received_msgs_;
    received_msgs_.clear();
    return msgs;
  }

  bool WaitForEvent(const std::string& event_type, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
      {
        std::lock_guard<std::mutex> lock(recv_mutex_);
        for (auto it = received_msgs_.begin(); it != received_msgs_.end();) {
          bool match = false;
          cJSON* json = cJSON_Parse(it->c_str());
          if (json) {
            cJSON* event = cJSON_GetObjectItemCaseSensitive(json, "event");
            if (cJSON_IsString(event) && (event_type == event->valuestring)) {
              match = true;
            }
            cJSON_Delete(json);
          }

          if (match) {
            received_msgs_.erase(it);
            return true;
          } else {
            ++it;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      waited += 100;
    }
    return false;
  }

  bool IsConnected() const {
    return connected_;
  }

 private:
  struct lws_context* context_ = nullptr;
  struct lws* wsi_ = nullptr;
  std::thread service_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> writeable_{false};
  uint32_t port_ = 0;

  std::vector<std::string> received_msgs_;
  std::mutex recv_mutex_;

  std::queue<std::string> send_queue_;
  std::mutex send_mutex_;

  static int Callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
    TestWebSocketClient* client = (TestWebSocketClient*)user;
    // Check if user is set in wsi context if it's not passed directly (sometimes lws differs)
    if (!client && wsi) {
      void* cx_user = lws_context_user(lws_get_context(wsi));
      if (cx_user)
        client = (TestWebSocketClient*)cx_user;
    }

    switch (reason) {
      case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (client) {
          client->connected_ = true;
          client->wsi_ = wsi;
          lws_callback_on_writable(wsi);
        }
        break;

      case LWS_CALLBACK_CLIENT_RECEIVE:
        if (client && in && len > 0) {
          std::lock_guard<std::mutex> lock(client->recv_mutex_);
          client->received_msgs_.emplace_back(std::string((char*)in, len));
        }
        break;

      case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (client) {
          client->writeable_ = true;
          std::lock_guard<std::mutex> lock(client->send_mutex_);
          if (!client->send_queue_.empty()) {
            std::string msg = client->send_queue_.front();
            size_t len = msg.length();
            std::vector<unsigned char> buf(LWS_PRE + len + 1);
            memcpy(&buf[LWS_PRE], msg.c_str(), len);
            buf[LWS_PRE + len] = '\0';
            printf("[test] sending message to server: %s\n", buf.data());
            int n = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
            printf("TestClient: lws_write returned %d (expected %zu)\n", n, len);
            client->send_queue_.pop();
          }
          lws_callback_on_writable(wsi);
        }
        break;
      }

      case LWS_CALLBACK_CLIENT_CLOSED:
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (client) {
          client->connected_ = false;
          client->wsi_ = nullptr;
        }
        break;

      default:
        break;
    }
    return 0;
  }

  void ServiceLoop() {
    while (running_) {
      lws_service(context_, 50);
    }

    lws_context_destroy(context_);
    context_ = nullptr;
    wsi_ = nullptr;
  }
};

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

  void SetUp() override {
    engine_set_log_level(LOG_LEVEL_TRACE);

    port_ = NextPort();
    EngineCreationInfo create_info = {
        .port = port_,
        .enable_statusbar = 0,
    };
    int ret = engine_init(&ectx_, create_info);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(ectx_, nullptr);
    engine_thread_ = std::thread(engine_run, ectx_);
    sleep(2);
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

int EngineTest::next_port_ = 9002;
std::mutex EngineTest::port_mtx_;

TEST_F(EngineTest, EngineInit) {
  ASSERT_TRUE(ectx_ != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  printf("client connected: %s\n", client.IsConnected() ? "true" : "false");
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000)) << "Did not receive request_tab_info message from daemon";
  printf("[test] Received request_tab_info request\n");

  // Simulate extension response
  const char* mock_response = R"pb({
                                     "event": "tabs.onAllTabs",
                                     "data":
                                     [ { "id": 101, "title": "Mock Tab 1", "url": "http://example.com" }
                                       , { "id": 102, "title": "Mock Tab 2", "url": "http://google.com" }],
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
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000));

  // 1. Send Initial State
  const char* init_response = R"pb({
                                     "event": "tabs.onAllTabs",
                                     "data":
                                     [ { "id": 201, "title": "Tab To Remove", "url": "http://example.com/1" }
                                       , { "id": 202, "title": "Tab To Keep", "url": "http://example.com/2" }],
                                     "activeTabIds": [ 201 ]
                                   })pb";
  client.Send(init_response);
  sleep(1);

  ASSERT_NE(ectx_->tab_state, nullptr);
  EXPECT_EQ(ectx_->tab_state->nb_tabs, 2);

  // 2. Send Removal Event
  const char* remove_event = R"pb({
                                    "event": "tabs.onRemoved",
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
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000));

  // Send Created Event
  const char* create_event =
      R"pb({
             "event": "tabs.onCreated",
             "data": { "id": 301, "title": "New Created Tab", "url": "http://example.com/new", "active": true }
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
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000));

  // 1. Send Initial State
  const char* init_response = R"pb({
                                     "event": "tabs.onAllTabs",
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
             "event": "tabs.onUpdated",
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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
