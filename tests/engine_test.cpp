#include "engine.h"

#include <gtest/gtest.h>
#include <stdio.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class TestWebSocketClient {
 public:
  TestWebSocketClient() {
  }

  ~TestWebSocketClient() {
    Disconnect();
  }

  void Connect(uint32_t port) {
    if (running)
      return;
    this->port = port;
    running = true;

    // Connect synchronously for simplicity or use thread
    connected = false;

    // Retry loop for connection
    int retries = 50;
    while (retries-- > 0 && !connected) {
      if (TryConnect()) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (connected) {
      service_thread = std::thread(&TestWebSocketClient::ServiceLoop, this);
    }
  }

  void Disconnect() {
    if (!running)
      return;
    running = false;
    if (sockfd >= 0) {
      close(sockfd);
      sockfd = -1;
    }
    if (service_thread.joinable()) {
      service_thread.join();
    }
    connected = false;
  }

  void Send(const std::string& msg) {
    if (!connected || sockfd < 0)
      return;

    // Simple WS Frame Framing (Client -> Server must be masked)
    std::vector<uint8_t> frame;
    frame.push_back(0x81);  // Fin | Text

    size_t len = msg.length();
    if (len < 126) {
      frame.push_back(0x80 | (uint8_t)len);  // Masked | len
    } else {
      // Not supporting > 125 for this simple test client yet
      return;
    }

    // Simple Masking Key
    uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    frame.push_back(mask[0]);
    frame.push_back(mask[1]);
    frame.push_back(mask[2]);
    frame.push_back(mask[3]);

    for (size_t i = 0; i < len; i++) {
      frame.push_back(msg[i] ^ mask[i % 4]);
    }

    send(sockfd, frame.data(), frame.size(), 0);
  }

  std::vector<std::string> GetReceivedMessages() {
    std::lock_guard<std::mutex> lock(recv_mutex);
    std::vector<std::string> msgs = received_msgs;
    received_msgs.clear();
    return msgs;
  }

  bool IsConnected() const {
    return connected;
  }

 private:
  int sockfd = -1;
  uint32_t port = 0;
  std::thread service_thread;
  std::atomic<bool> running{false};
  std::atomic<bool> connected{false};

  std::vector<std::string> received_msgs;
  std::mutex recv_mutex;

  bool TryConnect() {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
      return false;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
      close(sockfd);
      sockfd = -1;
      return false;
    }

    // Handshake
    const char* handshake =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    send(sockfd, handshake, strlen(handshake), 0);

    char buffer[1024];
    // Wait for 101 Switching Protocols
    ssize_t n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
      close(sockfd);
      sockfd = -1;
      return false;
    }
    buffer[n] = 0;
    if (strstr(buffer, "101 Switching Protocols") == NULL) {
      close(sockfd);
      sockfd = -1;
      return false;
    }

    return true;
  }

  void ServiceLoop() {
    while (running && sockfd >= 0) {
      uint8_t head[2];
      ssize_t n = recv(sockfd, head, 2, 0);
      if (n <= 0)
        break;  // connection closed

      // Parse Frame (Simple implementation)
      // Assuming unmasked from server
      uint8_t len = head[1] & 0x7F;
      size_t payload_len = len;
      if (len == 126) {
        uint8_t ext[2];
        recv(sockfd, ext, 2, 0);
        payload_len = (ext[0] << 8) | ext[1];
      } else if (len == 127) {
        // Not supported
        break;
      }

      std::vector<char> buf(payload_len);
      size_t total_read = 0;
      while (total_read < payload_len) {
        n = recv(sockfd, buf.data() + total_read, payload_len - total_read, 0);
        if (n <= 0) {
          running = false;
          break;
        }
        total_read += n;
      }

      if (running) {
        std::lock_guard<std::mutex> lock(recv_mutex);
        received_msgs.emplace_back(std::string(buf.data(), payload_len));
      }
    }
    connected = false;
    if (sockfd >= 0)
      close(sockfd);
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
};
int EngineTest::next_port = 9002;
std::mutex EngineTest::port_mtx;

TEST_F(EngineTest, EngineInit) {
  ASSERT_TRUE(ectx != nullptr);

  TestWebSocketClient client;
  client.Connect(GetPort());
  ASSERT_TRUE(client.IsConnected());
  client.Disconnect();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
