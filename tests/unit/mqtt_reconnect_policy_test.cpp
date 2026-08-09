#include "industrial_iot_gateway/mqtt/reconnect_policy.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using industrial_iot_gateway::mqtt::JitterSample;
using industrial_iot_gateway::mqtt::ReconnectPolicy;
using namespace std::chrono_literals;

TEST(MqttReconnectPolicyTest, UsesFrozenNominalBackoffSequence) {
  ReconnectPolicy policy;
  EXPECT_EQ(policy.nominal_delay(0U), 1s);
  EXPECT_EQ(policy.nominal_delay(1U), 2s);
  EXPECT_EQ(policy.nominal_delay(2U), 4s);
  EXPECT_EQ(policy.nominal_delay(3U), 8s);
  EXPECT_EQ(policy.nominal_delay(4U), 16s);
  EXPECT_EQ(policy.nominal_delay(5U), 30s);
  EXPECT_EQ(policy.nominal_delay(20U), 30s);
}

TEST(MqttReconnectPolicyTest, AppliesBoundedDeterministicJitter) {
  ReconnectPolicy policy;
  EXPECT_EQ(policy.jittered_delay(5U, JitterSample{-1.0}), 24s);
  EXPECT_EQ(policy.jittered_delay(5U, JitterSample{0.0}), 30s);
  EXPECT_EQ(policy.jittered_delay(5U, JitterSample{1.0}), 36s);
}

} // namespace
