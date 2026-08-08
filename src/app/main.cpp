#include <iostream>

#include "industrial_iot_gateway/build_info.hpp"

int main() {
  std::cout << industrial_iot_gateway::project_name() << ' '
            << industrial_iot_gateway::project_version() << '\n';
  return 0;
}
