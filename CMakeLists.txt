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

find_package(Threads REQUIRED)

add_library(forever_bots_roster STATIC src/bot_roster.cpp)
target_include_directories(forever_bots_roster PUBLIC src)

add_library(forever_bots_trade STATIC src/trade_policy.cpp)
target_include_directories(forever_bots_trade PUBLIC src)

add_library(forever_bots_settings STATIC src/bot_settings.cpp src/bot_ramp.cpp src/bot_pick.cpp src/dungeon_data.cpp src/bot_presence.cpp)
target_include_directories(forever_bots_settings PUBLIC src)

add_library(forever_bots_store STATIC src/bot_store.cpp)
target_include_directories(forever_bots_store PUBLIC src)

add_library(forever_bots_policy STATIC src/bot_policy.cpp src/chat_commands.cpp src/combat_rotation.cpp src/quest_data.cpp src/quest_planner.cpp src/scenario.cpp src/grind_data.cpp src/supply_policy.cpp src/chat_say.cpp)
target_include_directories(forever_bots_policy PUBLIC src)
target_link_libraries(forever_bots_policy PUBLIC forever_module_sdk)

add_library(forever_bots_files STATIC src/rotating_file.cpp)
target_include_directories(forever_bots_files PUBLIC src)

add_library(forever_bots_telemetry STATIC src/bot_telemetry.cpp)
target_include_directories(forever_bots_telemetry PUBLIC src)
target_link_libraries(forever_bots_telemetry PUBLIC forever_module_sdk)

add_library(forever_bots_live STATIC src/bot_live.cpp)
target_include_directories(forever_bots_live PUBLIC src)
target_link_libraries(forever_bots_live PUBLIC forever_module_sdk)

add_executable(forever-bots src/main.cpp)
target_link_libraries(forever-bots PRIVATE forever_bots_live forever_bots_trade forever_bots_roster forever_bots_policy forever_bots_settings forever_bots_store forever_bots_telemetry forever_bots_files forever_module_sdk Threads::Threads)

enable_testing()
add_executable(roster_test tests/roster_test.cpp)
target_link_libraries(roster_test PRIVATE forever_bots_roster)
add_test(NAME roster COMMAND roster_test)

add_executable(trade_test tests/trade_test.cpp)
target_link_libraries(trade_test PRIVATE forever_bots_trade)
add_test(NAME trade COMMAND trade_test)

add_executable(policy_test tests/policy_test.cpp)
target_link_libraries(policy_test PRIVATE forever_bots_policy)
add_test(NAME policy COMMAND policy_test)

add_executable(quest_test tests/quest_test.cpp)
target_link_libraries(quest_test PRIVATE forever_bots_policy)
add_test(NAME quest COMMAND quest_test)

add_executable(chat_test tests/chat_test.cpp)
target_link_libraries(chat_test PRIVATE forever_bots_policy)
add_test(NAME chat COMMAND chat_test)

add_executable(supply_test tests/supply_test.cpp)
target_link_libraries(supply_test PRIVATE forever_bots_policy)
add_test(NAME supply COMMAND supply_test)

add_executable(chat_say_test tests/chat_say_test.cpp)
target_link_libraries(chat_say_test PRIVATE forever_bots_policy)
add_test(NAME chat_say COMMAND chat_say_test)

add_executable(grind_test tests/grind_test.cpp)
target_link_libraries(grind_test PRIVATE forever_bots_policy)
add_test(NAME grind COMMAND grind_test)

add_executable(rotation_data_test tests/rotation_data_test.cpp)
target_link_libraries(rotation_data_test PRIVATE forever_bots_policy)
add_test(NAME rotation_data COMMAND rotation_data_test)

add_executable(scenario_test tests/scenario_test.cpp)
target_link_libraries(scenario_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME scenario COMMAND scenario_test)

add_executable(dungeon_test tests/dungeon_test.cpp)
target_link_libraries(dungeon_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME dungeon COMMAND dungeon_test)

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

add_executable(presence_test tests/presence_test.cpp)
target_link_libraries(presence_test PRIVATE forever_bots_settings)
add_test(NAME presence COMMAND presence_test)

add_executable(rotating_file_test tests/rotating_file_test.cpp)
target_link_libraries(rotating_file_test PRIVATE forever_bots_files)
add_test(NAME rotating_file COMMAND rotating_file_test)

add_executable(live_test tests/live_test.cpp)
target_link_libraries(live_test PRIVATE forever_bots_live)
add_test(NAME live COMMAND live_test)

# The pump at the 1500-bot ceiling: Decide stays bounded in time and in the actions it publishes.
add_executable(scale_test tests/scale_test.cpp)
target_link_libraries(scale_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME scale COMMAND scale_test)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME soak_report COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/soak_report_test.py)
    # The replay of a telemetry run, one section per open finding of the sim handoff.
    add_test(NAME snapshot_replay COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/snapshot_replay_test.py)
    # The spec of rotations made into a rotation file by tools/make_rotations.py.
    add_test(NAME make_rotations COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_rotations_test.py ${CMAKE_CURRENT_SOURCE_DIR})
endif()
