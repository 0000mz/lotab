#include "engine.h"

#include <gtest/gtest.h>
#include <stdio.h>
#include <mutex>
#include <thread>

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
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
