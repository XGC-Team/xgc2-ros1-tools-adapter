#pragma once

#include <ros/node_handle.h>
#include <ros/publisher.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "xgc_ros1_automation_gateway/json_codec.hpp"
#include "xgc_ros1_automation_gateway/type_registry.hpp"

namespace xgc_ros1_automation_gateway {

struct PublishRequest {
  std::string topic;
  std::string message_type;
  Json::Value message;
  bool latch{false};
  std::uint32_t queue_size{10};
  std::uint32_t wait_for_subscribers_ms{0};
};

struct PublishResult {
  std::string topic;
  std::string message_type;
  std::uint32_t serialized_bytes{0};
  std::uint32_t subscriber_count{0};
};

class PublisherRegistry {
 public:
  PublisherRegistry(ros::NodeHandle node_handle, TypeRegistry& types,
                    const JsonCodec& codec);

  PublishResult publish(const PublishRequest& request);

 private:
  struct Entry {
    std::string message_type;
    bool latch{false};
    std::uint32_t queue_size{0};
    ros::Publisher publisher;
  };

  ros::NodeHandle node_handle_;
  TypeRegistry& types_;
  const JsonCodec& codec_;
  std::mutex mutex_;
  std::map<std::string, Entry> entries_;
};

}  // namespace xgc_ros1_automation_gateway
