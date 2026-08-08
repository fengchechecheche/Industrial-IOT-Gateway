#include <gtest/gtest.h>

#include "industrial_iot_gateway/build_info.hpp"

TEST(BuildInfoTest, ExposesStableProjectIdentity) {
  EXPECT_STREQ(industrial_iot_gateway::project_name(), "industrial_iot_gateway");
  EXPECT_STREQ(industrial_iot_gateway::project_version(), "0.1.0-dev");
}
