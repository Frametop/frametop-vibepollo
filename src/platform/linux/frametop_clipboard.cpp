/**
 * @file src/platform/linux/frametop_clipboard.cpp
 * @brief Frametop: the desktop session's clipboard (text and PNG images) for /actions/clipboard.
 *
 * vibepollo-clipboard (packaging/linux/vibepollo-clipboard.c) is the clipboard, a Wayland
 * client of the desktop's compositor. Vibepollo starts it on the first request and keeps it,
 * so what a client copies stays on the clipboard: run as the desktop user it runs the helper
 * itself; as the system service the session broker runs it in the desktop session
 * (vibepollo-session-exec clipboard). Requests go to its stdin and replies come from its
 * stdout (packaging/linux/vibepollo-clipboard-protocol.h). A helper that stops answering, or
 * a session that changed under it, is stopped and the next request starts another. Only
 * sizes are logged, never the content.
 */
#include "packaging/linux/vibepollo-clipboard-protocol.h"
#include "src/logging.h"
#include "src/platform/common.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

using namespace std::literals;

namespace platf {
  namespace {
    constexpr auto SESSION_EXEC = "/usr/libexec/vibeshine/vibepollo-session-exec";
    constexpr auto HELPER = "/usr/libexec/vibeshine/vibepollo-clipboard";
    /// For the helper to say "ready", and to answer a request (its own wait for the app that
    /// owns the clipboard is 3 s for text, 8 s for an image).
    constexpr int READY_MS = 5000;
    constexpr int ANSWER_MS = 5000;
    constexpr int IMAGE_ANSWER_MS = 11000;
    /// For it to read a request's content, and to send a reply's.
    constexpr int TRANSFER_MS = 15000;
    constexpr int EXIT_MS = 1000;
    /// After it couldn't start, the next try waits this long.
    constexpr auto RETRY_AFTER = 5s;

    int64_t now_ms() {
      timespec now {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    }

    /// The helper run as the desktop user: next to this program's own folder's libexec (a
    /// package's /usr/bin and /usr/libexec, or a build's install tree), else the package's.
    std::string direct_helper() {
      char exe[PATH_MAX];
      const auto n = readlink("/proc/self/exe", exe, sizeof exe - 1);
      if (n > 0) {
        std::string path {exe, size_t(n)};
        path = path.substr(0, path.rfind('/'));
        path = path.substr(0, path.rfind('/')) + "/libexec/vibeshine/vibepollo-clipboard";
        if (access(path.c_str(), X_OK) == 0) {
          return path;
        }
      }
      return HELPER;
    }

    /// Blocks SIGPIPE on this thread while it lives: a write to a helper that ended is EPIPE.
    class sigpipe_guard_t {
    public:
      sigpipe_guard_t() {
        sigemptyset(&pipe_);
        sigaddset(&pipe_, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_, &old_);
      }

      ~sigpipe_guard_t() {
        const timespec now {};
        while (sigtimedwait(&pipe_, nullptr, &now) > 0) {}  // the ones its writes raised
        pthread_sigmask(SIG_SETMASK, &old_, nullptr);
      }

    private:
      sigset_t pipe_ {}, old_ {};
    };

    class helper_t {
    public:
      enum class answer_e {
        ok,  ///< *data is the content (a get's), or nothing (a set's)
        none,  ///< no text, or no PNG image, on the clipboard
        fail
      };

      /// A set sends in; a get's content comes in out.
      answer_e request(bool set, bool image, const std::string &in, std::string &out) {
        std::lock_guard lock {mutex_};
        sigpipe_guard_t guard;
        // A helper that ended since (its session changed) is replaced once, at once. One that
        // didn't answer in time isn't: a desktop that stopped answering would only hold this
        // request up as long again.
        for (int attempt = 0; attempt < 2; ++attempt) {
          if (pid_ < 0 && !start()) {
            return answer_e::fail;
          }
          bool lost = false;
          late_ = false;
          const auto answer = exchange(set, image, in, out, lost);
          if (!lost) {
            return answer;
          }
          stop();
          if (late_) {
            BOOST_LOG(warning) << "Frametop clipboard: the desktop's clipboard didn't answer in time"sv;
            return answer_e::fail;
          }
        }
        return answer_e::fail;
      }

      ~helper_t() {
        stop();
      }

    private:
      bool start() {
        if (std::chrono::steady_clock::now() < retry_at_) {
          return false;
        }
        retry_at_ = std::chrono::steady_clock::now() + RETRY_AFTER;
        int input[2] = {-1, -1}, output[2] = {-1, -1};
        if (pipe2(input, O_CLOEXEC) < 0 || pipe2(output, O_CLOEXEC) < 0) {
          for (int fd : {input[0], input[1], output[0], output[1]}) {
            if (fd >= 0) {
              close(fd);
            }
          }
          return said("couldn't make its pipes: "s + std::strerror(errno));
        }
        const bool machine = std::getenv("VIBEPOLLO_MACHINE_HOST") != nullptr;
        const std::string helper = machine ? std::string {SESSION_EXEC} : direct_helper();
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        // Nothing else of ours: the host's sockets aren't all close-on-exec (ENet's stream
        // ports, the HTTP listeners), and a helper that outlives a stream would keep them bound,
        // so the next stream couldn't bind its control port (RTSP 500, 2026-11-08).
        posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        sigset_t none;
        sigemptyset(&none);
        posix_spawnattr_setsigmask(&attributes, &none);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
        char *const machine_arguments[] = {const_cast<char *>(SESSION_EXEC), const_cast<char *>("clipboard"), nullptr};
        char *const direct_arguments[] = {const_cast<char *>(helper.c_str()), nullptr};
        const int spawned = posix_spawn(&pid_, helper.c_str(), &actions, &attributes, machine ? machine_arguments : direct_arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(input[0]);
        close(output[1]);
        to_ = input[1], from_ = output[0];
        if (spawned) {
          pid_ = -1;
          stop();
          return said("couldn't start "s + helper + ": " + std::strerror(spawned));
        }
        fcntl(to_, F_SETFL, fcntl(to_, F_GETFL) | O_NONBLOCK);
        std::string line;
        if (!read_line(line, READY_MS) || line != "ready") {
          const auto why = line.rfind("fail ", 0) == 0 ? line.substr(5) : "it didn't start (Vibepollo's log has its reason)"s;
          stop();
          return said("the desktop session's clipboard isn't available: " + why);
        }
        BOOST_LOG(info) << "Frametop clipboard: using the desktop session's clipboard"sv
                        << (machine ? " (through the session broker)"sv : ""sv);
        failing_ = false;
        retry_at_ = {};
        return true;
      }

      /// Logged once until it works again.
      bool said(const std::string &why) {
        if (!std::exchange(failing_, true)) {
          BOOST_LOG(warning) << "Frametop clipboard: "sv << why;
        }
        return false;
      }

      /// One request and its reply. *lost: the helper is gone or out of step, so it's stopped.
      answer_e exchange(bool set, bool image, const std::string &in, std::string &data, bool &lost) {
        const std::string kind = image ? "image" : "text";
        const std::string header = set ? "set " + kind + " " + std::to_string(in.size()) + "\n" : "get " + kind + "\n";
        if (!write_all(header.data(), header.size()) || (set && !write_all(in.data(), in.size()))) {
          lost = true;
          return answer_e::fail;
        }
        std::string line;
        if (!read_line(line, image ? IMAGE_ANSWER_MS : ANSWER_MS)) {
          lost = true;
          return answer_e::fail;
        }
        enum vibepollo_clipboard_reply reply;
        size_t length = 0;
        const char *why = "";
        if (!vibepollo_clipboard_parse_reply(line.c_str(), &reply, &length, &why) || (set && length)) {
          lost = true;
          return answer_e::fail;
        }
        if (reply == VIBEPOLLO_CLIPBOARD_FAIL) {
          BOOST_LOG(warning) << "Frametop clipboard: couldn't "sv << (set ? "set"sv : "read"sv) << " its "sv << kind << ": "sv << why;
          return answer_e::fail;
        }
        if (reply == VIBEPOLLO_CLIPBOARD_NONE) {
          return answer_e::none;
        }
        if (!set) {
          data.resize(length);
          if (!read_exactly(data.data(), length)) {
            data.clear();
            lost = true;
            return answer_e::fail;
          }
          BOOST_LOG(debug) << "Frametop clipboard: read its "sv << kind << " ("sv << length << " bytes)"sv;
        } else {
          BOOST_LOG(debug) << "Frametop clipboard: set its "sv << kind << " ("sv << in.size() << " bytes)"sv;
        }
        return answer_e::ok;
      }

      /// False when fd isn't ready by then (late_), or poll fails.
      bool wait_for(int fd, short events, int64_t until) {
        for (;;) {
          const auto left = until - now_ms();
          if (left <= 0) {
            late_ = true;
            return false;
          }
          pollfd p {fd, events, 0};
          const int polled = poll(&p, 1, int(left));
          if (polled > 0) {
            return true;
          }
          if (polled < 0 && errno != EINTR) {
            return false;
          }
        }
      }

      bool write_all(const char *at, size_t length) {
        const auto until = now_ms() + TRANSFER_MS;
        while (length) {
          if (!wait_for(to_, POLLOUT, until)) {
            return false;
          }
          const auto written = write(to_, at, length);
          if (written < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
          }
          if (written <= 0) {
            return false;
          }
          at += written, length -= size_t(written);
        }
        return true;
      }

      /// A reply's header line, without its "\n". Byte by byte: what follows is content.
      bool read_line(std::string &line, int within_ms) {
        line.clear();
        const auto until = now_ms() + within_ms;
        for (;;) {
          if (!wait_for(from_, POLLIN, until)) {
            return false;
          }
          char c;
          const auto got = read(from_, &c, 1);
          if (got < 0 && errno == EINTR) {
            continue;
          }
          if (got <= 0) {
            return false;
          }
          if (c == '\n') {
            return true;
          }
          if (line.size() + 1 >= VIBEPOLLO_CLIPBOARD_LINE) {
            return false;
          }
          line += c;
        }
      }

      bool read_exactly(char *at, size_t length) {
        const auto until = now_ms() + TRANSFER_MS;
        while (length) {
          if (!wait_for(from_, POLLIN, until)) {
            return false;
          }
          const auto got = read(from_, at, length);
          if (got < 0 && errno == EINTR) {
            continue;
          }
          if (got <= 0) {
            return false;
          }
          at += got, length -= size_t(got);
        }
        return true;
      }

      bool reaped(int within_ms) {
        for (int waited = 0;; waited += 20) {
          const auto done = waitpid(pid_, nullptr, WNOHANG);
          if (done == pid_ || (done < 0 && errno == ECHILD)) {
            return true;
          }
          if (waited >= within_ms) {
            return false;
          }
          const timespec pause {0, 20'000'000};
          nanosleep(&pause, nullptr);
        }
      }

      /// Its stdin closes, so it ends (and with it what it had on the clipboard); else it's
      /// stopped (session-exec's end makes the broker stop the helper).
      void stop() {
        for (int *fd : {&to_, &from_}) {
          if (*fd >= 0) {
            close(*fd);
            *fd = -1;
          }
        }
        if (pid_ > 0 && !reaped(EXIT_MS)) {
          kill(pid_, SIGTERM);
          if (!reaped(EXIT_MS)) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
          }
        }
        pid_ = -1;
      }

      std::mutex mutex_;
      bool late_ = false;  ///< the last wait_for ran out of time
      pid_t pid_ = -1;
      int to_ = -1, from_ = -1;
      bool failing_ = false;
      std::chrono::steady_clock::time_point retry_at_ {};
    };

    helper_t &helper() {
      static helper_t instance;
      return instance;
    }

    std::string get(bool image) {
      std::string data;
      return helper().request(false, image, {}, data) == helper_t::answer_e::ok ? data : std::string {};
    }

    bool set(bool image, const std::string &content) {
      if (content.empty() || content.size() > VIBEPOLLO_CLIPBOARD_MAX) {
        return false;
      }
      std::string unused;
      return helper().request(true, image, content, unused) == helper_t::answer_e::ok;
    }
  }  // namespace

  std::string get_clipboard() {
    return get(false);
  }

  bool set_clipboard(const std::string &content) {
    return set(false, content);
  }

  std::string get_clipboard_image() {
    return get(true);
  }

  bool set_clipboard_image(const std::string &png) {
    return set(true, png);
  }
}  // namespace platf
