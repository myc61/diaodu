#include "dispatcher/maps/png_writer.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <zlib.h>

namespace dispatcher::maps {
namespace {

constexpr std::array<std::uint32_t, 256> makeCrcTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
    }
    table[n] = c;
  }
  return table;
}

std::uint32_t crc32Png(const std::uint8_t* data, std::size_t length) {
  static constexpr auto table = makeCrcTable();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

void appendUint32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void appendChunk(
    std::vector<std::uint8_t>& out,
    const char type[4],
    const std::vector<std::uint8_t>& data) {
  appendUint32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t type_offset = out.size();
  out.push_back(static_cast<std::uint8_t>(type[0]));
  out.push_back(static_cast<std::uint8_t>(type[1]));
  out.push_back(static_cast<std::uint8_t>(type[2]));
  out.push_back(static_cast<std::uint8_t>(type[3]));
  out.insert(out.end(), data.begin(), data.end());
  const auto crc = crc32Png(out.data() + type_offset, 4 + data.size());
  appendUint32(out, crc);
}

}  // namespace

std::vector<std::uint8_t> encodeGrayscalePng(const PgmImage& image) {
  std::vector<std::uint8_t> raw;
  raw.reserve((image.width + 1U) * image.height);
  for (std::size_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const auto* row = image.pixels.data() + y * image.width;
    raw.insert(raw.end(), row, row + image.width);
  }

  uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
  std::vector<std::uint8_t> compressed(compressed_size);
  if (compress2(
          compressed.data(),
          &compressed_size,
          raw.data(),
          static_cast<uLong>(raw.size()),
          Z_BEST_SPEED) != Z_OK) {
    return {};
  }
  compressed.resize(compressed_size);

  std::vector<std::uint8_t> png{
      137, 80, 78, 71, 13, 10, 26, 10,
  };

  std::vector<std::uint8_t> ihdr;
  appendUint32(ihdr, static_cast<std::uint32_t>(image.width));
  appendUint32(ihdr, static_cast<std::uint32_t>(image.height));
  ihdr.push_back(8);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  appendChunk(png, "IHDR", ihdr);
  appendChunk(png, "IDAT", compressed);
  appendChunk(png, "IEND", {});
  return png;
}

bool writeBinaryFile(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

bool writeTextFile(const std::string& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(output);
}

}  // namespace dispatcher::maps
