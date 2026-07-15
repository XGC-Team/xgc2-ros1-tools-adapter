#include <ros/ros.h>

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "xgc_ros1_automation_gateway/gateway.hpp"
#include "xgc_ros1_automation_gateway/uds_server.hpp"

namespace {

std::atomic<bool> stop_requested{false};

void handleSignal(int) {
  stop_requested.store(true, std::memory_order_release);
}

std::string parseSocketPath(int argc, char** argv) {
  std::string socket_path;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--socket") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("--socket requires a path");
      }
      socket_path = argv[++index];
      continue;
    }
    constexpr char prefix[] = "--socket=";
    if (argument.compare(0, sizeof(prefix) - 1, prefix) == 0) {
      socket_path = argument.substr(sizeof(prefix) - 1);
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: xgc_ros1_automation_gateway_node --socket PATH\n";
      std::exit(0);
    }
  }
  if (socket_path.empty()) {
    throw std::invalid_argument("--socket PATH is required");
  }
  return socket_path;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string socket_path = parseSocketPath(argc, argv);
    ros::init(argc, argv, "xgc_ros1_automation_gateway",
              ros::init_options::NoSigintHandler);
    ros::NodeHandle node_handle;
    ros::AsyncSpinner spinner(2);
    spinner.start();

    xgc_ros1_automation_gateway::Gateway gateway(node_handle);
    xgc_ros1_automation_gateway::UdsServer server(
        socket_path, [&gateway](const std::string& request) {
          return gateway.handle(request);
        });

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::mutex server_error_mutex;
    std::exception_ptr server_error;
    std::thread server_thread([&]() {
      try {
        server.run();
      } catch (...) {
        std::lock_guard<std::mutex> lock(server_error_mutex);
        server_error = std::current_exception();
        stop_requested.store(true, std::memory_order_release);
      }
    });

    ROS_INFO_STREAM("XGC2 ROS1 Automation Gateway listening on "
                    << socket_path);
    ros::WallRate poll_rate(20.0);
    while (ros::ok() && !stop_requested.load(std::memory_order_acquire)) {
      poll_rate.sleep();
    }

    server.stop();
    if (server_thread.joinable()) {
      server_thread.join();
    }
    spinner.stop();
    ros::shutdown();

    {
      std::lock_guard<std::mutex> lock(server_error_mutex);
      if (server_error) {
        std::rethrow_exception(server_error);
      }
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "xgc_ros1_automation_gateway: " << exception.what() << '\n';
    return 1;
  }
}
