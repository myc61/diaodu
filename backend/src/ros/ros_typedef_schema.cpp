#include "dispatcher/ros/ros_typedef_schema.hpp"

#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dispatcher::ros {
namespace {

std::string stripArraySuffix(std::string type) {
  while (!type.empty() && type.back() == ']') {
    const auto bracket = type.rfind('[');
    if (bracket == std::string::npos) {
      break;
    }
    type = type.substr(0, bracket);
  }
  return type;
}

bool isPrimitive(const std::string& type) {
  static const std::unordered_set<std::string> primitives{
      "bool",
      "int8",
      "uint8",
      "int16",
      "uint16",
      "int32",
      "uint32",
      "int64",
      "uint64",
      "float32",
      "float64",
      "string",
      "time",
      "duration",
      "char",
      "byte",
  };
  return primitives.contains(stripArraySuffix(type));
}

nlohmann::json parseExample(const std::string& example, const std::string& type) {
  const auto base = stripArraySuffix(type);
  if (example.empty()) {
    if (base == "bool") {
      return false;
    }
    if (base == "string") {
      return "";
    }
    if (base == "time" || base == "duration") {
      return nlohmann::json{{"secs", 0}, {"nsecs", 0}};
    }
    if (base.rfind("int", 0) == 0 || base.rfind("uint", 0) == 0 ||
        base == "char" || base == "byte") {
      return 0;
    }
    if (base.rfind("float", 0) == 0) {
      return 0.0;
    }
    return nlohmann::json::object();
  }

  std::string lowered = example;
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (base == "bool") {
    return lowered == "1" || lowered == "true" || lowered == "True";
  }
  try {
    return nlohmann::json::parse(example);
  } catch (...) {
  }
  if (base.rfind("int", 0) == 0 || base.rfind("uint", 0) == 0 ||
      base == "char" || base == "byte") {
    try {
      return std::stoll(example);
    } catch (...) {
      return 0;
    }
  }
  if (base.rfind("float", 0) == 0) {
    try {
      return std::stod(example);
    } catch (...) {
      return 0.0;
    }
  }
  return example;
}

nlohmann::json primitiveSchema(const std::string& type, const std::string& example) {
  const auto base = stripArraySuffix(type);
  nlohmann::json field = nlohmann::json::object();
  if (base == "bool") {
    field["type"] = "boolean";
  } else if (
      base.rfind("int", 0) == 0 || base.rfind("uint", 0) == 0 || base == "char" ||
      base == "byte") {
    field["type"] = "integer";
  } else if (base.rfind("float", 0) == 0) {
    field["type"] = "number";
  } else if (base == "string") {
    field["type"] = "string";
  } else if (base == "time" || base == "duration") {
    field["type"] = "object";
    field["properties"] = {
        {"secs", {{"type", "integer"}, {"default", 0}}},
        {"nsecs", {{"type", "integer"}, {"default", 0}}},
    };
  } else {
    field["type"] = "string";
  }
  field["default"] = parseExample(example, type);
  return field;
}

using TypedefMap = std::unordered_map<std::string, nlohmann::json>;

TypedefMap indexTypedefs(const nlohmann::json& typedefs) {
  TypedefMap map;
  if (!typedefs.is_array()) {
    return map;
  }
  for (const auto& item : typedefs) {
    if (!item.is_object()) {
      continue;
    }
    const auto type = item.value("type", "");
    if (!type.empty()) {
      map[type] = item;
    }
  }
  return map;
}

nlohmann::json schemaForType(
    const std::string& type,
    const std::string& example,
    const TypedefMap& map,
    std::unordered_set<std::string>& visiting,
    int depth);

nlohmann::json schemaFromTypedef(
    const nlohmann::json& typedef_obj,
    const TypedefMap& map,
    std::unordered_set<std::string>& visiting,
    int depth) {
  nlohmann::json properties = nlohmann::json::object();
  nlohmann::json defaults = nlohmann::json::object();
  const auto names = typedef_obj.value("fieldnames", nlohmann::json::array());
  const auto types = typedef_obj.value("fieldtypes", nlohmann::json::array());
  const auto arrays = typedef_obj.value("fieldarraylen", nlohmann::json::array());
  const auto examples = typedef_obj.value("examples", nlohmann::json::array());

  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!names[i].is_string()) {
      continue;
    }
    const auto name = names[i].get<std::string>();
    const auto field_type =
        i < types.size() && types[i].is_string() ? types[i].get<std::string>()
                                                 : "string";
    // ROS TypeDef semantics: -1 scalar, 0 variable-length array, >0 fixed
    // array. A []/[N] suffix is also accepted for compatibility.
    const int array_len =
        i < arrays.size() && arrays[i].is_number_integer()
            ? arrays[i].get<int>()
            : -1;
    const bool type_is_array = field_type.find('[') != std::string::npos;
    const bool is_array = type_is_array || array_len >= 0;
    const auto example =
        i < examples.size() && examples[i].is_string()
            ? examples[i].get<std::string>()
            : std::string{};

    nlohmann::json field_schema;
    if (is_array) {
      auto item_schema = schemaForType(
          stripArraySuffix(field_type), "", map, visiting, depth + 1);
      nlohmann::json default_array = nlohmann::json::array();
      if (array_len > 0) {
        const auto item_default =
            item_schema.value("default", nlohmann::json::object());
        for (int n = 0; n < array_len; ++n) {
          default_array.push_back(item_default);
        }
      }
      field_schema = {
          {"type", "array"},
          {"items", item_schema},
          {"default", default_array},
      };
      if (array_len > 0) {
        field_schema["minItems"] = array_len;
        field_schema["maxItems"] = array_len;
      }
      defaults[name] = default_array;
    } else {
      field_schema = schemaForType(field_type, example, map, visiting, depth + 1);
      if (field_schema.contains("default")) {
        defaults[name] = field_schema["default"];
      } else {
        defaults[name] = nlohmann::json::object();
      }
    }
    field_schema["title"] = name;
    field_schema["description"] = field_type;
    properties[name] = field_schema;
  }

  return {
      {"type", "object"},
      {"properties", properties},
      {"default", defaults},
  };
}

nlohmann::json schemaForType(
    const std::string& type,
    const std::string& example,
    const TypedefMap& map,
    std::unordered_set<std::string>& visiting,
    int depth) {
  if (depth > 8) {
    return {{"type", "object"}, {"default", nlohmann::json::object()}};
  }
  const auto base = stripArraySuffix(type);
  if (isPrimitive(base)) {
    return primitiveSchema(base, example);
  }
  if (visiting.contains(base)) {
    return {{"type", "object"}, {"default", nlohmann::json::object()}};
  }
  const auto it = map.find(base);
  if (it == map.end()) {
    // Unknown compound: keep as object JSON for SchemaForm fallback.
    return {
        {"type", "object"},
        {"title", base},
        {"description", base},
        {"default", parseExample(example.empty() ? "{}" : example, "object")},
    };
  }
  visiting.insert(base);
  auto schema = schemaFromTypedef(it->second, map, visiting, depth);
  visiting.erase(base);
  return schema;
}

}  // namespace

TypedefSchemaResult schemaFromRosapiTypedefs(const nlohmann::json& typedefs) {
  TypedefSchemaResult result;
  if (!typedefs.is_array() || typedefs.empty()) {
    result.error = "rosapi typedefs empty";
    return result;
  }
  const auto map = indexTypedefs(typedefs);
  const auto& root = typedefs.front();
  result.root_type = root.value("type", "");
  std::unordered_set<std::string> visiting;
  auto schema = schemaFromTypedef(root, map, visiting, 0);
  result.parameter_schema = {
      {"type", "object"},
      {"properties", schema.value("properties", nlohmann::json::object())},
  };
  result.request_defaults = schema.value("default", nlohmann::json::object());
  return result;
}

}  // namespace dispatcher::ros
