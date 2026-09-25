cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES C CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(gesture_gate_test gesture_gate_test.cpp)
target_include_directories(gesture_gate_test PRIVATE ../main/apps/app_gesture)
target_compile_features(gesture_gate_test PRIVATE cxx_std_17)
add_test(NAME gesture_gate_test COMMAND $<TARGET_FILE:gesture_gate_test>)

add_executable(gesture_transition_test gesture_transition_test.cpp)
target_include_directories(gesture_transition_test PRIVATE ../main/apps/app_gesture)
target_compile_features(gesture_transition_test PRIVATE cxx_std_17)
add_test(NAME gesture_transition_test COMMAND $<TARGET_FILE:gesture_transition_test>)

add_executable(deferred_app_launch_test
    deferred_app_launch_test.cpp
    ../components/mooncake/src/mooncake.cpp
    ../components/mooncake/src/ability/app_ability.cpp
    ../components/mooncake/src/ability/ui_ability.cpp
    ../components/mooncake/src/ability/worker_ability.cpp
    ../components/mooncake/src/ability_manager/ability_manager.cpp
    ../components/mooncake/src/ability_manager/ability_api_wrapping.cpp
)
target_include_directories(deferred_app_launch_test PRIVATE
    ../main/apps/app_launcher
    ../components/mooncake/src
)
target_compile_features(deferred_app_launch_test PRIVATE cxx_std_17)
add_test(NAME deferred_app_launch_test COMMAND $<TARGET_FILE:deferred_app_launch_test>)

add_executable(sl_bus_parser_test
    sl_bus_parser_test.cpp
    ../main/apps/app_sl_bus/sl_bus_departures.cpp
    ../managed_components/espressif__cjson/cJSON/cJSON.c
)
target_include_directories(sl_bus_parser_test PRIVATE
    ../main/apps/app_sl_bus
    ../managed_components/espressif__cjson/cJSON
)
target_compile_features(sl_bus_parser_test PRIVATE cxx_std_17)
add_test(NAME sl_bus_parser_test COMMAND $<TARGET_FILE:sl_bus_parser_test>)

add_executable(sl_bus_reliability_test
    sl_bus_reliability_test.cpp
    ../main/apps/app_sl_bus/sl_bus_departure_client.cpp
    ../main/apps/app_sl_bus/sl_bus_departures.cpp
    ../main/apps/app_sl_bus/sl_bus_poll_schedule.cpp
    ../managed_components/espressif__cjson/cJSON/cJSON.c
)
target_include_directories(sl_bus_reliability_test PRIVATE
    ../main/apps/app_sl_bus
    ../managed_components/78__esp-ml307/include
    ../managed_components/espressif__cjson/cJSON
)
target_compile_features(sl_bus_reliability_test PRIVATE cxx_std_17)
add_test(NAME sl_bus_reliability_test COMMAND $<TARGET_FILE:sl_bus_reliability_test>)
