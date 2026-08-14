#include "dispatcher/maps/pgm.hpp"

#include <cctype>
#include <cstdlib>

namespace dispatcher::maps {
namespace {

void skipWhitespaceAndComments(std::string_view bytes, std::size_t& index) {
  while (index < bytes.size()) {
    const unsigned char ch = static_cast<unsigned char>(bytes[index]);
    if (std::isspace(ch) != 0) {
      ++index;
      continue;
    }
    if (ch == '#') {
      while (index < bytes.size() && bytes[index] != '\n') {
        ++index;
      }
      continue;
    }
    break;
  }
}

bool readToken(std::string_view bytes, std::size_t& index, std::string& token) {
  skipWhitespaceAndComments(bytes, index);
  if (index >= bytes.size()) {
    return false;
  }
  const std::size_t begin = index;
  while (index < bytes.size() &&
         std::isspace(static_cast<unsigned char>(bytes[index])) == 0 &&
         bytes[index] != '#') {
    ++index;
  }
  token = std::string(bytes.substr(begin, index - begin));
  return !token.empty();
}

}  // namespace

PgmParseResult parsePgm(std::string_view bytes) {
  PgmParseResult result;
  std::size_t index = 0;
  std::string magic;
  if (!readToken(bytes, index, magic) || (magic != "P2" && magic != "P5")) {
    result.error = "PGM must start with P2 or P5";
    return result;
  }

  std::string width_token;
  std::string height_token;
  std::string max_token;
  if (!readToken(bytes, index, width_token) ||
      !readToken(bytes, index, height_token) ||
      !readToken(bytes, index, max_token)) {
    result.error = "PGM header is incomplete";
    return result;
  }

  try {
    result.image.width = static_cast<std::size_t>(std::stoul(width_token));
    result.image.height = static_cast<std::size_t>(std::stoul(height_token));
    result.image.max_value =
        static_cast<std::uint16_t>(std::stoul(max_token));
  } catch (...) {
    result.error = "PGM dimensions are invalid";
    return result;
  }

  if (result.image.width == 0 || result.image.height == 0 ||
      result.image.max_value == 0 || result.image.max_value > 255) {
    result.error = "PGM dimensions/max value out of supported range";
    return result;
  }

  const std::size_t pixel_count = result.image.width * result.image.height;
  result.image.pixels.resize(pixel_count);

  if (magic == "P5") {
    // PNM binary payload begins after exactly one whitespace following maxval.
    if (index >= bytes.size() ||
        std::isspace(static_cast<unsigned char>(bytes[index])) == 0) {
      result.error = "PGM binary payload missing whitespace separator";
      return result;
    }
    ++index;
    if (bytes.size() - index < pixel_count) {
      result.error = "PGM binary payload truncated";
      return result;
    }
    for (std::size_t i = 0; i < pixel_count; ++i) {
      result.image.pixels[i] = static_cast<std::uint8_t>(bytes[index + i]);
    }
  } else {
    for (std::size_t i = 0; i < pixel_count; ++i) {
      std::string value;
      if (!readToken(bytes, index, value)) {
        result.error = "PGM ascii payload truncated";
        return result;
      }
      try {
        const auto parsed = std::stoul(value);
        if (parsed > result.image.max_value) {
          result.error = "PGM pixel exceeds max value";
          return result;
        }
        result.image.pixels[i] = static_cast<std::uint8_t>(parsed);
      } catch (...) {
        result.error = "PGM ascii pixel is invalid";
        return result;
      }
    }
  }

  if (result.image.max_value != 255) {
    for (auto& pixel : result.image.pixels) {
      pixel = static_cast<std::uint8_t>(
          (static_cast<unsigned>(pixel) * 255U) /
          result.image.max_value);
    }
    result.image.max_value = 255;
  }

  result.ok = true;
  return result;
}

}  // namespace dispatcher::maps
