add_library(rkmon_app STATIC application.cpp)
add_library(rkmon::app ALIAS rkmon_app)
target_include_directories(rkmon_app PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(rkmon_app PUBLIC cxx_std_20)
target_compile_options(rkmon_app PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rkmon_app PUBLIC rkmon::core)

add_executable(rkmon main.cpp)
target_link_libraries(rkmon PRIVATE rkmon::app)
