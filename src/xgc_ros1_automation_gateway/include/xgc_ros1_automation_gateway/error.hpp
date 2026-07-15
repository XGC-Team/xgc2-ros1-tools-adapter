#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace xgc_ros1_automation_gateway {

class GatewayError : public std::runtime_error {
 public:
  GatewayError(std::string code, std::string error_class, std::string message)
      : std::runtime_error(std::move(message)),
        code_(std::move(code)),
        error_class_(std::move(error_class)) {}

  const std::string& code() const noexcept { return code_; }
  const std::string& errorClass() const noexcept { return error_class_; }

 private:
  std::string code_;
  std::string error_class_;
};

[[noreturn]] inline void permanentError(const std::string& code,
                                        const std::string& message) {
  throw GatewayError(code, "permanent", message);
}

[[noreturn]] inline void transientError(const std::string& code,
                                        const std::string& message) {
  throw GatewayError(code, "transient", message);
}

[[noreturn]] inline void uncertainError(const std::string& code,
                                        const std::string& message) {
  throw GatewayError(code, "uncertain", message);
}

}  // namespace xgc_ros1_automation_gateway
