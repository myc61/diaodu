#pragma once

#include "dispatcher/domain/types.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dispatcher::ros {

struct CachedPose {
  domain::RobotPose pose;
  bool stale{true};
  bool scene_map_matched{false};
  std::chrono::system_clock::time_point updated_at;
};

struct CachedBattery {
  std::string robot_id;
  double percentage{0.0};
  double voltage{0.0};
  bool present{true};
  std::chrono::system_clock::time_point updated_at;
};

class PoseCache {
 public:
  void upsert(CachedPose pose);
  [[nodiscard]] std::optional<CachedPose> get(const std::string& robot_id) const;
  [[nodiscard]] std::vector<CachedPose> snapshot() const;

  void upsertBattery(CachedBattery battery);
  [[nodiscard]] std::optional<CachedBattery> getBattery(
      const std::string& robot_id) const;
  void eraseBattery(const std::string& robot_id);

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CachedPose> poses_;
  std::unordered_map<std::string, CachedBattery> batteries_;
};

}  // namespace dispatcher::ros
