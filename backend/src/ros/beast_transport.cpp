#include "dispatcher/ros/beast_transport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
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
  stop_requested_ = true;
  close();
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
  std::lock_guard lock(write_mutex_);
  if (impl_ == nullptr || impl_->ws == nullptr) {
    connected_ = false;
    return;
  }
  beast::error_code ec;
  impl_->ws->close(websocket::close_code::normal, ec);
  connected_ = false;
}

void BeastTransport::requestReconnect() {
  std::lock_guard lock(write_mutex_);
  connected_ = false;
  if (impl_ == nullptr || impl_->ws == nullptr) {
    return;
  }
  // A WebSocket close handshake can itself block on a half-open connection.
  // Shutting down the TCP layer wakes the worker's blocking read; run() then
  // publishes the disconnected lifecycle event and reconnects after backoff.
  beast::error_code ec;
  impl_->ws->next_layer().shutdown(tcp::socket::shutdown_both, ec);
  ec.clear();
  impl_->ws->next_layer().close(ec);
}

bool BeastTransport::connected() const noexcept {
  return connected_;
}

void BeastTransport::run() {
  while (!stop_requested_) {
    bool was_connected = false;
    std::string disconnect_reason = "disconnected";
    try {
      impl_->ioc.restart();
      tcp::resolver resolver(impl_->ioc);
      auto const results =
          resolver.resolve(host_, std::to_string(port_));
      auto socket = tcp::socket(impl_->ioc);
      // Avoid multi-minute hangs on unreachable robot IPs (blocks stop/join).
      beast::error_code connect_ec;
      socket.open(results.begin()->endpoint().protocol(), connect_ec);
      if (connect_ec) {
        throw boost::system::system_error(connect_ec);
      }
      ::timeval tv{};
      tv.tv_sec = 3;
      tv.tv_usec = 0;
      ::setsockopt(
          socket.native_handle(),
          SOL_SOCKET,
          SO_SNDTIMEO,
          &tv,
          sizeof(tv));
      ::setsockopt(
          socket.native_handle(),
          SOL_SOCKET,
          SO_RCVTIMEO,
          &tv,
          sizeof(tv));
      socket.connect(results.begin()->endpoint(), connect_ec);
      if (connect_ec) {
        throw boost::system::system_error(connect_ec);
      }
      auto ws = std::make_unique<websocket::stream<tcp::socket>>(
          std::move(socket));
      // Autobahn/rosbridge rejects Host without port on non-80/443 ports.
      const auto host_header = host_ + ":" + std::to_string(port_);
      ws->handshake(host_header, path_.empty() ? "/" : path_);
      {
        std::lock_guard lock(write_mutex_);
        impl_->ws = std::move(ws);
        connected_ = true;
        was_connected = true;
      }
      if (on_lifecycle_) {
        on_lifecycle_(true, "connected");
      }

      beast::flat_buffer buffer;
      while (!stop_requested_) {
        beast::error_code ec;
        impl_->ws->read(buffer, ec);
        if (ec) {
          disconnect_reason = ec.message();
          break;
        }
        const auto data = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        if (on_message_) {
          on_message_(data);
        }
      }
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
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

}  // namespace dispatcher::ros
