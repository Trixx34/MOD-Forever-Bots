cmake_minimum_required(VERSION 3.20)
project(forever_bots LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK can come from a local checkout (FOREVER_MODULE_SDK_SOURCE) or from GitHub.
set(FOREVER_MODULE_SDK_SOURCE "" CACHE PATH "Path to a forever-module-sdk checkout (optional)")
set(FOREVER_MODULE_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(FOREVER_MODULE_SDK_SOURCE)
    add_subdirectory("${FOREVER_MODULE_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/forever-module-sdk")
else()
    include(FetchContent)
    FetchContent_Declare(forever_module_sdk
        GIT_REPOSITORY https://github.com/Trixx34/forever-module-sdk
        GIT_TAG main)
    FetchContent_MakeAvailable(forever_module_sdk)
endif()

# The InfluxDB writer comes from MOD-Influx-logging (library target influx_logging). Use a local checkout with
# INFLUX_LOGGING_SOURCE, or leave it empty to fetch it from GitHub.
set(INFLUX_LOGGING_SOURCE "" CACHE PATH "Path to a MOD-Influx-logging checkout (optional)")
if(INFLUX_LOGGING_SOURCE)
    add_subdirectory("${INFLUX_LOGGING_SOURCE}" "${CMAKE_BINARY_DIR}/influx-logging")
else()
    include(FetchContent)
    FetchContent_Declare(influx_logging
        GIT_REPOSITORY https://github.com/Trixx34/MOD-Influx-logging
        GIT_TAG main)
    FetchContent_MakeAvailable(influx_logging)
endif()

find_package(Threads REQUIRED)

add_library(forever_bots_roster STATIC src/bot_roster.cpp)
target_include_directories(forever_bots_roster PUBLIC src)

add_library(forever_bots_settings STATIC src/bot_settings.cpp src/bot_ramp.cpp src/bot_pick.cpp)
target_include_directories(forever_bots_settings PUBLIC src)

add_library(forever_bots_store STATIC src/bot_store.cpp)
target_include_directories(forever_bots_store PUBLIC src)

add_library(forever_bots_policy STATIC src/bot_policy.cpp src/combat_rotation.cpp src/quest_data.cpp src/quest_planner.cpp src/scenario.cpp)
target_include_directories(forever_bots_policy PUBLIC src)
target_link_libraries(forever_bots_policy PUBLIC forever_module_sdk)

add_library(forever_bots_files STATIC src/rotating_file.cpp)
target_include_directories(forever_bots_files PUBLIC src)

add_library(forever_bots_telemetry STATIC src/bot_telemetry.cpp)
target_include_directories(forever_bots_telemetry PUBLIC src)
target_link_libraries(forever_bots_telemetry PUBLIC forever_module_sdk)

add_library(forever_bots_influx STATIC src/bot_influx.cpp)
target_include_directories(forever_bots_influx PUBLIC src)
target_link_libraries(forever_bots_influx PUBLIC forever_module_sdk influx_logging)

add_executable(forever-bots src/main.cpp)
target_link_libraries(forever-bots PRIVATE forever_bots_roster forever_bots_policy forever_bots_settings forever_bots_store forever_bots_telemetry forever_bots_influx forever_bots_files forever_module_sdk Threads::Threads)

enable_testing()
add_executable(roster_test tests/roster_test.cpp)
target_link_libraries(roster_test PRIVATE forever_bots_roster)
add_test(NAME roster COMMAND roster_test)

add_executable(policy_test tests/policy_test.cpp)
target_link_libraries(policy_test PRIVATE forever_bots_policy)
add_test(NAME policy COMMAND policy_test)

add_executable(quest_test tests/quest_test.cpp)
target_link_libraries(quest_test PRIVATE forever_bots_policy)
add_test(NAME quest COMMAND quest_test)

add_executable(scenario_test tests/scenario_test.cpp)
target_link_libraries(scenario_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME scenario COMMAND scenario_test)

add_executable(settings_test tests/settings_test.cpp)
target_link_libraries(settings_test PRIVATE forever_bots_settings)
add_test(NAME settings COMMAND settings_test)

add_executable(bot_store_test tests/bot_store_test.cpp)
target_link_libraries(bot_store_test PRIVATE forever_bots_store)
add_test(NAME bot_store COMMAND bot_store_test)

add_executable(rotation_test tests/rotation_test.cpp)
target_link_libraries(rotation_test PRIVATE forever_bots_policy)
add_test(NAME rotation COMMAND rotation_test)

add_executable(telemetry_test tests/telemetry_test.cpp)
target_link_libraries(telemetry_test PRIVATE forever_bots_telemetry)
add_test(NAME telemetry COMMAND telemetry_test)

add_executable(pick_test tests/pick_test.cpp)
target_link_libraries(pick_test PRIVATE forever_bots_settings)
add_test(NAME pick COMMAND pick_test)

add_executable(ramp_test tests/ramp_test.cpp)
target_link_libraries(ramp_test PRIVATE forever_bots_settings)
add_test(NAME ramp COMMAND ramp_test)

add_executable(rotating_file_test tests/rotating_file_test.cpp)
target_link_libraries(rotating_file_test PRIVATE forever_bots_files)
add_test(NAME rotating_file COMMAND rotating_file_test)

add_executable(influx_stats_test tests/influx_stats_test.cpp)
target_link_libraries(influx_stats_test PRIVATE forever_bots_influx forever_bots_settings forever_bots_telemetry)
add_test(NAME influx_stats COMMAND influx_stats_test)
