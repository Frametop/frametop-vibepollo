/**
 * @file src/platform/linux/frametop_together_helper.cpp
 * @brief Frametop Together: ft-pcsend run as this host's helper.
 */
#include "frametop_together_helper.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

using namespace std::literals;

namespace platf::frametop_together {
  namespace {
    constexpr auto HELPER = "/usr/libexec/vibeshine/ft-pcsend";
    constexpr auto CODE_VARIABLE = "FT_PCSEND_CODE="sv;
    /// For it to take one picture. It reads every few milliseconds, so one that waits this long
    /// has stopped reading.
    constexpr auto FEED_FOR = 1000ms;
    /// For it to end once its input closed, then once it was told to, before it is killed.
    constexpr auto EXIT_FOR = 1000ms;
    /// The longest line it prints is far shorter. More without a newline isn't its output.
    constexpr std::size_t MAX_LINE = 4096;

    /// Next to this program's own folder's libexec (a package's /usr/bin and /usr/libexec, or a
    /// build's install tree), else the package's.
    std::string helper_path() {
      char exe[PATH_MAX];
      const auto n = readlink("/proc/self/exe", exe, sizeof exe - 1);
      if (n > 0) {
        std::string path {exe, static_cast<std::size_t>(n)};
        path = path.substr(0, path.rfind('/'));
        path = path.substr(0, path.rfind('/')) + "/libexec/vibeshine/ft-pcsend";
        if (access(path.c_str(), X_OK) == 0) {
          return path;
        }
      }
      return HELPER;
    }

    /// Whether the process ended within the time.
    bool ended_within(pid_t pid, std::chrono::milliseconds time) {
      const auto until = std::chrono::steady_clock::now() + time;
      for (;;) {
        if (waitpid(pid, nullptr, WNOHANG) != 0) {
          return true;  // (it ended, or it was never ours to wait for)
        }
        if (std::chrono::steady_clock::now() >= until) {
          return false;
        }
        std::this_thread::sleep_for(10ms);
      }
    }
  }  // namespace

  std::unique_ptr<helper_t> helper_t::start(const std::string &address, int port, const std::string &code, std::string *error) {
    const auto helper = helper_path();
    if (access(helper.c_str(), X_OK) != 0) {
      *error = "this Vibepollo has no ft-pcsend (" + helper + ")";
      return nullptr;
    }
    // A socket, not a pipe, for the pictures: a send to a helper that ended is an error here,
    // not a SIGPIPE for the whole host.
    int input[2] = {-1, -1}, output[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input) < 0 || pipe2(output, O_CLOEXEC) < 0) {
      *error = "couldn't make ft-pcsend's pipes: "s + std::strerror(errno);
      for (int fd : {input[0], input[1], output[0], output[1]}) {
        if (fd >= 0) {
          close(fd);
        }
      }
      return nullptr;
    }

    // The code goes in its environment: a command line is there for every user of the PC to read.
    std::vector<std::string> variables;
    for (char **variable = environ; *variable; ++variable) {
      if (!std::string_view {*variable}.starts_with(CODE_VARIABLE)) {
        variables.emplace_back(*variable);
      }
    }
    variables.push_back(std::string {CODE_VARIABLE} + code);
    std::vector<char *> environment;
    for (auto &variable : variables) {
      environment.push_back(variable.data());
    }
    environment.push_back(nullptr);
    auto port_text = std::to_string(port);
    auto address_text = address;
    char *const arguments[] = {const_cast<char *>(helper.c_str()), address_text.data(), port_text.data(), const_cast<char *>("-"), const_cast<char *>("--feed"), nullptr};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    // Nothing else of ours: the host's sockets aren't all close-on-exec, and a helper that
    // outlives a stream would keep its ports bound.
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, helper.c_str(), &actions, &attributes, arguments, environment.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input[0]);
    close(output[1]);
    if (spawned) {
      close(input[1]);
      close(output[0]);
      *error = "couldn't start " + helper + ": " + std::strerror(spawned);
      return nullptr;
    }
    std::unique_ptr<helper_t> started {new helper_t};
    started->pid_ = pid;
    started->to_ = input[1];
    started->from_ = output[0];
    fcntl(started->from_, F_SETFL, fcntl(started->from_, F_GETFL) | O_NONBLOCK);
    return started;
  }

  helper_t::~helper_t() {
    stop();
    if (from_ >= 0) {
      close(from_);
    }
  }

  bool helper_t::feed(std::string_view record) {
    const auto until = std::chrono::steady_clock::now() + FEED_FOR;
    while (to_ >= 0 && !record.empty()) {
      const auto sent = send(to_, record.data(), record.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
      if (sent > 0) {
        record.remove_prefix(static_cast<std::size_t>(sent));
        continue;
      }
      const bool may_wait = errno == EAGAIN || errno == EINTR;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
      pollfd waiting {to_, POLLOUT, 0};
      if (!may_wait || left <= 0ms || (poll(&waiting, 1, static_cast<int>(left.count())) < 0 && errno != EINTR)) {
        // Part of a picture may be in: what follows would be read as the rest of it.
        close(to_);
        to_ = -1;
      }
    }
    return to_ >= 0;
  }

  bool helper_t::lines(std::vector<std::string> &out, int wait_ms) {
    if (from_ < 0) {
      return false;
    }
    pollfd waiting {from_, POLLIN, 0};
    if (poll(&waiting, 1, wait_ms) <= 0) {
      return true;
    }
    char buffer[4096];
    for (;;) {
      const auto n = read(from_, buffer, sizeof buffer);
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        break;
      }
      if (n <= 0) {
        close(from_);
        from_ = -1;
        break;
      }
      unread_.append(buffer, static_cast<std::size_t>(n));
    }
    for (auto end = unread_.find('\n'); end != std::string::npos; end = unread_.find('\n')) {
      out.push_back(unread_.substr(0, end));
      unread_.erase(0, end + 1);
    }
    if (unread_.size() > MAX_LINE) {
      unread_.clear();
    }
    return from_ >= 0 || !out.empty();
  }

  void helper_t::stop() {
    if (to_ >= 0) {
      close(to_);
      to_ = -1;
    }
    if (pid_ < 0) {
      return;
    }
    if (!ended_within(pid_, EXIT_FOR)) {
      kill(pid_, SIGTERM);
      if (!ended_within(pid_, EXIT_FOR)) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
      }
    }
    pid_ = -1;
  }
}  // namespace platf::frametop_together
