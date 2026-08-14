#include "dispatcher/ros/pose_cache.hpp"

namespace dispatcher::ros {

void PoseCache::upsert(CachedPose pose) {
  std::lock_guard lock(mutex_);
  poses_[pose.pose.robot_id] = std::move(pose);
}

std::optional<CachedPose> PoseCache::get(const std::string& robot_id) const {
  std::lock_guard lock(mutex_);
  const auto it = poses_.find(robot_id);
  if (it == poses_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<CachedPose> PoseCache::snapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<CachedPose> values;
  values.reserve(poses_.size());
  for (const auto& [_, pose] : poses_) {
    values.push_back(pose);
  }
  return values;
}

}  // namespace dispatcher::ros
