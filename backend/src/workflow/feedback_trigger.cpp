#include "dispatcher/workflow/feedback_trigger.hpp"

namespace dispatcher::workflow {

FeedbackTriggerGate::FeedbackTriggerGate(FeedbackTriggerPolicy policy)
    : policy_(policy) {}

bool FeedbackTriggerGate::evaluate(
    bool condition,
    Clock::time_point now) {
  const bool rising_edge = condition && !previous_condition_;
  previous_condition_ = condition;

  if (!rising_edge) {
    return false;
  }

  if (policy_.max_firings != 0 &&
      firing_count_ >= policy_.max_firings) {
    return false;
  }

  if (last_fired_at_.has_value() &&
      now - *last_fired_at_ < policy_.cooldown) {
    return false;
  }

  ++firing_count_;
  last_fired_at_ = now;
  return true;
}

void FeedbackTriggerGate::reset() noexcept {
  previous_condition_ = false;
  firing_count_ = 0;
  last_fired_at_.reset();
}

std::size_t FeedbackTriggerGate::firingCount() const noexcept {
  return firing_count_;
}

}  // namespace dispatcher::workflow

