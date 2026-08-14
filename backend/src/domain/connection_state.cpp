#include "dispatcher/domain/connection_state.hpp"

namespace dispatcher::domain {

ConnectionStateMachine::ConnectionStateMachine(ConnectionState initial_state)
    : state_(initial_state) {}

ConnectionState ConnectionStateMachine::state() const noexcept {
  return state_;
}

bool ConnectionStateMachine::canTransitionTo(
    ConnectionState next_state) const noexcept {
  if (next_state == state_) {
    return true;
  }

  switch (state_) {
    case ConnectionState::Disconnected:
      return next_state == ConnectionState::Connecting ||
             next_state == ConnectionState::Disabled;
    case ConnectionState::Connecting:
      return next_state == ConnectionState::Online ||
             next_state == ConnectionState::Disconnected ||
             next_state == ConnectionState::Reconnecting ||
             next_state == ConnectionState::Disabled;
    case ConnectionState::Online:
      return next_state == ConnectionState::Degraded ||
             next_state == ConnectionState::Reconnecting ||
             next_state == ConnectionState::Disconnected ||
             next_state == ConnectionState::Disabled;
    case ConnectionState::Degraded:
      return next_state == ConnectionState::Online ||
             next_state == ConnectionState::Reconnecting ||
             next_state == ConnectionState::Disconnected ||
             next_state == ConnectionState::Disabled;
    case ConnectionState::Reconnecting:
      return next_state == ConnectionState::Connecting ||
             next_state == ConnectionState::Online ||
             next_state == ConnectionState::Disconnected ||
             next_state == ConnectionState::Disabled;
    case ConnectionState::Disabled:
      return next_state == ConnectionState::Disconnected;
  }

  return false;
}

bool ConnectionStateMachine::transitionTo(
    ConnectionState next_state) noexcept {
  if (!canTransitionTo(next_state)) {
    return false;
  }

  state_ = next_state;
  return true;
}

std::string_view toString(ConnectionState state) noexcept {
  switch (state) {
    case ConnectionState::Disconnected:
      return "DISCONNECTED";
    case ConnectionState::Connecting:
      return "CONNECTING";
    case ConnectionState::Online:
      return "ONLINE";
    case ConnectionState::Degraded:
      return "DEGRADED";
    case ConnectionState::Reconnecting:
      return "RECONNECTING";
    case ConnectionState::Disabled:
      return "DISABLED";
  }

  return "UNKNOWN";
}

}  // namespace dispatcher::domain

