cmake_minimum_required(VERSION 3.14)
project(BleMeshPlugin LANGUAGES CXX)
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)
find_package(OpenSSL REQUIRED)

# The Qt Bluetooth radio (Phase 3) is built ONLY when qtconnectivity is present in the build env;
# otherwise the module falls back to the no-op StubRadio and still builds. This keeps ble_mesh
# green everywhere while enabling the real radio wherever Qt6::Bluetooth is available.
find_package(Qt6 COMPONENTS Bluetooth QUIET)
set(LOAM_RADIO_SOURCES "")
if(Qt6Bluetooth_FOUND)
    message(STATUS "ble_mesh: Qt6::Bluetooth found -> building the QtBleRadio")
    list(APPEND LOAM_RADIO_SOURCES src/mesh_radio_qt.hpp src/mesh_radio_qt.cpp)
else()
    message(STATUS "ble_mesh: Qt6::Bluetooth NOT found -> StubRadio only (add qtconnectivity to enable BLE)")
endif()

logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/ble_mesh_impl.h
        src/ble_mesh_impl.cpp
        src/mesh.hpp
        src/stub_radio.hpp
        ${LOAM_RADIO_SOURCES}
    INCLUDE_DIRS src
    FIND_PACKAGES OpenSSL
    LINK_LIBRARIES OpenSSL::Crypto
)

if(Qt6Bluetooth_FOUND)
    target_link_libraries(${MODULE_NAME}_module_plugin PRIVATE Qt6::Bluetooth)
    target_compile_definitions(${MODULE_NAME}_module_plugin PRIVATE LOAM_HAS_QTBLUETOOTH)
endif()
