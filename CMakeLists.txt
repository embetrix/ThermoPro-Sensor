cmake_minimum_required(VERSION 3.16)
project(thermopro_sensor VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(SQLite3 REQUIRED)

function(sensor_protocol_option name default description minimum maximum)
    set(${name} "${default}" CACHE STRING "${description}")
    if(NOT "${${name}}" MATCHES "^(0|[1-9][0-9]*|0[xX][0-9a-fA-F]+)$")
        message(FATAL_ERROR "${name} must be a decimal or hexadecimal integer")
    endif()
    math(EXPR numeric_value "${${name}}")
    if(numeric_value LESS minimum OR numeric_value GREATER maximum)
        message(FATAL_ERROR "${name} must be between ${minimum} and ${maximum}")
    endif()
    set(${name}_VALUE "${numeric_value}" PARENT_SCOPE)
endfunction()

sensor_protocol_option(THERMOPRO_PAYLOAD_LENGTH 5
    "Payload bytes without the two-byte manufacturer key" 3 65535)
sensor_protocol_option(THERMOPRO_PACKET_MARKER 0xC2
    "Required low byte of the manufacturer key" 0 255)
sensor_protocol_option(THERMOPRO_MAX_HUMIDITY 100
    "Highest allowed humidity percentage" 0 100)

function(enable_sensor_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(thermopro_core STATIC
    src/app_config.c
    src/thermopro_decoder.c
    src/sensor_registry.c
    src/json_output.c
    src/sqlite_store.c)
target_include_directories(thermopro_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(thermopro_core PUBLIC PkgConfig::JSON_C SQLite::SQLite3)
target_compile_definitions(thermopro_core PUBLIC
    THERMOPRO_PAYLOAD_LENGTH=${THERMOPRO_PAYLOAD_LENGTH_VALUE}
    THERMOPRO_PACKET_MARKER=${THERMOPRO_PACKET_MARKER_VALUE}
    THERMOPRO_MAX_HUMIDITY=${THERMOPRO_MAX_HUMIDITY_VALUE})
enable_sensor_warnings(thermopro_core)

add_executable(thermopro-sensor src/main.c src/bluez_scanner.c)
target_link_libraries(thermopro-sensor PRIVATE thermopro_core PkgConfig::SYSTEMD)
enable_sensor_warnings(thermopro-sensor)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_sensor tests/test_sensor.c)
    target_link_libraries(test_sensor PRIVATE thermopro_core)
    enable_sensor_warnings(test_sensor)
    add_test(NAME sensor_unit COMMAND test_sensor)
    add_test(NAME sensor_config COMMAND thermopro-sensor --check-config ${CMAKE_CURRENT_SOURCE_DIR}/config/sensors.json)

    option(ENABLE_BLUEZ_INTEGRATION_TESTS "Test with fake BlueZ (needs Python, dbus-fast, and dbus-daemon)" OFF)
    if(ENABLE_BLUEZ_INTEGRATION_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(DBUS_DAEMON dbus-daemon)
        if(NOT DBUS_DAEMON)
            message(FATAL_ERROR "The BlueZ test needs dbus-daemon")
        endif()
        add_test(NAME sensor_bluez
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bluez.py
                    $<TARGET_FILE:thermopro-sensor> ${DBUS_DAEMON}
                    ${THERMOPRO_PAYLOAD_LENGTH_VALUE} ${THERMOPRO_PACKET_MARKER_VALUE}
                    ${THERMOPRO_MAX_HUMIDITY_VALUE})
        set_tests_properties(sensor_bluez PROPERTIES TIMEOUT 30)
    endif()
endif()
