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

add_library(forever_bots_roster STATIC src/bot_roster.cpp)
target_include_directories(forever_bots_roster PUBLIC src)

add_executable(forever-bots src/main.cpp)
target_link_libraries(forever-bots PRIVATE forever_bots_roster forever_module_sdk)

enable_testing()
add_executable(roster_test tests/roster_test.cpp)
target_link_libraries(roster_test PRIVATE forever_bots_roster)
add_test(NAME roster COMMAND roster_test)
