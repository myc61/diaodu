#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace dispatcher::domain {

enum class RosVersion {
  Ros1,
  Ros2,
};

enum class OperationKind {
  Topic,
  Service,
  Action,
  Config,
};

enum class BlockingType {
  Hard,
  Soft,
  None,
  Navigation,
};

enum class ResourceAccess {
  Shared,
  Exclusive,
};

enum class ConnectionState {
  Disconnected,
  Connecting,
  Online,
  Degraded,
  Reconnecting,
  Disabled,
};

struct ResourceClaim {
  std::string name;
  ResourceAccess access{ResourceAccess::Shared};

  bool operator==(const ResourceClaim&) const = default;
};

struct LeaseRequest {
  std::string lease_id;
  std::string owner_id;
  std::string robot_id;
  BlockingType blocking_type{BlockingType::None};
  std::vector<ResourceClaim> additional_claims;
};

struct ResourceLease {
  std::string lease_id;
  std::string owner_id;
  std::string robot_id;
  BlockingType blocking_type{BlockingType::None};
  std::vector<ResourceClaim> claims;
  std::chrono::steady_clock::time_point acquired_at;
};

struct RobotPose {
  std::string robot_id;
  std::string scene_id;
  std::string map_version_id;
  std::string frame_id;
  double x{0.0};
  double y{0.0};
  double yaw{0.0};
  std::chrono::system_clock::time_point timestamp;
  std::string localization_status{"unknown"};
};

}  // namespace dispatcher::domain

