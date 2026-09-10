#include "dispatcher/api/register_routes.hpp"

#include "dispatcher/api/app_state.hpp"
#include "dispatcher/domain/map_transform.hpp"
#include "dispatcher/maps/map_import_service.hpp"
#include "dispatcher/ops/ops_log.hpp"
#include "dispatcher/workflow/event_spec.hpp"

#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dispatcher::api {
namespace {

using drogon::Delete;
using drogon::Get;
using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using drogon::Post;
using drogon::Put;

Json::Value toJsonValue(const nlohmann::json& json) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(json.dump());
  if (!Json::parseFromStream(builder, stream, &root, &errors)) {
    return Json::Value(Json::objectValue);
  }
  return root;
}

HttpResponsePtr jsonResponse(const nlohmann::json& body, int status = 200) {
  auto response = HttpResponse::newHttpJsonResponse(toJsonValue(body));
  response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
  response->addHeader("Cache-Control", "no-store");
  return response;
}

HttpResponsePtr errorResponse(const std::string& message, int status) {
  return jsonResponse({{"error", message}}, status);
}

// Reconnects must not run on the Drogon HTTP thread: dropping a rosbridge
// session joins its worker and can stall workspace/map preview requests.
void scheduleRefreshConnections() {
  if (appState().robot_runtime == nullptr) {
    return;
  }
  if (appState().execution != nullptr &&
      appState().execution->postBlocking([] {
        try {
          appState().robot_runtime->refreshConnections();
        } catch (const std::exception& ex) {
          ops::OpsLog::instance().warn(
              "ros",
              std::string("refreshConnections failed: ") + ex.what());
        }
      })) {
    return;
  }
  appState().robot_runtime->refreshConnections();
}

nlohmann::json mergeAndStoreRobotInterfaceCatalog(
    const std::string& robot_id,
    interfaces::InterfaceCatalog catalog) {
  if (const auto cached =
          appState().repository->getRobotInterfaceCache(robot_id);
      cached.has_value()) {
    catalog.preserveFailedCategoriesFrom(
        interfaces::InterfaceCatalog::fromJson(*cached));
  }
  auto body = catalog.toJson();
  appState().repository->setRobotInterfaceCache(robot_id, body);
  return body;
}

nlohmann::json sceneToJson(const db::SceneRecord& scene) {
  nlohmann::json json{
      {"id", scene.id},
      {"name", scene.name},
      {"description", scene.description},
  };
  if (scene.active_map_version_id.has_value()) {
    json["active_map_version_id"] = *scene.active_map_version_id;
  } else {
    json["active_map_version_id"] = nullptr;
  }
  return json;
}

nlohmann::json mapToJson(const db::MapVersionRecord& map) {
  return {
      {"id", map.id},
      {"scene_id", map.scene_id},
      {"version", map.version},
      {"status", map.status},
      {"width", map.width},
      {"height", map.height},
      {"resolution", map.resolution},
      {"origin",
       nlohmann::json::array({map.origin_x, map.origin_y, map.origin_yaw})},
      {"occupied_thresh", map.occupied_thresh},
      {"free_thresh", map.free_thresh},
      {"sha256", map.sha256},
      {"file_size_bytes", map.file_size_bytes},
      {"yaml_image", map.yaml_image},
      {"frame_id", map.frame_id},
      {"preview_url", "/api/v1/maps/" + map.id + "/preview"},
  };
}

domain::MapMetadata toMetadata(const db::MapVersionRecord& map) {
  return domain::MapMetadata{
      .width = static_cast<std::size_t>(map.width),
      .height = static_cast<std::size_t>(map.height),
      .resolution = map.resolution,
      .origin_x = map.origin_x,
      .origin_y = map.origin_y,
      .origin_yaw = map.origin_yaw,
  };
}

nlohmann::json stationToJson(
    const db::StationRecord& station,
    const std::optional<db::MapVersionRecord>& map = std::nullopt) {
  nlohmann::json json{
      {"id", station.id},
      {"scene_id", station.scene_id},
      {"map_version_id", station.map_version_id},
      {"name", station.name},
      {"x", station.x},
      {"y", station.y},
      {"yaw", station.yaw},
      {"tags", station.tags},
      {"notes", station.notes},
      {"status", station.status},
      {"metadata", station.metadata},
      {"pixel_x", nullptr},
      {"pixel_y", nullptr},
      {"pixel_yaw", nullptr},
  };
  if (map.has_value()) {
    const auto pixel = domain::worldToPixel(
        toMetadata(*map),
        domain::WorldPose2D{
            .x = station.x,
            .y = station.y,
            .yaw = station.yaw,
        });
    if (pixel.has_value()) {
      json["pixel_x"] = pixel->x;
      json["pixel_y"] = pixel->y;
      json["pixel_yaw"] = pixel->yaw;
    }
  }
  return json;
}

nlohmann::json stationActionToJson(const db::StationActionRecord& action) {
  nlohmann::json json{
      {"id", action.id},
      {"station_id", action.station_id},
      {"sequence_no", action.sequence_no},
      {"action_name", action.action_name},
      {"capability_key", action.capability_key},
      {"motion_ownership", action.motion_ownership},
      {"robot_selector_type", action.robot_selector_type},
      {"robot_name", action.robot_name},
      {"robot_group", action.robot_group},
      {"runtime_variable", action.runtime_variable},
      {"parameters", action.parameters},
      {"failure_policy", action.failure_policy},
      {"retry_count", action.retry_count},
      {"timeout_ms", action.timeout_ms},
      {"success_event_name", action.success_event_name},
      {"event_specs", action.event_specs},
  };
  if (action.parallel_group.has_value()) {
    json["parallel_group"] = *action.parallel_group;
  } else {
    json["parallel_group"] = nullptr;
  }
  if (action.capability_definition_id.has_value()) {
    json["capability_definition_id"] = *action.capability_definition_id;
  } else {
    json["capability_definition_id"] = nullptr;
  }
  if (action.robot_id.has_value()) {
    json["robot_id"] = *action.robot_id;
  } else {
    json["robot_id"] = nullptr;
  }
  if (action.post_navigation_station_id.has_value()) {
    json["post_navigation_station_id"] = *action.post_navigation_station_id;
  } else {
    json["post_navigation_station_id"] = nullptr;
  }
  if (!action.precondition.is_null()) {
    json["precondition"] = action.precondition;
  } else {
    json["precondition"] = nullptr;
  }
  return json;
}

nlohmann::json capabilityTemplateToJson(
    const db::CapabilityTemplateRecord& item) {
  return {
      {"id", item.id},
      {"profile_id", item.profile_id},
      {"profile_name", item.profile_name},
      {"capability_key", item.capability_key},
      {"operation_kind", item.operation_kind},
      {"endpoint_name", item.endpoint_name},
      {"ros_message_type", item.ros_message_type},
      {"motion_ownership", item.motion_ownership},
      {"blocking_type", item.blocking_type},
      {"timeout_ms", item.timeout_ms},
      {"parameter_schema", item.parameter_schema},
      {"request_template", item.request_template},
      {"feedback_mapping", item.feedback_mapping},
      {"result_mapping", item.result_mapping},
      {"success_condition", item.success_condition},
      {"failure_condition", item.failure_condition},
      {"retry_policy", item.retry_policy},
      {"cancel_policy", item.cancel_policy},
      {"resource_claims", item.resource_claims},
      {"protocol_config", item.protocol_config},
      {"event_specs", item.event_specs},
  };
}

nlohmann::json renderCapabilityTemplate(
    const nlohmann::json& node,
    const nlohmann::json& parameters) {
  if (node.is_string()) {
    const auto text = node.get<std::string>();
    if (text.size() >= 3 && text.front() == '$' && text[1] == '{') {
      const auto end = text.find('}');
      if (end != std::string::npos) {
        const auto key = text.substr(2, end - 2);
        if (parameters.contains(key)) {
          return parameters.at(key);
        }
      }
    }
    return text;
  }
  if (node.is_array()) {
    nlohmann::json rendered = nlohmann::json::array();
    for (const auto& item : node) {
      rendered.push_back(renderCapabilityTemplate(item, parameters));
    }
    return rendered;
  }
  if (node.is_object()) {
    nlohmann::json rendered = nlohmann::json::object();
    for (const auto& [key, value] : node.items()) {
      rendered[key] = renderCapabilityTemplate(value, parameters);
    }
    return rendered;
  }
  return node;
}

db::CapabilityUpsertRequest parseCapabilityUpsert(const nlohmann::json& json) {
  db::CapabilityUpsertRequest request{
      .profile_id = json.value("profile_id", ""),
      .capability_key = json.value("capability_key", ""),
      .operation_kind = json.value("operation_kind", "SERVICE"),
      .endpoint_name = json.value("endpoint_name", ""),
      .ros_message_type = json.value("ros_message_type", ""),
      .motion_ownership = json.value("motion_ownership", "DISPATCHER"),
      .blocking_type = json.value("blocking_type", "NONE"),
      .timeout_ms = json.value("timeout_ms", 30000),
      .parameter_schema =
          json.value("parameter_schema", nlohmann::json::object()),
      .request_template =
          json.value("request_template", nlohmann::json::object()),
      .feedback_mapping =
          json.value("feedback_mapping", nlohmann::json::object()),
      .result_mapping = json.value("result_mapping", nlohmann::json::object()),
      .retry_policy = json.value("retry_policy", nlohmann::json::object()),
      .cancel_policy = json.value("cancel_policy", nlohmann::json::object()),
      .resource_claims =
          json.value("resource_claims", nlohmann::json::array()),
      .protocol_config =
          json.value("protocol_config", nlohmann::json::object()),
      .event_specs = json.value("event_specs", nlohmann::json::array()),
  };
  if (json.contains("success_condition") && !json["success_condition"].is_null()) {
    request.success_condition = json["success_condition"];
  }
  if (json.contains("failure_condition") && !json["failure_condition"].is_null()) {
    request.failure_condition = json["failure_condition"];
  }
  if (request.operation_kind == "SSH") {
    const auto robot_id = request.protocol_config.value("ssh_robot_id", "");
    const auto profile_id = request.protocol_config.value("ssh_profile_id", "");
    const auto profile = profile_id.empty()
        ? std::nullopt
        : appState().repository->getRobotStartupProfile(profile_id);
    if (robot_id.empty() || !profile.has_value() ||
        profile->robot_id != robot_id) {
      throw std::runtime_error(
          "SSH capability requires a matching robot and controlled SSH profile");
    }
    request.endpoint_name = profile->name;
    request.ros_message_type = "SSH";
    request.protocol_config["adapter"] = "CONTROLLED_SSH";
  }
  return request;
}

db::StationUpsertRequest parseStationUpsert(const nlohmann::json& json) {
  db::StationUpsertRequest request{
      .scene_id = json.value("scene_id", ""),
      .map_version_id = json.value("map_version_id", ""),
      .name = json.value("name", ""),
      .x = json.value("x", 0.0),
      .y = json.value("y", 0.0),
      .yaw = json.value("yaw", 0.0),
      .notes = json.value("notes", ""),
      .metadata = json.value("metadata", nlohmann::json::object()),
  };
  if (json.contains("tags") && json["tags"].is_array()) {
    for (const auto& tag : json["tags"]) {
      request.tags.push_back(tag.get<std::string>());
    }
  }
  return request;
}

std::vector<db::StationActionUpsert> parseStationActions(
    const nlohmann::json& json) {
  std::vector<db::StationActionUpsert> actions;
  const auto& items = json.at("actions");
  actions.reserve(items.size());
  for (const auto& item : items) {
    db::StationActionUpsert action{
        .sequence_no = item.value("sequence_no", 1),
        .action_name = item.value(
            "action_name", item.value("capability_key", "")),
        .capability_key = item.value("capability_key", ""),
        .robot_selector_type = item.value("robot_selector_type", "FIXED"),
        .robot_group = item.value("robot_group", ""),
        .runtime_variable = item.value("runtime_variable", ""),
        .parameters = item.value("parameters", nlohmann::json::object()),
        .failure_policy = item.value("failure_policy", "FAIL"),
        .retry_count = item.value("retry_count", 0),
        .timeout_ms = item.value("timeout_ms", 30000),
        .success_event_name = item.value("success_event_name", ""),
        .event_specs = item.value("event_specs", nlohmann::json::array()),
    };
    if (item.contains("parallel_group") && !item["parallel_group"].is_null()) {
      action.parallel_group = item["parallel_group"].get<int>();
    }
    if (item.contains("capability_definition_id") &&
        !item["capability_definition_id"].is_null()) {
      action.capability_definition_id =
          item["capability_definition_id"].get<std::string>();
    }
    if (item.contains("robot_id") && !item["robot_id"].is_null() &&
        !item["robot_id"].get<std::string>().empty()) {
      action.robot_id = item["robot_id"].get<std::string>();
    }
    if (item.contains("post_navigation_station_id") &&
        !item["post_navigation_station_id"].is_null() &&
        !item["post_navigation_station_id"].get<std::string>().empty()) {
      action.post_navigation_station_id =
          item["post_navigation_station_id"].get<std::string>();
    }
    if (item.contains("precondition") && !item["precondition"].is_null()) {
      action.precondition = item["precondition"];
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

nlohmann::json robotToJson(const db::RobotRecord& robot) {
  nlohmann::json item{
      {"id", robot.id},
      {"name", robot.name},
      {"enabled", robot.enabled},
      {"robot_type", robot.robot_type},
      {"ros_version", robot.ros_version},
      {"ros_distribution", robot.ros_distribution},
      {"ros_namespace", robot.ros_namespace},
      {"connection_state", robot.connection_state},
      {"configuration_state", robot.configuration_state},
      {"business_status", robot.business_status},
      {"localization_status", robot.localization_status},
      {"rosbridge_url", robot.rosbridge_url},
      {"rosbridge_tls", robot.rosbridge_tls},
      {"stale_timeout_ms", robot.stale_timeout_ms},
      {"pose_mapping", robot.pose_mapping},
  };
  if (robot.current_scene_id.has_value()) {
    item["current_scene_id"] = *robot.current_scene_id;
  } else {
    item["current_scene_id"] = nullptr;
  }
  if (robot.current_map_version_id.has_value()) {
    item["current_map_version_id"] = *robot.current_map_version_id;
  } else {
    item["current_map_version_id"] = nullptr;
  }
  if (robot.host.has_value()) {
    item["host"] = *robot.host;
  } else {
    item["host"] = nullptr;
  }
  if (robot.rosbridge_port.has_value()) {
    item["rosbridge_port"] = *robot.rosbridge_port;
  } else {
    item["rosbridge_port"] = nullptr;
  }
  if (robot.rosbridge_path.has_value()) {
    item["rosbridge_path"] = *robot.rosbridge_path;
  } else {
    item["rosbridge_path"] = "/";
  }
  if (robot.pose_topic.has_value()) {
    item["pose_topic"] = *robot.pose_topic;
  } else {
    item["pose_topic"] = "";
  }
  if (robot.pose_message_type.has_value()) {
    item["pose_message_type"] = *robot.pose_message_type;
  } else {
    item["pose_message_type"] = "";
  }
  return item;
}

db::RobotUpsertRequest parseRobotUpsert(const nlohmann::json& json) {
  db::RobotUpsertRequest request;
  request.name = json.at("name").get<std::string>();
  request.enabled = json.value("enabled", true);
  request.robot_type = json.value("robot_type", "zj_humanoid");
  request.ros_version = json.value("ros_version", "ROS1");
  request.ros_distribution = json.value("ros_distribution", "noetic");
  request.ros_namespace = json.value("ros_namespace", "");
  request.host = json.at("host").get<std::string>();
  request.rosbridge_port = json.value("rosbridge_port", 9090);
  request.rosbridge_path = json.value("rosbridge_path", "/");
  request.rosbridge_tls = json.value("rosbridge_tls", false);
  request.pose_topic = json.value("pose_topic", "");
  request.pose_message_type = json.value(
      "pose_message_type",
      "nav_msgs/Odometry");
  request.stale_timeout_ms = json.value("stale_timeout_ms", 3000);
  if (json.contains("pose_mapping") && json.at("pose_mapping").is_object()) {
    request.pose_mapping = json.at("pose_mapping");
  }
  return request;
}

nlohmann::json startupProfileToJson(
    const db::RobotStartupProfileRecord& profile) {
  return {
      {"id", profile.id},
      {"robot_id", profile.robot_id},
      {"name", profile.name},
      {"description", profile.description},
      {"enabled", profile.enabled},
      {"ssh_port", profile.ssh_port},
      {"ssh_username", profile.ssh_username},
      {"credential_configured", !profile.credential_reference.empty()},
      {"known_hosts_configured", !profile.known_hosts_reference.empty()},
      {"steps", profile.steps},
      {"readiness_checks", profile.readiness_checks},
      {"stop_steps", profile.stop_steps},
      {"timeout_ms", profile.timeout_ms},
      {"version", profile.version},
  };
}

db::RobotStartupProfileUpsert parseStartupProfileUpsert(
    const nlohmann::json& json,
    const std::optional<db::RobotStartupProfileRecord>& existing =
        std::nullopt) {
  auto credential_reference = json.value("credential_reference", "");
  auto known_hosts_reference = json.value("known_hosts_reference", "");
  if (existing.has_value()) {
    if (credential_reference.empty()) {
      credential_reference = existing->credential_reference;
    }
    if (known_hosts_reference.empty()) {
      known_hosts_reference = existing->known_hosts_reference;
    }
  }
  db::RobotStartupProfileUpsert request{
      .name = json.value("name", ""),
      .description = json.value("description", ""),
      .enabled = json.value("enabled", true),
      .ssh_port = json.value("ssh_port", 22),
      .ssh_username = json.value("ssh_username", "naviai"),
      .credential_reference = std::move(credential_reference),
      .known_hosts_reference = std::move(known_hosts_reference),
      .steps = json.value("steps", nlohmann::json::array()),
      .readiness_checks =
          json.value("readiness_checks", nlohmann::json::array()),
      .stop_steps = json.value("stop_steps", nlohmann::json::array()),
      .timeout_ms = json.value("timeout_ms", 120000),
  };
  if (request.name.empty()) {
    throw std::runtime_error("startup profile name is required");
  }
  remote::ControlledSshRequest validation{
      .job_id = "validation",
      .host = "validation",
      .port = request.ssh_port,
      .username = request.ssh_username,
      .credential_reference = request.credential_reference,
      .known_hosts_reference = request.known_hosts_reference,
      .steps = request.steps,
      .readiness_checks = request.readiness_checks,
      .timeout_ms = request.timeout_ms,
  };
  if (const auto error =
          remote::ControlledSshExecutor::validateRequest(validation);
      error.has_value()) {
    throw std::runtime_error(*error);
  }
  if (!request.stop_steps.is_array()) {
    throw std::runtime_error("stop_steps must be an array");
  }
  if (!request.stop_steps.empty()) {
    validation.steps = request.stop_steps;
    validation.readiness_checks = nlohmann::json::array();
    if (const auto error =
            remote::ControlledSshExecutor::validateRequest(validation);
        error.has_value()) {
      throw std::runtime_error("invalid stop_steps: " + *error);
    }
  }
  return request;
}

}  // namespace

void registerRoutes() {
  drogon::app().registerHandler(
      "/api/v1/health",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        nlohmann::json body{
            {"status", appState().pool && appState().pool->healthy() ? "ok"
                                                                     : "degraded"},
            {"service", "dispatcher"},
            {"version", "0.1.0"},
        };
        callback(jsonResponse(body));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/system/summary",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        callback(jsonResponse({
            {"profile", "standard_dispatch"},
            {"authentication", false},
            {"modules",
             {{"robots", true},
              {"devices", false},
              {"maps", true},
              {"workflows", true},
              {"robot_config", false}}},
        }));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/scenes",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        nlohmann::json items = nlohmann::json::array();
        for (const auto& scene : appState().repository->listScenes()) {
          items.push_back(sceneToJson(scene));
        }
        callback(jsonResponse({{"items", items}}));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/scenes",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto name = json.value("name", "");
          if (name.empty()) {
            callback(errorResponse("name is required", 400));
            return;
          }
          const auto scene = appState().repository->createScene(
              name,
              json.value("description", ""));
          callback(jsonResponse(sceneToJson(scene), 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        const auto scene = appState().repository->getScene(id);
        if (!scene.has_value()) {
          callback(errorResponse("scene not found", 404));
          return;
        }
        callback(jsonResponse(sceneToJson(*scene)));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          appState().repository->deleteScene(id);
          scheduleRefreshConnections();
          callback(jsonResponse({{"deleted", true}, {"id", id}}));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "scene not found" ? 404 : 409));
        }
      },
      {drogon::Delete});

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}/active-map",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto map_id = json.value("map_version_id", "");
          if (map_id.empty()) {
            callback(errorResponse("map_version_id is required", 400));
            return;
          }
          if (!appState().repository->setActiveMap(id, map_id)) {
            callback(errorResponse(
                "active map not updated; keep previous affiliation",
                409));
            return;
          }
          callback(jsonResponse(sceneToJson(*appState().repository->getScene(id))));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}/robots",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          std::vector<std::string> robot_ids;
          for (const auto& item : json.at("robot_ids")) {
            robot_ids.push_back(item.get<std::string>());
          }
          appState().repository->replaceSceneRobots(id, robot_ids);
          scheduleRefreshConnections();
          ops::OpsLog::instance().info(
              "scene",
              "scene robots updated",
              {{"scene_id", id}, {"robot_ids", robot_ids}});
          callback(jsonResponse({
              {"scene_id", id},
              {"robot_ids", robot_ids},
          }));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 409));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}/workspace",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        const auto scene = appState().repository->getScene(id);
        if (!scene.has_value()) {
          callback(errorResponse("scene not found", 404));
          return;
        }

        nlohmann::json body{
            {"scene", sceneToJson(*scene)},
            {"map", nullptr},
            {"robots", nlohmann::json::array()},
            {"points", nlohmann::json::array()},
        };

        std::optional<db::MapVersionRecord> map;
        if (scene->active_map_version_id.has_value()) {
          map = appState().repository->getMapVersion(
              *scene->active_map_version_id);
          if (map.has_value()) {
            body["map"] = mapToJson(*map);
          }
        }

        const auto stations = appState().repository->listStations(
            id,
            scene->active_map_version_id);
        for (const auto& station : stations) {
          auto point = stationToJson(station, map);
          nlohmann::json actions = nlohmann::json::array();
          for (const auto& action :
               appState().repository->listStationActions(station.id)) {
            actions.push_back(stationActionToJson(action));
          }
          point["actions"] = actions;
          body["points"].push_back(point);
        }

        const auto robot_ids = appState().repository->listSceneRobotIds(id);
        for (const auto& robot_id : robot_ids) {
          const auto robot = appState().repository->getRobot(robot_id);
          if (!robot.has_value()) {
            continue;
          }
          nlohmann::json robot_json{
              {"id", robot->id},
              {"name", robot->name},
              {"connection_state", robot->connection_state},
              {"localization_status", robot->localization_status},
              {"pose", nullptr},
              {"drawable", false},
          };
          if (robot->current_scene_id.has_value()) {
            robot_json["current_scene_id"] = *robot->current_scene_id;
          } else {
            robot_json["current_scene_id"] = nullptr;
          }
          if (robot->current_map_version_id.has_value()) {
            robot_json["current_map_version_id"] =
                *robot->current_map_version_id;
          } else {
            robot_json["current_map_version_id"] = nullptr;
          }

          const auto cached = appState().pose_cache->get(robot_id);
          if (cached.has_value() && map.has_value()) {
            const auto now = std::chrono::system_clock::now();
            const auto age = now - cached->updated_at;
            const auto stale_timeout =
                std::chrono::milliseconds(robot->stale_timeout_ms);
            const bool stale = age > stale_timeout;
            const auto metadata = toMetadata(*map);
            const auto pixel = domain::worldToPixel(
                metadata,
                domain::WorldPose2D{
                    .x = cached->pose.x,
                    .y = cached->pose.y,
                    .yaw = cached->pose.yaw,
                });
            // Draw whenever this scene's map can place the pose. Stale poses
            // stay visible; the frontend uses a solid muted color instead of
            // hiding or fading the icon.
            const bool drawable =
                cached->scene_map_matched && pixel.has_value() &&
                robot->current_scene_id == scene->id &&
                robot->current_map_version_id == map->id;
            robot_json["drawable"] = drawable;
            nlohmann::json pose{
                {"x", cached->pose.x},
                {"y", cached->pose.y},
                {"yaw", cached->pose.yaw},
                {"stale", stale},
                {"scene_map_matched", cached->scene_map_matched},
                {"updated_at",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     cached->updated_at.time_since_epoch())
                     .count()},
            };
            if (pixel.has_value()) {
              pose["pixel_x"] = pixel->x;
              pose["pixel_y"] = pixel->y;
              pose["pixel_yaw"] = pixel->yaw;
            } else {
              pose["pixel_x"] = nullptr;
              pose["pixel_y"] = nullptr;
              pose["pixel_yaw"] = nullptr;
            }
            robot_json["pose"] = pose;
          }
          body["robots"].push_back(robot_json);
        }
        callback(jsonResponse(body));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/map-points",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto scene_id = req->getParameter("scene_id");
          if (scene_id.empty()) {
            callback(errorResponse("scene_id is required", 400));
            return;
          }
          const auto map_version_id = req->getParameter("map_version_id");
          std::optional<std::string> map_filter;
          if (!map_version_id.empty()) {
            map_filter = map_version_id;
          }
          nlohmann::json items = nlohmann::json::array();
          for (const auto& station :
               appState().repository->listStations(scene_id, map_filter)) {
            const auto map =
                appState().repository->getMapVersion(station.map_version_id);
            auto point = stationToJson(station, map);
            nlohmann::json actions = nlohmann::json::array();
            for (const auto& action :
                 appState().repository->listStationActions(station.id)) {
              actions.push_back(stationActionToJson(action));
            }
            point["actions"] = actions;
            items.push_back(point);
          }
          callback(jsonResponse({{"items", items}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/map-points",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto station =
              appState().repository->createStation(parseStationUpsert(json));
          const auto map =
              appState().repository->getMapVersion(station.map_version_id);
          auto body = stationToJson(station, map);
          body["actions"] = nlohmann::json::array();
          ops::OpsLog::instance().info(
              "point",
              "map point created: " + station.name,
              {{"id", station.id},
               {"scene_id", station.scene_id},
               {"x", station.x},
               {"y", station.y}});
          callback(jsonResponse(body, 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/map-points/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        const auto station = appState().repository->getStation(id);
        if (!station.has_value()) {
          callback(errorResponse("station not found", 404));
          return;
        }
        const auto map =
            appState().repository->getMapVersion(station->map_version_id);
        auto body = stationToJson(*station, map);
        nlohmann::json actions = nlohmann::json::array();
        for (const auto& action :
             appState().repository->listStationActions(id)) {
          actions.push_back(stationActionToJson(action));
        }
        body["actions"] = actions;
        callback(jsonResponse(body));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/map-points/{id}",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          auto request = parseStationUpsert(json);
          const auto existing = appState().repository->getStation(id);
          if (!existing.has_value()) {
            callback(errorResponse("station not found", 404));
            return;
          }
          request.scene_id = existing->scene_id;
          request.map_version_id = existing->map_version_id;
          const auto station =
              appState().repository->updateStation(id, request);
          const auto map =
              appState().repository->getMapVersion(station.map_version_id);
          auto body = stationToJson(station, map);
          nlohmann::json actions = nlohmann::json::array();
          for (const auto& action :
               appState().repository->listStationActions(id)) {
            actions.push_back(stationActionToJson(action));
          }
          body["actions"] = actions;
          callback(jsonResponse(body));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "station not found" ? 404 : 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/map-points/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto action =
              appState().repository->removeOrArchiveStation(id);
          ops::OpsLog::instance().info(
              "point",
              std::string("map point ") + action + ": " + id,
              {{"id", id}, {"action", action}});
          callback(jsonResponse({{"id", id}, {"action", action}}));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "station not found" ? 404 : 409));
        }
      },
      {drogon::Delete});

  drogon::app().registerHandler(
      "/api/v1/map-points/{id}/actions",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto saved = appState().repository->replaceStationActions(
              id,
              parseStationActions(json));
          nlohmann::json items = nlohmann::json::array();
          for (const auto& action : saved) {
            items.push_back(stationActionToJson(action));
          }
          callback(jsonResponse({{"station_id", id}, {"items", items}}));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "station not found" ? 404 : 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/capability-templates",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          nlohmann::json items = nlohmann::json::array();
          for (const auto& item :
               appState().repository->listCapabilityTemplates()) {
            items.push_back(capabilityTemplateToJson(item));
          }
          callback(jsonResponse({{"items", items}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 500));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/capability-templates",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto item = appState().repository->createCapabilityTemplate(
              parseCapabilityUpsert(json));
          callback(jsonResponse(capabilityTemplateToJson(item), 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/capability-templates/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        const auto item = appState().repository->getCapabilityTemplate(id);
        if (!item.has_value()) {
          callback(errorResponse("capability template not found", 404));
          return;
        }
        callback(jsonResponse(capabilityTemplateToJson(*item)));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/capability-templates/{id}",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto item = appState().repository->updateCapabilityTemplate(
              id, parseCapabilityUpsert(json));
          callback(jsonResponse(capabilityTemplateToJson(item)));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "capability template not found" ? 404 : 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/capability-templates/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->deleteCapabilityTemplate(id)) {
            callback(errorResponse("capability template not found", 404));
            return;
          }
          callback(jsonResponse({{"deleted", true}, {"id", id}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 409));
        }
      },
      {Delete});

  drogon::app().registerHandler(
      "/api/v1/capability-profiles",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          nlohmann::json items = nlohmann::json::array();
          for (const auto& item :
               appState().repository->listCapabilityProfiles()) {
            items.push_back({
                {"id", item.id},
                {"name", item.name},
                {"description", item.description},
            });
          }
          callback(jsonResponse({{"items", items}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 500));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/capability-templates/{id}/test",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto capability =
              appState().repository->getCapabilityTemplate(id);
          if (!capability.has_value()) {
            callback(errorResponse("capability template not found", 404));
            return;
          }
          const auto json = nlohmann::json::parse(req->body());
          const auto robot_id = json.value("robot_id", "");
          if (robot_id.empty()) {
            callback(errorResponse("robot_id is required", 400));
            return;
          }
          const auto robot = appState().repository->getRobot(robot_id);
          if (!robot.has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          const auto parameters =
              json.value("parameters", nlohmann::json::object());
          auto cap = std::make_shared<db::CapabilityTemplateRecord>(*capability);
          const auto request_payload = cap->request_template.empty()
              ? parameters
              : renderCapabilityTemplate(cap->request_template, parameters);
          auto protocol_config = cap->protocol_config;
          if (protocol_config.value("adapter", "") == "ROS1_ACTIONLIB" &&
              !protocol_config.contains("goal_id")) {
            protocol_config["goal_id"] =
                "cap-test-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count());
          }

          auto finished = std::make_shared<std::atomic_bool>(false);
          auto respond =
              [finished, callback = std::move(callback)](
                  nlohmann::json body, int status) mutable {
                if (finished->exchange(true)) {
                  return;
                }
                callback(jsonResponse(std::move(body), status));
              };

          if (cap->operation_kind == "SSH") {
            const auto ssh_robot_id =
                cap->protocol_config.value("ssh_robot_id", "");
            const auto ssh_profile_id =
                cap->protocol_config.value("ssh_profile_id", "");
            const auto profile = ssh_profile_id.empty()
                ? std::nullopt
                : appState().repository->getRobotStartupProfile(ssh_profile_id);
            if (ssh_robot_id != robot_id || !profile.has_value() ||
                profile->robot_id != robot_id || !profile->enabled ||
                !robot->host.has_value() || robot->host->empty()) {
              respond(
                  {{"capability_id", cap->id},
                   {"capability_key", cap->capability_key},
                   {"robot_id", robot_id},
                   {"operation_kind", "SSH"},
                   {"success", false},
                   {"error", "SSH capability profile does not match robot"},
                   {"workflow_advanced", false}},
                  400);
              return;
            }
            remote::ControlledSshRequest request{
                .job_id = "cap-test-" + cap->id,
                .host = *robot->host,
                .port = profile->ssh_port,
                .username = profile->ssh_username,
                .credential_reference = profile->credential_reference,
                .known_hosts_reference = profile->known_hosts_reference,
                .steps = profile->steps,
                .readiness_checks = profile->readiness_checks,
                .timeout_ms = cap->timeout_ms,
            };
            if (const auto error =
                    remote::ControlledSshExecutor::validateRequest(request);
                error.has_value()) {
              respond(
                  {{"capability_id", cap->id},
                   {"capability_key", cap->capability_key},
                   {"robot_id", robot_id},
                   {"operation_kind", "SSH"},
                   {"success", false},
                   {"error", *error},
                   {"workflow_advanced", false}},
                  400);
              return;
            }
            const auto started = std::chrono::steady_clock::now();
            if (!appState().ssh_executor->execute(
                    std::move(request),
                    [respond, cap, robot_id, started](
                        const remote::ControlledSshResult& result) mutable {
                      const auto elapsed_ms =
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
                      respond(
                          {{"capability_id", cap->id},
                           {"capability_key", cap->capability_key},
                           {"robot_id", robot_id},
                           {"operation_kind", "SSH"},
                           {"success", result.success},
                           {"result",
                            {{"timed_out", result.timed_out},
                             {"exit_code", result.exit_code},
                             {"steps", result.step_results}}},
                           {"error", result.error},
                           {"elapsed_ms", elapsed_ms},
                           {"workflow_advanced", false}},
                          result.success ? 200 : 502);
                    })) {
              respond(
                  {{"capability_id", cap->id},
                   {"capability_key", cap->capability_key},
                   {"robot_id", robot_id},
                   {"operation_kind", "SSH"},
                   {"success", false},
                   {"error", "SSH worker unavailable"},
                   {"workflow_advanced", false}},
                  503);
            }
            return;
          }

          const auto started = std::chrono::steady_clock::now();
          const auto dispatched = appState().robot_runtime->dispatchCapability(
              *robot,
              cap->operation_kind,
              cap->endpoint_name,
              cap->ros_message_type,
              request_payload,
              cap->timeout_ms,
              protocol_config,
              [respond, started, cap, robot_id, request_payload](
                  const ros::RosCommandEvent& event) mutable {
                if (event.kind == ros::RosCommandEvent::Kind::Feedback) {
                  return;
                }
                const auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
                respond(
                    {
                        {"capability_id", cap->id},
                        {"capability_key", cap->capability_key},
                        {"robot_id", robot_id},
                        {"operation_kind", cap->operation_kind},
                        {"endpoint_name", cap->endpoint_name},
                        {"request_payload", request_payload},
                        {"correlation_id", event.correlation_id},
                        {"success", event.success},
                        {"result", event.values},
                        {"error", event.error},
                        {"elapsed_ms", elapsed_ms},
                        {"workflow_advanced", false},
                    },
                    event.success ? 200 : 502);
              });

          if (!dispatched.accepted) {
            respond(
                {
                    {"capability_id", cap->id},
                    {"capability_key", cap->capability_key},
                    {"robot_id", robot_id},
                    {"request_payload", request_payload},
                    {"success", false},
                    {"error", dispatched.error},
                    {"workflow_advanced", false},
                },
                400);
            return;
          }

          // A topic publish has no service response/action result lifecycle.
          // Successful rosbridge publish is the terminal result for this
          // isolated test, matching STATION_ACTION topic semantics.
          if (cap->operation_kind == "TOPIC") {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            respond(
                {
                    {"capability_id", cap->id},
                    {"capability_key", cap->capability_key},
                    {"robot_id", robot_id},
                    {"operation_kind", cap->operation_kind},
                    {"endpoint_name", cap->endpoint_name},
                    {"request_payload", request_payload},
                    {"correlation_id", dispatched.correlation_id},
                    {"success", true},
                    {"result", {{"published", true}}},
                    {"error", ""},
                    {"elapsed_ms", elapsed_ms},
                    {"workflow_advanced", false},
                },
                200);
            return;
          }

          const double timeout_sec =
              std::max(1.0, static_cast<double>(cap->timeout_ms) / 1000.0);
          drogon::app().getLoop()->runAfter(
              timeout_sec,
              [respond,
               correlation = dispatched.correlation_id,
               cap,
               robot_id,
               request_payload]() mutable {
                respond(
                    {
                        {"capability_id", cap->id},
                        {"capability_key", cap->capability_key},
                        {"robot_id", robot_id},
                        {"request_payload", request_payload},
                        {"correlation_id", correlation},
                        {"success", false},
                        {"error", "capability test timed out waiting for result"},
                        {"workflow_advanced", false},
                    },
                    504);
              });
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/robots",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        nlohmann::json items = nlohmann::json::array();
        for (const auto& robot : appState().repository->listRobots()) {
          items.push_back(robotToJson(robot));
        }
        callback(jsonResponse({{"items", items}}));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/robots",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto robot =
              appState().repository->createRobot(parseRobotUpsert(json));
          scheduleRefreshConnections();
          callback(jsonResponse(robotToJson(robot), 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        const auto robot = appState().repository->getRobot(id);
        if (!robot.has_value()) {
          callback(errorResponse("robot not found", 404));
          return;
        }
        callback(jsonResponse(robotToJson(*robot)));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto robot =
              appState().repository->updateRobot(id, parseRobotUpsert(json));
          scheduleRefreshConnections();
          callback(jsonResponse(robotToJson(robot)));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "robot not found" ? 404 : 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (appState().repository->robotHasActiveScene(id)) {
            callback(errorResponse(
                "robot still belongs to an active scene; remove it from the scene first",
                409));
            return;
          }
          appState().robot_runtime->dropRobot(id);
          if (!appState().repository->deleteRobot(id)) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          callback(jsonResponse({{"deleted", true}, {"id", id}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 409));
        }
      },
      {drogon::Delete});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}/startup-profiles",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->getRobot(id).has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          nlohmann::json items = nlohmann::json::array();
          for (const auto& profile :
               appState().repository->listRobotStartupProfiles(id)) {
            items.push_back(startupProfileToJson(profile));
          }
          callback(jsonResponse({{"items", items}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}/startup-profiles",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->getRobot(id).has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          const auto json = nlohmann::json::parse(req->body());
          const auto profile = appState().repository->createRobotStartupProfile(
              id, parseStartupProfileUpsert(json));
          callback(jsonResponse(startupProfileToJson(profile), 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}/startup-profiles/{profile_id}",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id,
         const std::string& profile_id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto existing =
              appState().repository->getRobotStartupProfile(profile_id);
          if (!existing.has_value() || existing->robot_id != id) {
            callback(errorResponse("startup profile not found", 404));
            return;
          }
          const auto profile = appState().repository->updateRobotStartupProfile(
              id, profile_id, parseStartupProfileUpsert(json, existing));
          callback(jsonResponse(startupProfileToJson(profile)));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "startup profile not found" ? 404 : 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}/startup-profiles/{profile_id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id,
         const std::string& profile_id) {
        try {
          if (!appState().repository->deleteRobotStartupProfile(
                  id, profile_id)) {
            callback(errorResponse("startup profile not found", 404));
            return;
          }
          callback(jsonResponse(
              {{"id", profile_id}, {"robot_id", id}, {"deleted", true}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 409));
        }
      },
      {drogon::Delete});

  // Cached rosbridge interface catalog (survives offline). Optional ?refresh=1
  // triggers a live rosapi scan when the robot is ONLINE.
  drogon::app().registerHandler(
      "/api/v1/robots/{id}/interfaces",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto robot = appState().repository->getRobot(id);
          if (!robot.has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          const auto refresh = req->getParameter("refresh");
          if (refresh == "1" || refresh == "true") {
            auto catalog =
                appState().robot_runtime->discoverInterfaces(id, 10000);
            catalog.entity_id = id;
            if (!catalog.error.empty() && catalog.topics.empty() &&
                catalog.services.empty() && catalog.actions.empty()) {
              callback(errorResponse(catalog.error, 503));
              return;
            }
            catalog.live = catalog.error.empty();
            // Successful categories refresh independently; failed categories
            // retain their last known values in the persisted catalog.
            auto body = mergeAndStoreRobotInterfaceCatalog(id, std::move(catalog));
            callback(jsonResponse(std::move(body)));
            return;
          }
          auto cached = appState().repository->getRobotInterfaceCache(id);
          if (!cached.has_value()) {
            callback(jsonResponse(
                {{"entity_kind", "robot"},
                 {"entity_id", id},
                 {"transport", "ROSBRIDGE"},
                 {"scanned_at", ""},
                 {"live", false},
                 {"topics", nlohmann::json::array()},
                 {"services", nlohmann::json::array()},
                 {"actions", nlohmann::json::array()},
                 {"error", ""},
                 {"counts",
                  {{"topics", 0}, {"services", 0}, {"actions", 0}}}}));
            return;
          }
          (*cached)["entity_kind"] = "robot";
          (*cached)["entity_id"] = id;
          (*cached)["live"] = false;
          callback(jsonResponse(*cached));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 500));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}/interfaces/scan",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->getRobot(id).has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          auto catalog =
              appState().robot_runtime->discoverInterfaces(id, 10000);
          catalog.entity_id = id;
          if (!catalog.error.empty() && catalog.topics.empty() &&
              catalog.services.empty() && catalog.actions.empty()) {
            callback(errorResponse(catalog.error, 503));
            return;
          }
          auto body = mergeAndStoreRobotInterfaceCatalog(id, std::move(catalog));
          callback(jsonResponse(std::move(body)));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 500));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/robots/{id}/interfaces/type",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->getRobot(id).has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          const auto kind = req->getParameter("kind");
          const auto name = req->getParameter("name");
          if (kind.empty() || name.empty()) {
            callback(errorResponse("kind and name are required", 400));
            return;
          }
          const auto message_type =
              appState().robot_runtime->resolveInterfaceType(
                  id, kind, name, 5000);
          if (message_type.empty()) {
            callback(errorResponse(
                "unable to resolve type (robot offline or rosapi missing)",
                503));
            return;
          }
          callback(jsonResponse(
              {{"entity_id", id},
               {"operation_kind", kind},
               {"name", name},
               {"message_type", message_type},
               {"transport", "ROSBRIDGE"}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 500));
        }
      },
      {Get});

  // Resolve ROS type + request field schema (rosapi typedef → JSON Schema).
  drogon::app().registerHandler(
      "/api/v1/robots/{id}/interfaces/schema",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->getRobot(id).has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }
          const auto kind = req->getParameter("kind");
          const auto name = req->getParameter("name");
          const auto known_type = req->getParameter("type");
          if (kind.empty() || name.empty()) {
            callback(errorResponse("kind and name are required", 400));
            return;
          }
          const auto resolved =
              appState().robot_runtime->resolveInterfaceSchema(
                  id, kind, name, known_type, 8000);
          if (!resolved.error.empty() &&
              resolved.parameter_schema
                  .value("properties", nlohmann::json::object())
                  .empty()) {
            callback(errorResponse(resolved.error, 503));
            return;
          }
          callback(jsonResponse(
              {{"entity_id", id},
               {"operation_kind", kind},
               {"name", name},
               {"endpoint_name", resolved.endpoint_name.empty()
                                     ? name
                                     : resolved.endpoint_name},
               {"message_type", resolved.message_type},
               {"type_source", resolved.type_source},
               {"root_type", resolved.root_type},
               {"typedef_count", resolved.typedef_count},
               {"empty_request", resolved.empty_request},
               {"parameter_schema", resolved.parameter_schema},
               {"request_defaults", resolved.request_defaults},
               {"transport", "ROSBRIDGE"},
               {"error", resolved.error}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 500));
        }
      },
      {Get});

  // Reserved: device-side discovery (MQTT / HTTP / WebSocket). Robots use
  // rosbridge above; devices are not implemented yet.
  drogon::app().registerHandler(
      "/api/v1/devices/{id}/interfaces",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        callback(jsonResponse(
            {{"entity_kind", "device"},
             {"entity_id", id},
             {"implemented", false},
             {"transport_supported",
              nlohmann::json::array({"HTTP", "MQTT", "WEBSOCKET"})},
             {"transport", nullptr},
             {"topics", nlohmann::json::array()},
             {"services", nlohmann::json::array()},
             {"actions", nlohmann::json::array()},
             {"error",
              "device interface discovery is reserved; use robot "
              "rosbridge scan for robots"}},
            501));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/devices/{id}/interfaces/scan",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        callback(jsonResponse(
            {{"entity_kind", "device"},
             {"entity_id", id},
             {"implemented", false},
             {"transport_supported",
              nlohmann::json::array({"HTTP", "MQTT", "WEBSOCKET"})},
             {"error",
              "device interface scan not implemented (MQTT/HTTP reserved)"}},
            501));
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}/maps",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        if (!appState().repository->getScene(id).has_value()) {
          callback(errorResponse("scene not found", 404));
          return;
        }
        nlohmann::json items = nlohmann::json::array();
        for (const auto& map : appState().repository->listMapVersions(id)) {
          items.push_back(mapToJson(map));
        }
        callback(jsonResponse({{"items", items}}));
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/maps/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto map = appState().repository->getMapVersion(id);
          if (!map.has_value()) {
            callback(errorResponse("map version not found", 404));
            return;
          }
          const auto action =
              appState().repository->removeOrArchiveMapVersion(id);
          if (action == "deleted") {
            std::error_code ec;
            for (const auto& path :
                 {map->pgm_path, map->yaml_path, map->preview_path}) {
              if (!path.empty()) {
                std::filesystem::remove(path, ec);
              }
            }
            if (!map->pgm_path.empty()) {
              std::filesystem::remove(
                  std::filesystem::path(map->pgm_path).parent_path(),
                  ec);
            }
          }
          callback(jsonResponse({{"id", id}, {"action", action}}));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          callback(errorResponse(
              message,
              message == "map version not found" ? 404 : 409));
        }
      },
      {drogon::Delete});

  drogon::app().registerHandler(
      "/api/v1/maps/import",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          drogon::MultiPartParser parser;
          if (parser.parse(req) != 0) {
            callback(errorResponse("multipart parse failed", 400));
            return;
          }

          const auto& params = parser.getParameters();
          std::string scene_id;
          std::string scene_name;
          if (const auto it = params.find("scene_id"); it != params.end()) {
            scene_id = it->second;
          }
          if (const auto it = params.find("scene_name"); it != params.end()) {
            scene_name = it->second;
          }
          if (scene_id.empty()) {
            if (scene_name.empty()) {
              callback(errorResponse("scene_id or scene_name is required", 400));
              return;
            }
            scene_id =
                appState().repository->createScene(scene_name, "").id;
          } else if (!appState().repository->getScene(scene_id).has_value()) {
            callback(errorResponse("scene not found", 404));
            return;
          }

          const auto& files = parser.getFiles();
          std::string yaml_text;
          std::string pgm_bytes;
          auto endsWithInsensitive =
              [](const std::string& value, const std::string& suffix) {
                if (value.size() < suffix.size()) {
                  return false;
                }
                for (std::size_t i = 0; i < suffix.size(); ++i) {
                  const auto left = static_cast<unsigned char>(
                      value[value.size() - suffix.size() + i]);
                  const auto right = static_cast<unsigned char>(suffix[i]);
                  if (std::tolower(left) != std::tolower(right)) {
                    return false;
                  }
                }
                return true;
              };
          for (const auto& file : files) {
            const auto name = file.getFileName();
            const auto item = file.getItemName();
            const std::string content(
                file.fileData(),
                static_cast<std::size_t>(file.fileLength()));
            if (item == "yaml" || endsWithInsensitive(name, ".yaml") ||
                endsWithInsensitive(name, ".yml")) {
              yaml_text = content;
            } else if (item == "pgm" || endsWithInsensitive(name, ".pgm")) {
              pgm_bytes = content;
            }
          }
          if (yaml_text.empty() || pgm_bytes.empty()) {
            callback(errorResponse("both .yaml and .pgm files are required", 400));
            return;
          }

          const int version = appState().repository->nextMapVersion(scene_id);
          const auto materialized = maps::materializeMapFiles(
              appState().map_root,
              scene_id,
              version,
              yaml_text,
              pgm_bytes);
          if (!materialized.ok) {
            callback(jsonResponse(
                {{"error", "map import failed"},
                 {"details", materialized.errors}},
                400));
            return;
          }

          const auto map = appState().repository->insertMapVersion(
              scene_id,
              version,
              materialized.files);
          callback(jsonResponse(
              {{"scene", sceneToJson(*appState().repository->getScene(scene_id))},
               {"map", mapToJson(map)}},
              201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/maps/{id}/preview",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        const auto map = appState().repository->getMapVersion(id);
        if (!map.has_value()) {
          callback(errorResponse("map not found", 404));
          return;
        }
        if (!maps::ensurePreviewPng(map->preview_path, map->pgm_path)) {
          callback(errorResponse("preview unavailable", 404));
          return;
        }
        std::ifstream input(map->preview_path, std::ios::binary);
        if (!input) {
          callback(errorResponse("preview unavailable", 404));
          return;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        auto response = HttpResponse::newHttpResponse();
        response->setBody(buffer.str());
        response->setContentTypeString("image/png");
        response->addHeader("Cache-Control", "no-store");
        callback(response);
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/navigation/goals",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto robot_id = json.at("robot_id").get<std::string>();
          const auto scene_id = json.at("scene_id").get<std::string>();
          const auto map_version_id =
              json.at("map_version_id").get<std::string>();
          const double x = json.at("x").get<double>();
          const double y = json.at("y").get<double>();
          const double yaw = json.value("yaw", 0.0);
          const double distance_tolerance =
              json.value("distance_tolerance", 0.04);
          const double heading_tolerance =
              json.value("heading_tolerance", 0.04);
          if (!std::isfinite(distance_tolerance) ||
              distance_tolerance <= 0.0 ||
              !std::isfinite(heading_tolerance) ||
              heading_tolerance <= 0.0) {
            callback(errorResponse(
                "distance_tolerance and heading_tolerance must be positive",
                400));
            return;
          }

          const auto map =
              appState().repository->getMapVersion(map_version_id);
          if (!map.has_value()) {
            callback(errorResponse("map not found", 404));
            return;
          }
          const auto pixel = domain::worldToPixel(
              toMetadata(*map),
              domain::WorldPose2D{.x = x, .y = y, .yaw = yaw});
          if (!pixel.has_value() || !pixel->inside_map) {
            callback(errorResponse("navigation target outside map", 400));
            return;
          }

          const auto robot = appState().repository->getRobot(robot_id);
          if (!robot.has_value()) {
            callback(errorResponse("robot not found", 404));
            return;
          }

          const auto goal = appState().repository->enqueueNavigationGoal(
              robot_id,
              scene_id,
              map_version_id,
              x,
              y,
              yaw,
              distance_tolerance,
              heading_tolerance);
          const bool sent = appState().robot_runtime->sendNavigationGoal(
              *robot,
              goal,
              x,
              y,
              yaw,
              ros::NavigationGoalOptions{
                  .distance_tolerance = distance_tolerance,
                  .heading_tolerance = heading_tolerance,
              });
          if (!sent) {
            appState().repository->updateOutboxState(
                goal.outbox_id,
                "PENDING",
                nlohmann::json{
                    {"warning",
                     "robot offline or navigation capability unavailable; "
                     "command kept in outbox"}});
          }

          callback(jsonResponse(
              {{"command_id", goal.command_id},
               {"outbox_id", goal.outbox_id},
               {"state", sent ? "SENT" : "PENDING"},
               {"confirmation",
                {{"robot_id", robot_id},
                 {"robot_name", robot->name},
                 {"scene_id", scene_id},
                 {"map_version_id", map_version_id},
                 {"map_version", map->version},
                 {"x", x},
                 {"y", y},
                 {"yaw", yaw},
                 {"distance_tolerance", distance_tolerance},
                 {"heading_tolerance", heading_tolerance}}}},
              201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/maps/{id}/world-from-pixel",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto map = appState().repository->getMapVersion(id);
          if (!map.has_value()) {
            callback(errorResponse("map not found", 404));
            return;
          }
          const double pixel_x = json.at("pixel_x").get<double>();
          const double pixel_y = json.at("pixel_y").get<double>();
          if (!std::isfinite(pixel_x) || !std::isfinite(pixel_y) ||
              pixel_x < 0.0 || pixel_y < 0.0 ||
              pixel_x >= static_cast<double>(map->width) ||
              pixel_y >= static_cast<double>(map->height)) {
            callback(errorResponse("pixel coordinates outside map", 400));
            return;
          }
          const auto world = domain::pixelToWorld(
              toMetadata(*map),
              domain::PixelPose2D{
                  .x = pixel_x,
                  .y = pixel_y,
                  .yaw = json.value("yaw", 0.0),
              });
          if (!world.has_value()) {
            callback(errorResponse("invalid pixel coordinates", 400));
            return;
          }
          callback(jsonResponse(
              {{"x", world->x}, {"y", world->y}, {"yaw", world->yaw}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  // ---- M3 workflows ----
  auto workflowSummaryToJson =
      [](const db::WorkflowSummaryRecord& item) -> nlohmann::json {
    nlohmann::json json{
        {"id", item.id},
        {"name", item.name},
        {"description", item.description},
        {"trigger_type", item.trigger_type},
        {"trigger_config", item.trigger_config},
        {"draft_status", item.draft_status},
    };
    if (item.scene_id.has_value()) {
      json["scene_id"] = *item.scene_id;
    } else {
      json["scene_id"] = nullptr;
    }
    if (item.draft_version.has_value()) {
      json["draft_version"] = *item.draft_version;
    } else {
      json["draft_version"] = nullptr;
    }
    if (item.published_version.has_value()) {
      json["published_version"] = *item.published_version;
    } else {
      json["published_version"] = nullptr;
    }
    return json;
  };

  auto workflowDetailToJson =
      [workflowSummaryToJson](const db::WorkflowDetailRecord& detail)
      -> nlohmann::json {
    auto json = workflowSummaryToJson(detail.summary);
    json["graph"] = detail.graph;
    if (detail.draft_version_id.has_value()) {
      json["draft_version_id"] = *detail.draft_version_id;
    } else {
      json["draft_version_id"] = nullptr;
    }
    return json;
  };

  auto validateWorkflowGraph =
      [](const nlohmann::json& graph) -> std::optional<std::string> {
    if (!graph.is_object()) {
      return "graph must be an object";
    }
    const auto nodes = graph.value("nodes", nlohmann::json::array());
    const auto edges = graph.value("edges", nlohmann::json::array());
    if (!nodes.is_array() || !edges.is_array()) {
      return "graph.nodes and graph.edges must be arrays";
    }
    int start_count = 0;
    int end_count = 0;
    std::unordered_map<std::string, nlohmann::json> nodes_by_id;
    for (const auto& node : nodes) {
      const auto node_id = node.value("id", "");
      if (!node_id.empty()) {
        nodes_by_id[node_id] = node;
      }
      const auto type = node.value("type", "");
      if (type == "START") {
        ++start_count;
      }
      if (type == "END") {
        ++end_count;
      }
      if (type == "EVENT_WAIT") {
        const auto data = node.value("data", nlohmann::json::object());
        if (data.value("event_name", "").empty()) {
          return "EVENT_WAIT node requires data.event_name";
        }
      }
      if (type == "DELAY") {
        const auto data = node.value("data", nlohmann::json::object());
        const auto delay_ms = data.value("delay_ms", -1);
        if (delay_ms < 0 || delay_ms > 24 * 60 * 60 * 1000) {
          return "DELAY node requires data.delay_ms between 0 and 86400000";
        }
      }
      if (type == "MANUAL_CONFIRM") {
        const auto data = node.value("data", nlohmann::json::object());
        const auto prompt = data.value("prompt", "");
        if (prompt.empty()) {
          return "MANUAL_CONFIRM node requires data.prompt";
        }
        if (prompt.size() > 500) {
          return "MANUAL_CONFIRM data.prompt must not exceed 500 characters";
        }
      }
      if (type == "ROBOT_STARTUP" || type == "ROBOT_SSH") {
        const auto data = node.value("data", nlohmann::json::object());
        const auto robot_id = data.value("robot_id", "");
        const auto profile_id = data.value("startup_profile_id", "");
        if (robot_id.empty() || profile_id.empty()) {
          return "ROBOT_STARTUP requires data.robot_id and data.startup_profile_id";
        }
        const auto robot = appState().repository->getRobot(robot_id);
        const auto profile =
            appState().repository->getRobotStartupProfile(profile_id);
        if (!robot.has_value() || !profile.has_value() ||
            profile->robot_id != robot_id) {
          return "ROBOT_STARTUP references unknown robot/profile";
        }
        if (data.value("timeout_ms", profile->timeout_ms) < 1000 ||
            data.value("timeout_ms", profile->timeout_ms) > 3600000) {
          return "ROBOT_STARTUP timeout_ms must be between 1000 and 3600000";
        }
      }
      if (type == "SUBFLOW") {
        const auto data = node.value("data", nlohmann::json::object());
        const auto workflow_id = data.value("workflow_definition_id", "");
        if (workflow_id.empty()) {
          return "SUBFLOW requires data.workflow_definition_id";
        }
        if (!appState().repository->getLatestPublishedWorkflow(workflow_id)
                 .has_value()) {
          return "SUBFLOW requires an existing published workflow";
        }
      }
      if (type == "STATION_ACTION") {
        const auto data = node.value("data", nlohmann::json::object());
        if (data.value("station_action_id", "").empty() &&
            data.value("station_id", "").empty()) {
          return "STATION_ACTION node requires station_action_id or station_id";
        }
      }
      if (type == "ROBOT_CAPABILITY") {
        const auto data = node.value("data", nlohmann::json::object());
        const auto robot_id = data.value("robot_id", "");
        const auto capability_id = data.value("capability_definition_id", "");
        if (robot_id.empty()) {
          return "ROBOT_CAPABILITY node requires data.robot_id";
        }
        if (capability_id.empty()) {
          return "ROBOT_CAPABILITY node requires data.capability_definition_id";
        }
        if (!appState().repository->getRobot(robot_id).has_value()) {
          return "ROBOT_CAPABILITY references unknown robot";
        }
        if (!appState().repository->getCapabilityTemplate(capability_id)
                 .has_value()) {
          return "ROBOT_CAPABILITY references unknown capability";
        }
        if (data.contains("parameters") && !data["parameters"].is_object()) {
          return "ROBOT_CAPABILITY node parameters must be an object";
        }
        if (data.value("timeout_ms", 1) <= 0) {
          return "ROBOT_CAPABILITY node timeout_ms must be > 0";
        }
        if (data.value("retry_count", 0) < 0 ||
            data.value("retry_count", 0) > 100) {
          return "ROBOT_CAPABILITY retry_count must be between 0 and 100";
        }
        if (data.value("retry_delay_ms", 1000) < 0 ||
            data.value("retry_delay_ms", 1000) > 24 * 60 * 60 * 1000) {
          return "ROBOT_CAPABILITY retry_delay_ms must be between 0 and 86400000";
        }
      }
      if (type == "NAVIGATION") {
        const auto data = node.value("data", nlohmann::json::object());
        if (data.value("to_station_id", "").empty()) {
          return "NAVIGATION node requires data.to_station_id";
        }
        if (data.value("robot_id", "").empty()) {
          return "NAVIGATION node requires data.robot_id (which robot navigates)";
        }
      }
    }
    if (start_count != 1) {
      return "graph must contain exactly one START node";
    }
    if (end_count < 1) {
      return "graph must contain at least one END node";
    }
    const auto run_policy = graph.value("run_policy", nlohmann::json::object());
    const auto loop_count = run_policy.value("loop_count", 1);
    const auto loop_delay_ms = run_policy.value("loop_delay_ms", 0);
    if (loop_count < 1 || loop_count > 1000) {
      return "graph.run_policy.loop_count must be between 1 and 1000";
    }
    if (loop_delay_ms < 0 || loop_delay_ms > 24 * 60 * 60 * 1000) {
      return "graph.run_policy.loop_delay_ms must be between 0 and 86400000";
    }
    for (const auto& edge : edges) {
      const auto kind = edge.value("edge_kind", edge.value("label", "success"));
      if (kind == "failure" || kind == "FAILURE") {
        const auto source_id = edge.value("source", "");
        const auto source_it = nodes_by_id.find(source_id);
        if (source_it == nodes_by_id.end()) {
          return "failure edge references unknown source node";
        }
        if (source_it->second.value("type", "") == "START") {
          return "START node cannot emit failure edges; use a success edge";
        }
      }
      if (kind == "event" || kind == "EVENT") {
        const auto event_name = edge.value(
            "event_name",
            edge.value("data", nlohmann::json::object()).value("event_name", ""));
        if (event_name.empty()) {
          return "event edge requires event_name";
        }
        const auto source_id = edge.value("source", "");
        const auto source_it = nodes_by_id.find(source_id);
        if (source_it == nodes_by_id.end()) {
          return "event edge references unknown source node";
        }
        const auto source_type = source_it->second.value("type", "");
        const auto source_data = source_it->second.value(
            "data", nlohmann::json::object());
        std::vector<workflow::EventSpec> specs;
        if (source_type == "START") {
          return "START node cannot emit event edges; use a success edge";
        }
        if (source_type == "ROBOT_CAPABILITY") {
          const auto capability_id = source_data.value(
              "capability_definition_id", "");
          const auto capability =
              appState().repository->getCapabilityTemplate(capability_id);
          if (!capability.has_value()) {
            return "ROBOT_CAPABILITY event edge references unknown capability";
          }
          specs = workflow::resolveEventSpecs(
              capability->event_specs,
              nlohmann::json::array(),
              source_data.value("success_event_name", ""));
        } else if (source_type == "STATION_ACTION") {
          const auto action_id = source_data.value("station_action_id", "");
          const auto action = action_id.empty()
              ? std::nullopt
              : appState().repository->getStationAction(action_id);
          if (!action.has_value()) {
            return "STATION_ACTION event edge requires a valid station_action_id";
          }
          nlohmann::json capability_specs = nlohmann::json::array();
          if (action->capability_definition_id.has_value()) {
            const auto capability = appState().repository->getCapabilityTemplate(
                *action->capability_definition_id);
            if (capability.has_value()) {
              capability_specs = capability->event_specs;
            }
          }
          specs = workflow::resolveEventSpecs(
              capability_specs,
              action->event_specs,
              action->success_event_name);
        }
        if ((source_type == "ROBOT_CAPABILITY" ||
             source_type == "STATION_ACTION") &&
            !workflow::hasEmittableNodeEvent(specs, event_name)) {
          return source_type + " event edge '" + event_name +
              "' is not enabled for node emission";
        }
      }
    }
    return std::nullopt;
  };

  drogon::app().registerHandler(
      "/api/v1/scenes/{id}/orchestration-assets",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto scene = appState().repository->getScene(id);
          if (!scene.has_value()) {
            callback(errorResponse("scene not found", 404));
            return;
          }
          nlohmann::json robots = nlohmann::json::array();
          nlohmann::json startup_profiles = nlohmann::json::array();
          const auto robot_ids = appState().repository->listSceneRobotIds(id);
          for (const auto& robot_id : robot_ids) {
            const auto robot = appState().repository->getRobot(robot_id);
            if (!robot.has_value()) {
              continue;
            }
            robots.push_back({
                {"id", robot->id},
                {"name", robot->name},
                {"connection_state", robot->connection_state},
                {"localization_status", robot->localization_status},
                {"robot_type", robot->robot_type},
            });
            for (const auto& profile :
                 appState().repository->listRobotStartupProfiles(robot_id)) {
              if (profile.enabled) {
                startup_profiles.push_back(startupProfileToJson(profile));
              }
            }
          }

          nlohmann::json points = nlohmann::json::array();
          nlohmann::json suggested_events = nlohmann::json::array();
          std::optional<db::MapVersionRecord> map;
          if (scene->active_map_version_id.has_value()) {
            map = appState().repository->getMapVersion(
                *scene->active_map_version_id);
          }
          for (const auto& station :
               appState().repository->listStations(id, scene->active_map_version_id)) {
            auto point = stationToJson(station, map);
            nlohmann::json actions = nlohmann::json::array();
            for (const auto& action :
                 appState().repository->listStationActions(station.id)) {
              actions.push_back(stationActionToJson(action));
              std::optional<db::CapabilityTemplateRecord> capability;
              if (action.capability_definition_id.has_value()) {
                capability = appState().repository->getCapabilityTemplate(
                    *action.capability_definition_id);
              }
              const auto specs = workflow::resolveEventSpecs(
                  capability.has_value() ? capability->event_specs
                                         : nlohmann::json::array(),
                  action.event_specs,
                  action.success_event_name);
              for (const auto& spec : specs) {
                if (!spec.enabled || spec.event_name.empty()) {
                  continue;
                }
                suggested_events.push_back({
                    {"event_name", spec.event_name},
                    {"source", spec.source},
                    {"source_type", "event_spec"},
                    {"station_id", station.id},
                    {"station_name", station.name},
                    {"station_action_id", action.id},
                    {"action_name", action.action_name},
                    {"capability_key", action.capability_key},
                    {"when",
                     {{"field", spec.when.field},
                      {"op", spec.when.op},
                      {"value", spec.when.value}}},
                });
              }
            }
            point["actions"] = actions;
            points.push_back(point);
          }

          nlohmann::json capabilities = nlohmann::json::array();
          for (const auto& item :
               appState().repository->listCapabilityTemplates()) {
            capabilities.push_back({
                {"id", item.id},
                {"capability_key", item.capability_key},
                {"operation_kind", item.operation_kind},
                {"endpoint_name", item.endpoint_name},
                {"ros_message_type", item.ros_message_type},
                {"motion_ownership", item.motion_ownership},
                {"blocking_type", item.blocking_type},
                {"timeout_ms", item.timeout_ms},
                {"parameter_schema", item.parameter_schema},
                {"request_template", item.request_template},
                {"event_specs", item.event_specs},
            });
          }

          nlohmann::json body{
              {"scene", sceneToJson(*scene)},
              {"robots", robots},
              {"startup_profiles", startup_profiles},
              {"points", points},
              {"capabilities", capabilities},
              {"suggested_events", suggested_events},
          };
          if (map.has_value()) {
            body["map"] = mapToJson(*map);
          } else {
            body["map"] = nullptr;
          }
          callback(jsonResponse(body));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/workflows",
      [workflowSummaryToJson](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          std::optional<std::string> scene_id;
          const auto scene_id_param = req->getParameter("scene_id");
          if (!scene_id_param.empty()) {
            scene_id = scene_id_param;
          }
          nlohmann::json items = nlohmann::json::array();
          for (const auto& item :
               appState().repository->listWorkflows(scene_id)) {
            items.push_back(workflowSummaryToJson(item));
          }
          callback(jsonResponse({{"items", items}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/workflows",
      [workflowDetailToJson](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto name = json.at("name").get<std::string>();
          if (name.empty()) {
            callback(errorResponse("name is required", 400));
            return;
          }
          std::optional<std::string> scene_id;
          if (json.contains("scene_id") && !json["scene_id"].is_null()) {
            scene_id = json["scene_id"].get<std::string>();
          }
          const auto created = appState().repository->createWorkflow(
              name,
              json.value("description", ""),
              scene_id,
              json.value("trigger_type", "MANUAL"),
              json.value("trigger_config", nlohmann::json::object()),
              json.value(
                  "graph",
                  nlohmann::json{
                      {"nodes", nlohmann::json::array()},
                      {"edges", nlohmann::json::array()}}));
          callback(jsonResponse(workflowDetailToJson(created), 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/workflows/{id}",
      [workflowDetailToJson](
          const HttpRequestPtr&,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& id) {
        try {
          const auto detail = appState().repository->getWorkflow(id);
          if (!detail.has_value()) {
            callback(errorResponse("workflow not found", 404));
            return;
          }
          callback(jsonResponse(workflowDetailToJson(*detail)));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/workflows/{id}",
      [workflowDetailToJson, validateWorkflowGraph](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto graph = json.value(
              "graph",
              nlohmann::json{
                  {"nodes", nlohmann::json::array()},
                  {"edges", nlohmann::json::array()}});
          if (const auto error = validateWorkflowGraph(graph);
              error.has_value() && json.value("validate", false)) {
            callback(errorResponse(*error, 400));
            return;
          }
          std::optional<std::string> scene_id;
          if (json.contains("scene_id") && !json["scene_id"].is_null()) {
            scene_id = json["scene_id"].get<std::string>();
          }
          const auto updated = appState().repository->updateWorkflow(
              id,
              json.at("name").get<std::string>(),
              json.value("description", ""),
              scene_id,
              json.value("trigger_type", "MANUAL"),
              json.value("trigger_config", nlohmann::json::object()),
              graph);
          callback(jsonResponse(workflowDetailToJson(updated)));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Put});

  drogon::app().registerHandler(
      "/api/v1/workflows/{id}",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          if (!appState().repository->deleteWorkflow(id)) {
            callback(errorResponse("workflow not found", 404));
            return;
          }
          callback(jsonResponse({{"id", id}, {"deleted", true}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {drogon::Delete});

  drogon::app().registerHandler(
      "/api/v1/workflows/{id}/publish",
      [workflowDetailToJson, validateWorkflowGraph](
          const HttpRequestPtr&,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& id) {
        try {
          const auto existing = appState().repository->getWorkflow(id);
          if (!existing.has_value()) {
            callback(errorResponse("workflow not found", 404));
            return;
          }
          if (const auto error = validateWorkflowGraph(existing->graph);
              error.has_value()) {
            callback(errorResponse(*error, 400));
            return;
          }
          const auto published = appState().repository->publishWorkflow(id);
          callback(jsonResponse(workflowDetailToJson(published)));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  auto workflowRunToJson = [](const db::WorkflowRunRecord& run) -> nlohmann::json {
    return {
        {"id", run.id},
        {"workflow_version_id", run.workflow_version_id},
        {"workflow_definition_id", run.workflow_definition_id},
        {"workflow_name", run.workflow_name},
        {"workflow_version", run.workflow_version},
        {"trigger_type", run.trigger_type},
        {"trigger_metadata", run.trigger_metadata},
        {"state", run.state},
        {"input_data", run.input_data},
        {"context_data", run.context_data},
        {"parent_run_id", run.parent_run_id.value_or("")},
        {"root_run_id", run.root_run_id.value_or("")},
        {"source_run_id", run.source_run_id.value_or("")},
        {"parent_node_run_id", run.parent_node_run_id.value_or("")},
        {"causation_event_id", run.causation_event_id.value_or("")},
        {"business_key", run.business_key},
        {"started_at", run.started_at.value_or("")},
      {"finished_at", run.finished_at.value_or("")},
      {"archived_at", run.archived_at.value_or("")},
      {"created_at", run.created_at},
    };
  };

  auto workflowRunDetailToJson =
      [workflowRunToJson](const db::WorkflowRunDetail& detail) -> nlohmann::json {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : detail.nodes) {
      nlohmann::json item{
          {"id", node.id},
          {"workflow_run_id", node.workflow_run_id},
          {"node_key", node.node_key},
          {"attempt", node.attempt},
          {"state", node.state},
          {"assigned_robot_id", node.assigned_robot_id.value_or("")},
          {"input_data", node.input_data},
          {"output_data", node.output_data},
          {"started_at", node.started_at.value_or("")},
          {"finished_at", node.finished_at.value_or("")},
      };
      if (node.error_data.is_null()) {
        item["error_data"] = nullptr;
      } else {
        item["error_data"] = node.error_data;
      }
      nodes.push_back(item);
    }
    auto json = workflowRunToJson(detail.run);
    json["graph"] = detail.graph;
    json["parent_run_id"] = detail.run.parent_run_id.value_or("");
    json["root_run_id"] = detail.run.root_run_id.value_or("");
    json["source_run_id"] = detail.run.source_run_id.value_or("");
    json["parent_node_run_id"] =
        detail.run.parent_node_run_id.value_or("");
    json["causation_event_id"] =
        detail.run.causation_event_id.value_or("");
    json["business_key"] = detail.run.business_key;
    json["nodes"] = nodes;
    nlohmann::json commands = nlohmann::json::array();
    for (const auto& command : detail.commands) {
      commands.push_back({
          {"id", command.id},
          {"command_id", command.command_id},
          {"workflow_run_id", command.workflow_run_id},
          {"node_run_id", command.node_run_id},
          {"robot_id", command.robot_id},
          {"capability_definition_id",
           command.capability_definition_id.value_or("")},
          {"operation_kind", command.operation_kind},
          {"endpoint_name", command.endpoint_name},
          {"correlation_id", command.correlation_id},
          {"state", command.state},
          {"request_payload", command.request_payload},
          {"last_feedback", command.last_feedback},
          {"result_payload", command.result_payload},
          {"error_data", command.error_data},
          {"dispatched_at", command.dispatched_at.value_or("")},
          {"completed_at", command.completed_at.value_or("")},
          {"created_at", command.created_at},
      });
    }
    json["commands"] = commands;
    nlohmann::json events = nlohmann::json::array();
    for (const auto& event : detail.events) {
      events.push_back({
          {"id", event.id},
          {"workflow_run_id", event.workflow_run_id},
          {"node_run_id", event.node_run_id.value_or("")},
          {"event_type", event.event_type},
          {"payload", event.payload},
          {"occurred_at", event.occurred_at},
      });
    }
    json["events"] = events;
    return json;
  };

  drogon::app().registerHandler(
      "/api/v1/workflow-runs",
      [workflowRunToJson](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto archived = req->getParameter("archived").empty()
              ? "exclude"
              : req->getParameter("archived");
          int limit = 100;
          if (!req->getParameter("limit").empty()) {
            limit = std::stoi(req->getParameter("limit"));
          }
          nlohmann::json items = nlohmann::json::array();
          for (const auto& run :
               appState().repository->listWorkflowRuns(limit, archived)) {
            items.push_back(workflowRunToJson(run));
          }
          callback(jsonResponse({{"items", items}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/workflow-runs",
      [workflowRunDetailToJson](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          if (!appState().workflow_executor) {
            callback(errorResponse("workflow executor unavailable", 503));
            return;
          }
          const auto json = nlohmann::json::parse(req->body());
          const auto workflow_id = json.at("workflow_id").get<std::string>();
          const auto detail = appState().workflow_executor->startManual(
              workflow_id, json.value("input_data", nlohmann::json::object()));
          callback(jsonResponse(workflowRunDetailToJson(detail), 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/workflow-runs/{id}/archive",
      [](const HttpRequestPtr&,
         std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string& id) {
        try {
          const auto existing = appState().repository->getWorkflowRun(id);
          if (!existing.has_value()) {
            callback(errorResponse("workflow run not found", 404));
            return;
          }
          if (!appState().repository->archiveWorkflowRun(id)) {
            callback(errorResponse(
                "only terminal, non-archived workflow runs can be archived", 409));
            return;
          }
          callback(jsonResponse({{"id", id}, {"archived", true}}));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/workflow-runs/history/cleanup",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const int older_than_days = json.value("older_than_days", 30);
          if (!json.value("confirm", false)) {
            callback(errorResponse("confirm=true is required", 400));
            return;
          }
          const auto deleted =
              appState().repository->cleanupArchivedWorkflowRuns(older_than_days);
          callback(jsonResponse({
              {"deleted", deleted},
              {"older_than_days", older_than_days},
          }));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/workflow-runs/{id}",
      [workflowRunDetailToJson](
          const HttpRequestPtr&,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& id) {
        try {
          const auto detail = appState().repository->getWorkflowRun(id);
          if (!detail.has_value()) {
            callback(errorResponse("workflow run not found", 404));
            return;
          }
          callback(jsonResponse(workflowRunDetailToJson(*detail)));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Get});

  drogon::app().registerHandler(
      "/api/v1/workflow-runs/{id}/cancel",
      [workflowRunDetailToJson](
          const HttpRequestPtr&,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& id) {
        try {
          const auto detail = appState().workflow_executor->cancel(id);
          callback(jsonResponse(workflowRunDetailToJson(detail)));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/workflow-runs/{id}/nodes/{node_id}/manual-confirm",
      [workflowRunDetailToJson](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& id,
          const std::string& node_id) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto decision = json.value("decision", "");
          if (decision != "APPROVE" && decision != "REJECT") {
            callback(errorResponse(
                "decision must be APPROVE or REJECT", 400));
            return;
          }
          const auto note = json.value("note", "");
          if (note.size() > 1000) {
            callback(errorResponse("note must not exceed 1000 characters", 400));
            return;
          }
          const nlohmann::json audit{
              {"client_ip", req->peerAddr().toIp()},
              {"browser_session_id", req->getHeader("X-Engineer-Session")},
          };
          const bool approved = decision == "APPROVE";
          const auto detail =
              appState().workflow_executor->decideManualConfirmation(
                  id, node_id, approved, note, audit);
          ops::OpsLog::instance().info(
              "workflow",
              approved ? "manual confirmation approved"
                       : "manual confirmation rejected",
              {{"workflow_run_id", id},
               {"node_run_id", node_id},
               {"note", note},
               {"client_ip", audit["client_ip"]},
               {"browser_session_id", audit["browser_session_id"]}});
          callback(jsonResponse(workflowRunDetailToJson(detail)));
        } catch (const std::exception& ex) {
          const std::string message = ex.what();
          const int status = message.find("not found") != std::string::npos
              ? 404
              : message.find("already decided") != std::string::npos ||
                      message.find("not running") != std::string::npos
                  ? 409
                  : 400;
          callback(errorResponse(message, status));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/workflow-events",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        try {
          const auto json = nlohmann::json::parse(req->body());
          const auto event_name = json.at("event_name").get<std::string>();
          nlohmann::json payload =
              json.value("payload", nlohmann::json::object());
          if (!payload.is_object()) {
            callback(errorResponse("payload must be an object", 400));
            return;
          }
          const auto copyString = [&json, &payload](const char* key) {
            if (json.contains(key) && json[key].is_string()) {
              payload[key] = json[key];
            }
          };
          for (const auto* key : {"source_run_id", "source_node_run_id",
                                  "target_run_id",
                                  "target_workflow_definition_id",
                                  "business_key", "deduplication_key"}) {
            copyString(key);
          }
          const auto result = appState().workflow_executor->injectEvent(
              event_name, payload);
          callback(jsonResponse(result, 201));
        } catch (const std::exception& ex) {
          callback(errorResponse(ex.what(), 400));
        }
      },
      {Post});

  drogon::app().registerHandler(
      "/api/v1/ops-logs",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        std::size_t limit = 100;
        std::int64_t after_id = 0;
        try {
          const auto limit_raw = req->getParameter("limit");
          if (!limit_raw.empty()) {
            limit = static_cast<std::size_t>(std::stoul(limit_raw));
          }
          const auto after_raw = req->getParameter("after_id");
          if (!after_raw.empty()) {
            after_id = std::stoll(after_raw);
          }
        } catch (...) {
        }
        limit = std::min<std::size_t>(limit, 500);
        nlohmann::json items = nlohmann::json::array();
        for (const auto& entry : ops::OpsLog::instance().list(limit, after_id)) {
          items.push_back({
              {"id", entry.id},
              {"level", entry.level},
              {"source", entry.source},
              {"message", entry.message},
              {"detail", entry.detail},
              {"at", std::chrono::duration_cast<std::chrono::milliseconds>(
                         entry.at.time_since_epoch())
                         .count()},
          });
        }
        callback(jsonResponse({{"items", items}}));
      },
      {Get});

  // Connect READY robots after routes are registered (and on every process boot).
  if (appState().robot_runtime != nullptr) {
    ops::OpsLog::instance().clear();
    appState().robot_runtime->refreshConnections();
    ops::OpsLog::instance().info("system", "dispatcher started, refreshing robot connections");
  }
}

}  // namespace dispatcher::api
