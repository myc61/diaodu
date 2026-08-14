#pragma once

#include "dispatcher/maps/pgm.hpp"

#include <string>
#include <vector>

namespace dispatcher::maps {

[[nodiscard]] std::vector<std::uint8_t> encodeGrayscalePng(
    const PgmImage& image);

[[nodiscard]] bool writeBinaryFile(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes);

[[nodiscard]] bool writeTextFile(const std::string& path, std::string_view text);

}  // namespace dispatcher::maps
