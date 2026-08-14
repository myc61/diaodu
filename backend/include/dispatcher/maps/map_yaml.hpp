#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dispatcher::maps {

struct MapYaml {
  std::string image;
  double resolution{0.0};
  double origin_x{0.0};
  double origin_y{0.0};
  double origin_yaw{0.0};
  double occupied_thresh{0.65};
  double free_thresh{0.196};
  int negate{0};
};

struct MapYamlParseResult {
  bool ok{false};
  MapYaml yaml;
  std::vector<std::string> errors;
};

[[nodiscard]] MapYamlParseResult parseMapYaml(std::string_view text);

}  // namespace dispatcher::maps
