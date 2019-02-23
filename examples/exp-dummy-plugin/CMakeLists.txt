project(exp_dummy_plugin)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

message(STATUS "[Additional Plugin] exp_dummy_plugin plugin enabled")

add_library( exp_dummy_plugin
             exp_dummy_plugin.cpp
             ${HEADERS} )

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

chronicle_receiver_additional_plugin(exp_dummy_plugin)
