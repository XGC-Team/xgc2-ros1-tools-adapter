#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <json/json.h>
#include <ros/ros.h>
#include <std_msgs/String.h>
#include <std_srvs/SetBool.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <boost/function.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xgc_ros1_automation_gateway {
namespace {

constexpr char kSocketPath[] = "/tmp/xgc_ros1_automation_gateway_e2e.sock";

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

bool writeExact(int fd, const void* source, std::size_t size) {
  const auto* data = static_cast<const std::uint8_t*>(source);
  std::size_t written = 0;
  while (written < size) {
    const ssize_t count =
        ::send(fd, data + written, size - written, MSG_NOSIGNAL);
    if (count <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool readExact(int fd, void* destination, std::size_t size) {
  auto* data = static_cast<std::uint8_t*>(destination);
  std::size_t read = 0;
  while (read < size) {
    const ssize_t count = ::recv(fd, data + read, size - read, 0);
    if (count <= 0) {
      return false;
    }
    read += static_cast<std::size_t>(count);
  }
  return true;
}

std::string writeJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
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

int connectGateway() {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::strncpy(address.sun_path, kSocketPath, sizeof(address.sun_path) - 1);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0) {
        return fd;
      }
      ::close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return -1;
}

Json::Value requestGateway(const Json::Value& request) {
  FileDescriptor connection(connectGateway());
  EXPECT_GE(connection.get(), 0);
  if (connection.get() < 0) {
    return Json::Value();
  }
  const std::string payload = writeJson(request);
  const std::uint32_t length =
      htonl(static_cast<std::uint32_t>(payload.size()));
  EXPECT_TRUE(writeExact(connection.get(), &length, sizeof(length)));
  EXPECT_TRUE(writeExact(connection.get(), payload.data(), payload.size()));

  std::uint32_t response_length = 0;
  EXPECT_TRUE(
      readExact(connection.get(), &response_length, sizeof(response_length)));
  response_length = ntohl(response_length);
  EXPECT_GT(response_length, 0U);
  EXPECT_LE(response_length, 8U * 1024U * 1024U);
  std::string response(response_length, '\0');
  EXPECT_TRUE(readExact(connection.get(), response.data(), response.size()));
  return parseJson(response);
}

Json::Value baseRequest(const std::string& request_id,
                        const std::string& operation) {
  Json::Value request(Json::objectValue);
  request["protocolVersion"] = Json::UInt(1);
  request["requestId"] = request_id;
  request["operation"] = operation;
  return request;
}

TEST(GatewayE2E, HealthUsesTheStrictEnvelope) {
  const Json::Value response =
      requestGateway(baseRequest("health-e2e", "health"));
  EXPECT_EQ(
      (std::vector<std::string>{"protocolVersion", "requestId", "result"}),
      response.getMemberNames());
  EXPECT_EQ("ok", response["result"]["status"].asString());
  EXPECT_TRUE(response["result"]["rosOk"].asBool());
  EXPECT_TRUE(response["result"]["masterReachable"].asBool());
}

TEST(GatewayE2E, PublishesTypedMessagesAndDeduplicatesRequestIds) {
  ros::NodeHandle node_handle;
  std::mutex mutex;
  std::condition_variable received;
  std::vector<std::string> messages;
  const auto subscriber = node_handle.subscribe<std_msgs::String>(
      "/xgc_gateway_test/topic", 10,
      [&](const std_msgs::String::ConstPtr& message) {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(message->data);
        received.notify_all();
      });

  Json::Value request = baseRequest("publish-once", "publish");
  request["topic"] = "/xgc_gateway_test/topic";
  request["messageType"] = "std_msgs/String";
  request["message"]["data"] = "hello";
  request["waitForSubscribersMs"] = Json::UInt(2000);
  const Json::Value first = requestGateway(request);
  ASSERT_TRUE(first.isMember("result")) << writeJson(first);
  EXPECT_EQ("published", first["result"]["event"].asString());
  EXPECT_EQ("std_msgs/String", first["result"]["messageType"].asString());

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(received.wait_for(lock, std::chrono::seconds(2),
                                  [&]() { return messages.size() == 1; }));
  }

  const Json::Value duplicate = requestGateway(request);
  EXPECT_EQ(first, duplicate);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(1U, messages.size());
  }

  request["message"]["data"] = "different";
  const Json::Value conflict = requestGateway(request);
  ASSERT_TRUE(conflict.isMember("error"));
  EXPECT_EQ("request_id_conflict", conflict["error"]["code"].asString());
  EXPECT_EQ("permanent", conflict["error"]["class"].asString());
}

TEST(GatewayE2E, CallsGeneratedStdServiceAndDecodesItsResponse) {
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
      "/xgc_gateway_test/set_bool", callback);
  ASSERT_TRUE(server);

  Json::Value request = baseRequest("service-set-bool", "call_service");
  request["service"] = "/xgc_gateway_test/set_bool";
  request["serviceType"] = "std_srvs/SetBool";
  request["request"]["data"] = true;
  request["waitForServiceMs"] = Json::UInt(2000);
  request["callTimeoutMs"] = Json::UInt(2000);
  const Json::Value response = requestGateway(request);
  ASSERT_TRUE(response.isMember("result")) << writeJson(response);
  EXPECT_EQ("response", response["result"]["event"].asString());
  EXPECT_TRUE(response["result"]["response"]["success"].asBool());
  EXPECT_EQ("enabled", response["result"]["response"]["message"].asString());
}

TEST(GatewayE2E, ClassifiesUnavailableAndTimedOutServices) {
  Json::Value unavailable = baseRequest("service-unavailable", "call_service");
  unavailable["service"] = "/xgc_gateway_test/missing";
  unavailable["serviceType"] = "std_srvs/SetBool";
  unavailable["request"]["data"] = true;
  unavailable["waitForServiceMs"] = Json::UInt(10);
  unavailable["callTimeoutMs"] = Json::UInt(100);
  const Json::Value unavailable_response = requestGateway(unavailable);
  EXPECT_EQ("service_unavailable",
            unavailable_response["error"]["code"].asString());
  EXPECT_EQ("transient", unavailable_response["error"]["class"].asString());

  ros::NodeHandle node_handle;
  const boost::function<bool(std_srvs::SetBool::Request&,
                             std_srvs::SetBool::Response&)>
      slow_callback = [](std_srvs::SetBool::Request& request,
                         std_srvs::SetBool::Response& response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        response.success = request.data;
        return true;
      };
  const auto slow_server =
      node_handle.advertiseService<std_srvs::SetBool::Request,
                                   std_srvs::SetBool::Response>(
          "/xgc_gateway_test/slow", slow_callback);
  ASSERT_TRUE(slow_server);

  Json::Value timeout = baseRequest("service-timeout", "call_service");
  timeout["service"] = "/xgc_gateway_test/slow";
  timeout["serviceType"] = "std_srvs/SetBool";
  timeout["request"]["data"] = true;
  timeout["waitForServiceMs"] = Json::UInt(1000);
  timeout["callTimeoutMs"] = Json::UInt(20);
  const Json::Value timeout_response = requestGateway(timeout);
  EXPECT_EQ("service_call_timeout",
            timeout_response["error"]["code"].asString());
  EXPECT_EQ("uncertain", timeout_response["error"]["class"].asString());
}

}  // namespace
}  // namespace xgc_ros1_automation_gateway

int main(int argc, char** argv) {
  ros::init(
      argc, argv, "xgc_ros1_automation_gateway_e2e_tests",
      ros::init_options::AnonymousName | ros::init_options::NoSigintHandler);
  ros::AsyncSpinner spinner(4);
  spinner.start();
  testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  spinner.stop();
  ros::shutdown();
  return result;
}
