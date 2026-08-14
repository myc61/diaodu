#include "dispatcher/maps/map_yaml.hpp"

#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace dispatcher::maps {
namespace {

std::string trim(std::string_view input) {
  std::size_t begin = 0;
  while (begin < input.size() &&
         std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }
  std::size_t end = input.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return std::string(input.substr(begin, end - begin));
}

std::string stripQuotes(std::string value) {
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

bool parseDouble(std::string_view text, double& out) {
  try {
    std::size_t consumed = 0;
    const std::string owned(text);
    out = std::stod(owned, &consumed);
    return consumed == owned.size() && std::isfinite(out);
  } catch (...) {
    return false;
  }
}

bool parseOrigin(std::string_view text, MapYaml& yaml, std::string& error) {
  auto value = trim(text);
  if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
    error = "origin must be a 3-element array";
    return false;
  }
  value = value.substr(1, value.size() - 2);
  std::vector<double> numbers;
  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, ',')) {
    double number = 0.0;
    if (!parseDouble(trim(token), number)) {
      error = "origin contains invalid numbers";
      return false;
    }
    numbers.push_back(number);
  }
  if (numbers.size() != 3) {
    error = "origin must contain exactly 3 numbers";
    return false;
  }
  yaml.origin_x = numbers[0];
  yaml.origin_y = numbers[1];
  yaml.origin_yaw = numbers[2];
  return true;
}

}  // namespace

MapYamlParseResult parseMapYaml(std::string_view text) {
  MapYamlParseResult result;
  bool has_image = false;
  bool has_resolution = false;
  bool has_origin = false;
  bool has_occupied = false;
  bool has_free = false;

  std::stringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
    const auto trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    const auto colon = trimmed.find(':');
    if (colon == std::string::npos) {
      result.errors.push_back("invalid YAML line: " + trimmed);
      continue;
    }
    const auto key = trim(trimmed.substr(0, colon));
    auto value = stripQuotes(trim(trimmed.substr(colon + 1)));

    if (key == "image") {
      if (value.empty()) {
        result.errors.emplace_back("image must not be empty");
      } else {
        result.yaml.image = value;
        has_image = true;
      }
    } else if (key == "resolution") {
      if (!parseDouble(value, result.yaml.resolution) ||
          result.yaml.resolution <= 0.0) {
        result.errors.emplace_back("resolution must be a positive number");
      } else {
        has_resolution = true;
      }
    } else if (key == "origin") {
      std::string error;
      if (!parseOrigin(value, result.yaml, error)) {
        result.errors.push_back(error);
      } else {
        has_origin = true;
      }
    } else if (key == "occupied_thresh") {
      if (!parseDouble(value, result.yaml.occupied_thresh) ||
          result.yaml.occupied_thresh < 0.0 ||
          result.yaml.occupied_thresh > 1.0) {
        result.errors.emplace_back(
            "occupied_thresh must be between 0.0 and 1.0");
      } else {
        has_occupied = true;
      }
    } else if (key == "free_thresh") {
      if (!parseDouble(value, result.yaml.free_thresh) ||
          result.yaml.free_thresh < 0.0 || result.yaml.free_thresh > 1.0) {
        result.errors.emplace_back("free_thresh must be between 0.0 and 1.0");
      } else {
        has_free = true;
      }
    } else if (key == "negate") {
      try {
        result.yaml.negate = std::stoi(value);
      } catch (...) {
        result.errors.emplace_back("negate must be an integer");
      }
    }
  }

  if (!has_image) {
    result.errors.emplace_back("missing required field: image");
  }
  if (!has_resolution) {
    result.errors.emplace_back("missing required field: resolution");
  }
  if (!has_origin) {
    result.errors.emplace_back("missing required field: origin");
  }
  if (!has_occupied) {
    result.errors.emplace_back("missing required field: occupied_thresh");
  }
  if (!has_free) {
    result.errors.emplace_back("missing required field: free_thresh");
  }

  result.ok = result.errors.empty();
  return result;
}

}  // namespace dispatcher::maps
