#pragma once

#include <cstddef>
#include <optional>

namespace dispatcher::domain {

struct MapMetadata {
  std::size_t width{0};
  std::size_t height{0};
  double resolution{0.0};
  double origin_x{0.0};
  double origin_y{0.0};
  double origin_yaw{0.0};
};

struct WorldPose2D {
  double x{0.0};
  double y{0.0};
  double yaw{0.0};
};

struct PixelPose2D {
  double x{0.0};
  double y{0.0};
  double yaw{0.0};
  bool inside_map{false};
};

struct PlanarQuaternion {
  double x{0.0};
  double y{0.0};
  double z{0.0};
  double w{1.0};
};

[[nodiscard]] bool isValid(const MapMetadata& metadata) noexcept;

[[nodiscard]] std::optional<PixelPose2D> worldToPixel(
    const MapMetadata& metadata,
    const WorldPose2D& pose) noexcept;

[[nodiscard]] std::optional<WorldPose2D> pixelToWorld(
    const MapMetadata& metadata,
    const PixelPose2D& pose) noexcept;

[[nodiscard]] PlanarQuaternion yawToQuaternion(double yaw) noexcept;

}  // namespace dispatcher::domain
