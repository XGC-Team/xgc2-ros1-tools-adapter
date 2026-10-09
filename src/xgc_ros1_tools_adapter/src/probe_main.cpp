#include <ros/callback_queue.h>
#include <ros/network.h>
#include <ros/ros.h>
#include <ros_babel_fish/babel_fish.h>
#include <xmlrpcpp/XmlRpcClient.h>

#include <boost/asio.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
using Value = XmlRpc::XmlRpcValue;
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
void checkDeadline(Clock::time_point deadline) {
  if (stopped) throw std::runtime_error("probe cancelled");
  if (Clock::now() >= deadline) throw std::runtime_error("probe timed out");
}

Value call(const std::string& uri, const char* method, const Value& args,
           Clock::time_point deadline) {
  std::string host;
  uint32_t port = 0;
  if (!ros::network::splitURI(uri, host, port) || port == 0 || port > 65535)
    throw std::invalid_argument("invalid ROS XMLRPC URI");
  XmlRpc::XmlRpcClient client(host.c_str(), static_cast<int>(port), "/");
  if (!client.executeNonBlock(method, args))
    throw std::runtime_error(std::string(method) + " request failed");
  Value result;
  // Drive the ROS XMLRPC library's nonblocking dispatcher with our deadline.
  while (!client.executeCheckDone(result)) {
    checkDeadline(deadline);
    client._disp.work(0.005);
  }
  checkDeadline(deadline);
  if (client.isFault() || result.getType() != Value::TypeArray ||
      result.size() != 3 || result[0].getType() != Value::TypeInt ||
      static_cast<int>(result[0]) != 1)
    throw std::runtime_error(std::string(method) + " returned failure");
  return result[2];
}

Value arguments(const std::string& value = "") {
  Value args;
  args.setSize(value.empty() ? 1 : 2);
  args[0] = "/xgc_ros1_probe";
  if (!value.empty()) args[1] = value;
  return args;
}
std::string lookup(const std::string& master, const std::string& node,
                   Clock::time_point deadline) {
  Value uri = call(master, "lookupNode", arguments(node), deadline);
  if (uri.getType() != Value::TypeString)
    throw std::runtime_error("lookupNode did not return a URI");
  return static_cast<std::string>(uri);
}
std::string absoluteName(const std::string& value) {
  std::string reason;
  const auto start = value.find_first_not_of('/');
  const auto name =
      "/" + (start == std::string::npos ? "" : value.substr(start));
  if (name == "/" || !ros::names::validate(name, reason))
    throw std::invalid_argument("invalid ROS graph name");
  return name;
}

void topicRegistered(const std::string& master, const std::string& node,
                     const std::string& topic, Clock::time_point deadline) {
  if (!node.empty()) {
    Value args = arguments(topic);
    args.setSize(3);
    args[2].setSize(1);
    args[2][0].setSize(1);
    args[2][0][0] = "TCPROS";
    const auto endpoint =
        call(lookup(master, node, deadline), "requestTopic", args, deadline);
    if (endpoint.getType() != Value::TypeArray || endpoint.size() != 3 ||
        endpoint[0].getType() != Value::TypeString ||
        static_cast<std::string>(endpoint[0]) != "TCPROS" ||
        endpoint[1].getType() != Value::TypeString ||
        endpoint[2].getType() != Value::TypeInt ||
        static_cast<int>(endpoint[2]) <= 0)
      throw std::runtime_error("publisher did not serve the topic over TCPROS");
    return;
  }
  const auto state = call(master, "getSystemState", arguments(), deadline);
  if (state.getType() == Value::TypeArray && state.size() == 3 &&
      state[0].getType() == Value::TypeArray) {
    for (int i = 0; i < state[0].size(); ++i) {
      const auto& entry = state[0][i];
      if (entry.getType() == Value::TypeArray && entry.size() == 2 &&
          entry[0].getType() == Value::TypeString &&
          static_cast<std::string>(entry[0]) == topic &&
          entry[1].getType() == Value::TypeArray && entry[1].size() > 0)
        return;
    }
  }
  throw std::runtime_error("topic has no registered publisher");
}

void verifyTCP(const std::string& address, Clock::time_point deadline) {
  if (address.empty()) return;
  // Bracketed IPv6 and ordinary host:port use the same explicit contract.
  const auto colon = address.rfind(':');
  if (colon == std::string::npos)
    throw std::invalid_argument("invalid verify address");
  auto host = address.substr(0, colon);
  const auto port = address.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  if (host.empty() || host == "0.0.0.0" || host == "::") host = "127.0.0.1";
  boost::asio::io_context io;
  boost::asio::ip::tcp::resolver resolver(io);
  boost::asio::ip::tcp::socket socket(io);
  bool done = false;
  boost::system::error_code failure;
  resolver.async_resolve(host, port, [&](auto error, const auto& endpoints) {
    if (error) {
      failure = error;
      done = true;
      return;
    }
    boost::asio::async_connect(socket, endpoints,
                               [&](auto connect_error, const auto&) {
                                 failure = connect_error;
                                 done = true;
                               });
  });
  while (!done) {
    checkDeadline(deadline);
    io.run_for(std::chrono::milliseconds(5));
  }
  if (failure)
    throw std::runtime_error("verify TCP listener: " + failure.message());
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::map<std::string, std::string> options;
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 == argc || !options.emplace(argv[i], argv[i + 1]).second)
        throw std::invalid_argument("options must be unique name/value pairs");
    }
    for (const auto& option : options) {
      if (option.first != "--mode" && option.first != "--master-uri" &&
          option.first != "--node" && option.first != "--topic" &&
          option.first != "--verify-address" && option.first != "--timeout-ms")
        throw std::invalid_argument("unknown option: " + option.first);
    }
    const auto mode = options["--mode"], master = options["--master-uri"];
    if (master.empty()) throw std::invalid_argument("master URI is required");
    const auto timeout =
        options.count("--timeout-ms") ? options["--timeout-ms"] : "1000";
    if (timeout.empty() ||
        timeout.find_first_not_of("0123456789") != std::string::npos)
      throw std::invalid_argument("timeout must be integer milliseconds");
    const auto milliseconds = std::stoul(timeout);
    if (milliseconds < 1 || milliseconds > 86400000)
      throw std::invalid_argument("timeout must be 1..86400000 milliseconds");
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(milliseconds);
    const auto node =
        options["--node"].empty() ? "" : absoluteName(options["--node"]);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    ros::M_string remappings{{"__master", master}};
    ros::init(
        remappings, "xgc_ros1_probe",
        ros::init_options::AnonymousName | ros::init_options::NoSigintHandler);
    int pid = 0;
    if (mode == "master" || mode == "node") {
      if (mode == "node" && node.empty())
        throw std::invalid_argument("node is required");
      auto value =
          call(mode == "master" ? master : lookup(master, node, deadline),
               "getPid", arguments(), deadline);
      if (value.getType() != Value::TypeInt || static_cast<int>(value) <= 0)
        throw std::runtime_error("getPid did not return a positive PID");
      pid = static_cast<int>(value);
    } else if (mode == "topic" || mode == "message" ||
               mode == "mavros-connected") {
      const auto topic = absoluteName(options["--topic"]);
      if (mode == "topic") {
        topicRegistered(master, node, topic, deadline);
      } else {
        ros::NodeHandle handle;
        ros_babel_fish::BabelFish fish;
        bool received = false;
        std::string failure;
        const boost::function<void(
            const ros::MessageEvent<ros_babel_fish::BabelFishMessage const>&)>
            callback = [&](const auto& event) {
              if (!node.empty() && event.getPublisherName() != node) return;
              try {
                if (mode == "mavros-connected") {
                  const auto& message = event.getMessage();
                  if (message->dataType() != "mavros_msgs/State")
                    throw std::runtime_error("expected mavros_msgs/State");
                  if (!(*fish.translateMessage(*message))["connected"]
                           .template value<bool>())
                    throw std::runtime_error(
                        "MAVROS reports the FCU as disconnected");
                }
                received = true;
              } catch (const std::exception& error) {
                failure = error.what();
              }
            };
        auto subscription = handle.subscribe<ros_babel_fish::BabelFishMessage>(
            topic, 1, callback);
        while (!received && failure.empty() && ros::ok()) {
          checkDeadline(deadline);
          ros::getGlobalCallbackQueue()->callAvailable(
              ros::WallDuration(0.005));
        }
        checkDeadline(deadline);
        if (!failure.empty()) throw std::runtime_error(failure);
        if (!received)
          throw std::runtime_error("ROS shut down before a topic message");
      }
    } else {
      throw std::invalid_argument("unsupported probe mode");
    }
    verifyTCP(options["--verify-address"], deadline);
    if (pid != 0) std::cout << pid << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return stopped ? 130 : 1;
  }
}
