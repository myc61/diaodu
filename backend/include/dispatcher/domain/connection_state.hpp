#pragma once

#include "dispatcher/domain/types.hpp"

#include <string_view>

namespace dispatcher::domain {

class ConnectionStateMachine {
 public:
  explicit ConnectionStateMachine(
      ConnectionState initial_state = ConnectionState::Disconnected);

  [[nodiscard]] ConnectionState state() const noexcept;
  [[nodiscard]] bool canTransitionTo(ConnectionState next_state) const noexcept;
  bool transitionTo(ConnectionState next_state) noexcept;

 private:
  ConnectionState state_;
};

[[nodiscard]] std::string_view toString(ConnectionState state) noexcept;

}  // namespace dispatcher::domain

