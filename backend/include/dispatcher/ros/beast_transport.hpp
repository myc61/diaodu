#pragma once

#include "dispatcher/ros/rosbridge_session.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dispatcher::ros {

class BeastTransport : public IRosbridgeTransport,
                       public std::enable_shared_from_this<BeastTransport> {
 public:
  using MessageHandler = std::function<void(std::string_view)>;
  using LifecycleHandler = std::function<void(bool connected, std::string_view reason)>;

  BeastTransport(
      std::string host,
      std::uint16_t port,
      std::string path,
      bool secure = false);
  ~BeastTransport() override;

  void start(MessageHandler on_message, LifecycleHandler on_lifecycle = {});
  void stop();

  bool sendText(std::string_view payload) override;
  void close() override;
  // Abort the current socket so the worker reconnect loop can establish a
  // fresh WebSocket after a half-open connection is detected.
  void requestReconnect();

  [[nodiscard]] bool connected() const noexcept;

 private:
  void run();

  std::string host_;
  std::uint16_t port_;
  std::string path_;
  bool secure_{false};
  MessageHandler on_message_;
  LifecycleHandler on_lifecycle_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> connected_{false};
  std::mutex write_mutex_;
  std::condition_variable stop_cv_;
  std::mutex stop_mutex_;
  std::thread worker_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dispatcher::ros
