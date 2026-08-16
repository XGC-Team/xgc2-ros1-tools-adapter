#include "xgc_ros1_tools_adapter/tools_adapter.hpp"

#include <arpa/inet.h>
#include <ros/names.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "xgc_ros1_tools_adapter/error.hpp"
#include "xgc_ros1_tools_adapter/generated_contract.hpp"

namespace xgc_ros1_tools_adapter {
namespace {

constexpr std::uint32_t kMaximumQueueSize = 10000;
constexpr std::uint32_t kMaximumPublishCount = 10000;
constexpr double kMinimumPublishRateHz = 0.1;
constexpr double kMaximumPublishRateHz = 1000.0;
constexpr double kMaximumPublishDurationSeconds = 240.0;
constexpr std::uint32_t kMaximumWaitMilliseconds = 300000;
constexpr std::size_t kMaximumConfigurationBytes = 16u * 1024u;
constexpr std::size_t kMaximumMasterUriBytes = 2048;
constexpr std::size_t kMaximumNetworkIdentityBytes = 255;
constexpr std::size_t kMaximumRosNameBytes = 4096;
constexpr std::size_t kMaximumRosTypeBytes = 512;

const std::regex& rosTypePattern() {
  static const std::regex pattern(
      "^[A-Za-z][A-Za-z0-9_]*/[A-Za-z][A-Za-z0-9_]*$");
  return pattern;
}

const std::regex& absoluteRosNamePattern() {
  static const std::regex pattern(
      "^/[A-Za-z][A-Za-z0-9_]*(?:/[A-Za-z][A-Za-z0-9_]*)*$");
  return pattern;
}

const std::regex& masterUriPattern() {
  static const std::regex pattern(
      "^http://(?:[A-Za-z0-9._~-]+|\\[[0-9A-Fa-f:]+\\]):([0-9]{1,5})/?$");
  return pattern;
}

const std::regex& canonicalScopeKeyPattern() {
  static const std::regex pattern("^sha256:[0-9a-f]{64}$");
  return pattern;
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

void requireObject(const Json::Value& value, const std::string& field) {
  if (!value.isObject()) {
    permanentError("invalid_request", field + " must be a JSON object");
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

std::string requiredPossiblyEmptyString(const Json::Value& request,
                                        const std::string& field) {
  if (!request.isMember(field) || !request[field].isString()) {
    permanentError("invalid_configuration", field + " must be a string");
  }
  return request[field].asString();
}

bool requiredBool(const Json::Value& request, const std::string& field) {
  if (!request.isMember(field) || !request[field].isBool()) {
    permanentError("invalid_request", field + " must be a boolean");
  }
  return request[field].asBool();
}

std::uint32_t requiredUInt(const Json::Value& request, const std::string& field,
                           std::uint32_t minimum, std::uint32_t maximum) {
  if (!request.isMember(field)) {
    permanentError("invalid_request", field + " is required");
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

double requiredNumber(const Json::Value& request, const std::string& field,
                      double minimum, double maximum) {
  if (!request.isMember(field) || !request[field].isNumeric()) {
    permanentError("invalid_request", field + " must be a number");
  }
  const double parsed = request[field].asDouble();
  if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
    permanentError("invalid_request", field + " must be between " +
                                          std::to_string(minimum) + " and " +
                                          std::to_string(maximum));
  }
  return parsed;
}

void validateAbsoluteRosName(const std::string& value,
                             const std::string& field) {
  if (value.empty() || value.size() > kMaximumRosNameBytes || value == "/" ||
      value.front() != '/' ||
      !std::regex_match(value, absoluteRosNamePattern())) {
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
  if (value.size() > kMaximumRosTypeBytes ||
      !std::regex_match(value, rosTypePattern())) {
    permanentError("invalid_ros_type", field + " must use package/Type syntax");
  }
}

Json::Value parseStrictJson(const std::string& input,
                            const std::string& description) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;
  builder["rejectDupKeys"] = true;
  builder["stackLimit"] = 128;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string parse_errors;
  if (!reader->parse(input.data(), input.data() + input.size(), &value,
                     &parse_errors)) {
    permanentError("invalid_json",
                   description + " is not strict JSON: " + parse_errors);
  }
  return value;
}

std::string writeJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["commentStyle"] = "None";
  return Json::writeString(builder, value);
}

void validateNoControlCharacters(const std::string& value,
                                 const std::string& field) {
  if (std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      })) {
    permanentError("invalid_configuration",
                   field + " must not contain control characters");
  }
}

void validateLength(const std::string& value, std::size_t maximum,
                    const std::string& field) {
  if (value.size() > maximum) {
    permanentError("invalid_configuration",
                   field + " exceeds its maximum encoded length");
  }
}

void validateIpAddress(const std::string& value) {
  if (value.empty()) {
    return;
  }
  in_addr ipv4{};
  in6_addr ipv6{};
  if (::inet_pton(AF_INET, value.c_str(), &ipv4) != 1 &&
      ::inet_pton(AF_INET6, value.c_str(), &ipv6) != 1) {
    permanentError("invalid_configuration",
                   "rosIp must be an IPv4 or IPv6 address");
  }
}

void validateHostname(const std::string& value) {
  if (value.empty()) {
    return;
  }
  std::size_t label_start = 0;
  const std::size_t effective_size =
      value.back() == '.' ? value.size() - 1 : value.size();
  if (effective_size == 0) {
    permanentError("invalid_configuration", "rosHostname is invalid");
  }
  while (label_start < effective_size) {
    const std::size_t separator = value.find('.', label_start);
    const std::size_t label_end =
        separator == std::string::npos ? effective_size : separator;
    const std::size_t label_size = label_end - label_start;
    const auto alpha_numeric = [](unsigned char character) {
      return std::isalnum(character) != 0;
    };
    if (label_size == 0 || label_size > 63 ||
        !alpha_numeric(static_cast<unsigned char>(value[label_start])) ||
        !alpha_numeric(static_cast<unsigned char>(value[label_end - 1])) ||
        !std::all_of(value.begin() + static_cast<std::ptrdiff_t>(label_start),
                     value.begin() + static_cast<std::ptrdiff_t>(label_end),
                     [](unsigned char character) {
                       return std::isalnum(character) != 0 ||
                              character == '-' || character == '_';
                     })) {
      permanentError("invalid_configuration",
                     "rosHostname must contain valid DNS-style labels");
    }
    if (separator == std::string::npos || separator >= effective_size) {
      break;
    }
    label_start = separator + 1;
  }
}

void setEnvironment(const char* name, const std::string& value) {
  const int result =
      value.empty() ? ::unsetenv(name) : ::setenv(name, value.c_str(), 1);
  if (result != 0) {
    throw std::runtime_error("unable to configure " + std::string(name) + ": " +
                             std::strerror(errno));
  }
}

const xgc::adapter::v1::CapabilityEndpointContract* findEndpoint(
    const xgc::adapter::v1::CapabilityContract& contract,
    const std::string& endpoint_id) {
  for (const auto& endpoint : contract.endpoints()) {
    if (endpoint.endpoint_id() == endpoint_id) {
      return &endpoint;
    }
  }
  return nullptr;
}

bool schemaMatches(const xgc::v1::SchemaReference& actual,
                   const contract::Schema& expected) {
  return actual.message_id() == expected.message_id &&
         actual.type_name() == expected.type_name &&
         actual.schema_version() == expected.version &&
         actual.schema_fingerprint() == expected.fingerprint;
}

xgc::v1::SchemaReference schemaReference(const contract::Schema& source) {
  xgc::v1::SchemaReference result;
  result.set_message_id(source.message_id);
  result.set_type_name(source.type_name);
  result.set_schema_version(source.version);
  result.set_schema_fingerprint(source.fingerprint);
  return result;
}

bool validateExpectedContract(
    const xgc::adapter::v1::CapabilityContract& contract,
    const contract::Endpoint& expected, std::string* error) {
  const auto* endpoint = findEndpoint(contract, expected.endpoint_id);
  if (contract.capability_id() != expected.capability_id ||
      contract.contract_version() != expected.contract_version ||
      contract.contract_digest() != expected.contract_digest ||
      contract.endpoints_size() != 1 || endpoint == nullptr ||
      endpoint->interaction_mode() !=
          xgc::adapter::v1::INTERACTION_MODE_OPERATION ||
      !endpoint->has_input_schema() || !endpoint->has_output_schema() ||
      endpoint->has_event_schema() ||
      !schemaMatches(endpoint->input_schema(), expected.input_schema) ||
      !schemaMatches(endpoint->output_schema(), expected.output_schema) ||
      endpoint->side_effect_class() !=
          xgc::adapter::v1::SIDE_EFFECT_CLASS_NON_IDEMPOTENT ||
      endpoint->idempotency_mode() !=
          xgc::adapter::v1::IDEMPOTENCY_MODE_REQUIRED ||
      !endpoint->deadline_required() || !endpoint->cancellation_supported() ||
      endpoint->default_timeout_ms() != expected.default_timeout_ms ||
      endpoint->maximum_timeout_ms() != expected.maximum_timeout_ms ||
      !endpoint->has_limits() ||
      endpoint->limits().maximum_request_bytes() !=
          expected.limits.maximum_request_bytes ||
      endpoint->limits().maximum_response_bytes() !=
          expected.limits.maximum_response_bytes ||
      endpoint->limits().maximum_concurrency() !=
          expected.limits.maximum_concurrency ||
      endpoint->limits().maximum_streams() != expected.limits.maximum_streams ||
      endpoint->limits().maximum_stream_chunk_bytes() !=
          expected.limits.maximum_stream_chunk_bytes ||
      endpoint->limits().maximum_stream_chunk_messages() !=
          expected.limits.maximum_stream_chunk_messages) {
    if (error != nullptr) {
      *error = "trusted bootstrap contains an unexpected " +
               std::string(expected.capability_id) + " capability contract";
    }
    return false;
  }
  return true;
}

bool validateCapabilityGrant(const xgc::adapter::v1::EnabledCapability& grant,
                             const contract::Endpoint& expected,
                             std::string* error) {
  if (grant.capability_id() != expected.capability_id ||
      grant.contract_version() != expected.contract_version ||
      grant.contract_digest() != expected.contract_digest ||
      grant.enabled_endpoint_ids_size() != 1 ||
      grant.enabled_endpoint_ids(0) != expected.endpoint_id ||
      grant.has_configuration()) {
    if (error != nullptr) {
      *error = "enabled capability grant does not match the compiled " +
               std::string(expected.capability_id) + " contract";
    }
    return false;
  }
  return true;
}

void validateInvocationContext(const xgc::adapter::v1::WorkContext& context,
                               const contract::Endpoint& expected) {
  if (context.capability_id() != expected.capability_id ||
      context.endpoint_id() != expected.endpoint_id ||
      context.contract_version() != expected.contract_version ||
      context.contract_digest() != expected.contract_digest) {
    permanentError("invalid_endpoint",
                   "operation context does not match its compiled capability");
  }
  if (!context.has_deadline() ||
      context.deadline().deadline_unix_nanos() <= 0) {
    permanentError("invalid_deadline", "operation deadline is required");
  }
}

}  // namespace

NativeContext NativeContext::FromInstanceSpec(
    const xgc::adapter::v1::AdapterInstanceSpec& spec) {
  if (!spec.has_configuration() ||
      spec.configuration().encoding() != xgc::v1::PAYLOAD_ENCODING_JSON ||
      !spec.configuration().has_schema() ||
      !schemaMatches(spec.configuration().schema(), contract::kConfiguration)) {
    permanentError("invalid_configuration",
                   "instance configuration must use the compiled NativeContext "
                   "JSON schema");
  }
  if (spec.configuration().value().size() > kMaximumConfigurationBytes) {
    permanentError("invalid_configuration",
                   "instance configuration exceeds its maximum encoded size");
  }
  if (spec.secrets_size() != 0) {
    permanentError("invalid_configuration",
                   "ROS1 Tools Adapter does not accept secret references");
  }
  const Json::Value value =
      parseStrictJson(spec.configuration().value(), "instance configuration");
  requireObject(value, "instance configuration");
  rejectUnknownFields(value, {"rosMasterUri", "rosIp", "rosHostname"});

  NativeContext context;
  context.ros_master_uri = requiredString(value, "rosMasterUri");
  context.ros_ip = requiredPossiblyEmptyString(value, "rosIp");
  context.ros_hostname = requiredPossiblyEmptyString(value, "rosHostname");
  validateNoControlCharacters(context.ros_master_uri, "rosMasterUri");
  validateNoControlCharacters(context.ros_ip, "rosIp");
  validateNoControlCharacters(context.ros_hostname, "rosHostname");
  validateLength(context.ros_master_uri, kMaximumMasterUriBytes,
                 "rosMasterUri");
  validateLength(context.ros_ip, kMaximumNetworkIdentityBytes, "rosIp");
  validateLength(context.ros_hostname, kMaximumNetworkIdentityBytes,
                 "rosHostname");
  std::smatch master_uri_match;
  if (!std::regex_match(context.ros_master_uri, master_uri_match,
                        masterUriPattern())) {
    permanentError("invalid_configuration",
                   "rosMasterUri must be an absolute HTTP host and port");
  }
  const unsigned long master_port =
      std::stoul(master_uri_match[1].str(), nullptr, 10);
  if (master_port == 0 || master_port > 65535) {
    permanentError("invalid_configuration",
                   "rosMasterUri port must be between 1 and 65535");
  }
  if (!context.ros_ip.empty() && !context.ros_hostname.empty()) {
    permanentError("invalid_configuration",
                   "rosIp and rosHostname are mutually exclusive");
  }
  validateIpAddress(context.ros_ip);
  validateHostname(context.ros_hostname);

  if (!spec.has_scope() || spec.scope().kind() != "ros1-native-context") {
    permanentError("invalid_scope",
                   "instance scope must be ros1-native-context");
  }
  if (!std::regex_match(spec.scope().key(), canonicalScopeKeyPattern())) {
    permanentError("invalid_scope",
                   "instance scope key must be a canonical SHA-256 digest");
  }
  context.scope_key = spec.scope().key();
  const auto& attributes = spec.scope().attributes();
  const auto master = attributes.find("master-uri");
  if (master == attributes.end() || master->second != context.ros_master_uri) {
    permanentError("invalid_scope",
                   "scope master-uri does not match instance configuration");
  }
  const auto ip = attributes.find("ip");
  const auto hostname = attributes.find("hostname");
  const std::string scoped_ip = ip == attributes.end() ? "" : ip->second;
  const std::string scoped_hostname =
      hostname == attributes.end() ? "" : hostname->second;
  if (scoped_ip != context.ros_ip || scoped_hostname != context.ros_hostname ||
      attributes.size() !=
          static_cast<std::size_t>(1 + !context.ros_ip.empty() +
                                   !context.ros_hostname.empty())) {
    permanentError("invalid_scope",
                   "scope network identity does not match configuration");
  }
  return context;
}

void NativeContext::ApplyEnvironment() const {
  setEnvironment("ROS_MASTER_URI", ros_master_uri);
  setEnvironment("ROS_IP", ros_ip);
  setEnvironment("ROS_HOSTNAME", ros_hostname);
}

bool NativeContext::operator==(const NativeContext& other) const noexcept {
  return ros_master_uri == other.ros_master_uri && ros_ip == other.ros_ip &&
         ros_hostname == other.ros_hostname && scope_key == other.scope_key;
}

ToolsAdapter::ToolsAdapter(ros::NodeHandle node_handle,
                           NativeContext native_context)
    : native_context_(std::move(native_context)),
      codec_(),
      types_(),
      publishers_(node_handle, types_, codec_),
      services_(types_, codec_) {}

bool ToolsAdapter::ApplyInstanceSpec(
    const xgc::adapter::v1::AdapterInstanceSpec& spec, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  try {
    if (!(NativeContext::FromInstanceSpec(spec) == native_context_)) {
      if (error != nullptr) {
        *error = "a running ROS1 process cannot change its native context";
      }
      return false;
    }
    instance_spec_applied_.store(true, std::memory_order_release);
    return true;
  } catch (const std::exception& exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

void ToolsAdapter::ClearInstanceSpec() noexcept {
  StopPublishCapability();
  StopServiceCapability();
  instance_spec_applied_.store(false, std::memory_order_release);
}

bool ToolsAdapter::StartPublishCapability(
    const xgc::adapter::v1::AdapterInstanceSpec& spec,
    const xgc::adapter::v1::EnabledCapability& grant, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!instance_spec_applied_.load(std::memory_order_acquire)) {
    if (error != nullptr) {
      *error = "instance spec must be applied before publish capability start";
    }
    return false;
  }
  try {
    if (!(NativeContext::FromInstanceSpec(spec) == native_context_)) {
      if (error != nullptr) {
        *error =
            "publish capability instance spec does not match this native "
            "context";
      }
      return false;
    }
  } catch (const std::exception& exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
  if (!validateCapabilityGrant(grant, contract::kPublish, error)) {
    return false;
  }
  bool expected = false;
  if (!publish_enabled_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
    if (error != nullptr) {
      *error = "publish capability is already active";
    }
    return false;
  }
  return true;
}

void ToolsAdapter::StopPublishCapability() noexcept {
  publish_enabled_.store(false, std::memory_order_release);
  publishers_.clear();
}

bool ToolsAdapter::StartServiceCapability(
    const xgc::adapter::v1::AdapterInstanceSpec& spec,
    const xgc::adapter::v1::EnabledCapability& grant, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!instance_spec_applied_.load(std::memory_order_acquire)) {
    if (error != nullptr) {
      *error = "instance spec must be applied before service capability start";
    }
    return false;
  }
  try {
    if (!(NativeContext::FromInstanceSpec(spec) == native_context_)) {
      if (error != nullptr) {
        *error =
            "service capability instance spec does not match this native "
            "context";
      }
      return false;
    }
  } catch (const std::exception& exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
  if (!validateCapabilityGrant(grant, contract::kService, error)) {
    return false;
  }
  bool expected = false;
  if (!service_enabled_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
    if (error != nullptr) {
      *error = "service capability is already active";
    }
    return false;
  }
  return true;
}

void ToolsAdapter::StopServiceCapability() noexcept {
  service_enabled_.store(false, std::memory_order_release);
}

Json::Value ToolsAdapter::parseInput(
    const xgc::v1::Payload& payload,
    const xgc::v1::SchemaReference& expected_schema,
    std::uint32_t maximum_request_bytes) const {
  if (payload.encoding() != xgc::v1::PAYLOAD_ENCODING_JSON ||
      !payload.has_schema() ||
      payload.schema().SerializeAsString() !=
          expected_schema.SerializeAsString()) {
    permanentError("unsupported_input_contract",
                   "capability input must use its compiled JSON schema");
  }
  if (payload.value().size() > maximum_request_bytes) {
    resourceExhaustedError(
        "request_too_large",
        "capability input exceeds its compiled maximum request size");
  }
  Json::Value value = parseStrictJson(payload.value(), "capability input");
  requireObject(value, "capability input");
  return value;
}

void ToolsAdapter::validateSubject(
    const xgc::adapter::v1::WorkContext& context) const {
  if (!context.has_subject() ||
      context.subject().kind() != "ros1-native-context") {
    permanentError("invalid_subject",
                   "invocation subject must be ros1-native-context");
  }
  const auto& attributes = context.subject().attributes();
  const auto master = attributes.find("master-uri");
  const auto ip = attributes.find("ip");
  const auto hostname = attributes.find("hostname");
  const std::string subject_ip = ip == attributes.end() ? "" : ip->second;
  const std::string subject_hostname =
      hostname == attributes.end() ? "" : hostname->second;
  if (context.subject().key() != native_context_.scope_key ||
      master == attributes.end() ||
      master->second != native_context_.ros_master_uri ||
      subject_ip != native_context_.ros_ip ||
      subject_hostname != native_context_.ros_hostname ||
      attributes.size() !=
          static_cast<std::size_t>(1 + !native_context_.ros_ip.empty() +
                                   !native_context_.ros_hostname.empty())) {
    permanentError("invalid_subject",
                   "invocation subject does not match this native context");
  }
}

xgc2::adapter_runtime::OperationResult ToolsAdapter::Publish(
    const xgc::adapter::v1::OperationRequest& request,
    const xgc2::adapter_runtime::CancellationToken& cancellation) {
  try {
    if (!publish_enabled_.load(std::memory_order_acquire)) {
      rejectedError("publish_capability_disabled",
                    "ROS1 topic publish capability is not active");
    }
    validateInvocationContext(request.context(), contract::kPublish);
    validateSubject(request.context());
    const Json::Value input = parseInput(
        request.input(), schemaReference(contract::kPublish.input_schema),
        contract::kPublish.limits.maximum_request_bytes);
    rejectUnknownFields(
        input, {"topic", "messageType", "message", "publishCount",
                "publishRateHz", "latch", "queueSize", "waitForSubscribersMs"});
    PublishRequest publish_request;
    publish_request.topic = requiredString(input, "topic");
    publish_request.message_type = requiredString(input, "messageType");
    validateAbsoluteRosName(publish_request.topic, "topic");
    validateRosType(publish_request.message_type, "messageType");
    if (!input.isMember("message")) {
      permanentError("invalid_request", "message is required");
    }
    requireObject(input["message"], "message");
    publish_request.message = input["message"];
    publish_request.publish_count =
        requiredUInt(input, "publishCount", 1, kMaximumPublishCount);
    publish_request.publish_rate_hz = requiredNumber(
        input, "publishRateHz", kMinimumPublishRateHz, kMaximumPublishRateHz);
    if (static_cast<double>(publish_request.publish_count - 1u) /
            publish_request.publish_rate_hz >
        kMaximumPublishDurationSeconds) {
      permanentError(
          "invalid_request",
          "publishCount and publishRateHz must complete within 240 seconds");
    }
    publish_request.latch = requiredBool(input, "latch");
    publish_request.queue_size =
        requiredUInt(input, "queueSize", 1, kMaximumQueueSize);
    publish_request.wait_for_subscribers_ms = requiredUInt(
        input, "waitForSubscribersMs", 0, kMaximumWaitMilliseconds);
    publish_request.deadline_unix_nanos =
        request.context().deadline().deadline_unix_nanos();
    publish_request.cancellation_requested = [&cancellation] {
      return cancellation.IsCancellationRequested();
    };

    const PublishResult published = publishers_.publish(publish_request);
    try {
      Json::Value result(Json::objectValue);
      result["event"] = "published";
      result["topic"] = published.topic;
      result["messageType"] = published.message_type;
      result["serializedBytes"] = Json::UInt(published.serialized_bytes);
      result["publishedCount"] = Json::UInt(published.published_count);
      result["subscriberCount"] = Json::UInt(published.subscriber_count);
      return success(result, schemaReference(contract::kPublish.output_schema),
                     contract::kPublish.limits.maximum_response_bytes);
    } catch (const Ros1ToolsError&) {
      throw;
    } catch (const std::exception& exception) {
      uncertainError("publish_result_encoding_failed",
                     "ROS1 message was published but its result could not be "
                     "encoded: " +
                         std::string(exception.what()));
    } catch (...) {
      uncertainError("publish_result_encoding_failed",
                     "ROS1 message was published but its result could not be "
                     "encoded");
    }
  } catch (const std::exception& exception) {
    return failure(exception);
  }
}

MasterBindingState ToolsAdapter::ProbeMasterBinding(std::int64_t* current) {
  return publishers_.probeMasterBinding(current);
}

xgc2::adapter_runtime::OperationResult ToolsAdapter::CallService(
    const xgc::adapter::v1::OperationRequest& request,
    const xgc2::adapter_runtime::CancellationToken& cancellation) {
  try {
    if (!service_enabled_.load(std::memory_order_acquire)) {
      rejectedError("service_capability_disabled",
                    "ROS1 service-call capability is not active");
    }
    validateInvocationContext(request.context(), contract::kService);
    validateSubject(request.context());
    const Json::Value input = parseInput(
        request.input(), schemaReference(contract::kService.input_schema),
        contract::kService.limits.maximum_request_bytes);
    rejectUnknownFields(input, {"service", "serviceType", "request",
                                "waitForServiceMs", "callTimeoutMs"});
    ServiceCallRequest call_request;
    call_request.service = requiredString(input, "service");
    call_request.service_type = requiredString(input, "serviceType");
    validateAbsoluteRosName(call_request.service, "service");
    validateRosType(call_request.service_type, "serviceType");
    if (!input.isMember("request")) {
      permanentError("invalid_request", "request is required");
    }
    requireObject(input["request"], "request");
    call_request.request = input["request"];
    call_request.wait_for_service_ms =
        requiredUInt(input, "waitForServiceMs", 0, kMaximumWaitMilliseconds);
    call_request.call_timeout_ms =
        requiredUInt(input, "callTimeoutMs", 1, kMaximumWaitMilliseconds);
    call_request.deadline_unix_nanos =
        request.context().deadline().deadline_unix_nanos();
    call_request.cancellation_requested = [&cancellation] {
      return cancellation.IsCancellationRequested();
    };

    const ServiceCallResult called = services_.call(call_request);
    try {
      Json::Value result(Json::objectValue);
      result["event"] = "response";
      result["service"] = called.service;
      result["serviceType"] = called.service_type;
      result["response"] = called.response;
      result["durationMs"] = Json::UInt64(called.duration_ms);
      return success(result, schemaReference(contract::kService.output_schema),
                     contract::kService.limits.maximum_response_bytes);
    } catch (const Ros1ToolsError&) {
      throw;
    } catch (const std::exception& exception) {
      uncertainError("service_result_encoding_failed",
                     "ROS1 service executed but its result could not be "
                     "encoded: " +
                         std::string(exception.what()));
    } catch (...) {
      uncertainError("service_result_encoding_failed",
                     "ROS1 service executed but its result could not be "
                     "encoded");
    }
  } catch (const std::exception& exception) {
    return failure(exception);
  }
}

xgc2::adapter_runtime::OperationResult ToolsAdapter::success(
    const Json::Value& value, const xgc::v1::SchemaReference& output_schema,
    std::uint32_t maximum_response_bytes) const {
  xgc::v1::Payload payload;
  *payload.mutable_schema() = output_schema;
  payload.set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  payload.set_value(writeJson(value));
  if (payload.value().size() > maximum_response_bytes) {
    uncertainError("operation_result_too_large",
                   "native side effect committed but its result exceeds the "
                   "compiled response limit");
  }
  return xgc2::adapter_runtime::OperationResult::Success(std::move(payload),
                                                         true);
}

xgc2::adapter_runtime::OperationResult ToolsAdapter::failure(
    const std::exception& exception) const {
  const auto* typed = dynamic_cast<const Ros1ToolsError*>(&exception);
  if (typed == nullptr) {
    return xgc2::adapter_runtime::OperationResult::Failure(
        xgc::adapter::v1::ERROR_CLASS_TRANSIENT, "internal_error",
        exception.what());
  }
  xgc::adapter::v1::ErrorClass error_class =
      xgc::adapter::v1::ERROR_CLASS_PERMANENT;
  if (typed->errorClass() == "transient") {
    error_class = xgc::adapter::v1::ERROR_CLASS_TRANSIENT;
  } else if (typed->errorClass() == "uncertain") {
    error_class = xgc::adapter::v1::ERROR_CLASS_UNCERTAIN;
  } else if (typed->errorClass() == "rejected") {
    error_class = xgc::adapter::v1::ERROR_CLASS_REJECTED;
  } else if (typed->errorClass() == "resource-exhausted") {
    error_class = xgc::adapter::v1::ERROR_CLASS_RESOURCE_EXHAUSTED;
  } else if (typed->errorClass() == "cancelled") {
    error_class = xgc::adapter::v1::ERROR_CLASS_CANCELLED;
  } else if (typed->errorClass() == "deadline") {
    error_class = xgc::adapter::v1::ERROR_CLASS_DEADLINE;
  }
  return xgc2::adapter_runtime::OperationResult::Failure(
      error_class, typed->code(), typed->what());
}

bool BindCapabilities(xgc2::adapter_runtime::ClientConfig* config,
                      ToolsAdapter* adapter, std::string* error) {
  if (config == nullptr || adapter == nullptr) {
    if (error != nullptr) {
      *error = "Runtime config and ROS1 tools adapter are required";
    }
    return false;
  }
  if (config->capabilities().size() != 2) {
    if (error != nullptr) {
      *error = "ROS1 tools adapter requires exactly two capability contracts";
    }
    return false;
  }

  bool publish_bound = false;
  bool service_bound = false;
  for (const auto& binding : config->capabilities()) {
    const auto& bootstrap_contract = binding.contract;
    const contract::Endpoint* expected = nullptr;
    bool* bound = nullptr;
    if (bootstrap_contract.capability_id() ==
        contract::kPublish.capability_id) {
      expected = &contract::kPublish;
      bound = &publish_bound;
    } else if (bootstrap_contract.capability_id() ==
               contract::kService.capability_id) {
      expected = &contract::kService;
      bound = &service_bound;
    } else {
      if (error != nullptr) {
        *error = "trusted bootstrap advertises an unknown capability";
      }
      return false;
    }
    if (*bound) {
      if (error != nullptr) {
        *error = "trusted bootstrap contains a duplicate " +
                 std::string(expected->capability_id) + " capability contract";
      }
      return false;
    }
    if (!validateExpectedContract(bootstrap_contract, *expected, error)) {
      return false;
    }
    xgc2::adapter_runtime::CapabilityCallbacks callbacks;
    if (expected == &contract::kPublish) {
      callbacks.start = [adapter](const auto& spec, const auto& grant,
                                  std::string* start_error) {
        return adapter->StartPublishCapability(spec, grant, start_error);
      };
      callbacks.stop = [adapter] { adapter->StopPublishCapability(); };
      callbacks.operation = [adapter](const auto& request, const auto& token) {
        return adapter->Publish(request, token);
      };
    } else {
      callbacks.start = [adapter](const auto& spec, const auto& grant,
                                  std::string* start_error) {
        return adapter->StartServiceCapability(spec, grant, start_error);
      };
      callbacks.stop = [adapter] { adapter->StopServiceCapability(); };
      callbacks.operation = [adapter](const auto& request, const auto& token) {
        return adapter->CallService(request, token);
      };
    }
    if (!config->BindCapability(bootstrap_contract.capability_id(),
                                bootstrap_contract.contract_version(),
                                bootstrap_contract.contract_digest(),
                                std::move(callbacks), error)) {
      return false;
    }
    *bound = true;
  }
  return publish_bound && service_bound;
}

}  // namespace xgc_ros1_tools_adapter
