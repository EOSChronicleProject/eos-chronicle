cmake_minimum_required (VERSION 3.11)
project(eosio-chronicle VERSION 2.1)

set(default_build_type "Release")
 
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()


include(GNUInstallDirs)

SET(Boost_USE_STATIC_LIBS ON CACHE STRING "ON or OFF")
FIND_PACKAGE(Boost 1.67 REQUIRED COMPONENTS
thread unit_test_framework filesystem date_time system iostreams program_options)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/fc/CMakeModules")
include(SetupTargetMacros)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PLATFORM_SPECIFIC_LIBS pthread)

set(CMAKE_BUILD_TYPE Debug)

add_subdirectory(external/abieos EXCLUDE_FROM_ALL)
add_subdirectory(external/chainbase EXCLUDE_FROM_ALL)
add_subdirectory(external/appbase EXCLUDE_FROM_ALL)
add_subdirectory(external/fc EXCLUDE_FROM_ALL)

add_executable(chronicle-receiver
  external/abieos/src/abieos.cpp
  chronicle-receiver/main.cpp
  chronicle-receiver/receiver_plugin.cpp
  chronicle-receiver/decoder_plugin.cpp
  chronicle-receiver/exp_ws_plugin.cpp
)

include_directories(
  chronicle-receiver
  external/appbase/include
  external/abieos/src
  external/abieos/external/rapidjson/include
  external/abieos/include
  external/chainbase/include
  external/fc/include
  ${Boost_INCLUDE_DIR})

target_link_libraries(chronicle-receiver
  PRIVATE abieos chainbase appbase fc
  PUBLIC Boost::date_time Boost::system Boost::iostreams Boost::program_options z pthread)


if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(chronicle-receiver PRIVATE -Wall -Wextra -Wno-unused-parameter -fcolor-diagnostics)
endif()

## Infrastructure for third-party plugins

macro(chronicle_receiver_additional_plugin)
  set(ADDITIONAL_PLUGINS_TARGET "${ADDITIONAL_PLUGINS_TARGET};${ARGN}" PARENT_SCOPE)
endmacro()

set(whole_archive_flag "--whole-archive")
set(no_whole_archive_flag "--no-whole-archive")

foreach(ADDITIONAL_PLUGIN_SOURCE_DIR ${CHRONICLE_RECEIVER_ADDITIONAL_PLUGINS})
  string(UUID ADDITIONAL_PLUGIN_SOURCE_DIR_MD5 NAMESPACE
    "00000000-0000-0000-0000-000000000000" NAME ${ADDITIONAL_PLUGIN_SOURCE_DIR} TYPE MD5)
  message(STATUS "[Additional Plugin] ${ADDITIONAL_PLUGIN_SOURCE_DIR} => ${CMAKE_BINARY_DIR}/additional_plugins/${ADDITIONAL_PLUGIN_SOURCE_DIR_MD5}")
  add_subdirectory(${ADDITIONAL_PLUGIN_SOURCE_DIR} ${CMAKE_BINARY_DIR}/additional_plugins/${ADDITIONAL_PLUGIN_SOURCE_DIR_MD5})
endforeach()

foreach(ADDITIONAL_PLUGIN_TARGET ${ADDITIONAL_PLUGINS_TARGET})
  target_link_libraries(chronicle-receiver PRIVATE -Wl,${whole_archive_flag} ${ADDITIONAL_PLUGIN_TARGET} -Wl,${no_whole_archive_flag} )
endforeach()


## Installables
install(TARGETS chronicle-receiver RUNTIME DESTINATION sbin)
install(FILES systemd/chronicle_receiver\@.service DESTINATION share)

include(package.cmake)
include(CPack)
