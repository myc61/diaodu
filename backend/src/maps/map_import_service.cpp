#include "dispatcher/maps/map_import_service.hpp"

#include "dispatcher/maps/png_writer.hpp"

#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>

namespace dispatcher::maps {
namespace {

std::string toHex(const unsigned char* data, std::size_t length) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < length; ++i) {
    stream << std::setw(2) << static_cast<unsigned>(data[i]);
  }
  return stream.str();
}

}  // namespace

std::string sha256Hex(std::string_view data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    return {};
  }
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, data.data(), data.size()) != 1 ||
      EVP_DigestFinal_ex(context, digest.data(), &digest_length) != 1) {
    EVP_MD_CTX_free(context);
    return {};
  }
  EVP_MD_CTX_free(context);
  return toHex(digest.data(), digest_length);
}

MapImportResult materializeMapFiles(
    const std::filesystem::path& root_dir,
    const std::string& scene_id,
    int version,
    std::string_view yaml_text,
    std::string_view pgm_bytes) {
  MapImportResult result;
  const auto yaml = parseMapYaml(yaml_text);
  if (!yaml.ok) {
    result.errors = yaml.errors;
    return result;
  }

  const auto pgm = parsePgm(pgm_bytes);
  if (!pgm.ok) {
    result.errors.push_back(pgm.error);
    return result;
  }

  const auto preview = encodeGrayscalePng(pgm.image);
  if (preview.empty()) {
    result.errors.emplace_back("failed to encode PNG preview");
    return result;
  }

  const auto directory = root_dir / scene_id / std::to_string(version);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    result.errors.emplace_back("failed to create map directory");
    return result;
  }

  const auto pgm_path = directory / "map.pgm";
  const auto yaml_path = directory / "map.yaml";
  const auto preview_path = directory / "preview.png";

  if (!writeBinaryFile(
          pgm_path.string(),
          std::vector<std::uint8_t>(pgm_bytes.begin(), pgm_bytes.end())) ||
      !writeTextFile(yaml_path.string(), yaml_text) ||
      !writeBinaryFile(preview_path.string(), preview)) {
    result.errors.emplace_back("failed to write map files");
    return result;
  }

  std::string checksum_input;
  checksum_input.reserve(yaml_text.size() + pgm_bytes.size());
  checksum_input.append(yaml_text);
  checksum_input.append(pgm_bytes);
  const auto sha = sha256Hex(checksum_input);
  if (sha.empty()) {
    result.errors.emplace_back("failed to compute SHA-256");
    return result;
  }

  result.ok = true;
  result.files = ImportedMapFiles{
      .pgm_path = pgm_path.string(),
      .yaml_path = yaml_path.string(),
      .preview_path = preview_path.string(),
      .sha256 = sha,
      .file_size_bytes =
          static_cast<std::uint64_t>(yaml_text.size() + pgm_bytes.size()),
      .width = pgm.image.width,
      .height = pgm.image.height,
      .yaml = yaml.yaml,
  };
  return result;
}

}  // namespace dispatcher::maps
