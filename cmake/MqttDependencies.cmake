include_guard(GLOBAL)

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11.3 REQUIRED)

find_path(
  GATEWAY_PAHO_MQTT_CPP_INCLUDE_DIR
  NAMES mqtt/async_client.h
  REQUIRED
)
find_library(
  GATEWAY_PAHO_MQTT_CPP_LIBRARY
  NAMES paho-mqttpp3
  REQUIRED
)
find_library(
  GATEWAY_PAHO_MQTT_C_ASYNC_LIBRARY
  NAMES paho-mqtt3a
  REQUIRED
)

add_library(gateway_paho_mqtt INTERFACE)
target_include_directories(
  gateway_paho_mqtt
  SYSTEM INTERFACE "${GATEWAY_PAHO_MQTT_CPP_INCLUDE_DIR}"
)
target_link_libraries(
  gateway_paho_mqtt
  INTERFACE
    "${GATEWAY_PAHO_MQTT_CPP_LIBRARY}"
    "${GATEWAY_PAHO_MQTT_C_ASYNC_LIBRARY}"
    Threads::Threads
)
