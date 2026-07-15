#pragma once

#include <json/json.h>
#include <ros/node_handle.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "xgc_ros1_automation_gateway/json_codec.hpp"
#include "xgc_ros1_automation_gateway/type_registry.hpp"

namespace xgc_ros1_automation_gateway {

struct ServiceCallRequest {
  std::string service;
  std::string service_type;
  Json::Value request;
  std::uint32_t wait_for_service_ms{5000};
  std::uint32_t call_timeout_ms{5000};
};

struct ServiceCallResult {
  std::string service;
  std::string service_type;
  Json::Value response;
  std::uint64_t duration_ms{0};
};

class ServiceInvoker {
 public:
  ServiceInvoker(ros::NodeHandle node_handle, TypeRegistry& types,
                 const JsonCodec& codec, std::uint32_t maximum_in_flight = 16);

  ServiceCallResult call(const ServiceCallRequest& request);

 private:
  ros::NodeHandle node_handle_;
  TypeRegistry& types_;
  const JsonCodec& codec_;
  std::uint32_t maximum_in_flight_;
  std::shared_ptr<std::atomic<std::uint32_t>> in_flight_;
};

}  // namespace xgc_ros1_automation_gateway
