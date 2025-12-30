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
    if (running)
      return;
    this->port = port;
    running = true;

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

    context = lws_create_context(&info);
    if (!context)
      return;

    struct lws_client_connect_info i;
    memset(&i, 0, sizeof i);
    i.context = context;
    i.address = "localhost";
    i.port = port;
    i.path = "/";
    i.host = i.address;
    i.origin = i.address;
    i.protocol = "minimal";
    i.userdata = this;

    ASSERT_NE(lws_client_connect_via_info(&i), nullptr) << "Error connecting to server.";
    service_thread = std::thread(&TestWebSocketClient::ServiceLoop, this);

    // Wait for connection AND writeable
    int retries = 50;
    while (retries-- > 0 && (!connected || !writeable)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void Disconnect() {
    if (!running)
      return;
    running = false;
    if (service_thread.joinable()) {
      service_thread.join();
    }
    connected = false;
  }

  void Send(const std::string& msg) {
    send_mutex.lock();
    send_queue.push(msg);
    send_mutex.unlock();
    if (wsi)
      lws_callback_on_writable(wsi);
  }

  std::vector<std::string> GetReceivedMessages() {
    std::lock_guard<std::mutex> lock(recv_mutex);
    std::vector<std::string> msgs = received_msgs;
    received_msgs.clear();
    return msgs;
  }

  bool WaitForEvent(const std::string& event_type, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
      {
        std::lock_guard<std::mutex> lock(recv_mutex);
        for (auto it = received_msgs.begin(); it != received_msgs.end();) {
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
            received_msgs.erase(it);
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
    return connected;
  }

 private:
  struct lws_context* context = nullptr;
  struct lws* wsi = nullptr;
  std::thread service_thread;
  std::atomic<bool> running{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> writeable{false};
  uint32_t port = 0;

  std::vector<std::string> received_msgs;
  std::mutex recv_mutex;

  std::queue<std::string> send_queue;
  std::mutex send_mutex;

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
          client->connected = true;
          client->wsi = wsi;
          lws_callback_on_writable(wsi);
        }
        break;

      case LWS_CALLBACK_CLIENT_RECEIVE:
        if (client && in && len > 0) {
          std::lock_guard<std::mutex> lock(client->recv_mutex);
          client->received_msgs.emplace_back(std::string((char*)in, len));
        }
        break;

      case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (client) {
          client->writeable = true;
          std::lock_guard<std::mutex> lock(client->send_mutex);
          if (!client->send_queue.empty()) {
            std::string msg = client->send_queue.front();
            size_t len = msg.length();
            std::vector<unsigned char> buf(LWS_PRE + len + 1);
            memcpy(&buf[LWS_PRE], msg.c_str(), len);
            buf[LWS_PRE + len] = '\0';
            printf("[test] sending message to server: %s\n", buf.data());
            int n = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
            printf("TestClient: lws_write returned %d (expected %zu)\n", n, len);
            client->send_queue.pop();
          }
          lws_callback_on_writable(wsi);
        }
        break;
      }

      case LWS_CALLBACK_CLIENT_CLOSED:
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (client) {
          client->connected = false;
          client->wsi = nullptr;
        }
        break;

      default:
        break;
    }
    return 0;
  }

  void ServiceLoop() {
    while (running) {
      lws_service(context, 50);
    }

    lws_context_destroy(context);
    context = nullptr;
    wsi = nullptr;
  }
};

class EngineTest : public ::testing::Test {
 protected:
  EngineContext* ectx = nullptr;
  std::thread engine_thread;
  uint32_t port;

  static std::mutex port_mtx;
  static int next_port;

  static uint32_t NextPort() {
    port_mtx.lock();
    int p = next_port++;
    port_mtx.unlock();
    return p;
  }

  uint32_t GetPort() {
    return port;
  }

  void SetUp() override {
    engine_set_log_level(LOG_LEVEL_TRACE);

    port = NextPort();
    EngineCreationInfo create_info = {
        .port = port,
        .enable_statusbar = 0,
    };
    int ret = engine_init(&ectx, create_info);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(ectx, nullptr);
    engine_thread = std::thread(engine_run, ectx);
    sleep(2);
  }

  void TearDown() override {
    printf("EngineTest::TearDown\n");
    if (ectx) {
      printf("engine::destroy\n");
      engine_destroy(ectx);
      ectx = nullptr;
    }
    if (engine_thread.joinable()) {
      engine_thread.join();
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

int EngineTest::next_port = 9002;
std::mutex EngineTest::port_mtx;

TEST_F(EngineTest, EngineInit) {
  ASSERT_TRUE(ectx != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  printf("client connected: %s\n", client.IsConnected() ? "true" : "false");
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000)) << "Did not receive request_tab_info message from daemon";
  printf("[test] Received request_tab_info request\n");

  // Simulate extension response
  const char* mock_response =
      "{"
      "  \"event\": \"tabs.onAllTabs\","
      "  \"data\": ["
      "    { \"id\": 101, \"title\": \"Mock Tab 1\", \"url\": \"http://example.com\" },"
      "    { \"id\": 102, \"title\": \"Mock Tab 2\", \"url\": \"http://google.com\" }"
      "  ],"
      "  \"activeTabIds\": [101]"
      "}";
  client.Send(mock_response);
  printf("[test] Sent mock response\n");
  sleep(1);

  ASSERT_NE(ectx->tab_state, nullptr);
  EXPECT_EQ(ectx->tab_state->nb_tabs, 2);

  TabInfo* active_tab = nullptr;
  FindActiveTab(ectx, &active_tab);
  ASSERT_NE(active_tab, nullptr);
  EXPECT_EQ(active_tab->id, 101ul);
}

TEST_F(EngineTest, TabRemoved) {
  ASSERT_TRUE(ectx != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000));

  // 1. Send Initial State
  const char* init_response =
      "{"
      "  \"event\": \"tabs.onAllTabs\","
      "  \"data\": ["
      "    { \"id\": 201, \"title\": \"Tab To Remove\", \"url\": \"http://example.com/1\" },"
      "    { \"id\": 202, \"title\": \"Tab To Keep\", \"url\": \"http://example.com/2\" }"
      "  ],"
      "  \"activeTabIds\": [201]"
      "}";
  client.Send(init_response);
  sleep(1);

  ASSERT_NE(ectx->tab_state, nullptr);
  EXPECT_EQ(ectx->tab_state->nb_tabs, 2);

  // 2. Send Removal Event
  const char* remove_event =
      "{"
      "  \"event\": \"tabs.onRemoved\","
      "  \"data\": {"
      "    \"tabId\": 201,"
      "    \"removeInfo\": { \"windowId\": 1, \"isWindowClosing\": false }"
      "  }"
      "}";
  client.Send(remove_event);
  sleep(1);

  // 3. Verify Removal
  EXPECT_EQ(ectx->tab_state->nb_tabs, 1);
  TabInfo* current = ectx->tab_state->tabs;
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->id, 202ul);
}

TEST_F(EngineTest, TabCreated) {
  ASSERT_TRUE(ectx != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000));

  // Send Created Event
  const char* create_event =
      "{"
      "  \"event\": \"tabs.onCreated\","
      "  \"data\": {"
      "    \"id\": 301,"
      "    \"title\": \"New Created Tab\","
      "    \"url\": \"http://example.com/new\","
      "    \"active\": true"
      "  }"
      "}";
  client.Send(create_event);
  sleep(1);

  ASSERT_NE(ectx->tab_state, nullptr);
  EXPECT_EQ(ectx->tab_state->nb_tabs, 1);
  TabInfo* tab = ectx->tab_state->tabs;
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->id, 301ul);
  EXPECT_STREQ(tab->title, "New Created Tab");
}

TEST_F(EngineTest, TabUpdated) {
  ASSERT_TRUE(ectx != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  ASSERT_TRUE(client.WaitForEvent("request_tab_info", 2000));

  // 1. Send Initial State
  const char* init_response =
      "{"
      "  \"event\": \"tabs.onAllTabs\","
      "  \"data\": ["
      "    { \"id\": 401, \"title\": \"Old Title\", \"url\": \"http://example.com\" }"
      "  ],"
      "  \"activeTabIds\": [401]"
      "}";
  client.Send(init_response);
  sleep(1);

  ASSERT_NE(ectx->tab_state, nullptr);
  EXPECT_EQ(ectx->tab_state->nb_tabs, 1);
  TabInfo* tab = ectx->tab_state->tabs;
  EXPECT_STREQ(tab->title, "Old Title");

  // 2. Send Update Event
  const char* update_event =
      "{"
      "  \"event\": \"tabs.onUpdated\","
      "  \"data\": {"
      "    \"tabId\": 401,"
      "    \"changeInfo\": { \"title\": \"New Title\" },"
      "    \"tab\": {"
      "      \"id\": 401,"
      "      \"title\": \"New Title\","
      "      \"url\": \"http://example.com\","
      "      \"active\": true"
      "    }"
      "  }"
      "}";
  client.Send(update_event);
  sleep(1);

  // 3. Verify Update
  EXPECT_EQ(ectx->tab_state->nb_tabs, 1);
  tab = ectx->tab_state->tabs;
  EXPECT_STREQ(tab->title, "New Title");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
