#import "Logging.hpp"
#import <beast/core/handler_helpers.hpp>
#import <beast/core/handler_ptr.hpp>
#import <beast/core/placeholders.hpp>
#import <beast/core/streambuf.hpp>
#import <beast/http.hpp>
#import <boost/asio.hpp>
#import <cstddef>
#import <cstdio>
#import <iostream>
#import <memory>
#import <mutex>
#import <sstream>
#import <thread>
#import <utility>

namespace ssvim {
namespace http {

using namespace beast;
using namespace beast::http;

struct ServiceContext {
public:
  const std::string secret;
  const LogLevel logLevel;
  ServiceContext(std::string secret, LogLevel logLevel)
      : secret(secret), logLevel(logLevel) {
  }
};

/**
 * SSVI HTTP Server is a HTTP front end for Swift Semantic
 * tasks.
 */
class SemanticHTTPServer {
  using endpoint_type = boost::asio::ip::tcp::endpoint;
  using address_type = boost::asio::ip::address;
  using socket_type = boost::asio::ip::tcp::socket;

  std::mutex _sharedMutex;
  boost::asio::io_service _ioService;
  boost::asio::ip::tcp::acceptor _acceptor;
  socket_type _socket;
  std::string _root_path;
  std::vector<std::thread> _thread;
  ServiceContext _context;

public:
  SemanticHTTPServer(endpoint_type const &ep, std::size_t threads,
                     std::string const &root, ServiceContext const context)
      : _acceptor(_ioService), _socket(_ioService), _root_path(root),
        _context(context) {
    _acceptor.open(ep.protocol());
    _acceptor.bind(ep);
    _acceptor.listen(boost::asio::socket_base::max_connections);
    _acceptor.async_accept(_socket,
                           std::bind(&SemanticHTTPServer::onAccept, this,
                                     beast::asio::placeholders::error));
    _thread.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
      _thread.emplace_back([&] { _ioService.run(); });
  }

  ~SemanticHTTPServer() {
    error_code ec;
    _ioService.dispatch([&] { _acceptor.close(ec); });
    for (auto &t : _thread)
      t.join();
  }

private:
  template <class Stream, class Handler, bool isRequest, class Body,
            class Fields>
  class writeOp {
    struct data {
      bool cont;
      Stream &s;
      message<isRequest, Body, Fields> m;

      data(Handler &handler, Stream &s_,
           message<isRequest, Body, Fields> &&_sharedMutex)
          : cont(beast_asio_helpers::is_continuation(handler)), s(s_),
            m(std::move(_sharedMutex)) {
      }
    };

    handler_ptr<data, Handler> d_;

  public:
    writeOp(writeOp &&) = default;
    writeOp(writeOp const &) = default;

    template <class DeducedHandler, class... Args>
    writeOp(DeducedHandler &&h, Stream &s, Args &&... args)
        : d_(std::forward<DeducedHandler>(h), s, std::forward<Args>(args)...) {
      (*this)(error_code{}, false);
    }

    void operator()(error_code ec, bool again = true) {
      auto &d = *d_;
      d.cont = d.cont || again;
      if (!again) {
        beast::http::async_write(d.s, d.m, std::move(*this));
        return;
      }
      d_.invoke(ec);
    }

    friend void *asio_handler_allocate(std::size_t size, writeOp *op) {
      return beast_asio_helpers::allocate(size, op->d_.handler());
    }

    friend void asio_handler_deallocate(void *p, std::size_t size,
                                        writeOp *op) {
      return beast_asio_helpers::deallocate(p, size, op->d_.handler());
    }

    friend bool asio_handler_is_continuation(writeOp *op) {
      return op->d_->cont;
    }

    template <class Function>
    friend void asio_handler_invoke(Function &&f, writeOp *op) {
      return beast_asio_helpers::invoke(f, op->d_.handler());
    }
  };

  template <class Stream, bool isRequest, class Body, class Fields,
            class DeducedHandler>
  static void asyncWrite(Stream &stream, message<isRequest, Body, Fields> &&msg,
                         DeducedHandler &&handler) {
    writeOp<Stream, typename std::decay<DeducedHandler>::type, isRequest, Body,
            Fields>{std::forward<DeducedHandler>(handler), stream,
                    std::move(msg)};
  }

  void onAccept(error_code ec);
};

} // namespace http
} // namespace ssvim
