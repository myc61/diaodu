#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

namespace dispatcher::workflow {

struct FeedbackTriggerPolicy {
  std::size_t max_firings{1};
  std::chrono::milliseconds cooldown{0};
};

class FeedbackTriggerGate {
 public:
  using Clock = std::chrono::steady_clock;

  explicit FeedbackTriggerGate(FeedbackTriggerPolicy policy = {});

  bool evaluate(bool condition, Clock::time_point now);
  void reset() noexcept;

  [[nodiscard]] std::size_t firingCount() const noexcept;

 private:
  FeedbackTriggerPolicy policy_;
  bool previous_condition_{false};
  std::size_t firing_count_{0};
  std::optional<Clock::time_point> last_fired_at_;
};

}  // namespace dispatcher::workflow

