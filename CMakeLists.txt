cmake_minimum_required(VERSION 3.16)
project(ampdac LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
endif()
if(NOT LIBUSB_FOUND)
    find_library(LIBUSB_SO NAMES usb-1.0 libusb-1.0.so.0)
    if(NOT LIBUSB_SO)
        message(FATAL_ERROR "libusb-1.0 not found (install libusb-1.0-0-dev)")
    endif()
endif()

set(ANDROID_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/native)

add_library(ampdac SHARED
    src/ampdac.cpp
    src/ampdac_android_log.cpp
    src/ampdac_kernel.cpp
    ${ANDROID_NATIVE}/usb_session_transport.cpp
    ${ANDROID_NATIVE}/usb_teardown.cpp
    ${ANDROID_NATIVE}/uac2_descriptor_parser.cpp
    ${ANDROID_NATIVE}/uac2_dsd_detector.cpp
    ${ANDROID_NATIVE}/uac2_clock_control.cpp
    ${ANDROID_NATIVE}/usb_device_controller.cpp
    ${ANDROID_NATIVE}/usb_iso_transfer_pool.cpp
    ${ANDROID_NATIVE}/spsc_ring_buffer.cpp
    ${ANDROID_NATIVE}/pcm_wire_formatter.cpp
    ${ANDROID_NATIVE}/usb_playback_state.cpp
    ${ANDROID_NATIVE}/usb_producer_coordinator.cpp
    ${ANDROID_NATIVE}/dop_formatter.cpp
    ${ANDROID_NATIVE}/native_dsd_formatter.cpp
    ${ANDROID_NATIVE}/dsd_playback_manager.cpp
    ${ANDROID_NATIVE}/libusb_event_thread.cpp
    ${ANDROID_NATIVE}/decoder_to_ring_bridge.cpp
)

target_include_directories(ampdac
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${ANDROID_NATIVE}
)

target_compile_definitions(ampdac PRIVATE _GNU_SOURCE)
target_compile_options(ampdac PRIVATE -Wall -Wextra -Werror=return-type -Wno-unused-parameter)

if(LIBUSB_FOUND)
    target_link_libraries(ampdac PRIVATE PkgConfig::LIBUSB pthread)
else()
    target_link_libraries(ampdac PRIVATE ${LIBUSB_SO} pthread)
endif()

add_executable(ampdac-cli examples/ampdac_cli.cpp)
target_link_libraries(ampdac-cli PRIVATE ampdac)
target_include_directories(ampdac-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(ddb_out_ampdac MODULE plugins/deadbeef/ampdac_output.c)
set_target_properties(ddb_out_ampdac PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ddb_out_ampdac"
)
target_include_directories(ddb_out_ampdac PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/deadbeef/include
)
target_compile_definitions(ddb_out_ampdac PRIVATE _GNU_SOURCE)
target_link_libraries(ddb_out_ampdac PRIVATE ampdac))

install(TARGETS ampdac ampdac-cli
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(FILES include/ampdac.h DESTINATION include)
install(FILES udev/99-ampdac.rules DESTINATION lib/udev/rules.d)
