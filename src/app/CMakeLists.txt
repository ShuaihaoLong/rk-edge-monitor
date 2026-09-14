add_library(rkmon_app STATIC application.cpp config.cpp)
add_library(rkmon::app ALIAS rkmon_app)
target_include_directories(rkmon_app PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(rkmon_app PUBLIC cxx_std_17)
target_compile_options(rkmon_app PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rkmon_app PUBLIC rkmon::core rkmon::camera rkmon::video rkmon::ai rkmon::mqtt)

add_executable(rkmon main.cpp)
target_link_libraries(rkmon PRIVATE rkmon::app)
