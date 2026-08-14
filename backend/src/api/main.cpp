#include "dispatcher/api/app_state.hpp"
#include "dispatcher/api/register_routes.hpp"

#include <drogon/drogon.h>

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

}  // namespace

int main() {
  dispatcher::api::initializeAppState();
  dispatcher::api::registerRoutes();

  drogon::app()
      .addListener("0.0.0.0", serverPort())
      .setThreadNum(2)
      .run();
}
