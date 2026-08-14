cmake_minimum_required(VERSION 3.22)

project(
  dispatcher
  VERSION 0.1.0
  DESCRIPTION "Multi-robot and multi-device dispatch system"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(DISPATCHER_BUILD_API "Build the Drogon HTTP API" ON)
option(DISPATCHER_REQUIRE_DROGON "Fail configuration when Drogon is unavailable" OFF)

add_subdirectory(backend)

