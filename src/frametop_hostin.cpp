/**
 * @file src/frametop_hostin.cpp
 * @brief Frametop host input: the process's one manager, and the capture service it talks to.
 */
#include "frametop_hostin.h"

#include "config.h"
#include "frametop_hostin_manager.h"
#include "logging.h"

#include <memory>

#ifdef __linux__
  #include <atomic>
  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace frametop_hostin {
  namespace {
    void log_line(const std::string &line) {
      BOOST_LOG(info) << "Frametop host input: "sv << line;
    }

#ifdef __linux__
    constexpr char SOCKET_PATH[] = "/run/vibepollo/host-input.sock";
    /// The longest message the service sends ("ev" with its most events) fits with room.
    constexpr std::size_t MAX_MESSAGE = 4096;
    /// A service that can't be reached is tried again this often, not on every lease.
    constexpr auto RETRY_INTERVAL = 2s;

    manager_t &the_manager();

    /// One connection to vibepollo-host-input. Its reader thread holds it until the socket
    /// closes; the fd goes with the last holder.
    struct connection_t {
      int fd;
      std::uint64_t id;
      std::atomic<bool> closed {false};

      ~connection_t() {
        close(fd);
      }
    };

    void read_service(std::shared_ptr<connection_t> connection) {
      char buffer[MAX_MESSAGE];
      for (;;) {
        const ssize_t n = recv(connection->fd, buffer, sizeof buffer, MSG_TRUNC);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (std::size_t(n) > sizeof buffer) continue;  // nothing the service sends
        the_manager().service_message(connection->id, std::string_view {buffer, std::size_t(n)});
      }
      connection->closed = true;
      the_manager().service_gone(connection->id);
    }

    /// The capture service, connected on the first engage and again after it goes. The
    /// manager's lock is held for every call.
    class socket_service_t: public manager_t::service_t {
    public:
      bool send(std::string_view request) override {
        if (connection_ && connection_->closed) {
          // Its reader told the manager already; the next request connects again.
          connection_.reset();
          return false;
        }
        if (!connection_ && !connect_service()) return false;
        if (::send(connection_->fd, request.data(), request.size(), MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) return true;
        // Full too: the service isn't reading. Ending the connection gives the input back there.
        log_line("the capture service's connection failed: "s + std::strerror(errno));
        shutdown(connection_->fd, SHUT_RDWR);  // its reader sees the end and lets go
        connection_.reset();
        return false;
      }

      const char *unavailable() const override {
        return off_ ? "off" : "service";
      }

      std::uint64_t connection() const override {
        return connection_ ? connection_->id : 0;
      }

    private:
      bool connect_service() {
        const auto now = std::chrono::steady_clock::now();
        if (now < retry_at_) return false;
        retry_at_ = now + RETRY_INTERVAL;
        // Nonblocking, so a full backlog fails here rather than holding the manager's lock.
        const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return false;
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, SOCKET_PATH, sizeof SOCKET_PATH);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) < 0) {
          const int error = errno;
          // No socket: the host's owner hasn't turned it on (vibepollo-host-input.socket).
          off_ = error == ENOENT || error == ECONNREFUSED;
          if (!logged_failure_) log_line("can't reach the capture service ("s + SOCKET_PATH + "): " + std::strerror(error));
          logged_failure_ = true;
          close(fd);
          return false;
        }
        // Its reader blocks; requests go with MSG_DONTWAIT.
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        off_ = false;
        logged_failure_ = false;
        connection_ = std::make_shared<connection_t>(fd, ++last_id_);
        std::thread {read_service, connection_}.detach();
        log_line("connected to the capture service");
        return true;
      }

      std::shared_ptr<connection_t> connection_;
      std::uint64_t last_id_ = 0;
      std::chrono::steady_clock::time_point retry_at_ {};
      bool logged_failure_ = false;
      bool off_ = false;
    };

    using service_type = socket_service_t;
#else
    /// No capture service on this platform yet: every lease is answered "unsupported".
    class unsupported_service_t: public manager_t::service_t {
    public:
      bool send(std::string_view) override {
        return false;
      }

      const char *unavailable() const override {
        return "unsupported";
      }

      std::uint64_t connection() const override {
        return 0;
      }
    };

    using service_type = unsupported_service_t;
#endif

    /// Built on first use, after the config is read, and never destroyed: a reader thread may
    /// still be running when the process exits.
    manager_t &the_manager() {
      static auto *service = new service_type;
      static auto *manager = new manager_t {*service, config::nvhttp.sunshine_name, log_line};
      return *manager;
    }
  }  // namespace

  void on_lease(const void *session, bool permitted, std::string_view payload) {
    the_manager().lease(session, permitted, payload, manager_t::clock::now());
  }

  void on_session_end(const void *session) {
    the_manager().session_end(session);
  }

  std::vector<std::string> take(const void *session, bool permitted) {
    return the_manager().take(session, permitted, manager_t::clock::now());
  }

  bool active() {
    return the_manager().active();
  }
}  // namespace frametop_hostin
