#ifndef TEST_TEST_UTIL_H_
#define TEST_TEST_UTIL_H_

#include <cJSON.h>

#include <gtest/gtest.h>
#include <stdio.h>
#include <string>
#include <thread>

extern "C" {
#include <libwebsockets.h>
}

class TestWebSocketClient {
 public:
  TestWebSocketClient() = default;

  ~TestWebSocketClient();

  void Connect(const uint32_t port);

  void Disconnect();
  void Send(const std::string& msg);
  std::vector<std::string> GetReceivedMessages();
  bool WaitForEvent(const std::string& event_type, int timeout_ms);
  bool IsConnected() const;

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

  static int Callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);

  void ServiceLoop();
};

class TestUDSServer {
 public:
  TestUDSServer(const std::string& path);
  ~TestUDSServer();

  bool Accept(int timeout_sec = 2);
  bool Send(const std::string& data);
  bool Receive(std::string& out_data);
  bool WaitForEvent(const std::string& event_type, int timeout_ms, std::string* out_msg = nullptr);

 private:
  std::string path_;
  int server_fd_;
  int client_fd_;
  std::vector<std::string> buffered_msgs_;
};

#endif  // TEST_TEST_UTIL_H_
