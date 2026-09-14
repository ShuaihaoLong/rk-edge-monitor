add_library(rkmon_mqtt STATIC mqtt_client.cpp device_status_service.cpp)
add_library(rkmon::mqtt ALIAS rkmon_mqtt)
target_include_directories(rkmon_mqtt
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(rkmon_mqtt PUBLIC cxx_std_17)
target_compile_options(rkmon_mqtt PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rkmon_mqtt PUBLIC rkmon::core)
