#include "dispatcher/domain/resource_arbiter.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace dispatcher::domain {
namespace {

std::vector<ResourceClaim> mergeClaims(
    std::vector<ResourceClaim> claims) {
  std::map<std::string, ResourceAccess> merged;

  for (const auto& claim : claims) {
    if (claim.name.empty()) {
      continue;
    }

    const auto [iterator, inserted] =
        merged.emplace(claim.name, claim.access);
    if (!inserted && claim.access == ResourceAccess::Exclusive) {
      iterator->second = ResourceAccess::Exclusive;
    }
  }

  std::vector<ResourceClaim> result;
  result.reserve(merged.size());
  for (const auto& [name, access] : merged) {
    result.push_back(ResourceClaim{.name = name, .access = access});
  }
  return result;
}

}  // namespace

AcquireResult ResourceArbiter::tryAcquire(const LeaseRequest& request) {
  if (request.lease_id.empty()) {
    return {
        .acquired = false,
        .reason = "lease_id must not be empty",
        .lease = std::nullopt,
    };
  }
  if (request.owner_id.empty()) {
    return {
        .acquired = false,
        .reason = "owner_id must not be empty",
        .lease = std::nullopt,
    };
  }
  if (request.robot_id.empty()) {
    return {
        .acquired = false,
        .reason = "robot_id must not be empty",
        .lease = std::nullopt,
    };
  }

  auto claims = defaultClaims(request.robot_id, request.blocking_type);
  claims.insert(
      claims.end(),
      request.additional_claims.begin(),
      request.additional_claims.end());
  claims = mergeClaims(std::move(claims));

  std::lock_guard lock(mutex_);
  if (leases_.contains(request.lease_id)) {
    return {
        .acquired = false,
        .reason = "lease_id already exists",
        .lease = std::nullopt,
    };
  }

  for (const auto& [active_id, active_lease] : leases_) {
    static_cast<void>(active_id);
    for (const auto& requested_claim : claims) {
      const auto conflict = std::find_if(
          active_lease.claims.begin(),
          active_lease.claims.end(),
          [&requested_claim](const ResourceClaim& active_claim) {
            return conflicts(requested_claim, active_claim);
          });

      if (conflict != active_lease.claims.end()) {
        return {
            .acquired = false,
            .reason =
                "resource is held by lease " + active_lease.lease_id +
                ": " + requested_claim.name,
            .lease = std::nullopt,
        };
      }
    }
  }

  ResourceLease lease{
      .lease_id = request.lease_id,
      .owner_id = request.owner_id,
      .robot_id = request.robot_id,
      .blocking_type = request.blocking_type,
      .claims = std::move(claims),
      .acquired_at = std::chrono::steady_clock::now(),
  };
  leases_.emplace(lease.lease_id, lease);

  return {
      .acquired = true,
      .reason = {},
      .lease = std::move(lease),
  };
}

bool ResourceArbiter::release(const std::string& lease_id) {
  std::lock_guard lock(mutex_);
  return leases_.erase(lease_id) > 0;
}

bool ResourceArbiter::contains(const std::string& lease_id) const {
  std::lock_guard lock(mutex_);
  return leases_.contains(lease_id);
}

std::vector<ResourceLease> ResourceArbiter::activeForRobot(
    const std::string& robot_id) const {
  std::lock_guard lock(mutex_);
  std::vector<ResourceLease> result;
  for (const auto& [lease_id, lease] : leases_) {
    static_cast<void>(lease_id);
    if (lease.robot_id == robot_id) {
      result.push_back(lease);
    }
  }
  return result;
}

std::size_t ResourceArbiter::size() const {
  std::lock_guard lock(mutex_);
  return leases_.size();
}

std::vector<ResourceClaim> ResourceArbiter::defaultClaims(
    const std::string& robot_id,
    BlockingType blocking_type) {
  const std::string robot_resource = "robot/" + robot_id;
  const std::string motion_resource = robot_resource + "/motion";

  switch (blocking_type) {
    case BlockingType::Hard:
      return {{
          .name = robot_resource,
          .access = ResourceAccess::Exclusive,
      }};
    case BlockingType::Soft:
      return {
          {
              .name = robot_resource,
              .access = ResourceAccess::Shared,
          },
          {
              .name = motion_resource,
              .access = ResourceAccess::Shared,
          },
      };
    case BlockingType::None:
      return {{
          .name = robot_resource,
          .access = ResourceAccess::Shared,
      }};
    case BlockingType::Navigation:
      return {
          {
              .name = robot_resource,
              .access = ResourceAccess::Shared,
          },
          {
              .name = motion_resource,
              .access = ResourceAccess::Exclusive,
          },
      };
  }

  return {};
}

bool ResourceArbiter::conflicts(
    const ResourceClaim& lhs,
    const ResourceClaim& rhs) noexcept {
  if (lhs.name != rhs.name) {
    return false;
  }

  return lhs.access == ResourceAccess::Exclusive ||
         rhs.access == ResourceAccess::Exclusive;
}

}  // namespace dispatcher::domain
