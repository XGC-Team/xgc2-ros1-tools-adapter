#include <ros/ros.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "xgc2/adapter_runtime/client.hpp"
#include "xgc_ros1_tools_adapter/tools_adapter.hpp"

namespace {

std::atomic<bool> stop_requested{false};
std::atomic<bool> session_lost{false};

void handleSignal(int) {
  stop_requested.store(true, std::memory_order_release);
}

std::string parseBootstrapPath(int argc, char** argv) {
  if (argc == 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    std::cout << "Usage: xgc_ros1_tools_adapter_node "
                 "--adapter-bootstrap-file PATH\n";
    std::exit(0);
  }
  if (argc == 3 && std::string(argv[1]) == "--adapter-bootstrap-file" &&
      argv[2][0] != '\0') {
    return argv[2];
  }
  throw std::invalid_argument(
      "exactly --adapter-bootstrap-file PATH is required");
}

void logRuntime(xgc2::adapter_runtime::LogLevel level,
                const std::string& message) {
  switch (level) {
    case xgc2::adapter_runtime::LogLevel::kDebug:
      ROS_DEBUG_STREAM(message);
      break;
    case xgc2::adapter_runtime::LogLevel::kInfo:
      ROS_INFO_STREAM(message);
      break;
    case xgc2::adapter_runtime::LogLevel::kWarning:
      ROS_WARN_STREAM(message);
      break;
    case xgc2::adapter_runtime::LogLevel::kError:
      ROS_ERROR_STREAM(message);
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string bootstrap_path = parseBootstrapPath(argc, argv);
    auto runtime_config =
        xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path);

    // roscpp reads network identity during ros::init, so the trusted initial
    // full spec is parsed before any ROS global state exists.
    const auto native_context =
        xgc_ros1_tools_adapter::NativeContext::FromInstanceSpec(
            runtime_config.initial_spec());
    native_context.ApplyEnvironment();

    ros::init(argc, argv, "xgc_ros1_tools_adapter",
              ros::init_options::AnonymousName |
                  ros::init_options::NoSigintHandler |
                  ros::init_options::NoRosout);
    ros::NodeHandle node_handle;
    ros::AsyncSpinner spinner(4);
    spinner.start();

    xgc_ros1_tools_adapter::ToolsAdapter adapter(node_handle, native_context);
    std::string error;
    if (!xgc_ros1_tools_adapter::BindCapabilities(&runtime_config, &adapter,
                                                  &error)) {
      throw std::runtime_error(error);
    }
    runtime_config.dispatch_workers = 16;

    xgc2::adapter_runtime::ClientCallbacks callbacks;
    callbacks.apply_instance_spec = [&adapter](const auto& spec,
                                               std::string* apply_error) {
      return adapter.ApplyInstanceSpec(spec, apply_error);
    };
    callbacks.clear_instance_spec = [&adapter] { adapter.ClearInstanceSpec(); };
    callbacks.stop_requested = [](const auto& request) {
      ROS_INFO_STREAM(
          "Adapter Runtime requested process stop: " << request.reason());
      stop_requested.store(true, std::memory_order_release);
    };
    callbacks.session_lost = [](const std::string& reason) {
      ROS_ERROR_STREAM("Adapter Runtime session was lost: " << reason);
      session_lost.store(true, std::memory_order_release);
      stop_requested.store(true, std::memory_order_release);
    };
    callbacks.log = logRuntime;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    xgc2::adapter_runtime::Client runtime(std::move(runtime_config),
                                          std::move(callbacks));
    if (!runtime.Start(&error)) {
      throw std::runtime_error("Adapter Runtime startup failed: " + error);
    }
    ROS_INFO("XGC2 ROS1 Tools Adapter is ready");

    ros::WallRate poll_rate(20.0);
    int unavailable_polls = 0;
    constexpr int kUnavailableExitPolls = 10;
    while (ros::ok() && !stop_requested.load(std::memory_order_acquire)) {
      switch (adapter.ProbeMasterBinding()) {
        case xgc_ros1_tools_adapter::MasterBindingState::Changed:
          ROS_WARN(
              "ROS master process generation changed; exiting so Runtime can "
              "start a fresh Adapter generation");
          stop_requested.store(true, std::memory_order_release);
          continue;
        case xgc_ros1_tools_adapter::MasterBindingState::Unavailable:
          ++unavailable_polls;
          if (unavailable_polls >= kUnavailableExitPolls) {
            ROS_WARN(
                "ROS master is gone; exiting this Adapter process so the next "
                "publish starts a fresh ros::init");
            stop_requested.store(true, std::memory_order_release);
            continue;
          }
          break;
        case xgc_ros1_tools_adapter::MasterBindingState::Bound:
        case xgc_ros1_tools_adapter::MasterBindingState::Unbound:
          unavailable_polls = 0;
          break;
      }
      poll_rate.sleep();
    }

    runtime.Stop();
    spinner.stop();
    ros::shutdown();
    return session_lost.load(std::memory_order_acquire) ? 2 : 0;
  } catch (const std::exception& exception) {
    std::cerr << "xgc_ros1_tools_adapter: " << exception.what() << '\n';
    if (ros::isInitialized()) {
      ros::shutdown();
    }
    return 1;
  }
}
