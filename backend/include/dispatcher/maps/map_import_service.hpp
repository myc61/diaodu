#pragma once

#include "dispatcher/maps/map_yaml.hpp"
#include "dispatcher/maps/pgm.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dispatcher::maps {

struct ImportedMapFiles {
  std::string pgm_path;
  std::string yaml_path;
  std::string preview_path;
  std::string sha256;
  std::uint64_t file_size_bytes{0};
  std::size_t width{0};
  std::size_t height{0};
  MapYaml yaml;
};

struct MapImportResult {
  bool ok{false};
  ImportedMapFiles files;
  std::vector<std::string> errors;
};

[[nodiscard]] std::string sha256Hex(std::string_view data);

[[nodiscard]] MapImportResult materializeMapFiles(
    const std::filesystem::path& root_dir,
    const std::string& scene_id,
    int version,
    std::string_view yaml_text,
    std::string_view pgm_bytes);

// Rebuild preview.png from map.pgm when the preview is missing or empty.
[[nodiscard]] bool ensurePreviewPng(
    const std::string& preview_path,
    const std::string& pgm_path);

}  // namespace dispatcher::maps
