#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dispatcher::maps {

struct PgmImage {
  std::size_t width{0};
  std::size_t height{0};
  std::uint16_t max_value{255};
  std::vector<std::uint8_t> pixels;
};

struct PgmParseResult {
  bool ok{false};
  PgmImage image;
  std::string error;
};

[[nodiscard]] PgmParseResult parsePgm(std::string_view bytes);

}  // namespace dispatcher::maps
