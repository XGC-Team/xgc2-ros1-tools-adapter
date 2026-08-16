#pragma once

#include <json/json.h>
#include <ros/node_handle.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "xgc/adapter/v1/adapter.pb.h"
#include "xgc/v1/message.pb.h"
#include "xgc2/adapter_runtime/client.hpp"
#include "xgc_ros1_tools_adapter/json_codec.hpp"
#include "xgc_ros1_tools_adapter/publisher_registry.hpp"
#include "xgc_ros1_tools_adapter/service_invoker.hpp"
#include "xgc_ros1_tools_adapter/type_registry.hpp"

namespace xgc_ros1_tools_adapter {

struct NativeContext {
  std::string ros_master_uri;
  std::string ros_ip;
  std::string ros_hostname;
  std::string scope_key;

  static NativeContext FromInstanceSpec(
      const xgc::adapter::v1::AdapterInstanceSpec& spec);
  void ApplyEnvironment() const;

  bool operator==(const NativeContext& other) const noexcept;
};

// Implements only the native ROS behavior behind capability callbacks. The
// generic SDK owns registration, fencing, Work dispatch, idempotency replay,
// cancellation delivery, and all Runtime transport.
class ToolsAdapter {
 public:
  ToolsAdapter(ros::NodeHandle node_handle, NativeContext native_context);

  bool ApplyInstanceSpec(const xgc::adapter::v1::AdapterInstanceSpec& spec,
                         std::string* error);
  void ClearInstanceSpec() noexcept;

  bool StartPublishCapability(const xgc::adapter::v1::AdapterInstanceSpec& spec,
                              const xgc::adapter::v1::EnabledCapability& grant,
                              std::string* error);
  void StopPublishCapability() noexcept;
  bool StartServiceCapability(const xgc::adapter::v1::AdapterInstanceSpec& spec,
                              const xgc::adapter::v1::EnabledCapability& grant,
                              std::string* error);
  void StopServiceCapability() noexcept;

  xgc2::adapter_runtime::OperationResult Publish(
      const xgc::adapter::v1::OperationRequest& request,
      const xgc2::adapter_runtime::CancellationToken& cancellation);
  xgc2::adapter_runtime::OperationResult CallService(
      const xgc::adapter::v1::OperationRequest& request,
      const xgc2::adapter_runtime::CancellationToken& cancellation);
  MasterBindingState ProbeMasterBinding(std::int64_t* current = nullptr);

 private:
  Json::Value parseInput(const xgc::v1::Payload& payload,
                         const xgc::v1::SchemaReference& expected_schema,
                         std::uint32_t maximum_request_bytes) const;
  void validateSubject(const xgc::adapter::v1::WorkContext& context) const;
  xgc2::adapter_runtime::OperationResult success(
      const Json::Value& value, const xgc::v1::SchemaReference& output_schema,
      std::uint32_t maximum_response_bytes) const;
  xgc2::adapter_runtime::OperationResult failure(
      const std::exception& exception) const;

  NativeContext native_context_;
  JsonCodec codec_;
  TypeRegistry types_;
  PublisherRegistry publishers_;
  ServiceInvoker services_;
  std::atomic<bool> instance_spec_applied_{false};
  std::atomic<bool> publish_enabled_{false};
  std::atomic<bool> service_enabled_{false};
};

// Binds the two exact contracts advertised by the trusted bootstrap. Unknown,
// missing, duplicate, or structurally different contracts fail startup.
bool BindCapabilities(xgc2::adapter_runtime::ClientConfig* config,
                      ToolsAdapter* adapter, std::string* error);

}  // namespace xgc_ros1_tools_adapter
