/**
 * @file src/platform/linux/frametop_together_helper.h
 * @brief Frametop Together: ft-pcsend run as this host's helper.
 *
 * ft-pcsend (Frametop's together/ft-pcsend.cpp) is this PC's end of a Frametop Together room:
 * it calls the headset and sends it the pictures this host hands over. It is run with the
 * one-time code in its environment, never on its command line, where other users of the PC
 * could read it. Pictures go to its standard input, its lines come from its standard output
 * (src/frametop_together_wire.h).
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace platf::frametop_together {
  class helper_t {
  public:
    /// Starts ft-pcsend to call address:port with the code. nullptr with *error set when it
    /// couldn't be started.
    static std::unique_ptr<helper_t> start(const std::string &address, int port, const std::string &code, std::string *error);

    /// Ends it.
    ~helper_t();
    helper_t(const helper_t &) = delete;
    helper_t &operator=(const helper_t &) = delete;

    /// One record to its standard input, whole. False when it didn't take it in time or is
    /// gone: nothing more is fed then, and it ends. One thread at a time may call this or stop.
    bool feed(std::string_view record);

    /// Adds the lines it printed since, waiting up to wait_ms for one. False once it has ended
    /// and every line was read. For one thread, which may be another than feed's.
    bool lines(std::vector<std::string> &out, int wait_ms);

    /// Closes its standard input, which ends it, and sees that it does.
    void stop();

  private:
    helper_t() = default;

    pid_t pid_ = -1;
    int to_ = -1;
    int from_ = -1;
    std::string unread_;
  };
}  // namespace platf::frametop_together
