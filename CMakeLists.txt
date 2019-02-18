cmake_minimum_required (VERSION 3.11)
project(eos-chronicle VERSION 0.1 LANGUAGES CXX)

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

## load in pkg-config support
find_package(PkgConfig REQUIRED)
## use pkg-config to get hints for 0mq locations
pkg_check_modules(PC_ZeroMQ REQUIRED libzmq)

## use the hint from above to find where 'zmq.hpp' is located
find_path(ZeroMQ_INCLUDE_DIR
  NAMES zmq.hpp
  PATHS ${PC_ZeroMQ_INCLUDE_DIRS}
  )

## use the hint from about to find the location of libzmq
find_library(ZeroMQ_LIBRARY
  NAMES zmq
  PATHS ${PC_ZeroMQ_LIBRARY_DIRS}
  )

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Debug)

add_subdirectory(external/chainbase)
add_subdirectory(external/appbase)
add_subdirectory(external/fc)

add_executable(chronicle-receiver
  external/abieos/src/abieos.cpp
  chronicle-receiver/main.cpp
  chronicle-receiver/chain_state_types.cpp
  chronicle-receiver/receiver_plugin.cpp
  chronicle-receiver/decoder_plugin.cpp
  chronicle-receiver/exp_ws_plugin.cpp
  chronicle-receiver/exp_zmq_plugin.cpp
)

target_include_directories(chronicle-receiver
  PUBLIC external/abieos/src external/abieos/external/rapidjson/include 
  external/chainbase/include
  external/fc/include
  PRIVATE ${Boost_INCLUDE_DIR})

target_link_libraries(chronicle-receiver
  PRIVATE chainbase appbase fc
  PUBLIC Boost::date_time Boost::system Boost::iostreams Boost::program_options z pthread ${ZeroMQ_LIBRARY})


if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(chronicle-receiver PRIVATE -Wall -Wextra -Wno-unused-parameter -fcolor-diagnostics)
endif()
