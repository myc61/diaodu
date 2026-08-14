#include "dispatcher/ros/pose_mapper.hpp"

#include <cmath>

namespace dispatcher::ros {
namespace {

const Json* findPath(const Json& root, const std::string& path) {
  if (path.empty()) {
    return nullptr;
  }
  const Json* current = &root;
  std::size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('.', start);
    const auto segment = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (segment.empty()) {
      return nullptr;
    }
    if (current->is_object()) {
      const auto it = current->find(segment);
      if (it == current->end()) {
        return nullptr;
      }
      current = &*it;
    } else if (current->is_array()) {
      try {
        const auto index = std::stoull(segment);
        if (index >= current->size()) {
          return nullptr;
        }
        current = &(*current)[index];
      } catch (...) {
        return nullptr;
      }
    } else {
      return nullptr;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return current;
}

std::optional<double> numberAt(const Json& root, const std::string& path) {
  const auto* value = findPath(root, path);
  if (value == nullptr || !value->is_number()) {
    return std::nullopt;
  }
  return value->get<double>();
}

std::optional<std::string> stringAt(
    const Json& root, const std::optional<std::string>& path) {
  if (!path.has_value()) {
    return std::nullopt;
  }
  const auto* value = findPath(root, *path);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  return value->get<std::string>();
}

std::optional<std::chrono::system_clock::time_point> timeAt(
    const Json& root, const std::optional<std::string>& path) {
  if (!path.has_value()) {
    return std::nullopt;
  }
  const auto* value = findPath(root, *path);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->is_number()) {
    const auto seconds = value->get<double>();
    if (!std::isfinite(seconds) || seconds < 0.0) {
      return std::nullopt;
    }
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>{seconds})};
  }
  if (!value->is_object()) {
    return std::nullopt;
  }
  const auto sec_it = value->find("sec");
  const auto nsec_it = value->find("nanosec");
  const auto nsecs_it = value->find("nsec");
  const auto secs_it = value->find("secs");
  const auto nsecs_alt_it = value->find("nsecs");
  const Json* seconds = sec_it != value->end() ? &*sec_it
                         : secs_it != value->end() ? &*secs_it
                                                   : nullptr;
  const Json* nanoseconds = nsec_it != value->end()
                                ? &*nsec_it
                                : nsecs_it != value->end()
                                      ? &*nsecs_it
                                      : nsecs_alt_it != value->end()
                                            ? &*nsecs_alt_it
                                            : nullptr;
  if (seconds == nullptr || nanoseconds == nullptr ||
      !seconds->is_number_integer() || !nanoseconds->is_number_integer()) {
    return std::nullopt;
  }
  const auto sec_count = seconds->get<std::int64_t>();
  const auto nsec_count = nanoseconds->get<std::int64_t>();
  if (sec_count < 0 || nsec_count < 0 || nsec_count >= 1'000'000'000) {
    return std::nullopt;
  }
  return std::chrono::system_clock::time_point{
      std::chrono::seconds{sec_count} + std::chrono::nanoseconds{nsec_count}};
}

}  // namespace

PoseMappingResult PoseMapper::parse(
    const Json& message,
    const PoseFieldMapping& mapping,
    std::string robot_id,
    std::string fallback_scene_id,
    std::string fallback_map_version_id,
    std::string fallback_frame_id,
    std::chrono::system_clock::time_point received_at) {
  PoseMappingResult result;
  const auto x = numberAt(message, mapping.x_path);
  const auto y = numberAt(message, mapping.y_path);
  if (!x.has_value() || !y.has_value() || !std::isfinite(*x) ||
      !std::isfinite(*y)) {
    result.error = "x/y field is missing or not numeric";
    return result;
  }

  double yaw = 0.0;
  if (mapping.yaw_path.has_value()) {
    const auto value = numberAt(message, *mapping.yaw_path);
    if (!value.has_value() || !std::isfinite(*value)) {
      result.error = "yaw field is missing or not numeric";
      return result;
    }
    yaw = *value;
  } else if (mapping.quaternion_x_path.has_value() &&
             mapping.quaternion_y_path.has_value() &&
             mapping.quaternion_z_path.has_value() &&
             mapping.quaternion_w_path.has_value()) {
    const auto qx = numberAt(message, *mapping.quaternion_x_path);
    const auto qy = numberAt(message, *mapping.quaternion_y_path);
    const auto qz = numberAt(message, *mapping.quaternion_z_path);
    const auto qw = numberAt(message, *mapping.quaternion_w_path);
    if (!qx.has_value() || !qy.has_value() || !qz.has_value() ||
        !qw.has_value()) {
      result.error = "quaternion field is incomplete";
      return result;
    }
    const auto norm = std::sqrt(*qx * *qx + *qy * *qy + *qz * *qz + *qw * *qw);
    if (!std::isfinite(norm) || norm < 1e-12) {
      result.error = "quaternion norm is invalid";
      return result;
    }
    const auto nx = *qx / norm;
    const auto ny = *qy / norm;
    const auto nz = *qz / norm;
    const auto nw = *qw / norm;
    yaw = std::atan2(2.0 * (nw * nz + nx * ny),
                     1.0 - 2.0 * (ny * ny + nz * nz));
  } else {
    result.error = "yaw or complete quaternion mapping is required";
    return result;
  }

  const auto timestamp = timeAt(message, mapping.timestamp_path);
  if (mapping.timestamp_path.has_value() && !timestamp.has_value()) {
    result.error = "timestamp field is invalid";
    return result;
  }

  result.valid = true;
  result.pose.robot_id = std::move(robot_id);
  result.pose.scene_id = stringAt(message, mapping.scene_id_path)
                             .value_or(std::move(fallback_scene_id));
  result.pose.map_version_id =
      stringAt(message, mapping.map_version_id_path)
          .value_or(std::move(fallback_map_version_id));
  result.pose.frame_id =
      stringAt(message, mapping.frame_id_path).value_or(std::move(fallback_frame_id));
  result.pose.x = *x;
  result.pose.y = *y;
  result.pose.yaw = yaw;
  result.pose.timestamp = timestamp.value_or(received_at);
  result.pose.localization_status =
      stringAt(message, mapping.localization_status_path).value_or("unknown");
  return result;
}

bool PoseMapper::isFresh(
    const domain::RobotPose& pose,
    std::chrono::milliseconds stale_timeout,
    std::chrono::system_clock::time_point now) {
  if (stale_timeout.count() < 0) {
    return false;
  }
  const auto age = now - pose.timestamp;
  if (age < std::chrono::system_clock::duration::zero()) {
    // Robot clocks slightly ahead of wall clock still count as fresh.
    return -age <= stale_timeout;
  }
  return age <= stale_timeout;
}

}  // namespace dispatcher::ros
