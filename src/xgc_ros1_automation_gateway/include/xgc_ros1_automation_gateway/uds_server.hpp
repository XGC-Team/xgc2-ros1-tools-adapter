#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace xgc_ros1_automation_gateway {

class UdsServer {
 public:
  static constexpr std::size_t kMaximumFrameBytes = 8U * 1024U * 1024U;

  using Handler = std::function<std::string(const std::string&)>;

  UdsServer(std::string socket_path, Handler handler,
            std::uint32_t maximum_connections = 32);
  ~UdsServer();

  UdsServer(const UdsServer&) = delete;
  UdsServer& operator=(const UdsServer&) = delete;

  void run();
  void stop() noexcept;
  const std::string& socketPath() const noexcept { return socket_path_; }

 private:
  void prepareSocket();
  void serveClient(int client_fd) noexcept;

  std::string socket_path_;
  Handler handler_;
  std::uint32_t maximum_connections_;
  std::atomic<std::uint32_t> active_connections_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> owns_socket_{false};
  std::atomic<int> listen_fd_{-1};
};

}  // namespace xgc_ros1_automation_gateway
