#include "dispatcher/api/app_state.hpp"
#include "dispatcher/api/register_routes.hpp"

#include <drogon/drogon.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

std::uint16_t serverPort() {
  constexpr std::uint16_t default_port = 8080;
  const char* value = std::getenv("DISPATCHER_HTTP_PORT");
  if (value == nullptr) {
    return default_port;
  }

  try {
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > 65535) {
      return default_port;
    }
    return static_cast<std::uint16_t>(parsed);
  } catch (...) {
    return default_port;
  }
}

std::size_t maxBodySize() {
  constexpr std::size_t default_size = 64 * 1024 * 1024;
  const char* value = std::getenv("DISPATCHER_MAX_BODY_SIZE");
  if (value == nullptr || *value == '\0') {
    return default_size;
  }
  try {
    const auto parsed = std::stoull(value);
    if (parsed < 1024 * 1024) {
      return default_size;
    }
    return static_cast<std::size_t>(parsed);
  } catch (...) {
    return default_size;
  }
}

}  // namespace

int main() {
  dispatcher::api::initializeAppState();
  dispatcher::api::registerRoutes();

  const auto max_body_size = maxBodySize();
  drogon::app()
      .addListener("0.0.0.0", serverPort())
      .setThreadNum(2)
      .setClientMaxBodySize(max_body_size)
      .setClientMaxMemoryBodySize(max_body_size)
      .run();
}
