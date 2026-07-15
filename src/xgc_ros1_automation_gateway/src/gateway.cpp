#include "xgc_ros1_automation_gateway/gateway.hpp"

#include <ros/master.h>
#include <ros/names.h>
#include <ros/ros.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "xgc_ros1_automation_gateway/error.hpp"

namespace xgc_ros1_automation_gateway {
namespace {

constexpr std::uint32_t kMaximumQueueSize = 10000;
constexpr std::uint32_t kMaximumWaitMilliseconds = 300000;
constexpr std::size_t kMaximumRequestIdBytes = 128;

const std::regex& rosTypePattern() {
  static const std::regex pattern(
      "^[A-Za-z][A-Za-z0-9_]*/[A-Za-z][A-Za-z0-9_]*$");
  return pattern;
}

void requireObject(const Json::Value& value, const std::string& field) {
  if (!value.isObject()) {
    permanentError("invalid_request", field + " must be a JSON object");
  }
}

void rejectUnknownFields(const Json::Value& request,
                         const std::set<std::string>& allowed) {
  for (const auto& member : request.getMemberNames()) {
    if (allowed.count(member) == 0) {
      permanentError("invalid_request",
                     "unknown request field '" + member + "'");
    }
  }
}

std::string requiredString(const Json::Value& request,
                           const std::string& field) {
  if (!request.isMember(field) || !request[field].isString() ||
      request[field].asString().empty()) {
    permanentError("invalid_request", field + " must be a non-empty string");
  }
  return request[field].asString();
}

bool optionalBool(const Json::Value& request, const std::string& field,
                  bool default_value) {
  if (!request.isMember(field)) {
    return default_value;
  }
  if (!request[field].isBool()) {
    permanentError("invalid_request", field + " must be a boolean");
  }
  return request[field].asBool();
}

std::uint32_t optionalUInt(const Json::Value& request, const std::string& field,
                           std::uint32_t default_value, std::uint32_t minimum,
                           std::uint32_t maximum) {
  if (!request.isMember(field)) {
    return default_value;
  }
  const Json::Value& value = request[field];
  if (!value.isIntegral() || (value.isInt64() && value.asInt64() < 0)) {
    permanentError("invalid_request", field + " must be an unsigned integer");
  }
  const Json::UInt64 parsed = value.asUInt64();
  if (parsed < minimum || parsed > maximum) {
    permanentError("invalid_request", field + " must be between " +
                                          std::to_string(minimum) + " and " +
                                          std::to_string(maximum));
  }
  return static_cast<std::uint32_t>(parsed);
}

void validateAbsoluteRosName(const std::string& value,
                             const std::string& field) {
  if (value.empty() || value.front() != '/') {
    permanentError("invalid_ros_name",
                   field + " must be an absolute ROS1 graph name");
  }
  std::string validation_error;
  if (!ros::names::validate(value, validation_error)) {
    permanentError(
        "invalid_ros_name",
        field + " is not a valid ROS1 graph name: " + validation_error);
  }
}

void validateRosType(const std::string& value, const std::string& field) {
  if (!std::regex_match(value, rosTypePattern())) {
    permanentError("invalid_ros_type", field + " must use package/Type syntax");
  }
}

std::string validatedRequestId(const Json::Value& request) {
  const std::string request_id = requiredString(request, "requestId");
  if (request_id.size() > kMaximumRequestIdBytes) {
    permanentError("invalid_request", "requestId exceeds 128 UTF-8 bytes");
  }
  if (std::any_of(request_id.begin(), request_id.end(),
                  [](unsigned char c) { return std::iscntrl(c) != 0; })) {
    permanentError("invalid_request",
                   "requestId must not contain control characters");
  }
  return request_id;
}

void validateProtocolVersion(const Json::Value& request) {
  if (!request.isMember("protocolVersion") ||
      !request["protocolVersion"].isIntegral() ||
      request["protocolVersion"].asUInt64() != Gateway::kProtocolVersion) {
    permanentError("unsupported_protocol", "protocolVersion must be 1");
  }
}

}  // namespace

Gateway::Gateway(ros::NodeHandle node_handle)
    : node_handle_(std::move(node_handle)),
      codec_(),
      types_(),
      publishers_(node_handle_, types_, codec_),
      services_(node_handle_, types_, codec_),
      requests_() {}

std::string Gateway::handle(const std::string& request_json) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

  Json::Value request;
  std::string parse_errors;
  if (!reader->parse(request_json.data(),
                     request_json.data() + request_json.size(), &request,
                     &parse_errors)) {
    return errorResponse("", "invalid_json", "permanent",
                         "request is not valid strict JSON: " + parse_errors);
  }
  if (!request.isObject()) {
    return errorResponse("", "invalid_request", "permanent",
                         "request root must be a JSON object");
  }

  std::string request_id;
  bool owns_claim = false;
  try {
    validateProtocolVersion(request);
    request_id = validatedRequestId(request);
    requiredString(request, "operation");
    const std::string fingerprint = writeJson(request);
    const RequestCache::Claim claim = requests_.claim(request_id, fingerprint);
    if (!claim.owner) {
      return claim.cached_response;
    }
    owns_claim = true;

    const std::string response = successResponse(request_id, execute(request));
    requests_.complete(request_id, response);
    return response;
  } catch (const GatewayError& error) {
    const std::string response = errorResponse(
        request_id, error.code(), error.errorClass(), error.what());
    if (owns_claim) {
      requests_.complete(request_id, response);
    }
    return response;
  } catch (const std::exception& error) {
    const std::string response =
        errorResponse(request_id, "internal_error", "transient", error.what());
    if (owns_claim) {
      requests_.complete(request_id, response);
    }
    return response;
  } catch (...) {
    const std::string response =
        errorResponse(request_id, "internal_error", "transient",
                      "unexpected gateway failure");
    if (owns_claim) {
      requests_.complete(request_id, response);
    }
    return response;
  }
}

Json::Value Gateway::execute(const Json::Value& request) {
  const std::string operation = request["operation"].asString();
  if (operation == "health") {
    rejectUnknownFields(request, {"protocolVersion", "requestId", "operation"});
    return health();
  }
  if (operation == "publish") {
    return publish(request);
  }
  if (operation == "call_service") {
    return callService(request);
  }
  permanentError("unsupported_operation",
                 "unsupported operation '" + operation + "'");
}

Json::Value Gateway::health() const {
  const bool ros_ok = ros::ok();
  const bool master_reachable = ros::master::check();
  Json::Value result(Json::objectValue);
  result["status"] = ros_ok && master_reachable ? "ok" : "degraded";
  result["rosOk"] = ros_ok;
  result["masterReachable"] = master_reachable;
  result["pid"] = Json::UInt64(static_cast<Json::UInt64>(::getpid()));
  return result;
}

Json::Value Gateway::publish(const Json::Value& request) {
  rejectUnknownFields(request, {"protocolVersion", "requestId", "operation",
                                "topic", "messageType", "message", "latch",
                                "queueSize", "waitForSubscribersMs"});
  PublishRequest publish_request;
  publish_request.topic = requiredString(request, "topic");
  publish_request.message_type = requiredString(request, "messageType");
  validateAbsoluteRosName(publish_request.topic, "topic");
  validateRosType(publish_request.message_type, "messageType");
  if (!request.isMember("message")) {
    permanentError("invalid_request", "message is required");
  }
  requireObject(request["message"], "message");
  publish_request.message = request["message"];
  publish_request.latch = optionalBool(request, "latch", false);
  publish_request.queue_size =
      optionalUInt(request, "queueSize", 10, 1, kMaximumQueueSize);
  publish_request.wait_for_subscribers_ms = optionalUInt(
      request, "waitForSubscribersMs", 0, 0, kMaximumWaitMilliseconds);

  const PublishResult published = publishers_.publish(publish_request);
  Json::Value result(Json::objectValue);
  result["event"] = "published";
  result["topic"] = published.topic;
  result["messageType"] = published.message_type;
  result["serializedBytes"] = Json::UInt(published.serialized_bytes);
  result["subscriberCount"] = Json::UInt(published.subscriber_count);
  return result;
}

Json::Value Gateway::callService(const Json::Value& request) {
  rejectUnknownFields(
      request, {"protocolVersion", "requestId", "operation", "service",
                "serviceType", "request", "waitForServiceMs", "callTimeoutMs"});
  ServiceCallRequest call_request;
  call_request.service = requiredString(request, "service");
  call_request.service_type = requiredString(request, "serviceType");
  validateAbsoluteRosName(call_request.service, "service");
  validateRosType(call_request.service_type, "serviceType");
  if (!request.isMember("request")) {
    permanentError("invalid_request", "request is required");
  }
  requireObject(request["request"], "request");
  call_request.request = request["request"];
  call_request.wait_for_service_ms = optionalUInt(
      request, "waitForServiceMs", 5000, 0, kMaximumWaitMilliseconds);
  call_request.call_timeout_ms =
      optionalUInt(request, "callTimeoutMs", 5000, 1, kMaximumWaitMilliseconds);

  const ServiceCallResult called = services_.call(call_request);
  Json::Value result(Json::objectValue);
  result["event"] = "response";
  result["service"] = called.service;
  result["serviceType"] = called.service_type;
  result["response"] = called.response;
  result["durationMs"] = Json::UInt64(called.duration_ms);
  return result;
}

std::string Gateway::successResponse(const std::string& request_id,
                                     const Json::Value& result) const {
  Json::Value response(Json::objectValue);
  response["protocolVersion"] = Json::UInt(kProtocolVersion);
  response["requestId"] = request_id;
  response["result"] = result;
  return writeJson(response);
}

std::string Gateway::errorResponse(const std::string& request_id,
                                   const std::string& code,
                                   const std::string& error_class,
                                   const std::string& message) const {
  Json::Value response(Json::objectValue);
  response["protocolVersion"] = Json::UInt(kProtocolVersion);
  response["requestId"] = request_id;
  response["error"]["code"] = code;
  response["error"]["class"] = error_class;
  response["error"]["message"] = message;
  return writeJson(response);
}

std::string Gateway::writeJson(const Json::Value& value) const {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["commentStyle"] = "None";
  return Json::writeString(builder, value);
}

}  // namespace xgc_ros1_automation_gateway
