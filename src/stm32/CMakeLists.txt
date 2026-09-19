add_library(rkmon_stm32 STATIC protocol.cpp serial_port.cpp stm32_service.cpp)
add_library(rkmon::stm32 ALIAS rkmon_stm32)
target_include_directories(rkmon_stm32 PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(rkmon_stm32 PUBLIC cxx_std_17)
target_compile_options(rkmon_stm32 PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rkmon_stm32 PUBLIC rkmon::mqtt rkmon::core)
