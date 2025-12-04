cmake_minimum_required(VERSION 3.16)
project(srtla_rec VERSION 1.0.0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/Modules")

include(FetchContent)

# Fetch and build spdlog statically
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/irlserver/spdlog.git
    GIT_TAG 1.9.2
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog as shared library")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples")
FetchContent_MakeAvailable(spdlog)

add_library(common_obj OBJECT
    src/common.c
    src/common.h)

add_executable(srtla_rec
    src/receiver_main.cpp
    src/connection/connection.cpp
    src/connection/connection_group.cpp
    src/connection/connection_registry.cpp
    src/quality/metrics_collector.cpp
    src/quality/quality_evaluator.cpp
    src/quality/load_balancer.cpp
    src/protocol/srtla_handler.cpp
    src/protocol/srt_handler.cpp
    src/utils/network_utils.cpp
    src/utils/nak_dedup.cpp)
 
target_include_directories(srtla_rec PRIVATE
    "deps/argparse/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(srtla_rec PRIVATE
    common_obj
    spdlog::spdlog
)
target_compile_features(srtla_rec PRIVATE cxx_std_17)
#target_compile_options(srtla_rec PRIVATE -Wall -Wextra)
target_compile_definitions(srtla_rec PUBLIC VERSION="${CMAKE_PROJECT_VERSION}")

add_executable(srtla_send
    src/sender.cpp
    src/sender.h)

target_include_directories(srtla_send PRIVATE
    "deps/argparse/include")
target_link_libraries(srtla_send PRIVATE
    common_obj
    spdlog::spdlog
)
target_compile_features(srtla_send PRIVATE cxx_std_17)
#target_compile_options(srtla_send PRIVATE -Wall -Wextra)
target_compile_definitions(srtla_send PUBLIC VERSION="${CMAKE_PROJECT_VERSION}")

set(CMAKE_BUILD_TYPE RelWithDebInfo)
install(TARGETS srtla_rec srtla_send RUNTIME DESTINATION bin)
