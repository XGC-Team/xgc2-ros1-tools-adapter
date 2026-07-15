#include "xgc_ros1_automation_gateway/uds_server.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace xgc_ros1_automation_gateway {
namespace {

bool readExact(int fd, void* destination, std::size_t size,
               const std::atomic<bool>& stopping) {
  auto* output = static_cast<std::uint8_t*>(destination);
  std::size_t consumed = 0;
  while (consumed < size && !stopping.load(std::memory_order_relaxed)) {
    const ssize_t count = ::recv(fd, output + consumed, size - consumed, 0);
    if (count == 0) {
      return false;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    }
    consumed += static_cast<std::size_t>(count);
  }
  return consumed == size;
}

bool writeExact(int fd, const void* source, std::size_t size) {
  const auto* input = static_cast<const std::uint8_t*>(source);
  std::size_t consumed = 0;
  while (consumed < size) {
    const ssize_t count =
        ::send(fd, input + consumed, size - consumed, MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (count == 0) {
      return false;
    }
    consumed += static_cast<std::size_t>(count);
  }
  return true;
}

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

}  // namespace

UdsServer::UdsServer(std::string socket_path, Handler handler,
                     std::uint32_t maximum_connections)
    : socket_path_(std::move(socket_path)),
      handler_(std::move(handler)),
      maximum_connections_(maximum_connections) {
  if (socket_path_.empty()) {
    throw std::invalid_argument("Unix socket path must not be empty");
  }
  if (!handler_) {
    throw std::invalid_argument("Unix socket handler must not be empty");
  }
  if (maximum_connections_ == 0) {
    throw std::invalid_argument(
        "maximum Unix socket connections must be greater than zero");
  }
}

UdsServer::~UdsServer() {
  stop();
  while (active_connections_.load(std::memory_order_acquire) != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (owns_socket_.exchange(false, std::memory_order_acq_rel)) {
    ::unlink(socket_path_.c_str());
  }
}

void UdsServer::prepareSocket() {
  if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
    throw std::runtime_error("Unix socket path is too long: " + socket_path_);
  }

  struct stat existing {};
  if (::lstat(socket_path_.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode)) {
      throw std::runtime_error("refusing to replace a non-socket path: " +
                               socket_path_);
    }

    FileDescriptor probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (probe.get() < 0) {
      throw std::runtime_error("unable to create Unix socket probe: " +
                               std::string(std::strerror(errno)));
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path_.c_str(),
                 sizeof(address.sun_path) - 1);
    if (::connect(probe.get(), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0) {
      throw std::runtime_error(
          "Unix socket is already accepting connections: " + socket_path_);
    }
    if (errno != ECONNREFUSED && errno != ENOENT) {
      throw std::runtime_error("unable to verify existing Unix socket: " +
                               std::string(std::strerror(errno)));
    }
    if (::unlink(socket_path_.c_str()) != 0 && errno != ENOENT) {
      throw std::runtime_error("unable to remove stale Unix socket: " +
                               std::string(std::strerror(errno)));
    }
  } else if (errno != ENOENT) {
    throw std::runtime_error("unable to inspect Unix socket path: " +
                             std::string(std::strerror(errno)));
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::runtime_error("unable to create Unix socket: " +
                             std::string(std::strerror(errno)));
  }
  listen_fd_.store(fd, std::memory_order_release);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path_.c_str(),
               sizeof(address.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string error = std::strerror(errno);
    listen_fd_.store(-1, std::memory_order_release);
    ::close(fd);
    throw std::runtime_error("unable to bind Unix socket: " + error);
  }
  owns_socket_.store(true, std::memory_order_release);
  if (::chmod(socket_path_.c_str(), 0660) != 0) {
    const std::string error = std::strerror(errno);
    stop();
    throw std::runtime_error("unable to set Unix socket permissions: " + error);
  }
  if (::listen(fd, static_cast<int>(maximum_connections_)) != 0) {
    const std::string error = std::strerror(errno);
    stop();
    throw std::runtime_error("unable to listen on Unix socket: " + error);
  }
}

void UdsServer::run() {
  stopping_.store(false, std::memory_order_release);
  prepareSocket();
  while (!stopping_.load(std::memory_order_acquire)) {
    const int listening = listen_fd_.load(std::memory_order_acquire);
    if (listening < 0) {
      break;
    }
    const int client = ::accept4(listening, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (stopping_.load(std::memory_order_acquire) || errno == EBADF ||
          errno == EINVAL) {
        break;
      }
      throw std::runtime_error("unable to accept Unix socket connection: " +
                               std::string(std::strerror(errno)));
    }

    const std::uint32_t previous =
        active_connections_.fetch_add(1U, std::memory_order_acq_rel);
    if (previous >= maximum_connections_) {
      active_connections_.fetch_sub(1U, std::memory_order_acq_rel);
      ::close(client);
      continue;
    }
    std::thread([this, client]() {
      serveClient(client);
      active_connections_.fetch_sub(1U, std::memory_order_acq_rel);
    }).detach();
  }
}

void UdsServer::stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  const int fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
}

void UdsServer::serveClient(int client_fd) noexcept {
  FileDescriptor client(client_fd);
  timeval timeout{};
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
               sizeof(timeout));
  ::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
               sizeof(timeout));

  while (!stopping_.load(std::memory_order_acquire)) {
    std::uint32_t network_length = 0;
    if (!readExact(client.get(), &network_length, sizeof(network_length),
                   stopping_)) {
      return;
    }
    const std::size_t length = ntohl(network_length);
    if (length == 0 || length > kMaximumFrameBytes) {
      return;
    }
    std::string request(length, '\0');
    if (!readExact(client.get(), request.data(), request.size(), stopping_)) {
      return;
    }

    std::string response;
    try {
      response = handler_(request);
    } catch (...) {
      return;
    }
    if (response.empty() || response.size() > kMaximumFrameBytes) {
      return;
    }
    const std::uint32_t response_length =
        htonl(static_cast<std::uint32_t>(response.size()));
    if (!writeExact(client.get(), &response_length, sizeof(response_length)) ||
        !writeExact(client.get(), response.data(), response.size())) {
      return;
    }
  }
}

}  // namespace xgc_ros1_automation_gateway
