#include "test_util.h"

#include <cJSON.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <string>
#include <vector>

extern "C" {
#include <libwebsockets.h>
#include <sys/socket.h>
#include <sys/un.h>
}

TestWebSocketClient::~TestWebSocketClient() {
  Disconnect();
}

void TestWebSocketClient::Connect(const uint32_t port) {
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

void TestWebSocketClient::Disconnect() {
  if (!running_)
    return;
  running_ = false;
  if (service_thread_.joinable()) {
    service_thread_.join();
  }
  connected_ = false;
}

void TestWebSocketClient::Send(const std::string& msg) {
  send_mutex_.lock();
  send_queue_.push(msg);
  send_mutex_.unlock();
  if (wsi_)
    lws_callback_on_writable(wsi_);
}

std::vector<std::string> TestWebSocketClient::GetReceivedMessages() {
  std::lock_guard<std::mutex> lock(recv_mutex_);
  std::vector<std::string> msgs = received_msgs_;
  received_msgs_.clear();
  return msgs;
}

bool TestWebSocketClient::WaitForEvent(const std::string& event_type, int timeout_ms) {
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

bool TestWebSocketClient::IsConnected() const {
  return connected_;
}

int TestWebSocketClient::Callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
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

void TestWebSocketClient::ServiceLoop() {
  while (running_) {
    lws_service(context_, 50);
  }

  lws_context_destroy(context_);
  context_ = nullptr;
  wsi_ = nullptr;
}

TestUDSServer::TestUDSServer(const std::string& path) : path_(path), server_fd_(-1), client_fd_(-1) {
  unlink(path.c_str());
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    perror("socket");
    return;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(server_fd_);
    server_fd_ = -1;
    return;
  }

  if (listen(server_fd_, 5) < 0) {
    perror("listen");
    close(server_fd_);
    server_fd_ = -1;
  }
}

TestUDSServer::~TestUDSServer() {
  if (client_fd_ >= 0)
    close(client_fd_);
  if (server_fd_ >= 0)
    close(server_fd_);
  unlink(path_.c_str());
}

bool TestUDSServer::Accept(const int timeout_sec) {
  if (server_fd_ < 0)
    return false;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(server_fd_, &fds);

  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;

  int ret = select(server_fd_ + 1, &fds, NULL, NULL, &tv);
  if (ret > 0) {
    client_fd_ = accept(server_fd_, NULL, NULL);
    return client_fd_ >= 0;
  }
  return false;
}

bool TestUDSServer::Send(const std::string& data) {
  if (client_fd_ < 0)
    return false;
  // Protocol: [Length: 4 bytes] [Payload]
  // Just send length + payload
  uint32_t len = data.size();
  if (write(client_fd_, &len, sizeof(len)) != sizeof(len))
    return false;
  if (write(client_fd_, data.data(), len) != len)
    return false;
  return true;
}

bool TestUDSServer::Receive(std::string& out_data) {
  if (client_fd_ < 0)
    return false;
  uint32_t len;
  if (read(client_fd_, &len, sizeof(len)) != sizeof(len))
    return false;
  std::vector<char> buf(len);
  if (read(client_fd_, buf.data(), len) != len)
    return false;
  out_data.assign(buf.begin(), buf.end());
  return true;
}

bool TestUDSServer::WaitForEvent(const std::string& event_type, int timeout_ms, std::string* out_msg) {
  int waited = 0;
  while (waited < timeout_ms) {
    // Check buffer first
    for (auto it = buffered_msgs_.begin(); it != buffered_msgs_.end();) {
      bool match = false;
      cJSON* json = cJSON_Parse(it->c_str());
      if (json) {
        cJSON* event = cJSON_GetObjectItemCaseSensitive(json, "event");
        if (cJSON_IsString(event) && (event_type == event->valuestring)) {
          match = true;
          if (out_msg) {
            *out_msg = *it;
          }
        }
        cJSON_Delete(json);
      }

      if (match) {
        buffered_msgs_.erase(it);
        return true;
      } else {
        ++it;
      }
    }

    // Attempt to read more
    if (client_fd_ >= 0) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(client_fd_, &fds);

      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 100000;  // 100ms

      int ret = select(client_fd_ + 1, &fds, NULL, NULL, &tv);
      if (ret > 0) {
        std::string msg;
        if (Receive(msg)) {
          buffered_msgs_.push_back(msg);
          continue;  // Re-check buffer immediately
        }
      } else {
        waited += 100;
      }
    } else {
      // Not connected yet? Wait a bit
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      waited += 100;
    }
  }
  return false;
}
