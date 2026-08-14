find_package(Threads REQUIRED)
find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_library(
  dispatcher_core
  src/concurrency/execution_runtime.cpp
  src/domain/connection_state.cpp
  src/domain/map_transform.cpp
  src/domain/resource_arbiter.cpp
  src/domain/robot_connection_config.cpp
  src/maps/map_import_service.cpp
  src/maps/map_yaml.cpp
  src/maps/pgm.cpp
  src/maps/png_writer.cpp
  src/ros/beast_transport.cpp
  src/ros/pose_cache.cpp
  src/ros/pose_mapper.cpp
  src/ros/rosbridge_protocol.cpp
  src/ros/rosbridge_session.cpp
  src/ros/ros_typedef_schema.cpp
  src/remote/controlled_ssh_executor.cpp
  src/ops/ops_log.cpp
  src/workflow/feedback_trigger.cpp
  src/workflow/event_spec.cpp
)

target_include_directories(
  dispatcher_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  dispatcher_core
  PUBLIC
    Threads::Threads
    Boost::system
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    ZLIB::ZLIB
)
target_compile_features(dispatcher_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    dispatcher_core
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

add_executable(dispatcher_core_cli src/app/main.cpp)
target_link_libraries(dispatcher_core_cli PRIVATE dispatcher_core)

find_package(PostgreSQL)
find_path(PQXX_INCLUDE_DIR pqxx/pqxx)
find_library(PQXX_LIBRARY pqxx)

set(DISPATCHER_HAS_PQXX OFF)
if(PostgreSQL_FOUND AND PQXX_INCLUDE_DIR AND PQXX_LIBRARY)
  set(DISPATCHER_HAS_PQXX ON)
  add_library(
    dispatcher_persist
    src/db/pg_pool.cpp
    src/db/workspace_repository.cpp
    src/ros/robot_runtime.cpp
    src/workflow/workflow_executor.cpp
  )
  target_include_directories(
    dispatcher_persist
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${PQXX_INCLUDE_DIR}
      ${PostgreSQL_INCLUDE_DIRS}
  )
  target_link_libraries(
    dispatcher_persist
    PUBLIC
      dispatcher_core
      ${PQXX_LIBRARY}
      ${PostgreSQL_LIBRARIES}
  )
endif()

if(DISPATCHER_BUILD_API)
  find_package(Drogon CONFIG QUIET)

  if(Drogon_FOUND)
    if(NOT DISPATCHER_HAS_PQXX)
      if(DISPATCHER_REQUIRE_DROGON)
        message(FATAL_ERROR "libpqxx is required to build dispatcher_api")
      else()
        message(STATUS "libpqxx not found; dispatcher_api target is skipped")
      endif()
    else()
      add_executable(
        dispatcher_api
        src/api/main.cpp
        src/api/app_state.cpp
        src/api/register_routes.cpp
      )
      target_link_libraries(
        dispatcher_api
        PRIVATE
          dispatcher_persist
          Drogon::Drogon
      )
      install(TARGETS dispatcher_api RUNTIME DESTINATION bin)
    endif()
  elseif(DISPATCHER_REQUIRE_DROGON)
    message(FATAL_ERROR "Drogon is required but was not found")
  else()
    message(STATUS "Drogon not found; dispatcher_api target is skipped")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(dispatcher_core_tests tests/core_tests.cpp)
  target_link_libraries(dispatcher_core_tests PRIVATE dispatcher_core)
  add_test(NAME dispatcher_core_tests COMMAND dispatcher_core_tests)
endif()

install(TARGETS dispatcher_core_cli RUNTIME DESTINATION bin)
