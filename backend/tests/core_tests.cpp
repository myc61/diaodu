#include "dispatcher/concurrency/execution_runtime.hpp"
#include "dispatcher/domain/connection_state.hpp"
#include "dispatcher/domain/map_transform.hpp"
#include "dispatcher/domain/resource_arbiter.hpp"
#include "dispatcher/domain/robot_connection_config.hpp"
#include "dispatcher/interfaces/interface_catalog.hpp"
#include "dispatcher/maps/map_import_service.hpp"
#include "dispatcher/maps/map_yaml.hpp"
#include "dispatcher/maps/pgm.hpp"
#include "dispatcher/ros/action_result.hpp"
#include "dispatcher/ros/rosbridge_protocol.hpp"
#include "dispatcher/ros/rosbridge_session.hpp"
#include "dispatcher/ros/pose_mapper.hpp"
#include "dispatcher/remote/controlled_ssh_executor.hpp"
#include "dispatcher/ros/ros_typedef_schema.hpp"
#include "dispatcher/workflow/edge_join.hpp"
#include "dispatcher/workflow/event_spec.hpp"
#include "dispatcher/workflow/feedback_trigger.hpp"

#include <filesystem>
#include <fstream>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

class MockTransport final : public dispatcher::ros::IRosbridgeTransport {
 public:
  bool sendText(std::string_view payload) override {
    sent.push_back(dispatcher::ros::Json::parse(payload));
    return send_result;
  }

  void close() override { closed = true; }

  bool send_result{true};
  bool closed{false};
  std::vector<dispatcher::ros::Json> sent;
};

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

void updateMaximum(std::atomic<int>& maximum, int value) {
  auto observed = maximum.load(std::memory_order_relaxed);
  while (observed < value &&
         !maximum.compare_exchange_weak(
             observed, value, std::memory_order_relaxed)) {
  }
}

void testExecutionRuntime() {
  using dispatcher::concurrency::ExecutionRuntime;
  using dispatcher::concurrency::ExecutionRuntimeOptions;
  using namespace std::chrono_literals;

  std::atomic<int> reported_errors{0};
  ExecutionRuntime runtime(
      ExecutionRuntimeOptions{.protocol_threads = 4, .blocking_threads = 2},
      [&](std::string_view, std::string_view) { ++reported_errors; });
  expect(runtime.protocolThreadCount() == 4, "protocol pool size should match");
  expect(runtime.blockingThreadCount() == 2, "blocking pool size should match");

  std::atomic<int> same_robot_active{0};
  std::atomic<int> same_robot_maximum{0};
  std::mutex order_mutex;
  std::vector<int> order;
  for (int index = 0; index < 20; ++index) {
    expect(
        runtime.postRobot("robot-1", [&, index] {
          const auto active = ++same_robot_active;
          updateMaximum(same_robot_maximum, active);
          std::this_thread::sleep_for(1ms);
          {
            std::lock_guard lock(order_mutex);
            order.push_back(index);
          }
          --same_robot_active;
        }),
        "robot task should be accepted");
  }

  std::barrier robot_parallel_barrier{2};
  std::atomic<int> different_robot_active{0};
  std::atomic<int> different_robot_maximum{0};
  const auto parallel_robot_task = [&] {
    const auto active = ++different_robot_active;
    updateMaximum(different_robot_maximum, active);
    robot_parallel_barrier.arrive_and_wait();
    --different_robot_active;
  };
  expect(runtime.postRobot("robot-2", parallel_robot_task),
         "second robot task should be accepted");
  expect(runtime.postRobot("robot-3", parallel_robot_task),
         "third robot task should be accepted");

  std::barrier pool_isolation_barrier{2};
  std::atomic<int> isolated_pool_tasks{0};
  const auto isolated_task = [&] {
    ++isolated_pool_tasks;
    pool_isolation_barrier.arrive_and_wait();
  };
  expect(runtime.postParallel(isolated_task), "parallel task should be accepted");
  expect(runtime.postBlocking(isolated_task), "blocking task should be accepted");
  expect(runtime.postParallel([] { throw std::runtime_error("expected"); }),
         "throwing task should be accepted");

  runtime.join();

  expect(same_robot_maximum == 1,
         "same robot tasks must never execute concurrently");
  expect(order.size() == 20, "all same robot tasks should finish");
  for (int index = 0; index < 20; ++index) {
    expect(order[static_cast<std::size_t>(index)] == index,
           "same robot task order should be preserved");
  }
  expect(different_robot_maximum >= 2,
         "different robot tasks should execute concurrently");
  expect(isolated_pool_tasks == 2,
         "protocol and blocking pools should execute independently");
  expect(reported_errors == 1, "task exceptions should reach error handler");
  expect(!runtime.postParallel([] {}), "joined runtime should reject tasks");
}

void testDelayedRobotTask() {
  using namespace std::chrono_literals;
  using dispatcher::concurrency::ExecutionRuntime;

  {
    ExecutionRuntime runtime;
    std::atomic<int> ran{0};
    expect(
        runtime.postRobotAfter("robot-delay", 25ms, [&] { ran = 1; }),
        "delayed robot task should be accepted");
    std::this_thread::sleep_for(80ms);
    runtime.join();
    expect(ran == 1, "delayed robot task should run after the wait");
  }

  {
    ExecutionRuntime runtime;
    std::atomic<int> ran{0};
    const auto started = std::chrono::steady_clock::now();
    expect(
        runtime.postRobotAfter("robot-delay", 30s, [&] { ran = 1; }),
        "long delayed task should be accepted");
    runtime.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(
        elapsed < 2s, "join should cancel pending delayed tasks");
    expect(ran == 0, "cancelled delayed task should not run");
  }
}

void testConnectionTransitions() {
  using namespace dispatcher::domain;

  ConnectionStateMachine state;
  expect(
      !state.transitionTo(ConnectionState::Online),
      "disconnected must not transition directly to online");
  expect(
      state.transitionTo(ConnectionState::Connecting),
      "disconnected should transition to connecting");
  expect(
      state.transitionTo(ConnectionState::Online),
      "connecting should transition to online");
  expect(
      state.transitionTo(ConnectionState::Degraded),
      "online should transition to degraded");
  expect(
      state.transitionTo(ConnectionState::Reconnecting),
      "degraded should transition to reconnecting");
}

void testRobotConnectionConfig() {
  using dispatcher::domain::RemoteAccessProtocol;
  using dispatcher::domain::RobotConnectionConfig;
  using dispatcher::domain::Ros1ActionlibEndpointConfig;
  using dispatcher::domain::RosVersion;

  RobotConnectionConfig config{
      .name = "zj-01",
      .ros_version = RosVersion::Ros1,
      .ros_distribution = "noetic",
      .rosbridge = {
          .host = "192.168.10.21",
          .port = 9090,
          .path = "/",
      },
  };
  expect(config.validationErrors().empty(),
         "Noetic robot should allow connection-only draft config");
  expect(config.rosbridge.websocketUrl() == "ws://192.168.10.21:9090/",
         "robot IP should produce rosbridge websocket URL");

  config.remote_access = {
      .protocol = RemoteAccessProtocol::Sftp,
      .port = 22,
      .username = "naviai",
      .credential_reference = "robot-zj-01-key",
      .known_hosts_reference = "robot-zj-01-known-hosts",
  };
  expect(config.validationErrors().empty(),
         "controlled SFTP config should validate");

  const auto navigation = Ros1ActionlibEndpointConfig::fromGoalTopic(
      "/zj_humanoid/navigation/navigation/goal");
  expect(navigation.server_name == "/zj_humanoid/navigation/navigation",
         "ROS 1 action server should derive from goal topic");
  expect(navigation.cancel_topic ==
             "/zj_humanoid/navigation/navigation/cancel",
         "ROS 1 cancel topic should derive from server name");
  expect(!navigation.ready(),
         "navigation contract should wait for the action type");

  const auto ready_navigation = Ros1ActionlibEndpointConfig::fromGoalTopic(
      "/zj_humanoid/navigation/navigation/goal",
      "zj_navigation_msgs/NavigationAction");
  expect(ready_navigation.ready(),
         "navigation action should be ready after type is configured");
}

void testControlledSshValidation() {
  using dispatcher::remote::ControlledSshExecutor;
  using dispatcher::remote::ControlledSshRequest;

  ControlledSshRequest request{
      .job_id = "startup-test",
      .host = "192.0.2.10",
      .port = 22,
      .username = "dispatcher",
      .credential_reference = "/run/secrets/robot_ssh_key",
      .known_hosts_reference = "/run/secrets/robot_known_hosts",
      .steps = nlohmann::json::array({
          {{"script", "./start.sh"},
           {"working_directory", "/home/naviai/robot"},
           {"args", nlohmann::json::array({"navigation"})},
           {"timeout_ms", 1000}},
      }),
      .readiness_checks = nlohmann::json::array(),
      .timeout_ms = 2000,
  };
  expect(
      !ControlledSshExecutor::validateRequest(request).has_value(),
      "controlled SSH profile with a relative script should validate");

  request.steps[0]["script"] = "/bin/sh";
  expect(
      ControlledSshExecutor::validateRequest(request).has_value(),
      "controlled SSH must reject a general-purpose shell");

  request.steps[0]["script"] = "../bin/run.sh";
  expect(
      ControlledSshExecutor::validateRequest(request).has_value(),
      "controlled SSH must reject path traversal");

  request.steps[0]["script"] = "./start.sh";
  request.credential_reference = "/tmp/robot_ssh_key";
  expect(
      ControlledSshExecutor::validateRequest(request).has_value(),
      "controlled SSH must reject credentials outside secret roots");

  request.credential_reference = "/run/secrets/robot_ssh_key";
  request.host = "-oProxyCommand=bad";
  expect(
      ControlledSshExecutor::validateRequest(request).has_value(),
      "controlled SSH must reject option-like host values");

  request.host = "192.0.2.10";
  request.username = "root";
  expect(
      ControlledSshExecutor::validateRequest(request).has_value(),
      "controlled SSH must reject root accounts");

  request.username = "naviai";
  request.steps = nlohmann::json::array({
      {{"command", nlohmann::json::array({"roslaunch", "robot", "bringup.launch"})},
       {"mode", "detached"},
       {"log_path", "/tmp/dispatcher-robot.log"},
       {"timeout_ms", 1000}},
  });
  expect(
      !ControlledSshExecutor::validateRequest(request).has_value(),
      "structured detached command should validate");

  request.steps[0]["command"] = nlohmann::json::array({"bash", "-c", "anything"});
  expect(
      ControlledSshExecutor::validateRequest(request).has_value(),
      "structured command must reject shell interpreters");
}

void testResourceMatrix() {
  using namespace dispatcher::domain;

  ResourceArbiter arbiter;

  const auto soft_one = arbiter.tryAcquire({
      .lease_id = "soft-1",
      .owner_id = "node-1",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Soft,
  });
  const auto soft_two = arbiter.tryAcquire({
      .lease_id = "soft-2",
      .owner_id = "node-2",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Soft,
  });
  const auto navigation_while_soft = arbiter.tryAcquire({
      .lease_id = "navigation-1",
      .owner_id = "node-3",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Navigation,
  });

  expect(soft_one.acquired, "first soft action should acquire");
  expect(soft_two.acquired, "soft actions should coexist");
  expect(
      !navigation_while_soft.acquired,
      "navigation must conflict with soft actions");

  expect(arbiter.release("soft-1"), "first soft lease should release");
  expect(arbiter.release("soft-2"), "second soft lease should release");

  const auto navigation = arbiter.tryAcquire({
      .lease_id = "navigation-2",
      .owner_id = "node-4",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Navigation,
  });
  const auto none_action = arbiter.tryAcquire({
      .lease_id = "none-1",
      .owner_id = "node-5",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::None,
  });
  const auto second_navigation = arbiter.tryAcquire({
      .lease_id = "navigation-3",
      .owner_id = "node-6",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Navigation,
  });
  const auto hard_action = arbiter.tryAcquire({
      .lease_id = "hard-1",
      .owner_id = "node-7",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Hard,
  });
  const auto other_robot_hard = arbiter.tryAcquire({
      .lease_id = "hard-2",
      .owner_id = "node-8",
      .robot_id = "robot-2",
      .blocking_type = BlockingType::Hard,
  });

  expect(navigation.acquired, "navigation should acquire after soft release");
  expect(none_action.acquired, "none action should coexist with navigation");
  expect(
      !second_navigation.acquired,
      "two navigation actions must not coexist");
  expect(!hard_action.acquired, "hard action must conflict with all work");
  expect(
      other_robot_hard.acquired,
      "leases on another robot should not conflict");
}

void testNamedResources() {
  using namespace dispatcher::domain;

  ResourceArbiter arbiter;
  const auto lift_one = arbiter.tryAcquire({
      .lease_id = "lift-1",
      .owner_id = "node-1",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::None,
      .additional_claims = {{
          .name = "robot/robot-1/lift",
          .access = ResourceAccess::Exclusive,
      }},
  });
  const auto lift_two = arbiter.tryAcquire({
      .lease_id = "lift-2",
      .owner_id = "node-2",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::None,
      .additional_claims = {{
          .name = "robot/robot-1/lift",
          .access = ResourceAccess::Exclusive,
      }},
  });

  expect(lift_one.acquired, "first named resource should acquire");
  expect(!lift_two.acquired, "exclusive named resources must conflict");
}

void testMapTransform() {
  using namespace dispatcher::domain;

  const MapMetadata map{
      .width = 100,
      .height = 100,
      .resolution = 0.05,
      .origin_x = 0.0,
      .origin_y = 0.0,
      .origin_yaw = 0.0,
  };
  const auto pixel = worldToPixel(
      map,
      WorldPose2D{.x = 1.0, .y = 2.0, .yaw = std::numbers::pi / 2.0});

  expect(pixel.has_value(), "valid map transform should produce a pose");
  if (pixel.has_value()) {
    expect(std::abs(pixel->x - 20.0) < 1e-9, "pixel x should match");
    expect(std::abs(pixel->y - 59.0) < 1e-9, "pixel y should match");
    expect(pixel->inside_map, "pose should be inside map");
    expect(
        std::abs(pixel->yaw + std::numbers::pi / 2.0) < 1e-9,
        "canvas yaw should account for y axis inversion");
  }

  const auto invalid = worldToPixel(MapMetadata{}, WorldPose2D{});
  expect(!invalid.has_value(), "invalid metadata should be rejected");

  if (pixel.has_value()) {
    const auto world = pixelToWorld(map, *pixel);
    expect(world.has_value(), "pixelToWorld should invert worldToPixel");
    if (world.has_value()) {
      expect(std::abs(world->x - 1.0) < 1e-9, "round-trip x");
      expect(std::abs(world->y - 2.0) < 1e-9, "round-trip y");
      expect(std::abs(world->yaw - std::numbers::pi / 2.0) < 1e-9,
             "round-trip yaw");
    }
  }

  const auto quarter_turn = yawToQuaternion(std::numbers::pi / 2.0);
  const double half_sqrt_two = std::sqrt(0.5);
  expect(std::abs(quarter_turn.x) < 1e-12, "planar quaternion x is zero");
  expect(std::abs(quarter_turn.y) < 1e-12, "planar quaternion y is zero");
  expect(
      std::abs(quarter_turn.z - half_sqrt_two) < 1e-12,
      "quaternion z must use sin(yaw/2)");
  expect(
      std::abs(quarter_turn.w - half_sqrt_two) < 1e-12,
      "quaternion w must use cos(yaw/2)");
  expect(
      std::abs(
          quarter_turn.z * quarter_turn.z +
          quarter_turn.w * quarter_turn.w - 1.0) < 1e-12,
      "yaw quaternion must be normalized");
}

void testMapImportCore() {
  using namespace dispatcher::maps;

  const char* yaml_text =
      "image: map.pgm\n"
      "resolution: 0.05\n"
      "origin: [-1.0, -2.0, 0.0]\n"
      "occupied_thresh: 0.65\n"
      "free_thresh: 0.196\n";
  const auto parsed = parseMapYaml(yaml_text);
  expect(parsed.ok, "valid map yaml should parse");
  expect(parsed.yaml.image == "map.pgm", "yaml image field");
  expect(std::abs(parsed.yaml.resolution - 0.05) < 1e-12, "yaml resolution");

  const auto missing = parseMapYaml("resolution: 0.05\n");
  expect(!missing.ok, "yaml missing required fields should fail");

  const std::string pgm =
      "P2\n2 2\n255\n0 64\n128 255\n";
  const auto image = parsePgm(pgm);
  expect(image.ok, "ascii pgm should parse");
  expect(image.image.width == 2 && image.image.height == 2, "pgm size");

  const auto root =
      std::filesystem::temp_directory_path() / "dispatcher-map-import-test";
  std::filesystem::remove_all(root);
  const auto first = materializeMapFiles(root, "scene-1", 1, yaml_text, pgm);
  expect(first.ok, "first map import should succeed");
  expect(std::filesystem::exists(first.files.preview_path), "preview exists");
  const auto second = materializeMapFiles(root, "scene-1", 2, yaml_text, pgm);
  expect(second.ok, "second version should not overwrite first");
  expect(first.files.pgm_path != second.files.pgm_path, "paths differ by version");
  std::filesystem::remove_all(root);
}

void testFeedbackTrigger() {
  using namespace std::chrono_literals;
  using dispatcher::workflow::FeedbackTriggerGate;
  using dispatcher::workflow::FeedbackTriggerPolicy;

  const auto start = FeedbackTriggerGate::Clock::time_point{};
  FeedbackTriggerGate once;
  expect(!once.evaluate(false, start), "false condition should not fire");
  expect(once.evaluate(true, start + 1ms), "first rising edge should fire");
  expect(!once.evaluate(true, start + 2ms), "steady true should not fire");
  expect(!once.evaluate(false, start + 3ms), "falling edge should not fire");
  expect(
      !once.evaluate(true, start + 4ms),
      "default policy should fire only once");

  FeedbackTriggerGate repeat(FeedbackTriggerPolicy{
      .max_firings = 2,
      .cooldown = 10ms,
  });
  expect(repeat.evaluate(true, start), "repeat gate should fire first edge");
  expect(!repeat.evaluate(false, start + 1ms), "reset edge should not fire");
  expect(
      !repeat.evaluate(true, start + 5ms),
      "edge inside cooldown should not fire");
  expect(!repeat.evaluate(false, start + 11ms), "falling edge should not fire");
  expect(
      repeat.evaluate(true, start + 12ms),
      "edge after cooldown should fire");
  expect(repeat.firingCount() == 2, "repeat gate should count firings");
}

void testRosTypedefSchema() {
  using dispatcher::ros::schemaFromRosapiTypedefs;
  using nlohmann::json;

  const json typedefs = json::array({
      {
          {"type", "upperlimb/MoveLRequest"},
          {"fieldnames", json::array({"pose", "v", "acc", "is_async"})},
          {"fieldtypes",
           json::array({"geometry_msgs/Pose", "float64", "float64", "bool"})},
          {"fieldarraylen", json::array({-1, -1, -1, -1})},
          {"examples", json::array({"{}" , "0.0", "0.0", "False"})},
      },
      {
          {"type", "geometry_msgs/Pose"},
          {"fieldnames", json::array({"position", "orientation"})},
          {"fieldtypes",
           json::array(
               {"geometry_msgs/Point", "geometry_msgs/Quaternion"})},
          {"fieldarraylen", json::array({-1, -1})},
          {"examples", json::array({"{}" , "{}"})},
      },
      {
          {"type", "geometry_msgs/Point"},
          {"fieldnames", json::array({"x", "y", "z"})},
          {"fieldtypes", json::array({"float64", "float64", "float64"})},
          {"fieldarraylen", json::array({-1, -1, -1})},
          {"examples", json::array({"0.0", "0.0", "0.0"})},
      },
      {
          {"type", "geometry_msgs/Quaternion"},
          {"fieldnames", json::array({"x", "y", "z", "w"})},
          {"fieldtypes",
           json::array({"float64", "float64", "float64", "float64"})},
          {"fieldarraylen", json::array({-1, -1, -1, -1})},
          {"examples", json::array({"0.0", "0.0", "0.0", "1.0"})},
      },
  });

  const auto result = schemaFromRosapiTypedefs(typedefs);
  expect(result.error.empty(), "typedef schema should succeed");
  const auto& props = result.parameter_schema.at("properties");
  expect(props.contains("pose"), "schema should include pose");
  expect(props.contains("v"), "schema should include v");
  expect(props.contains("acc"), "schema should include acc");
  expect(props.contains("is_async"), "schema should include is_async");
  expect(props.at("v").at("type") == "number", "v should be number");
  expect(props.at("is_async").at("type") == "boolean", "is_async boolean");
  expect(
      result.request_defaults.contains("v") &&
          result.request_defaults.at("v") == 0.0,
      "default v should be 0.0");
  expect(
      result.request_defaults.contains("is_async") &&
          result.request_defaults.at("is_async") == false,
      "default is_async should be false");

  // Fixed-length array: fieldarraylen>0 even when type has no [] suffix.
  const json fixed = json::array({
      {
          {"type", "demo/FixedPoseRequest"},
          {"fieldnames", json::array({"pose", "v"})},
          {"fieldtypes", json::array({"geometry_msgs/Pose", "float64"})},
          {"fieldarraylen", json::array({2, -1})},
          {"examples", json::array({"[]", "0.0"})},
      },
      {
          {"type", "geometry_msgs/Pose"},
          {"fieldnames", json::array({"position", "orientation"})},
          {"fieldtypes",
           json::array(
               {"geometry_msgs/Point", "geometry_msgs/Quaternion"})},
          {"fieldarraylen", json::array({-1, -1})},
          {"examples", json::array({"{}" , "{}"})},
      },
      {
          {"type", "geometry_msgs/Point"},
          {"fieldnames", json::array({"x", "y", "z"})},
          {"fieldtypes", json::array({"float64", "float64", "float64"})},
          {"fieldarraylen", json::array({-1, -1, -1})},
          {"examples", json::array({"0.0", "0.0", "0.0"})},
      },
      {
          {"type", "geometry_msgs/Quaternion"},
          {"fieldnames", json::array({"x", "y", "z", "w"})},
          {"fieldtypes",
           json::array({"float64", "float64", "float64", "float64"})},
          {"fieldarraylen", json::array({-1, -1, -1, -1})},
          {"examples", json::array({"0.0", "0.0", "0.0", "1.0"})},
      },
  });
  const auto fixed_result = schemaFromRosapiTypedefs(fixed);
  expect(
      fixed_result.parameter_schema.at("properties").at("pose").at("type") ==
          "array",
      "fixed pose should be array");
  expect(
      fixed_result.request_defaults.at("pose").is_array() &&
          fixed_result.request_defaults.at("pose").size() == 2,
      "fixed pose default should contain 2 items");

  const json dynamic = json::array({
      {
          {"type", "demo/DynamicRequest"},
          {"fieldnames", json::array({"values", "label"})},
          {"fieldtypes", json::array({"float64", "string"})},
          {"fieldarraylen", json::array({0, -1})},
          {"examples", json::array({"[]", ""})},
      },
  });
  const auto dynamic_result = schemaFromRosapiTypedefs(dynamic);
  const auto& dynamic_props = dynamic_result.parameter_schema.at("properties");
  expect(
      dynamic_props.at("values").at("type") == "array",
      "fieldarraylen=0 must be a dynamic array");
  expect(
      dynamic_result.request_defaults.at("values").is_array() &&
          dynamic_result.request_defaults.at("values").empty(),
      "dynamic array default should be empty");
  expect(
      dynamic_props.at("label").at("type") == "string",
      "fieldarraylen=-1 must remain scalar");
}

void testEventSpecPredicate() {
  using dispatcher::workflow::EventWhen;
  using dispatcher::workflow::evaluateWhen;
  using dispatcher::workflow::hasEmittableNodeEvent;
  using dispatcher::workflow::parseEventSpec;
  using dispatcher::workflow::resolveEventSpecs;
  using dispatcher::workflow::validateEventSpecsForOperation;
  using nlohmann::json;

  const json feedback{{"status", "grasped"}, {"progress", 0.8}};
  expect(
      evaluateWhen(
          EventWhen{.field = "status", .op = "eq", .value = "grasped"},
          feedback,
          true),
      "eq status should match");
  expect(
      !evaluateWhen(
          EventWhen{.field = "status", .op = "eq", .value = "idle"},
          feedback,
          true),
      "eq status mismatch");
  expect(
      evaluateWhen(
          EventWhen{.field = "progress", .op = "gte", .value = 0.5},
          feedback,
          true),
      "gte progress should match");
  expect(
      evaluateWhen(EventWhen{.op = "ros_success"}, json::object(), true),
      "ros_success true");
  expect(
      !evaluateWhen(EventWhen{.op = "ros_success"}, json::object(), false),
      "ros_success false");

  const auto specs = resolveEventSpecs(
      json::array(
          {{{"event_name", "from_cap"},
            {"source", "FEEDBACK"},
            {"when", {{"field", "status"}, {"op", "eq"}, {"value", "grasped"}}}}}),
      json::array(),
      "legacy_done");
  expect(specs.size() == 2, "resolve should keep capability + legacy RESULT");
  expect(specs[0].event_name == "from_cap", "first spec from capability");
  expect(specs[1].event_name == "legacy_done", "legacy success_event_name");
  expect(specs[1].source == "RESULT", "legacy source RESULT");
  expect(
      hasEmittableNodeEvent(specs, "from_cap"),
      "enabled emit_on_node event should be connectable");
  expect(
      !hasEmittableNodeEvent(
          std::vector<dispatcher::workflow::EventSpec>{
              parseEventSpec(
                  json{{"event_name", "disabled"}, {"enabled", false}}),
              parseEventSpec(
                  json{{"event_name", "workflow_only"},
                       {"emit_on_node", false}})},
          "workflow_only"),
      "disabled or workflow-only events must not be node event edges");

  bool rejected_negative_max = false;
  try {
    (void)parseEventSpec(
        json{{"event_name", "invalid"}, {"max_firings", -1}});
  } catch (const std::runtime_error&) {
    rejected_negative_max = true;
  }
  expect(
      rejected_negative_max,
      "negative event max_firings must be rejected before size_t conversion");

  validateEventSpecsForOperation(
      "SERVICE",
      json::array({{{"event_name", "service_done"}, {"source", "RESULT"}}}));
  validateEventSpecsForOperation(
      "ACTION",
      json::array({{{"event_name", "action_progress"},
                    {"source", "FEEDBACK"}}}));

  bool rejected_service_feedback = false;
  try {
    validateEventSpecsForOperation(
        "SERVICE",
        json::array({{{"event_name", "invalid_service_feedback"},
                      {"source", "FEEDBACK"}}}));
  } catch (const std::runtime_error&) {
    rejected_service_feedback = true;
  }
  expect(
      rejected_service_feedback,
      "SERVICE capabilities must reject FEEDBACK event specs");

  bool rejected_topic_feedback = false;
  try {
    validateEventSpecsForOperation(
        "TOPIC",
        json::array({{{"event_name", "invalid_topic_feedback"},
                      {"source", "FEEDBACK"}}}));
  } catch (const std::runtime_error&) {
    rejected_topic_feedback = true;
  }
  expect(
      rejected_topic_feedback,
      "outbound TOPIC capabilities must reject FEEDBACK event specs");
}

void testEdgeJoin() {
  using dispatcher::workflow::evaluateJoin;
  using nlohmann::json;

  const json edges = json::array(
      {{{"id", "e1"},
        {"source", "A"},
        {"target", "C"},
        {"edge_kind", "success"}},
       {{"id", "e2"},
        {"source", "B"},
        {"target", "C"},
        {"edge_kind", "success"}},
       {{"id", "e3"},
        {"source", "D"},
        {"target", "C"},
        {"edge_kind", "failure"}}});

  const auto waiting = evaluateJoin(
      edges,
      "C",
      1,
      {{"A", "SUCCEEDED"}, {"B", "RUNNING"}},
      {});
  expect(!waiting.ready, "two success incoming should wait for both");
  expect(
      waiting.waiting_on.size() == 1 && waiting.waiting_on[0] == "B",
      "join should report the unfinished success predecessor");

  const auto ready = evaluateJoin(
      edges,
      "C",
      1,
      {{"A", "SUCCEEDED"}, {"B", "SUCCEEDED"}},
      {});
  expect(ready.ready, "both success predecessors should release the join");
  expect(ready.waiting_on.empty(), "ready join should not wait");

  const json event_edges = json::array(
      {{{"id", "ev1"},
        {"source", "A"},
        {"target", "C"},
        {"edge_kind", "event"},
        {"event_name", "done_a"}},
       {{"id", "ev2"},
        {"source", "B"},
        {"target", "C"},
        {"edge_kind", "event"},
        {"event_name", "done_b"}}});
  const json first_event{
      {"edge_id", "ev1"},
      {"source", "A"},
      {"target", "C"},
      {"edge_kind", "event"},
      {"event_name", "done_a"},
      {"attempt", 1}};
  const auto event_waiting = evaluateJoin(
      event_edges, "C", 1, {}, {first_event});
  expect(!event_waiting.ready, "two event incoming should wait for both");

  const json second_event{
      {"edge_id", "ev2"},
      {"source", "B"},
      {"target", "C"},
      {"edge_kind", "event"},
      {"event_name", "done_b"},
      {"attempt", 1}};
  const auto event_ready = evaluateJoin(
      event_edges, "C", 1, {}, {first_event, second_event});
  expect(event_ready.ready, "both event edges should release the join");

  const json stale{
      {"edge_id", "ev2"},
      {"source", "B"},
      {"target", "C"},
      {"edge_kind", "event"},
      {"event_name", "done_b"},
      {"attempt", 1}};
  const auto wrong_cycle = evaluateJoin(
      event_edges, "C", 2, {}, {first_event, stale});
  expect(!wrong_cycle.ready, "previous cycle event traversal must not join");

  const json single = json::array(
      {{{"id", "s1"},
        {"source", "START"},
        {"target", "A"},
        {"edge_kind", "success"}}});
  const auto single_ready = evaluateJoin(
      single, "A", 1, {{"START", "SUCCEEDED"}}, {});
  expect(single_ready.ready, "single success incoming should activate immediately");
}

void testRosbridgeMessages() {
  using dispatcher::ros::Json;
  using dispatcher::ros::RosbridgeMessageFactory;

  const auto subscribe = RosbridgeMessageFactory::subscribe(
      "pose-sub",
      "/dispatch/pose",
      "geometry_msgs/msg/PoseStamped",
      {.throttle_rate_ms = 100, .queue_length = 1});
  expect(
      subscribe.at("op") == "subscribe",
      "subscribe operation should be encoded");
  expect(
      subscribe.at("topic") == "/dispatch/pose",
      "subscribe topic should be encoded");
  expect(
      subscribe.at("throttle_rate") == 100,
      "subscribe throttle should be encoded");

  const auto publish = RosbridgeMessageFactory::publish(
      "/dispatch/command",
      Json{{"command_id", "command-1"}},
      "std_msgs/msg/String");
  expect(publish.at("op") == "publish", "publish operation should be encoded");
  expect(
      publish.at("msg").at("command_id") == "command-1",
      "publish message should be preserved");
  expect(
      publish.at("type") == "std_msgs/msg/String",
      "publish type should be encoded");

  const auto service = RosbridgeMessageFactory::callService(
      "service-1",
      "/dispatch/load_map",
      Json{{"map_url", "/maps/scene-1.yaml"}},
      2.5);
  expect(
      service.at("op") == "call_service",
      "service operation should be encoded");
  expect(service.at("timeout") == 2.5, "service timeout should be encoded");

  const auto action = RosbridgeMessageFactory::sendActionGoal(
      "goal-1",
      "/dispatch/navigate",
      "nav2_msgs/action/NavigateToPose",
      Json{{"pose", Json{{"x", 1.0}}}});
  expect(
      action.at("op") == "send_action_goal",
      "action operation should be encoded");
  expect(action.at("id") == "goal-1", "action ID should be encoded");
  expect(
      action.at("feedback"),
      "action feedback must be requested by default");

  const auto cancel =
      RosbridgeMessageFactory::cancelActionGoal("goal-1", "/dispatch/navigate");
  expect(
      cancel.at("op") == "cancel_action_goal",
      "cancel operation should be encoded");
}

void testRosbridgeSession() {
  using dispatcher::domain::ConnectionState;
  using dispatcher::ros::ActionEvent;
  using dispatcher::ros::Json;
  using dispatcher::ros::RosbridgeSession;

  auto transport = std::make_shared<MockTransport>();
  RosbridgeSession session(
      transport,
      {.initial_delay = std::chrono::milliseconds{10},
       .max_delay = std::chrono::milliseconds{40}});

  session.onTransportConnected();
  expect(
      session.state() == ConnectionState::Online,
      "transport handshake should make session online");

  int pose_messages = 0;
  const auto subscription_id = session.subscribe(
      "/robot/pose",
      "geometry_msgs/msg/PoseStamped",
      {},
      [&](std::string_view topic, const Json& message) {
        ++pose_messages;
        expect(topic == "/robot/pose", "topic callback should preserve topic");
        expect(message.at("x") == 1, "topic callback should preserve message");
      });
  expect(!transport->sent.empty(), "online subscribe should send immediately");
  expect(
      transport->sent.back().at("op") == "subscribe",
      "session should send subscribe operation");
  expect(
      session.handleIncoming(
          R"({"op":"publish","topic":"/robot/pose","msg":{"x":1}})"),
      "publish message should be accepted");
  expect(pose_messages == 1, "topic callback should fire once");

  bool service_called = false;
  const auto service_id = session.callService(
      "/robot/load_map",
      Json{{"map_url", "/maps/a.yaml"}},
      2.0,
      [&](const dispatcher::ros::ServiceResponse& response) {
        service_called = response.success;
        expect(response.service == "/robot/load_map", "service name should correlate");
      });
  expect(
      session.handleIncoming(
          std::string{"{\"op\":\"service_response\",\"id\":\""} +
          service_id + R"(","result":true,"values":{"ok":true}})"),
      "service response should be accepted");
  expect(service_called, "service callback should receive success");

  const auto abandoned_service_id = session.callService(
      "/rosapi/services", Json::object(), 1.0, [](const auto&) {});
  expect(
      session.forgetServiceCall(abandoned_service_id),
      "timed-out service correlation should be removable");
  expect(
      !session.handleIncoming(
          std::string{"{\"op\":\"service_response\",\"id\":\""} +
          abandoned_service_id + R"(","result":true,"values":{}})"),
      "late response for a forgotten service call should be ignored");

  int feedback_count = 0;
  int result_count = 0;
  const auto goal_id = session.sendActionGoal(
      "/robot/navigate",
      "nav_msgs/action/NavigateToPose",
      Json{{"x", 2}},
      [&](const ActionEvent& event) {
        if (event.kind == ActionEvent::Kind::Feedback) {
          ++feedback_count;
        } else {
          ++result_count;
        }
      });
  expect(
      session.handleIncoming(
          std::string{"{\"op\":\"action_feedback\",\"id\":\""} +
          goal_id + R"(","values":{"distance":0.5}})"),
      "action feedback should be accepted");
  expect(
      session.handleIncoming(
          std::string{"{\"op\":\"action_result\",\"id\":\""} +
          goal_id + R"(","result":true,"values":{"done":true}})"),
      "action result should be accepted");
  expect(feedback_count == 1, "action feedback callback should fire once");
  expect(result_count == 1, "action result callback should fire once");
  expect(
      !session.handleIncoming(
          std::string{"{\"op\":\"action_result\",\"id\":\""} +
          goal_id + R"(","result":true})"),
      "completed action should not accept duplicate result");

  const auto now = RosbridgeSession::Clock::time_point{};
  session.onTransportClosed(now);
  expect(
      session.state() == ConnectionState::Reconnecting,
      "transport close should enter reconnecting state");
  expect(
      !session.tick(now + std::chrono::milliseconds{9}),
      "reconnect should wait for backoff");
  expect(
      session.tick(now + std::chrono::milliseconds{10}),
      "reconnect should become due after backoff");
  session.onTransportConnected();
  expect(
      transport->sent.back().at("id") == subscription_id,
      "subscription should be restored after reconnect");

  expect(session.unsubscribe(subscription_id), "subscription should unsubscribe");
  expect(!session.unsubscribe(subscription_id), "unknown subscription should fail");
}

void testInterfaceCatalogPartialMerge() {
  using dispatcher::interfaces::DiscoveredEndpoint;
  using dispatcher::interfaces::InterfaceCatalog;

  InterfaceCatalog previous;
  previous.topics.push_back(DiscoveredEndpoint{
      .name = "/old/topic", .operation_kind = "TOPIC"});
  previous.services.push_back(DiscoveredEndpoint{
      .name = "/old/service", .operation_kind = "SERVICE"});
  previous.actions.push_back(DiscoveredEndpoint{
      .name = "/old/action", .operation_kind = "ACTION"});

  InterfaceCatalog current;
  current.services.push_back(DiscoveredEndpoint{
      .name = "/robot_task", .operation_kind = "SERVICE"});
  current.topic_error = "topics timed out";
  current.action_error = "actions unavailable";
  current.preserveFailedCategoriesFrom(previous);

  expect(
      current.services.size() == 1 &&
          current.services.front().name == "/robot_task",
      "successful service scan should replace cached services");
  expect(
      current.topics.size() == 1 &&
          current.topics.front().name == "/old/topic",
      "failed topic scan should retain cached topics");
  expect(
      current.actions.size() == 1 &&
          current.actions.front().name == "/old/action",
      "failed action discovery should retain cached actions");

  const auto round_trip = InterfaceCatalog::fromJson(current.toJson());
  expect(
      round_trip.topic_error == "topics timed out" &&
          round_trip.service_error.empty(),
      "catalog category errors should survive JSON round trip");
}

void testActionResultParsing() {
  using dispatcher::ros::actionMessageGoalId;
  using dispatcher::ros::actionResultSucceeded;
  using nlohmann::json;

  // Flattened vehicle /result: actionlib status=3 AND NavigationState=6.
  const json flat = {
      {"causes", json::array()},
      {"distance_deviation", 0.008216971436484054},
      {"duration", 13.21315414},
      {"goal_id", "web_goal_d00841424d8c"},
      {"heading_deviation", 1.2893750376877873},
      {"state", 6},
      {"status", 3},
      {"text", ""}};
  expect(actionMessageGoalId(flat) == "web_goal_d00841424d8c",
         "string goal_id should be read from flattened result");
  const auto flat_ok = actionResultSucceeded(flat, true);
  expect(flat_ok.has_value() && *flat_ok,
         "flattened status=3 state=6 should be navigation success");

  // Nested actionlib ActionResult — previously misread status.status=3 as Arrived.
  const json nested = {
      {"status",
       {{"goal_id", {{"id", "cmd-1"}}}, {"status", 3}, {"text", ""}}},
      {"result",
       {{"duration", 13.2},
        {"distance_deviation", 0.008},
        {"heading_deviation", 1.29},
        {"state", {{"value", 6}}},
        {"causes", json::array()}}}};
  expect(actionMessageGoalId(nested) == "cmd-1",
         "nested GoalStatus.goal_id.id should match");
  const auto nested_ok = actionResultSucceeded(nested, true);
  expect(nested_ok.has_value() && *nested_ok,
         "ActionResult GoalStatus=3 and NavigationState=6 should succeed");

  // Vehicle rostopic echo /zj_humanoid/navigation/navigation/result
  const json vehicle_result = {
      {"header",
       {{"seq", 41},
        {"stamp", {{"secs", 1788856500}, {"nsecs", 981770511}}},
        {"frame_id", ""}}},
      {"status",
       {{"goal_id",
         {{"stamp", {{"secs", 1788856495}, {"nsecs", 211368876}}},
          {"id", "9e7bd567-e707-4d8e-bea9-8bb1f52c2973"}}},
        {"status", 3},
        {"text", ""}}},
      {"result",
       {{"header",
         {{"seq", 0},
          {"stamp", {{"secs", 1788856500}, {"nsecs", 981745134}}},
          {"frame_id", ""}}},
        {"duration", {{"secs", 5}, {"nsecs", 770126905}}},
        {"distance_deviation", 0.003069798689085083},
        {"heading_deviation", 0.5168647558594399},
        {"state", {{"value", 6}}},
        {"causes", json::array()}}}};
  expect(actionMessageGoalId(vehicle_result) ==
             "9e7bd567-e707-4d8e-bea9-8bb1f52c2973",
         "vehicle /result goal_id should match");
  const auto vehicle_ok = actionResultSucceeded(vehicle_result, true);
  expect(vehicle_ok.has_value() && *vehicle_ok,
         "vehicle /result status=3 and state.value=6 should succeed");

  const auto arrived_feedback = actionResultSucceeded(
      json{{"status",
            {{"status", 1},
             {"text", "This goal has been accepted by the simple action server"},
             {"goal_id", {{"id", "cmd-1"}}}}},
           {"feedback", {{"state", {{"value", 3}}}, {"faults", json::array()}}}},
      true);
  expect(!arrived_feedback.has_value(),
         "feedback Arrived(3) should wait for /result Succeeded(6)");

  const auto running_feedback = actionResultSucceeded(
      json{{"status", {{"status", 1}, {"goal_id", {{"id", "cmd-1"}}}}},
           {"feedback", {{"state", {{"value", 2}}}}}},
      true);
  expect(!running_feedback.has_value(),
         "feedback NavigationState Running(2) should keep waiting");

  const auto arrived_result = actionResultSucceeded(
      json{{"status", {{"status", 3}, {"goal_id", {{"id", "cmd-1"}}}}},
           {"result", {{"state", {{"value", 3}}}}}},
      true);
  expect(!arrived_result.has_value(),
         "NavigationState Arrived(3) should wait for Succeeded(6)");

  const auto only_actionlib = actionResultSucceeded(
      json{{"status", {{"status", 3}, {"goal_id", {{"id", "cmd-1"}}}}}},
      true);
  expect(!only_actionlib.has_value(),
         "actionlib SUCCEEDED(3) alone is not navigation success");

  const json failed = {
      {"status", {{"status", 4}, {"goal_id", {{"id", "cmd-1"}}}}},
      {"result", {{"state", {{"value", 7}}}}}};
  const auto failed_ok = actionResultSucceeded(failed, true);
  expect(failed_ok.has_value() && !*failed_ok,
         "NavigationState Failed(7) should fail the node");
}

void testPoseMapper() {
  using dispatcher::ros::Json;
  using dispatcher::ros::PoseFieldMapping;
  using dispatcher::ros::PoseMapper;
  using namespace std::chrono_literals;

  const auto received = std::chrono::system_clock::time_point{} + 10s + 250ms;
  const auto message = Json{
      {"header", {{"stamp", {{"sec", 10}, {"nanosec", 250'000'000}}},
                   {"frame_id", "map"}}},
      {"pose", {{"position", {{"x", 1.5}, {"y", -2.0}}},
                 {"orientation", {{"x", 0.0}, {"y", 0.0}, {"z", 0.70710678},
                                  {"w", 0.70710678}}}}},
      {"status", "localized"},
  };
  const auto parsed = PoseMapper::parse(
      message,
      PoseFieldMapping{
          .x_path = "pose.position.x",
          .y_path = "pose.position.y",
          .quaternion_x_path = "pose.orientation.x",
          .quaternion_y_path = "pose.orientation.y",
          .quaternion_z_path = "pose.orientation.z",
          .quaternion_w_path = "pose.orientation.w",
          .timestamp_path = "header.stamp",
          .frame_id_path = "header.frame_id",
          .localization_status_path = "status",
      },
      "robot-1",
      "scene-a",
      "map-v1",
      "map",
      received);
  expect(parsed.valid, "pose mapper should parse nested pose: " + parsed.error);
  if (parsed.valid) {
    expect(std::abs(parsed.pose.x - 1.5) < 1e-9, "pose x should be mapped");
    expect(std::abs(parsed.pose.y + 2.0) < 1e-9, "pose y should be mapped");
    expect(std::abs(parsed.pose.yaw - std::numbers::pi / 2.0) < 1e-6,
           "quaternion should be converted to yaw");
    expect(parsed.pose.frame_id == "map", "frame id should be mapped");
    expect(parsed.pose.localization_status == "localized",
           "localization status should be mapped");
    expect(PoseMapper::isFresh(parsed.pose, 300ms, received + 200ms),
           "recent pose should be fresh");
    expect(!PoseMapper::isFresh(parsed.pose, 300ms, received + 1s),
           "old pose should be stale");
  }

  const auto bad = PoseMapper::parse(
      Json{{"x", "bad"}, {"y", 1.0}},
      PoseFieldMapping{.x_path = "x", .y_path = "y", .yaw_path = "yaw"},
      "robot-1", "scene-a", "map-v1", "map", received);
  expect(!bad.valid, "invalid pose fields should be rejected");
}

}  // namespace

int main() {
  testExecutionRuntime();
  testDelayedRobotTask();
  testConnectionTransitions();
  testRobotConnectionConfig();
  testControlledSshValidation();
  testResourceMatrix();
  testNamedResources();
  testMapTransform();
  testMapImportCore();
  testFeedbackTrigger();
  testEventSpecPredicate();
  testEdgeJoin();
  testRosTypedefSchema();
  testRosbridgeMessages();
  testRosbridgeSession();
  testInterfaceCatalogPartialMerge();
  testActionResultParsing();
  testPoseMapper();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "all dispatcher core tests passed\n";
  return EXIT_SUCCESS;
}
