#pragma once

#include "dispatcher/domain/types.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dispatcher::domain {

struct AcquireResult {
  bool acquired{false};
  std::string reason;
  std::optional<ResourceLease> lease;
};

class ResourceArbiter {
 public:
  AcquireResult tryAcquire(const LeaseRequest& request);
  bool release(const std::string& lease_id);

  [[nodiscard]] bool contains(const std::string& lease_id) const;
  [[nodiscard]] std::vector<ResourceLease> activeForRobot(
      const std::string& robot_id) const;
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] static std::vector<ResourceClaim> defaultClaims(
      const std::string& robot_id,
      BlockingType blocking_type);

 private:
  [[nodiscard]] static bool conflicts(
      const ResourceClaim& lhs,
      const ResourceClaim& rhs) noexcept;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, ResourceLease> leases_;
};

}  // namespace dispatcher::domain

