#include "pty_slave_support.hpp"

int main(const int argc, char **argv) {
  return industrial_iot_gateway::pty_slave::run_pty_slave_main(argc, argv);
}
