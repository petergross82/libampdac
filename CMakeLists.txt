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
    ${ANDROID_NATIVE}/uac2_feature_volume.cpp
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

add_library(ampdac_client SHARED src/ampdac_client.c)
target_include_directories(ampdac_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(ampdac_client PRIVATE _GNU_SOURCE)
target_compile_options(ampdac_client PRIVATE -Wall -Wextra)

add_executable(ampdacd src/ampdacd.cpp)
target_link_libraries(ampdacd PRIVATE ampdac pthread)
target_include_directories(ampdacd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(ipc_smoke tests/ipc_smoke.c)
target_link_libraries(ipc_smoke PRIVATE ampdac_client)
target_include_directories(ipc_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(PkgConfig_FOUND)
    pkg_check_modules(ALSA alsa)
endif()
if(ALSA_FOUND)
    add_library(asound_module_pcm_ampdac MODULE alsa/pcm_ampdac.c)
    set_target_properties(asound_module_pcm_ampdac PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "asound_module_pcm_ampdac"
    )
    target_include_directories(asound_module_pcm_ampdac PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ALSA_INCLUDE_DIRS}
    )
    target_compile_definitions(asound_module_pcm_ampdac PRIVATE _GNU_SOURCE PIC)
    target_link_libraries(asound_module_pcm_ampdac PRIVATE ampdac_client ${ALSA_LIBRARIES})
    install(TARGETS asound_module_pcm_ampdac
            LIBRARY DESTINATION ${ALSA_LIBDIR}/alsa-lib)
endif()

install(TARGETS ampdac ampdac_client ampdac-cli ampdacd
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(FILES include/ampdac.h include/ampdac_ipc.h DESTINATION include)
install(FILES udev/99-ampdac.rules DESTINATION lib/udev/rules.d)
install(FILES alsa/50-ampdac.conf DESTINATION share/alsa/alsa.conf.d)
install(FILES systemd/ampdac.service systemd/ampdac.socket DESTINATION lib/systemd/system)
install(FILES wireplumber/50-ampdac.conf DESTINATION share/wireplumber/wireplumber.conf.d)
install(PROGRAMS volumio/ampdac/ampdac DESTINATION bin)
