#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace xgc_ros1_automation_gateway {

class RequestCache {
 public:
  struct Claim {
    bool owner{false};
    std::string cached_response;
  };

  explicit RequestCache(std::size_t maximum_entries = 2048);

  Claim claim(const std::string& request_id, const std::string& fingerprint);
  void complete(const std::string& request_id, std::string response);

 private:
  struct Entry {
    std::string fingerprint;
    std::string response;
    bool complete{false};
    std::condition_variable ready;
  };

  void evictCompletedLocked();

  std::size_t maximum_entries_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Entry>> entries_;
  std::deque<std::string> completed_order_;
};

}  // namespace xgc_ros1_automation_gateway
