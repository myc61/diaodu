#include "dispatcher/ros/beast_transport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/stream_base.hpp>

#include <chrono>
#include <functional>
#include <system_error>
#include <utility>

namespace dispatcher::ros {
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct BeastTransport::Impl {
  net::io_context ioc;
  std::unique_ptr<websocket::stream<tcp::socket>> ws;
};

BeastTransport::BeastTransport(
    std::string host,
    std::uint16_t port,
    std::string path,
    bool secure)
    : host_(std::move(host)),
      port_(port),
      path_(std::move(path)),
      secure_(secure),
      impl_(std::make_unique<Impl>()) {
  (void)secure_;
}

BeastTransport::~BeastTransport() {
  stop();
}

void BeastTransport::start(
    MessageHandler on_message,
    LifecycleHandler on_lifecycle) {
  stop();
  on_message_ = std::move(on_message);
  on_lifecycle_ = std::move(on_lifecycle);
  stop_requested_ = false;
  worker_ = std::thread([self = shared_from_this()] { self->run(); });
}

void BeastTransport::stop() {
  stop_requested_.store(true, std::memory_order_release);
  connected_.store(false, std::memory_order_release);
  stop_cv_.notify_all();
  impl_->ioc.stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool BeastTransport::sendText(std::string_view payload) {
  std::lock_guard lock(write_mutex_);
  if (!connected_ || impl_ == nullptr || impl_->ws == nullptr) {
    return false;
  }
  beast::error_code ec;
  impl_->ws->text(true);
  impl_->ws->write(net::buffer(payload.data(), payload.size()), ec);
  return !ec;
}

void BeastTransport::close() {
  connected_.store(false, std::memory_order_release);
  if (impl_ == nullptr) {
    return;
  }
  impl_->ioc.post([this] {
    std::lock_guard lock(write_mutex_);
    if (impl_ == nullptr || impl_->ws == nullptr) {
      return;
    }
    beast::error_code ec;
    impl_->ws->next_layer().shutdown(tcp::socket::shutdown_both, ec);
    ec.clear();
    impl_->ws->next_layer().close(ec);
  });
}

void BeastTransport::requestReconnect() {
  connected_.store(false, std::memory_order_release);
  if (impl_ == nullptr) {
    return;
  }
  impl_->ioc.post([this] {
    std::lock_guard lock(write_mutex_);
    if (impl_ == nullptr || impl_->ws == nullptr) {
      return;
    }
    beast::error_code ec;
    impl_->ws->next_layer().shutdown(tcp::socket::shutdown_both, ec);
    ec.clear();
    impl_->ws->next_layer().close(ec);
  });
}

bool BeastTransport::connected() const noexcept {
  return connected_;
}

void BeastTransport::run() {
  while (!stop_requested_) {
    bool was_connected = false;
    std::string disconnect_reason = "disconnected";
    bool cycle_finished = false;
    beast::flat_buffer buffer;
    try {
      impl_->ioc.restart();
      tcp::resolver resolver(impl_->ioc);
      net::steady_timer connection_timer(impl_->ioc);
      connection_timer.expires_after(std::chrono::seconds(3));

      const auto finish_cycle = [&] {
        if (!cycle_finished) {
          cycle_finished = true;
          connection_timer.cancel();
        }
      };

      std::function<void()> read_next;
      read_next = [&] {
        if (stop_requested_ || cycle_finished || impl_->ws == nullptr) {
          finish_cycle();
          return;
        }
        impl_->ws->async_read(
            buffer,
            [&, read_next](beast::error_code ec, std::size_t) mutable {
              if (ec) {
                disconnect_reason = ec.message();
                finish_cycle();
                return;
              }
              const auto data = beast::buffers_to_string(buffer.data());
              buffer.consume(buffer.size());
              if (on_message_) {
                on_message_(data);
              }
              read_next();
            });
      };

      connection_timer.async_wait([&](beast::error_code ec) {
        if (ec || cycle_finished || stop_requested_) {
          return;
        }
        disconnect_reason = "connection timeout";
        finish_cycle();
        impl_->ioc.stop();
      });

      resolver.async_resolve(
          host_,
          std::to_string(port_),
          [&](beast::error_code ec, tcp::resolver::results_type results) {
            if (stop_requested_ || cycle_finished) {
              finish_cycle();
              return;
            }
            if (ec) {
              disconnect_reason = ec.message();
              finish_cycle();
              return;
            }

            auto ws = std::make_unique<websocket::stream<tcp::socket>>(
                impl_->ioc);
            websocket::stream_base::timeout ws_timeout{};
            ws_timeout.handshake_timeout = std::chrono::seconds(3);
            ws_timeout.idle_timeout = std::chrono::seconds(3);
            ws_timeout.keep_alive_pings = true;
            ws->set_option(ws_timeout);
            {
              std::lock_guard lock(write_mutex_);
              impl_->ws = std::move(ws);
            }

            net::async_connect(
                impl_->ws->next_layer(),
                results,
                [&](beast::error_code connect_ec, const tcp::endpoint&) {
                  if (stop_requested_ || cycle_finished) {
                    finish_cycle();
                    return;
                  }
                  if (connect_ec) {
                    disconnect_reason = connect_ec.message();
                    finish_cycle();
                    return;
                  }
                  const auto host_header =
                      host_ + ":" + std::to_string(port_);
                  impl_->ws->async_handshake(
                      host_header,
                      path_.empty() ? "/" : path_,
                      [&](beast::error_code handshake_ec) {
                        if (stop_requested_ || cycle_finished) {
                          finish_cycle();
                          return;
                        }
                        if (handshake_ec) {
                          disconnect_reason = handshake_ec.message();
                          finish_cycle();
                          return;
                        }
                        connection_timer.cancel();
                        {
                          std::lock_guard lock(write_mutex_);
                          connected_ = true;
                          was_connected = true;
                        }
                        if (on_lifecycle_) {
                          on_lifecycle_(true, "connected");
                        }
                        read_next();
                      });
                });
          });
      impl_->ioc.run();
    } catch (const std::exception& ex) {
      disconnect_reason = ex.what();
      was_connected = was_connected || connected_;
    } catch (...) {
      disconnect_reason = "unknown transport error";
      was_connected = was_connected || connected_;
    }

    {
      std::lock_guard lock(write_mutex_);
      impl_->ws.reset();
      connected_ = false;
    }
    if (on_lifecycle_) {
      on_lifecycle_(false, disconnect_reason);
    }
    (void)was_connected;
    if (stop_requested_) {
      break;
    }
    std::unique_lock lock(stop_mutex_);
    stop_cv_.wait_for(lock, std::chrono::seconds(2), [this] {
      return stop_requested_.load(std::memory_order_acquire);
    });
  }
}

}  // namespace dispatcher::ros
