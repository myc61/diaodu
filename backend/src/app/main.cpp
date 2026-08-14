#include "dispatcher/domain/connection_state.hpp"
#include "dispatcher/domain/map_transform.hpp"
#include "dispatcher/domain/resource_arbiter.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
  using dispatcher::domain::BlockingType;
  using dispatcher::domain::ConnectionState;
  using dispatcher::domain::ConnectionStateMachine;
  using dispatcher::domain::LeaseRequest;
  using dispatcher::domain::MapMetadata;
  using dispatcher::domain::ResourceArbiter;
  using dispatcher::domain::WorldPose2D;

  const bool self_check =
      argc == 2 && std::string_view(argv[1]) == "--self-check";
  if (!self_check) {
    std::cout
        << "dispatcher core is ready; use --self-check to validate it\n";
    return 0;
  }

  ConnectionStateMachine connection;
  const bool connection_ok =
      connection.transitionTo(ConnectionState::Connecting) &&
      connection.transitionTo(ConnectionState::Online);

  ResourceArbiter arbiter;
  const auto lease = arbiter.tryAcquire(LeaseRequest{
      .lease_id = "self-check-lease",
      .owner_id = "self-check",
      .robot_id = "robot-1",
      .blocking_type = BlockingType::Navigation,
  });

  const auto pixel_pose = dispatcher::domain::worldToPixel(
      MapMetadata{
          .width = 100,
          .height = 100,
          .resolution = 0.05,
          .origin_x = 0.0,
          .origin_y = 0.0,
          .origin_yaw = 0.0,
      },
      WorldPose2D{.x = 1.0, .y = 1.0, .yaw = 0.0});

  const bool healthy =
      connection_ok && lease.acquired && pixel_pose.has_value();
  std::cout
      << "{\"status\":\"" << (healthy ? "ok" : "failed")
      << "\",\"connection\":\""
      << dispatcher::domain::toString(connection.state())
      << "\",\"active_leases\":" << arbiter.size() << "}\n";

  return healthy ? 0 : 1;
}

