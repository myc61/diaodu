#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace dispatcher::ros {

// Convert rosapi TypeDef[] (service_request_details / message_details) into a
// JSON Schema object suitable for SchemaForm + capability parameter_schema.
// Also builds a flat-ish default request object from typedef examples.
struct TypedefSchemaResult {
  nlohmann::json parameter_schema = nlohmann::json::object({
      {"type", "object"},
      {"properties", nlohmann::json::object()},
  });
  nlohmann::json request_defaults = nlohmann::json::object();
  std::string root_type;
  std::string error;
};

[[nodiscard]] TypedefSchemaResult schemaFromRosapiTypedefs(
    const nlohmann::json& typedefs);

}  // namespace dispatcher::ros
