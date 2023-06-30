cmake_minimum_required (VERSION 3.13)
project(antelope-chronicle VERSION 3.2)

set(default_build_type "RelWithDebInfo")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()


set(CHRONICLE_RECEIVER_NAME "chronicle-receiver" CACHE INTERNAL "")
set(CHRONICLE_JSON_EXPORT TRUE CACHE INTERNAL "")

include(GNUInstallDirs)

SET(Boost_USE_STATIC_LIBS ON CACHE STRING "ON or OFF")
FIND_PACKAGE(Boost 1.67 REQUIRED COMPONENTS
thread unit_test_framework filesystem date_time system iostreams program_options)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PLATFORM_SPECIFIC_LIBS pthread)

add_subdirectory(external/abieos EXCLUDE_FROM_ALL)
add_subdirectory(external/leap/libraries/chainbase EXCLUDE_FROM_ALL)
add_subdirectory(external/leap/libraries/appbase EXCLUDE_FROM_ALL)
add_subdirectory(external/leap/libraries/libfc EXCLUDE_FROM_ALL)

add_executable(${CHRONICLE_RECEIVER_NAME}
  external/abieos/src/abieos.cpp
  chronicle-receiver/main.cpp
  chronicle-receiver/receiver_plugin.cpp
  )

if(CHRONICLE_JSON_EXPORT)
  target_sources(${CHRONICLE_RECEIVER_NAME} PUBLIC
    chronicle-receiver/decoder_plugin.cpp
    chronicle-receiver/exp_ws_plugin.cpp
    )
endif()


include_directories(
  chronicle-receiver
  external/leap/libraries/appbase/include
  external/abieos/src
  external/abieos/external/rapidjson/include
  external/abieos/include
  external/leap/libraries/chainbase/include
  external/leap/libraries/libfc/include
  ${Boost_INCLUDE_DIR})

target_link_libraries(${CHRONICLE_RECEIVER_NAME}
  PRIVATE abieos chainbase appbase fc
  PUBLIC Boost::date_time Boost::system Boost::iostreams Boost::program_options z pthread)


if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${CHRONICLE_RECEIVER_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fcolor-diagnostics)
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
  target_link_libraries(${CHRONICLE_RECEIVER_NAME} PRIVATE -Wl,${whole_archive_flag} ${ADDITIONAL_PLUGIN_TARGET} -Wl,${no_whole_archive_flag} )
endforeach()


## Installables
install(TARGETS ${CHRONICLE_RECEIVER_NAME} RUNTIME DESTINATION sbin)
install(FILES systemd/chronicle_receiver\@.service DESTINATION share)

include(package.cmake)
include(CPack)
