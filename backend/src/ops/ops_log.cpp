#include "dispatcher/ops/ops_log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <utility>

namespace dispatcher::ops {

std::string localIsoNow() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const auto time = system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&time, &local);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
  char out[40];
  std::snprintf(out, sizeof(out), "%s.%03d", buf, static_cast<int>(ms.count()));
  return out;
}

std::int64_t localUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

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

void OpsLog::clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  next_id_ = 1;
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
  entries_.push_back(entry);
  std::cerr << localIsoNow() << " [" << entry.level << "][" << entry.source
            << "] " << entry.message;
  if (!entry.detail.is_null() && !entry.detail.empty()) {
    std::cerr << ' ' << entry.detail.dump();
  }
  std::cerr << '\n';
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
