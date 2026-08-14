#include "dispatcher/ops/ops_log.hpp"

namespace dispatcher::ops {

OpsLog& OpsLog::instance() {
  static OpsLog log;
  return log;
}

void OpsLog::info(std::string source, std::string message,
                  nlohmann::json detail) {
  append("info", std::move(source), std::move(message), std::move(detail));
}

void OpsLog::warn(std::string source, std::string message,
                  nlohmann::json detail) {
  append("warn", std::move(source), std::move(message), std::move(detail));
}

void OpsLog::error(std::string source, std::string message,
                   nlohmann::json detail) {
  append("error", std::move(source), std::move(message), std::move(detail));
}

void OpsLog::append(std::string level, std::string source, std::string message,
                    nlohmann::json detail) {
  std::lock_guard lock(mutex_);
  OpsLogEntry entry{
      .id = next_id_++,
      .level = std::move(level),
      .source = std::move(source),
      .message = std::move(message),
      .detail = std::move(detail),
      .at = std::chrono::system_clock::now(),
  };
  entries_.push_back(std::move(entry));
  while (entries_.size() > kCapacity) {
    entries_.pop_front();
  }
}

std::vector<OpsLogEntry> OpsLog::list(std::size_t limit,
                                      std::int64_t after_id) const {
  std::lock_guard lock(mutex_);
  std::vector<OpsLogEntry> out;
  out.reserve(std::min(limit, entries_.size()));
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->id <= after_id) {
      continue;
    }
    out.push_back(*it);
    if (out.size() >= limit) {
      break;
    }
  }
  return out;
}

}  // namespace dispatcher::ops
