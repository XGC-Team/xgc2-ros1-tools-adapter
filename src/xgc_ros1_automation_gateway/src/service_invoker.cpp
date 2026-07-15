#include "xgc_ros1_automation_gateway/service_invoker.hpp"

#include <ros/serialization.h>
#include <ros/service.h>
#include <ros/service_client.h>
#include <ros/service_client_options.h>
#include <ros_babel_fish/generation/message_creation.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "xgc_ros1_automation_gateway/error.hpp"

namespace xgc_ros1_automation_gateway {
namespace {

ros::SerializedMessage serializeServiceRequest(
    const ros_babel_fish::Message& message) {
  const std::size_t payload_size = message._sizeInBytes();
  if (payload_size > std::numeric_limits<std::uint32_t>::max() - 4U) {
    permanentError("service_request_too_large",
                   "serialized ROS1 service request exceeds 4 GiB");
  }

  ros::SerializedMessage result;
  result.num_bytes = payload_size + 4U;
  result.buf.reset(new std::uint8_t[result.num_bytes]);
  ros::serialization::OStream stream(
      result.buf.get(), static_cast<std::uint32_t>(result.num_bytes));
  stream.next(static_cast<std::uint32_t>(payload_size));
  result.message_start = stream.getData();
  message.writeToStream(
      stream.advance(static_cast<std::uint32_t>(payload_size)));
  return result;
}

struct CallState {
  ros::ServiceClient client;
  ros::SerializedMessage request;
  ros::SerializedMessage response;
  std::string md5;
  std::promise<bool> completed;
};

std::size_t responsePayloadSize(const ros::SerializedMessage& response) {
  if (!response.buf || response.message_start == nullptr ||
      response.message_start < response.buf.get()) {
    uncertainError("invalid_service_response",
                   "ROS1 service returned an invalid response buffer");
  }
  const std::size_t offset =
      static_cast<std::size_t>(response.message_start - response.buf.get());
  if (offset > response.num_bytes) {
    uncertainError("invalid_service_response",
                   "ROS1 service returned an invalid response offset");
  }
  return response.num_bytes - offset;
}

}  // namespace

ServiceInvoker::ServiceInvoker(ros::NodeHandle node_handle, TypeRegistry& types,
                               const JsonCodec& codec,
                               std::uint32_t maximum_in_flight)
    : node_handle_(std::move(node_handle)),
      types_(types),
      codec_(codec),
      maximum_in_flight_(maximum_in_flight),
      in_flight_(std::make_shared<std::atomic<std::uint32_t>>(0)) {
  if (maximum_in_flight_ == 0) {
    permanentError("invalid_configuration",
                   "maximum in-flight service calls must be greater than zero");
  }
}

ServiceCallResult ServiceInvoker::call(const ServiceCallRequest& request) {
  const auto started_at = std::chrono::steady_clock::now();
  const auto description = types_.resolveService(request.service_type);
  auto dynamic_request = types_.createServiceRequest(description);
  codec_.decode(request.request, *dynamic_request);

  bool service_available = false;
  if (request.wait_for_service_ms == 0) {
    service_available = ros::service::exists(request.service, false);
  } else {
    service_available = ros::service::waitForService(
        request.service,
        static_cast<std::int32_t>(request.wait_for_service_ms));
  }
  if (!service_available) {
    transientError("service_unavailable",
                   "ROS1 service '" + request.service +
                       "' was not available before waitForServiceMs elapsed");
  }

  std::uint32_t current = in_flight_->load(std::memory_order_relaxed);
  while (true) {
    if (current >= maximum_in_flight_) {
      transientError("service_call_busy",
                     "maximum concurrent ROS1 service calls reached");
    }
    if (in_flight_->compare_exchange_weak(current, current + 1U,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
      break;
    }
  }

  auto state = std::make_shared<CallState>();
  ros::ServiceClientOptions options(request.service, description->md5, false,
                                    ros::M_string{});
  state->client = node_handle_.serviceClient(options);
  state->request = serializeServiceRequest(*dynamic_request);
  state->md5 = description->md5;
  std::future<bool> completed = state->completed.get_future();
  const auto in_flight = in_flight_;
  std::thread([state, in_flight]() {
    try {
      const bool success =
          state->client.call(state->request, state->response, state->md5);
      state->completed.set_value(success);
    } catch (...) {
      try {
        state->completed.set_exception(std::current_exception());
      } catch (...) {
      }
    }
    in_flight->fetch_sub(1U, std::memory_order_acq_rel);
  }).detach();

  if (completed.wait_for(std::chrono::milliseconds(request.call_timeout_ms)) !=
      std::future_status::ready) {
    state->client.shutdown();
    uncertainError("service_call_timeout",
                   "ROS1 service call to '" + request.service +
                       "' exceeded callTimeoutMs; the server may have executed "
                       "the request");
  }

  bool call_succeeded = false;
  try {
    call_succeeded = completed.get();
  } catch (const std::exception& exception) {
    uncertainError("service_call_failed",
                   "ROS1 service call to '" + request.service +
                       "' failed after dispatch: " + exception.what());
  } catch (...) {
    uncertainError("service_call_failed",
                   "ROS1 service call failed after dispatch");
  }
  if (!call_succeeded) {
    uncertainError(
        "service_call_failed",
        "ROS1 service call to '" + request.service + "' failed after dispatch");
  }

  const std::size_t payload_size = responsePayloadSize(state->response);
  std::size_t bytes_read = 0;
  ros_babel_fish::Message::Ptr dynamic_response;
  try {
    dynamic_response = ros_babel_fish::createMessageFromTemplate(
        description->response->message_template, state->response.message_start,
        payload_size, bytes_read);
  } catch (const std::exception& exception) {
    uncertainError("invalid_service_response",
                   "unable to decode response from ROS1 service '" +
                       request.service + "': " + exception.what());
  }
  if (!dynamic_response || bytes_read != payload_size) {
    uncertainError("invalid_service_response",
                   "ROS1 service response length did not match its type");
  }

  ServiceCallResult result;
  result.service = request.service;
  result.service_type = request.service_type;
  result.response = codec_.encode(*dynamic_response);
  result.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_at)
          .count());
  return result;
}

}  // namespace xgc_ros1_automation_gateway
