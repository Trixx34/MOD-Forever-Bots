# SPDX-License-Identifier: GPL-2.0-or-later
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

add_library(forever_bots_settings STATIC src/bot_settings.cpp src/bot_ramp.cpp src/bot_pick.cpp src/dungeon_data.cpp src/mini_json.cpp src/dungeon_files.cpp src/bot_presence.cpp)
target_include_directories(forever_bots_settings PUBLIC src)
# the dungeon loader builds dungeon_events::Event values and reads the old events file (dungeon_events.cpp)
target_link_libraries(forever_bots_settings PUBLIC forever_bots_policy)

add_library(forever_bots_store STATIC src/bot_store.cpp)
target_include_directories(forever_bots_store PUBLIC src)

add_library(forever_bots_policy STATIC src/bot_policy.cpp src/bot_policy_result.cpp src/bot_policy_chat.cpp src/bot_policy_spells.cpp src/bot_policy_units.cpp src/bot_policy_turn_life.cpp src/bot_policy_telegraph.cpp src/bot_policy_turn_dead.cpp src/bot_policy_turn_target.cpp src/bot_policy_turn_move.cpp src/bot_policy_turn_town.cpp src/bot_policy_turn_cast.cpp src/chat_commands.cpp src/combat_rotation.cpp src/quest_data.cpp src/quest_planner.cpp src/scenario.cpp src/grind_data.cpp src/supply_policy.cpp src/auction_policy.cpp src/auction_buy.cpp src/auction_log.cpp src/taxi_policy.cpp src/dungeon_events.cpp src/chat_say.cpp src/errand_policy.cpp src/sell_policy.cpp src/errand_trainers.cpp src/spell_costs.cpp src/crafting_foci.cpp src/ground_avoid.cpp src/tame_beasts.cpp src/level_dungeon.cpp src/duels.cpp src/battlegrounds.cpp src/inn_downtime.cpp src/guild_join.cpp src/mail_gifts.cpp src/riding_training.cpp src/weapon_training.cpp src/fishing.cpp src/rare_hunts.cpp src/ambient_chat.cpp src/chat_reply.cpp src/lfg_fill.cpp src/bot_brackets.cpp src/gather_nodes.cpp src/bot_policy_features.cpp src/bot_policy_social.cpp src/bot_policy_guilds.cpp src/social_actions.cpp src/realism.cpp src/advisor_text.cpp src/advisor_send.cpp src/lfg_queue_fill.cpp src/death_pause.cpp src/trade_ads.cpp src/party_mana.cpp src/whisper_matrix.cpp src/path_jitter.cpp src/ah_batch.cpp src/guild_milestone.cpp src/city_inspect.cpp src/city_greet.cpp src/quest_invite.cpp src/bot_policy_advisor.cpp src/party_planner.cpp src/party_view_adapter.cpp src/gear_score.cpp src/quest_group.cpp src/raid_rules.cpp)
target_include_directories(forever_bots_policy PUBLIC src)
target_link_libraries(forever_bots_policy PUBLIC forever_module_sdk forever_bots_guilds)

add_library(forever_bots_files STATIC src/rotating_file.cpp)
target_include_directories(forever_bots_files PUBLIC src)

add_library(forever_bots_telemetry STATIC src/bot_telemetry.cpp)
target_include_directories(forever_bots_telemetry PUBLIC src)
target_link_libraries(forever_bots_telemetry PUBLIC forever_module_sdk)

add_library(forever_bots_live STATIC src/bot_live.cpp)
target_include_directories(forever_bots_live PUBLIC src)
target_link_libraries(forever_bots_live PUBLIC forever_module_sdk)

# The module version is written once, in module.json (pre-1.0 scheme 0.MINOR.PATCH, forever-ops docs/96-versioning.md); the build reads it into the manifest.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/module.json" FOREVER_MODULE_JSON)
string(JSON FOREVER_MODULE_VERSION GET "${FOREVER_MODULE_JSON}" version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module.json")

add_executable(forever-bots src/main.cpp src/main_pump.cpp src/main_util.cpp src/main_spawn.cpp src/main_sales.cpp src/main_commands.cpp src/main_options.cpp src/main_feeds.cpp)
target_compile_definitions(forever-bots PRIVATE FOREVER_MODULE_VERSION="${FOREVER_MODULE_VERSION}")
target_link_libraries(forever-bots PRIVATE forever_bots_live forever_bots_trade forever_bots_roster forever_bots_policy forever_bots_settings forever_bots_store forever_bots_telemetry forever_bots_files forever_module_sdk Threads::Threads)

enable_testing()
add_executable(roster_test tests/roster_test.cpp)
target_link_libraries(roster_test PRIVATE forever_bots_roster)
add_test(NAME roster COMMAND roster_test)

add_executable(trade_test tests/trade_test.cpp)
target_link_libraries(trade_test PRIVATE forever_bots_trade)
add_test(NAME trade COMMAND trade_test)

add_executable(policy_test tests/policy_test.cpp
    tests/policy_test_01.cpp tests/policy_test_02.cpp tests/policy_test_03.cpp tests/policy_test_04.cpp
    tests/policy_test_05.cpp tests/policy_test_06.cpp tests/policy_test_07.cpp tests/policy_test_08.cpp tests/policy_test_09.cpp)
target_link_libraries(policy_test PRIVATE forever_bots_policy)
add_test(NAME policy COMMAND policy_test)

add_executable(raid_test tests/raid_test.cpp)
target_link_libraries(raid_test PRIVATE forever_bots_policy)
add_test(NAME raid COMMAND raid_test)

add_executable(quest_test tests/quest_test.cpp)
target_link_libraries(quest_test PRIVATE forever_bots_policy)
add_test(NAME quest COMMAND quest_test)

add_executable(chat_test tests/chat_test.cpp)
target_link_libraries(chat_test PRIVATE forever_bots_policy)
add_test(NAME chat COMMAND chat_test)

add_executable(level_dungeon_test tests/level_dungeon_test.cpp)
target_link_libraries(level_dungeon_test PRIVATE forever_bots_policy)
add_test(NAME level_dungeon COMMAND level_dungeon_test)

add_executable(quest_group_test tests/quest_group_test.cpp)
target_link_libraries(quest_group_test PRIVATE forever_bots_policy)
add_test(NAME quest_group COMMAND quest_group_test)

add_executable(lfg_fill_test tests/lfg_fill_test.cpp)
target_link_libraries(lfg_fill_test PRIVATE forever_bots_policy)
add_test(NAME lfg_fill COMMAND lfg_fill_test)

add_executable(supply_test tests/supply_test.cpp)
target_link_libraries(supply_test PRIVATE forever_bots_policy)
add_test(NAME supply COMMAND supply_test)

add_executable(auction_test tests/auction_test.cpp)
target_link_libraries(auction_test PRIVATE forever_bots_policy)
add_test(NAME auction COMMAND auction_test)

add_executable(taxi_test tests/taxi_test.cpp)
target_link_libraries(taxi_test PRIVATE forever_bots_policy)
add_test(NAME taxi COMMAND taxi_test)

add_executable(duels_test tests/duels_test.cpp)
target_link_libraries(duels_test PRIVATE forever_bots_policy)
add_test(NAME duels COMMAND duels_test)

add_executable(battlegrounds_test tests/battlegrounds_test.cpp)
target_link_libraries(battlegrounds_test PRIVATE forever_bots_policy)
add_test(NAME battlegrounds COMMAND battlegrounds_test)

add_executable(inn_downtime_test tests/inn_downtime_test.cpp)
target_link_libraries(inn_downtime_test PRIVATE forever_bots_policy)
add_test(NAME inn_downtime COMMAND inn_downtime_test)

# Second-list features (G9a): riding, weapon skills, fishing spots, rare hunts. Pure logic; each test links the policy library.
add_executable(riding_training_test tests/riding_training_test.cpp)
target_link_libraries(riding_training_test PRIVATE forever_bots_policy)
add_test(NAME riding_training COMMAND riding_training_test)

add_executable(weapon_training_test tests/weapon_training_test.cpp)
target_link_libraries(weapon_training_test PRIVATE forever_bots_policy)
add_test(NAME weapon_training COMMAND weapon_training_test)

add_executable(fishing_test tests/fishing_test.cpp)
target_link_libraries(fishing_test PRIVATE forever_bots_policy)
add_test(NAME fishing COMMAND fishing_test)

add_executable(rare_hunts_test tests/rare_hunts_test.cpp)
target_link_libraries(rare_hunts_test PRIVATE forever_bots_policy)
add_test(NAME rare_hunts COMMAND rare_hunts_test)

add_executable(guild_join_test tests/guild_join_test.cpp)
target_link_libraries(guild_join_test PRIVATE forever_bots_policy)
add_test(NAME guild_join COMMAND guild_join_test)

add_library(forever_bots_guilds STATIC src/bot_guilds.cpp)
target_include_directories(forever_bots_guilds PUBLIC src)

add_executable(bot_guilds_test tests/bot_guilds_test.cpp)
target_link_libraries(bot_guilds_test PRIVATE forever_bots_guilds)
add_test(NAME bot_guilds COMMAND bot_guilds_test)

add_executable(main_util_test tests/main_util_test.cpp src/main_util.cpp)
target_link_libraries(main_util_test PRIVATE forever_bots_policy forever_bots_settings forever_bots_store forever_bots_files forever_bots_live forever_bots_telemetry forever_bots_trade forever_bots_roster)
target_compile_definitions(main_util_test PRIVATE FOREVER_MODULE_VERSION="test")
add_test(NAME main_util COMMAND main_util_test)

add_executable(gear_score_test tests/gear_score_test.cpp)
target_link_libraries(gear_score_test PRIVATE forever_bots_policy)
add_test(NAME gear_score COMMAND gear_score_test)

add_executable(mail_gifts_test tests/mail_gifts_test.cpp)
target_link_libraries(mail_gifts_test PRIVATE forever_bots_policy)
add_test(NAME mail_gifts COMMAND mail_gifts_test)

# social_test plays a fake core on a Unix socket (poll, read, sockaddr_un), which the Windows build has no header for; the Windows ModuleClient
# transport is covered by the SDK's own tests. Left out on Windows so the module and its other tests build there (Windows package #33, 2026-11-04).
if(NOT WIN32)
    add_executable(social_test tests/social_test.cpp)
    target_link_libraries(social_test PRIVATE forever_bots_policy)
    add_test(NAME social COMMAND social_test)
endif()

add_executable(errand_test tests/errand_test.cpp)
target_link_libraries(errand_test PRIVATE forever_bots_policy)
add_test(NAME errand COMMAND errand_test)

add_executable(crafting_foci_test tests/crafting_foci_test.cpp)
target_link_libraries(crafting_foci_test PRIVATE forever_bots_policy)
target_compile_definitions(crafting_foci_test PRIVATE FOREVER_CRAFTING_FOCI_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/crafting_foci.txt")
add_test(NAME crafting_foci COMMAND crafting_foci_test)

# Gathering nodes (G7): the node table and the nearest node of a skill. Pure logic.
add_executable(gather_nodes_test tests/gather_nodes_test.cpp)
target_link_libraries(gather_nodes_test PRIVATE forever_bots_policy)
add_test(NAME gather_nodes COMMAND gather_nodes_test)
add_executable(errand_trainers_test tests/errand_trainers_test.cpp)
target_link_libraries(errand_trainers_test PRIVATE forever_bots_policy)
target_compile_definitions(errand_trainers_test PRIVATE FOREVER_ERRAND_TRAINERS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/errand_trainers.txt")
add_test(NAME errand_trainers COMMAND errand_trainers_test)

add_executable(dungeon_events_test tests/dungeon_events_test.cpp)
target_link_libraries(dungeon_events_test PRIVATE forever_bots_policy)
add_test(NAME dungeon_events COMMAND dungeon_events_test)

add_executable(chat_say_test tests/chat_say_test.cpp)
target_link_libraries(chat_say_test PRIVATE forever_bots_policy)
add_test(NAME chat_say COMMAND chat_say_test)
add_executable(ambient_chat_test tests/ambient_chat_test.cpp)
target_link_libraries(ambient_chat_test PRIVATE forever_bots_policy)
add_test(NAME ambient_chat COMMAND ambient_chat_test)
target_compile_definitions(ambient_chat_test PRIVATE FOREVER_BOTS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_executable(realism_test tests/realism_test.cpp)
target_link_libraries(realism_test PRIVATE forever_bots_policy)
add_test(NAME realism COMMAND realism_test)
add_executable(chat_reply_test tests/chat_reply_test.cpp)
target_link_libraries(chat_reply_test PRIVATE forever_bots_policy)
add_test(NAME chat_reply COMMAND chat_reply_test)

add_executable(grind_test tests/grind_test.cpp)
target_link_libraries(grind_test PRIVATE forever_bots_policy)
add_test(NAME grind COMMAND grind_test)

add_executable(tame_test tests/tame_test.cpp)
target_link_libraries(tame_test PRIVATE forever_bots_policy)
add_test(NAME tame COMMAND tame_test)

add_executable(rotation_data_test tests/rotation_data_test.cpp)
target_link_libraries(rotation_data_test PRIVATE forever_bots_policy)
target_compile_definitions(rotation_data_test PRIVATE FOREVER_BOTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME rotation_data COMMAND rotation_data_test)

add_executable(spell_costs_test tests/spell_costs_test.cpp)
target_link_libraries(spell_costs_test PRIVATE forever_bots_policy)
add_test(NAME spell_costs COMMAND spell_costs_test)

add_executable(ground_avoid_test tests/ground_avoid_test.cpp)
target_link_libraries(ground_avoid_test PRIVATE forever_bots_policy)
target_compile_definitions(ground_avoid_test PRIVATE FOREVER_AVOID_SPELLS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/avoid_spells.txt")
add_test(NAME ground_avoid COMMAND ground_avoid_test)
add_executable(move_gate_test tests/move_gate_test.cpp)
target_link_libraries(move_gate_test PRIVATE forever_bots_policy)
add_test(NAME move_gate COMMAND move_gate_test)

add_executable(dungeon_clear_test tests/dungeon_clear_test.cpp)
target_link_libraries(dungeon_clear_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME dungeon_clear COMMAND dungeon_clear_test)

add_executable(bot_random_test tests/bot_random_test.cpp)
target_include_directories(bot_random_test PRIVATE src)
add_test(NAME bot_random COMMAND bot_random_test)

add_executable(text_helpers_test tests/text_helpers_test.cpp)
target_include_directories(text_helpers_test PRIVATE src)
add_test(NAME text_helpers COMMAND text_helpers_test)

add_executable(scenario_test tests/scenario_test.cpp
    tests/scenario_test_01.cpp tests/scenario_test_02.cpp tests/scenario_test_03.cpp tests/scenario_test_04.cpp)
target_link_libraries(scenario_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME scenario COMMAND scenario_test)

add_executable(dungeon_test tests/dungeon_test.cpp)
target_link_libraries(dungeon_test PRIVATE forever_bots_policy forever_bots_settings)
add_test(NAME dungeon COMMAND dungeon_test)

add_executable(dungeon_files_test tests/dungeon_files_test.cpp)
target_link_libraries(dungeon_files_test PRIVATE forever_bots_policy forever_bots_settings)
target_compile_definitions(dungeon_files_test PRIVATE FOREVER_BOTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME dungeon_files COMMAND dungeon_files_test)

add_executable(settings_test tests/settings_test.cpp)
target_link_libraries(settings_test PRIVATE forever_bots_settings)
add_test(NAME settings COMMAND settings_test)

add_executable(bot_store_test tests/bot_store_test.cpp)
target_link_libraries(bot_store_test PRIVATE forever_bots_store)
add_test(NAME bot_store COMMAND bot_store_test)

add_executable(raid_rules_test tests/raid_rules_test.cpp)
target_link_libraries(raid_rules_test PRIVATE forever_bots_policy)
target_compile_definitions(raid_rules_test PRIVATE FOREVER_RAID_RULES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/raid_rules.txt")
add_test(NAME raid_rules COMMAND raid_rules_test)
add_executable(party_planner_test tests/party_planner_test.cpp)
target_link_libraries(party_planner_test PRIVATE forever_bots_policy)
add_test(NAME party_planner COMMAND party_planner_test)

# Layout 25: the party view and the telegraphs read from a crafted snapshot through the SDK's view helpers and the planner adapter.
add_executable(party_view_test tests/party_view_test.cpp)
target_link_libraries(party_view_test PRIVATE forever_bots_policy)
add_test(NAME party_view COMMAND party_view_test)

add_executable(rotation_test tests/rotation_test.cpp)
target_link_libraries(rotation_test PRIVATE forever_bots_policy)
add_test(NAME rotation COMMAND rotation_test)

add_executable(telemetry_test tests/telemetry_test.cpp)
target_link_libraries(telemetry_test PRIVATE forever_bots_telemetry)
add_test(NAME telemetry COMMAND telemetry_test)

add_executable(route_triage_test tests/route_triage_test.cpp)
target_include_directories(route_triage_test PRIVATE src)
add_test(NAME route_triage COMMAND route_triage_test)

add_executable(party_cut_test tests/party_cut_test.cpp)
target_include_directories(party_cut_test PRIVATE src)
add_test(NAME party_cut COMMAND party_cut_test)

add_executable(pick_test tests/pick_test.cpp)
target_link_libraries(pick_test PRIVATE forever_bots_settings)
add_test(NAME pick COMMAND pick_test)

add_executable(ramp_test tests/ramp_test.cpp)
target_link_libraries(ramp_test PRIVATE forever_bots_settings)
add_test(NAME ramp COMMAND ramp_test)

add_executable(presence_test tests/presence_test.cpp)
target_link_libraries(presence_test PRIVATE forever_bots_settings)
add_test(NAME presence COMMAND presence_test)

add_executable(bracket_test tests/bracket_test.cpp)
target_link_libraries(bracket_test PRIVATE forever_bots_policy)
add_test(NAME bracket COMMAND bracket_test)

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
    # The world database tools: dump_to_sqlite.py and export_world_quests.py on a tiny made-up dump.
    add_test(NAME world_data COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/world_data_test.py ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_executable(sell_test tests/sell_test.cpp)
target_link_libraries(sell_test PRIVATE forever_bots_policy)
add_test(NAME sell COMMAND sell_test)

add_executable(talent_respec_test tests/talent_respec_test.cpp)
target_include_directories(talent_respec_test PRIVATE src)
add_test(NAME talent_respec COMMAND talent_respec_test)

add_executable(lfg_queue_fill_test tests/lfg_queue_fill_test.cpp)
target_link_libraries(lfg_queue_fill_test PRIVATE forever_bots_policy)
add_test(NAME lfg_queue_fill COMMAND lfg_queue_fill_test)

add_executable(death_pause_test tests/death_pause_test.cpp)
target_link_libraries(death_pause_test PRIVATE forever_bots_policy)
add_test(NAME death_pause COMMAND death_pause_test)

add_executable(city_greet_test tests/city_greet_test.cpp)
target_link_libraries(city_greet_test PRIVATE forever_bots_policy)
add_test(NAME city_greet COMMAND city_greet_test)

add_executable(trade_ads_test tests/trade_ads_test.cpp)
target_link_libraries(trade_ads_test PRIVATE forever_bots_policy)
target_compile_definitions(trade_ads_test PRIVATE FOREVER_BOTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME trade_ads COMMAND trade_ads_test)

add_executable(role_profile_test tests/role_profile_test.cpp)
target_link_libraries(role_profile_test PRIVATE forever_bots_policy)
add_test(NAME role_profile COMMAND role_profile_test)

add_executable(party_mana_test tests/party_mana_test.cpp)
target_link_libraries(party_mana_test PRIVATE forever_bots_policy)
add_test(NAME party_mana COMMAND party_mana_test)

add_executable(whisper_matrix_test tests/whisper_matrix_test.cpp)
target_link_libraries(whisper_matrix_test PRIVATE forever_bots_policy)
target_compile_definitions(whisper_matrix_test PRIVATE FOREVER_BOTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME whisper_matrix COMMAND whisper_matrix_test)

add_executable(path_jitter_test tests/path_jitter_test.cpp)
target_link_libraries(path_jitter_test PRIVATE forever_bots_policy)
add_test(NAME path_jitter COMMAND path_jitter_test)

add_executable(ah_batch_test tests/ah_batch_test.cpp)
target_link_libraries(ah_batch_test PRIVATE forever_bots_policy)
add_test(NAME ah_batch COMMAND ah_batch_test)

add_executable(guild_milestone_test tests/guild_milestone_test.cpp)
target_link_libraries(guild_milestone_test PRIVATE forever_bots_policy)
add_test(NAME guild_milestone COMMAND guild_milestone_test)

add_executable(city_inspect_test tests/city_inspect_test.cpp)
target_link_libraries(city_inspect_test PRIVATE forever_bots_policy)
add_test(NAME city_inspect COMMAND city_inspect_test)

add_executable(quest_invite_test tests/quest_invite_test.cpp)
target_link_libraries(quest_invite_test PRIVATE forever_bots_policy)
add_test(NAME quest_invite COMMAND quest_invite_test)

add_executable(transport_ride_test tests/transport_ride_test.cpp)
target_include_directories(transport_ride_test PRIVATE src)
add_test(NAME transport_ride COMMAND transport_ride_test)
