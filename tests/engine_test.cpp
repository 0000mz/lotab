#include "engine.h"

#include <gtest/gtest.h>

class EngineTest : public ::testing::Test {
 protected:
};

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
