#include "dispatcher/domain/map_transform.hpp"

#include <cmath>
#include <numbers>

namespace dispatcher::domain {
namespace {

double normalizeAngle(double angle) noexcept {
  constexpr double two_pi = 2.0 * std::numbers::pi;
  angle = std::fmod(angle + std::numbers::pi, two_pi);
  if (angle < 0.0) {
    angle += two_pi;
  }
  return angle - std::numbers::pi;
}

}  // namespace

bool isValid(const MapMetadata& metadata) noexcept {
  return metadata.width > 0 && metadata.height > 0 &&
         std::isfinite(metadata.resolution) && metadata.resolution > 0.0 &&
         std::isfinite(metadata.origin_x) &&
         std::isfinite(metadata.origin_y) &&
         std::isfinite(metadata.origin_yaw);
}

std::optional<PixelPose2D> worldToPixel(
    const MapMetadata& metadata,
    const WorldPose2D& pose) noexcept {
  if (!isValid(metadata) || !std::isfinite(pose.x) ||
      !std::isfinite(pose.y) || !std::isfinite(pose.yaw)) {
    return std::nullopt;
  }

  const double dx = pose.x - metadata.origin_x;
  const double dy = pose.y - metadata.origin_y;
  const double cos_origin = std::cos(metadata.origin_yaw);
  const double sin_origin = std::sin(metadata.origin_yaw);

  const double local_x = cos_origin * dx + sin_origin * dy;
  const double local_y = -sin_origin * dx + cos_origin * dy;
  const double pixel_x = local_x / metadata.resolution;
  const double pixel_y =
      static_cast<double>(metadata.height - 1U) -
      local_y / metadata.resolution;

  const bool inside_map =
      pixel_x >= 0.0 && pixel_y >= 0.0 &&
      pixel_x < static_cast<double>(metadata.width) &&
      pixel_y < static_cast<double>(metadata.height);

  return PixelPose2D{
      .x = pixel_x,
      .y = pixel_y,
      .yaw = normalizeAngle(-(pose.yaw - metadata.origin_yaw)),
      .inside_map = inside_map,
  };
}

std::optional<WorldPose2D> pixelToWorld(
    const MapMetadata& metadata,
    const PixelPose2D& pose) noexcept {
  if (!isValid(metadata) || !std::isfinite(pose.x) ||
      !std::isfinite(pose.y) || !std::isfinite(pose.yaw)) {
    return std::nullopt;
  }

  const double local_x = pose.x * metadata.resolution;
  const double local_y =
      (static_cast<double>(metadata.height - 1U) - pose.y) *
      metadata.resolution;
  const double cos_origin = std::cos(metadata.origin_yaw);
  const double sin_origin = std::sin(metadata.origin_yaw);
  const double dx = cos_origin * local_x - sin_origin * local_y;
  const double dy = sin_origin * local_x + cos_origin * local_y;

  return WorldPose2D{
      .x = metadata.origin_x + dx,
      .y = metadata.origin_y + dy,
      .yaw = normalizeAngle(metadata.origin_yaw - pose.yaw),
  };
}

PlanarQuaternion yawToQuaternion(double yaw) noexcept {
  const double half_yaw = yaw / 2.0;
  return PlanarQuaternion{
      .x = 0.0,
      .y = 0.0,
      .z = std::sin(half_yaw),
      .w = std::cos(half_yaw),
  };
}

}  // namespace dispatcher::domain
