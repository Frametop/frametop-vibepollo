/**
 * @file src/platform/windows/frametop_hostin.h
 * @brief Frametop host input on Windows: the capture service, in the process (see
 *        frametop_hostin.cpp next to this).
 */
#pragma once

#include "src/frametop_hostin_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace platf::frametop_hostin {
  struct callbacks_t {
    /// A message for the manager (manager_t::service_message), on the capture thread.
    std::function<void(std::uint64_t connection, std::string_view message)> message;
    /// The capture thread stopped (manager_t::service_gone).
    std::function<void(std::uint64_t connection)> gone;
    std::function<void(const std::string &)> log;
  };

  /// Takes this computer's keyboard and mouse with low-level hooks while a client holds a lease,
  /// when config::input.frametop_host_input allows it.
  std::unique_ptr<::frametop_hostin::manager_t::service_t> make_service(callbacks_t callbacks);
}  // namespace platf::frametop_hostin
