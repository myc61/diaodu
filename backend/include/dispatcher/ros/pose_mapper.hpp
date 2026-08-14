#pragma once

#include "dispatcher/domain/types.hpp"
#include "dispatcher/ros/rosbridge_protocol.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace dispatcher::ros {

struct PoseFieldMapping {
  std::string x_path;
  std::string y_path;
  std::optional<std::string> yaw_path;
  std::optional<std::string> quaternion_x_path;
  std::optional<std::string> quaternion_y_path;
  std::optional<std::string> quaternion_z_path;
  std::optional<std::string> quaternion_w_path;
  std::optional<std::string> timestamp_path;
  std::optional<std::string> frame_id_path;
  std::optional<std::string> scene_id_path;
  std::optional<std::string> map_version_id_path;
  std::optional<std::string> localization_status_path;
};

struct PoseMappingResult {
  bool valid{false};
  domain::RobotPose pose;
  std::string error;
};

class PoseMapper {
 public:
  [[nodiscard]] static PoseMappingResult parse(
      const Json& message,
      const PoseFieldMapping& mapping,
      std::string robot_id,
      std::string fallback_scene_id,
      std::string fallback_map_version_id,
      std::string fallback_frame_id,
      std::chrono::system_clock::time_point received_at =
          std::chrono::system_clock::now());

  [[nodiscard]] static bool isFresh(
      const domain::RobotPose& pose,
      std::chrono::milliseconds stale_timeout,
      std::chrono::system_clock::time_point now =
          std::chrono::system_clock::now());
};

}  // namespace dispatcher::ros
