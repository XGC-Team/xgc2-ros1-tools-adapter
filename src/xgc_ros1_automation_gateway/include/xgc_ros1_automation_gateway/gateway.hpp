#pragma once

#include <json/json.h>
#include <ros/node_handle.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "xgc_ros1_automation_gateway/json_codec.hpp"
#include "xgc_ros1_automation_gateway/publisher_registry.hpp"
#include "xgc_ros1_automation_gateway/request_cache.hpp"
#include "xgc_ros1_automation_gateway/service_invoker.hpp"
#include "xgc_ros1_automation_gateway/type_registry.hpp"

namespace xgc_ros1_automation_gateway {

class Gateway {
 public:
  static constexpr std::uint32_t kProtocolVersion = 1;

  explicit Gateway(ros::NodeHandle node_handle);

  std::string handle(const std::string& request_json);

 private:
  Json::Value execute(const Json::Value& request);
  Json::Value health() const;
  Json::Value publish(const Json::Value& request);
  Json::Value callService(const Json::Value& request);

  std::string successResponse(const std::string& request_id,
                              const Json::Value& result) const;
  std::string errorResponse(const std::string& request_id,
                            const std::string& code,
                            const std::string& error_class,
                            const std::string& message) const;
  std::string writeJson(const Json::Value& value) const;

  ros::NodeHandle node_handle_;
  JsonCodec codec_;
  TypeRegistry types_;
  PublisherRegistry publishers_;
  ServiceInvoker services_;
  RequestCache requests_;
};

}  // namespace xgc_ros1_automation_gateway
