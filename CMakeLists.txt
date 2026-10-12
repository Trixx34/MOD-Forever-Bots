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

add_library(forever_bots_settings STATIC src/bot_settings.cpp)
target_include_directories(forever_bots_settings PUBLIC src)

add_library(forever_bots_state STATIC src/bot_state_file.cpp)
target_include_directories(forever_bots_state PUBLIC src)

add_library(forever_bots_policy STATIC src/bot_policy.cpp)
target_include_directories(forever_bots_policy PUBLIC src)
target_link_libraries(forever_bots_policy PUBLIC forever_module_sdk)

add_executable(forever-bots src/main.cpp)
target_link_libraries(forever-bots PRIVATE forever_bots_roster forever_bots_policy forever_bots_settings forever_bots_state forever_module_sdk Threads::Threads)

enable_testing()
add_executable(roster_test tests/roster_test.cpp)
target_link_libraries(roster_test PRIVATE forever_bots_roster)
add_test(NAME roster COMMAND roster_test)

add_executable(policy_test tests/policy_test.cpp)
target_link_libraries(policy_test PRIVATE forever_bots_policy)
add_test(NAME policy COMMAND policy_test)

add_executable(settings_test tests/settings_test.cpp)
target_link_libraries(settings_test PRIVATE forever_bots_settings)
add_test(NAME settings COMMAND settings_test)

add_executable(bot_state_file_test tests/bot_state_file_test.cpp)
target_link_libraries(bot_state_file_test PRIVATE forever_bots_state)
add_test(NAME bot_state_file COMMAND bot_state_file_test)
