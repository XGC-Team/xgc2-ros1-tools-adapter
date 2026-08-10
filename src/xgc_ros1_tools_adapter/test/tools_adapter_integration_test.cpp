#include <gtest/gtest.h>
#include <json/json.h>
#include <ros/ros.h>
#include <std_msgs/String.h>
#include <std_srvs/Empty.h>
#include <std_srvs/SetBool.h>

#include <atomic>
#include <boost/function.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xgc_ros1_tools_adapter/error.hpp"
#include "xgc_ros1_tools_adapter/generated_contract.hpp"
#include "xgc_ros1_tools_adapter/service_invoker.hpp"
#include "xgc_ros1_tools_adapter/tools_adapter.hpp"

namespace xgc_ros1_tools_adapter {
namespace {

constexpr char kScopeKey[] =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::int64_t deadlineNanos(std::chrono::seconds after) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             (std::chrono::system_clock::now() + after).time_since_epoch())
      .count();
}

xgc::v1::SchemaReference schema(const contract::Schema& source) {
  xgc::v1::SchemaReference result;
  result.set_message_id(source.message_id);
  result.set_type_name(source.type_name);
  result.set_schema_version(source.version);
  result.set_schema_fingerprint(source.fingerprint);
  return result;
}

xgc::adapter::v1::OperationRequest operation(
    const std::string& json, const contract::Endpoint& endpoint) {
  xgc::adapter::v1::OperationRequest request;
  auto* context = request.mutable_context();
  context->set_capability_id(endpoint.capability_id);
  context->set_contract_version(endpoint.contract_version);
  context->set_contract_digest(endpoint.contract_digest);
  context->set_endpoint_id(endpoint.endpoint_id);
  context->mutable_deadline()->set_deadline_unix_nanos(
      deadlineNanos(std::chrono::seconds(5)));
  auto* subject = context->mutable_subject();
  subject->set_kind("ros1-native-context");
  subject->set_key(kScopeKey);
  (*subject->mutable_attributes())["master-uri"] = "http://127.0.0.1:11311";
  *request.mutable_input()->mutable_schema() = schema(endpoint.input_schema);
  request.mutable_input()->set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  request.mutable_input()->set_value(json);
  return request;
}

Json::Value parseJson(const std::string& input) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  EXPECT_TRUE(
      reader->parse(input.data(), input.data() + input.size(), &value, &errors))
      << errors;
  return value;
}

ToolsAdapter createAdapter() {
  return ToolsAdapter(ros::NodeHandle(), NativeContext{"http://127.0.0.1:11311",
                                                       "", "", kScopeKey});
}

xgc::adapter::v1::AdapterInstanceSpec instanceSpec() {
  xgc::adapter::v1::AdapterInstanceSpec spec;
  *spec.mutable_configuration()->mutable_schema() =
      schema(contract::kConfiguration);
  spec.mutable_configuration()->set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  spec.mutable_configuration()->set_value(
      R"({"rosMasterUri":"http://127.0.0.1:11311","rosIp":"","rosHostname":""})");
  spec.mutable_scope()->set_kind("ros1-native-context");
  spec.mutable_scope()->set_key(kScopeKey);
  (*spec.mutable_scope()->mutable_attributes())["master-uri"] =
      "http://127.0.0.1:11311";
  return spec;
}

xgc::adapter::v1::EnabledCapability grant(const contract::Endpoint& endpoint) {
  xgc::adapter::v1::EnabledCapability result;
  result.set_capability_id(endpoint.capability_id);
  result.set_contract_version(endpoint.contract_version);
  result.set_contract_digest(endpoint.contract_digest);
  result.add_enabled_endpoint_ids(endpoint.endpoint_id);
  return result;
}

void activatePublish(ToolsAdapter* adapter) {
  auto spec = instanceSpec();
  std::string error;
  ASSERT_TRUE(adapter->ApplyInstanceSpec(spec, &error)) << error;
  ASSERT_TRUE(
      adapter->StartPublishCapability(spec, grant(contract::kPublish), &error))
      << error;
}

void activateService(ToolsAdapter* adapter) {
  auto spec = instanceSpec();
  std::string error;
  ASSERT_TRUE(adapter->ApplyInstanceSpec(spec, &error)) << error;
  ASSERT_TRUE(
      adapter->StartServiceCapability(spec, grant(contract::kService), &error))
      << error;
}

TEST(ToolsAdapterIntegration, PublishesTypedJsonMessage) {
  ros::NodeHandle node_handle;
  std::mutex mutex;
  std::condition_variable received;
  std::string received_value;
  std::uint32_t received_count = 0;
  const auto subscriber = node_handle.subscribe<std_msgs::String>(
      "/xgc_ros1_tools_adapter_test/topic", 10,
      [&](const std_msgs::String::ConstPtr& message) {
        std::lock_guard<std::mutex> lock(mutex);
        received_value = message->data;
        ++received_count;
        received.notify_all();
      });
  ASSERT_TRUE(subscriber);

  auto adapter = createAdapter();
  activatePublish(&adapter);
  auto request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/topic",
    "messageType":"std_msgs/String",
    "message":{"data":"hello"},
    "publishCount":3,
    "publishRateHz":20,
    "latch":false,
    "queueSize":10,
    "waitForSubscribersMs":2000
  })",
                           contract::kPublish);
  const auto result =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  ASSERT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, result.phase)
      << result.error.message();
  ASSERT_TRUE(result.has_output);
  const Json::Value output = parseJson(result.output.value());
  EXPECT_EQ("published", output["event"].asString());
  EXPECT_EQ("std_msgs/String", output["messageType"].asString());
  EXPECT_EQ(3u, output["publishedCount"].asUInt());
  EXPECT_EQ(schema(contract::kPublish.output_schema).SerializeAsString(),
            result.output.schema().SerializeAsString());

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(received.wait_for(lock, std::chrono::seconds(2),
                                [&] { return received_count == 3; }));
  EXPECT_EQ("hello", received_value);
}

TEST(ToolsAdapterIntegration, CallsTypedJsonService) {
  ros::NodeHandle node_handle;
  const boost::function<bool(std_srvs::SetBool::Request&,
                             std_srvs::SetBool::Response&)>
      callback = [](std_srvs::SetBool::Request& request,
                    std_srvs::SetBool::Response& response) {
        response.success = request.data;
        response.message = request.data ? "enabled" : "disabled";
        return true;
      };
  const auto server = node_handle.advertiseService<std_srvs::SetBool::Request,
                                                   std_srvs::SetBool::Response>(
      "/xgc_ros1_tools_adapter_test/set_bool", callback);
  ASSERT_TRUE(server);

  auto adapter = createAdapter();
  activateService(&adapter);
  auto request = operation(R"({
    "service":"/xgc_ros1_tools_adapter_test/set_bool",
    "serviceType":"std_srvs/SetBool",
    "request":{"data":true},
    "waitForServiceMs":2000,
    "callTimeoutMs":2000
  })",
                           contract::kService);
  const auto result =
      adapter.CallService(request, xgc2::adapter_runtime::CancellationToken());
  ASSERT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, result.phase)
      << result.error.message();
  const Json::Value output = parseJson(result.output.value());
  EXPECT_EQ("response", output["event"].asString());
  EXPECT_TRUE(output["response"]["success"].asBool());
  EXPECT_EQ("enabled", output["response"]["message"].asString());
}

TEST(ToolsAdapterIntegration, CallsServiceWithAnEmptyWireResponse) {
  ros::NodeHandle node_handle;
  const boost::function<bool(std_srvs::Empty::Request&,
                             std_srvs::Empty::Response&)>
      callback = [](std_srvs::Empty::Request&, std_srvs::Empty::Response&) {
        return true;
      };
  const auto server = node_handle.advertiseService<std_srvs::Empty::Request,
                                                   std_srvs::Empty::Response>(
      "/xgc_ros1_tools_adapter_test/empty", callback);
  ASSERT_TRUE(server);

  auto adapter = createAdapter();
  activateService(&adapter);
  auto request = operation(R"({
    "service":"/xgc_ros1_tools_adapter_test/empty",
    "serviceType":"std_srvs/Empty",
    "request":{},
    "waitForServiceMs":2000,
    "callTimeoutMs":2000
  })",
                           contract::kService);
  const auto result =
      adapter.CallService(request, xgc2::adapter_runtime::CancellationToken());
  ASSERT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, result.phase)
      << result.error.message();
  const Json::Value output = parseJson(result.output.value());
  EXPECT_TRUE(output["response"].isObject());
  EXPECT_TRUE(output["response"].empty());
}

TEST(ToolsAdapterIntegration, KillsAndReapsAServiceThatExceedsCallTimeout) {
  ros::NodeHandle node_handle;
  std::mutex mutex;
  std::condition_variable entered_condition;
  std::condition_variable release_condition;
  bool entered = false;
  bool release = false;
  const boost::function<bool(std_srvs::SetBool::Request&,
                             std_srvs::SetBool::Response&)>
      callback = [&](std_srvs::SetBool::Request&,
                     std_srvs::SetBool::Response& response) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        entered_condition.notify_all();
        release_condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return release; });
        response.success = true;
        return true;
      };
  const auto server = node_handle.advertiseService<std_srvs::SetBool::Request,
                                                   std_srvs::SetBool::Response>(
      "/xgc_ros1_tools_adapter_test/hanging", callback);
  ASSERT_TRUE(server);

  auto adapter = createAdapter();
  activateService(&adapter);
  auto request = operation(R"({
    "service":"/xgc_ros1_tools_adapter_test/hanging",
    "serviceType":"std_srvs/SetBool",
    "request":{"data":true},
    "waitForServiceMs":2000,
    "callTimeoutMs":100
  })",
                           contract::kService);
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      adapter.CallService(request, xgc2::adapter_runtime::CancellationToken());
  const auto elapsed = std::chrono::steady_clock::now() - started;

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  release_condition.notify_all();

  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_UNCERTAIN, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_UNCERTAIN, result.error.class_());
  EXPECT_EQ("service_call_timeout", result.error.code());
  EXPECT_LT(elapsed, std::chrono::seconds(2));
  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_TRUE(entered);
}

TEST(ToolsAdapterIntegration, PreCancelledServiceDoesNotReachRos) {
  ros::NodeHandle node_handle;
  std::atomic<int> calls{0};
  const boost::function<bool(std_srvs::Empty::Request&,
                             std_srvs::Empty::Response&)>
      callback = [&](std_srvs::Empty::Request&, std_srvs::Empty::Response&) {
        ++calls;
        return true;
      };
  const auto server = node_handle.advertiseService<std_srvs::Empty::Request,
                                                   std_srvs::Empty::Response>(
      "/xgc_ros1_tools_adapter_test/pre_cancelled", callback);
  ASSERT_TRUE(server);

  auto adapter = createAdapter();
  activateService(&adapter);
  auto request = operation(R"({
    "service":"/xgc_ros1_tools_adapter_test/pre_cancelled",
    "serviceType":"std_srvs/Empty",
    "request":{},
    "waitForServiceMs":2000,
    "callTimeoutMs":2000
  })",
                           contract::kService);
  auto cancellation =
      std::make_shared<xgc2::adapter_runtime::CancellationState>();
  cancellation->requested.store(true, std::memory_order_release);
  const auto result = adapter.CallService(
      request, xgc2::adapter_runtime::CancellationToken(cancellation));
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_CANCELLED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_CANCELLED, result.error.class_());
  EXPECT_EQ("service_call_cancelled", result.error.code());
  EXPECT_EQ(0, calls.load());
}

TEST(ToolsAdapterIntegration, RejectsInvocationOutsideCapabilityLifecycle) {
  auto adapter = createAdapter();
  const auto publish_request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/lifecycle",
    "messageType":"std_msgs/String",
    "message":{"data":"must-not-publish"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":false,
    "queueSize":10,
    "waitForSubscribersMs":0
  })",
                                         contract::kPublish);
  const auto service_request = operation(R"({
    "service":"/xgc_ros1_tools_adapter_test/lifecycle",
    "serviceType":"std_srvs/Empty",
    "request":{},
    "waitForServiceMs":0,
    "callTimeoutMs":100
  })",
                                         contract::kService);

  const auto never_started = adapter.Publish(
      publish_request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_REJECTED, never_started.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_REJECTED,
            never_started.error.class_());
  EXPECT_EQ("publish_capability_disabled", never_started.error.code());

  activatePublish(&adapter);
  adapter.StopPublishCapability();
  const auto after_stop = adapter.Publish(
      publish_request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_REJECTED, after_stop.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_REJECTED, after_stop.error.class_());
  EXPECT_EQ("publish_capability_disabled", after_stop.error.code());

  activateService(&adapter);
  adapter.ClearInstanceSpec();
  const auto after_clear = adapter.CallService(
      service_request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_REJECTED, after_clear.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_REJECTED, after_clear.error.class_());
  EXPECT_EQ("service_capability_disabled", after_clear.error.code());
}

TEST(ToolsAdapterIntegration,
     RejectsInputSchemaFingerprintDriftWithoutPublishing) {
  ros::NodeHandle node_handle;
  std::atomic<int> received{0};
  const auto subscriber = node_handle.subscribe<std_msgs::String>(
      "/xgc_ros1_tools_adapter_test/schema_drift", 10,
      [&](const std_msgs::String::ConstPtr&) { ++received; });
  ASSERT_TRUE(subscriber);

  auto adapter = createAdapter();
  activatePublish(&adapter);
  auto request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/schema_drift",
    "messageType":"std_msgs/String",
    "message":{"data":"must-not-publish"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":false,
    "queueSize":10,
    "waitForSubscribersMs":0
  })",
                           contract::kPublish);
  request.mutable_input()->mutable_schema()->set_schema_fingerprint(
      contract::kPublish.input_schema.fingerprint ^ 1u);

  const auto result =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_FAILED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_PERMANENT, result.error.class_());
  EXPECT_EQ("unsupported_input_contract", result.error.code());
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_EQ(0, received.load());
}

TEST(ToolsAdapterIntegration, StopPublishUnregistersLatchedPublisher) {
  auto adapter = createAdapter();
  activatePublish(&adapter);
  const auto request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/stopped_latch",
    "messageType":"std_msgs/String",
    "message":{"data":"stale"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":true,
    "queueSize":1,
    "waitForSubscribersMs":0
  })",
                                 contract::kPublish);
  const auto result =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  ASSERT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, result.phase)
      << result.error.message();

  adapter.StopPublishCapability();
  ros::NodeHandle node_handle;
  std::atomic<int> received{0};
  const auto subscriber = node_handle.subscribe<std_msgs::String>(
      "/xgc_ros1_tools_adapter_test/stopped_latch", 1,
      [&](const std_msgs::String::ConstPtr&) { ++received; });
  ASSERT_TRUE(subscriber);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(0, received.load());
}

TEST(ToolsAdapterIntegration,
     FailedSubscriberWaitReleasesUncommittedPublisher) {
  auto adapter = createAdapter();
  activatePublish(&adapter);
  auto request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/failed_wait_lease",
    "messageType":"std_msgs/String",
    "message":{"data":"not-dispatched"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":false,
    "queueSize":1,
    "waitForSubscribersMs":100
  })",
                           contract::kPublish);

  const auto timed_out =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_FAILED, timed_out.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_TRANSIENT, timed_out.error.class_());
  EXPECT_EQ("subscriber_wait_timeout", timed_out.error.code());

  request.mutable_input()->set_value(R"({
    "topic":"/xgc_ros1_tools_adapter_test/failed_wait_lease",
    "messageType":"std_msgs/String",
    "message":{"data":"replacement"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":true,
    "queueSize":2,
    "waitForSubscribersMs":0
  })");
  const auto replacement =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, replacement.phase)
      << replacement.error.message();
}

TEST(ToolsAdapterIntegration, RejectsServiceCallsAboveInFlightLimit) {
  ros::NodeHandle node_handle;
  std::mutex mutex;
  std::condition_variable entered_condition;
  std::condition_variable release_condition;
  bool entered = false;
  bool release = false;
  const boost::function<bool(std_srvs::SetBool::Request&,
                             std_srvs::SetBool::Response&)>
      callback = [&](std_srvs::SetBool::Request&,
                     std_srvs::SetBool::Response& response) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        entered_condition.notify_all();
        release_condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return release; });
        response.success = true;
        return true;
      };
  const auto server = node_handle.advertiseService<std_srvs::SetBool::Request,
                                                   std_srvs::SetBool::Response>(
      "/xgc_ros1_tools_adapter_test/in_flight_limit", callback);
  ASSERT_TRUE(server);

  TypeRegistry types;
  JsonCodec codec;
  ServiceInvoker invoker(types, codec, 1);
  ServiceCallRequest request;
  request.service = "/xgc_ros1_tools_adapter_test/in_flight_limit";
  request.service_type = "std_srvs/SetBool";
  request.request = Json::Value(Json::objectValue);
  request.request["data"] = true;
  request.wait_for_service_ms = 2000;
  request.call_timeout_ms = 4000;
  request.deadline_unix_nanos = deadlineNanos(std::chrono::seconds(5));

  auto first =
      std::async(std::launch::async, [&] { return invoker.call(request); });
  bool first_entered = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    first_entered = entered_condition.wait_for(lock, std::chrono::seconds(2),
                                               [&] { return entered; });
  }

  if (first_entered) {
    try {
      (void)invoker.call(request);
      ADD_FAILURE() << "second service call unexpectedly acquired quota";
    } catch (const Ros1ToolsError& error) {
      EXPECT_EQ("resource-exhausted", error.errorClass());
      EXPECT_EQ("service_call_busy", error.code());
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  release_condition.notify_all();
  ASSERT_TRUE(first_entered);
  ASSERT_EQ(std::future_status::ready, first.wait_for(std::chrono::seconds(2)));
  const ServiceCallResult first_result = first.get();
  EXPECT_EQ("/xgc_ros1_tools_adapter_test/in_flight_limit",
            first_result.service);
}

TEST(ToolsAdapterIntegration, RejectsOutOfContractEnvelopeFields) {
  auto adapter = createAdapter();
  activatePublish(&adapter);
  auto request = operation(R"({
    "protocolVersion":1,
    "requestId":"legacy",
    "operation":"publish",
    "topic":"/legacy",
    "messageType":"std_msgs/String",
    "message":{"data":"legacy"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":false,
    "queueSize":10,
    "waitForSubscribersMs":0
  })",
                           contract::kPublish);
  const auto result =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_FAILED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_PERMANENT, result.error.class_());
  EXPECT_EQ("invalid_request", result.error.code());
}

TEST(ToolsAdapterIntegration, RejectsPublishScheduleAboveDurationLimit) {
  auto adapter = createAdapter();
  activatePublish(&adapter);
  const auto request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/oversized_schedule",
    "messageType":"std_msgs/String",
    "message":{"data":"must-not-publish"},
    "publishCount":10000,
    "publishRateHz":0.1,
    "latch":false,
    "queueSize":10,
    "waitForSubscribersMs":0
  })",
                                 contract::kPublish);

  const auto result =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_FAILED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_PERMANENT, result.error.class_());
  EXPECT_EQ("invalid_request", result.error.code());
}

TEST(ToolsAdapterIntegration, RejectsInvocationForAnotherNativeContext) {
  auto adapter = createAdapter();
  activatePublish(&adapter);
  auto request = operation(R"({
    "topic":"/xgc_ros1_tools_adapter_test/wrong_context",
    "messageType":"std_msgs/String",
    "message":{"data":"must-not-publish"},
    "publishCount":1,
    "publishRateHz":1,
    "latch":false,
    "queueSize":10,
    "waitForSubscribersMs":0
  })",
                           contract::kPublish);
  (*request.mutable_context()
        ->mutable_subject()
        ->mutable_attributes())["master-uri"] = "http://127.0.0.1:11312";

  const auto result =
      adapter.Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_FAILED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_PERMANENT, result.error.class_());
  EXPECT_EQ("invalid_subject", result.error.code());
}

}  // namespace
}  // namespace xgc_ros1_tools_adapter

int main(int argc, char** argv) {
  ros::init(
      argc, argv, "xgc_ros1_tools_adapter_integration_tests",
      ros::init_options::AnonymousName | ros::init_options::NoSigintHandler);
  ros::AsyncSpinner spinner(4);
  spinner.start();
  testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  spinner.stop();
  ros::shutdown();
  return result;
}
