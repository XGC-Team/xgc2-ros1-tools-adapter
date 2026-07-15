#include "xgc_ros1_automation_gateway/publisher_registry.hpp"

#include <ros/ros.h>

#include <chrono>
#include <thread>
#include <utility>

#include "xgc_ros1_automation_gateway/error.hpp"

namespace xgc_ros1_automation_gateway {

PublisherRegistry::PublisherRegistry(ros::NodeHandle node_handle,
                                     TypeRegistry& types,
                                     const JsonCodec& codec)
    : node_handle_(std::move(node_handle)), types_(types), codec_(codec) {}

PublishResult PublisherRegistry::publish(const PublishRequest& request) {
  auto message = types_.createMessage(request.message_type);
  codec_.decode(request.message, *message);
  auto serialized = types_.serialize(message);
  if (!serialized) {
    permanentError("message_serialization_failed",
                   "dynamic ROS1 serializer returned no message");
  }

  ros::Publisher publisher;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto iterator = entries_.find(request.topic);
    if (iterator == entries_.end()) {
      Entry entry;
      entry.message_type = request.message_type;
      entry.latch = request.latch;
      entry.queue_size = request.queue_size;
      entry.publisher =
          types_.advertise(node_handle_, request.message_type, request.topic,
                           request.queue_size, request.latch);
      publisher = entry.publisher;
      entries_.emplace(request.topic, std::move(entry));
    } else {
      const Entry& entry = iterator->second;
      if (entry.message_type != request.message_type ||
          entry.latch != request.latch ||
          entry.queue_size != request.queue_size) {
        permanentError("publisher_configuration_conflict",
                       "topic '" + request.topic +
                           "' is already cached with a different type, latch, "
                           "or queueSize");
      }
      publisher = entry.publisher;
    }
  }

  if (!publisher) {
    transientError(
        "publisher_unavailable",
        "ROS1 publisher is not available for topic '" + request.topic + "'");
  }

  if (request.wait_for_subscribers_ms > 0) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(request.wait_for_subscribers_ms);
    while (publisher.getNumSubscribers() == 0 && ros::ok() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (publisher.getNumSubscribers() == 0) {
      transientError("subscriber_wait_timeout",
                     "no subscriber connected to topic '" + request.topic +
                         "' before waitForSubscribersMs elapsed");
    }
  }

  if (!ros::ok()) {
    transientError("ros_shutdown",
                   "ROS1 is shutting down before the message can be published");
  }
  publisher.publish(*serialized);

  PublishResult result;
  result.topic = request.topic;
  result.message_type = request.message_type;
  result.serialized_bytes = serialized->size();
  result.subscriber_count = publisher.getNumSubscribers();
  return result;
}

}  // namespace xgc_ros1_automation_gateway
