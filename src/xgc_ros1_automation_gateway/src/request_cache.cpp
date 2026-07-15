#include "xgc_ros1_automation_gateway/request_cache.hpp"

#include <utility>

#include "xgc_ros1_automation_gateway/error.hpp"

namespace xgc_ros1_automation_gateway {

RequestCache::RequestCache(std::size_t maximum_entries)
    : maximum_entries_(maximum_entries) {
  if (maximum_entries_ == 0) {
    permanentError("invalid_configuration",
                   "request cache capacity must be greater than zero");
  }
}

RequestCache::Claim RequestCache::claim(const std::string& request_id,
                                        const std::string& fingerprint) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iterator = entries_.find(request_id);
  if (iterator == entries_.end()) {
    evictCompletedLocked();
    if (entries_.size() >= maximum_entries_) {
      transientError("request_cache_busy",
                     "request deduplication cache is full");
    }
    auto entry = std::make_shared<Entry>();
    entry->fingerprint = fingerprint;
    entries_.emplace(request_id, std::move(entry));
    return Claim{true, {}};
  }

  const auto& entry = iterator->second;
  if (entry->fingerprint != fingerprint) {
    permanentError("request_id_conflict",
                   "requestId was already used for different content");
  }
  while (!entry->complete) {
    entry->ready.wait(lock);
  }
  return Claim{false, entry->response};
}

void RequestCache::complete(const std::string& request_id,
                            std::string response) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = entries_.find(request_id);
  if (iterator == entries_.end()) {
    return;
  }
  auto& entry = iterator->second;
  entry->response = std::move(response);
  entry->complete = true;
  completed_order_.push_back(request_id);
  entry->ready.notify_all();
  evictCompletedLocked();
}

void RequestCache::evictCompletedLocked() {
  while (entries_.size() >= maximum_entries_ && !completed_order_.empty()) {
    const std::string request_id = std::move(completed_order_.front());
    completed_order_.pop_front();
    const auto iterator = entries_.find(request_id);
    if (iterator != entries_.end() && iterator->second->complete) {
      entries_.erase(iterator);
    }
  }
}

}  // namespace xgc_ros1_automation_gateway
