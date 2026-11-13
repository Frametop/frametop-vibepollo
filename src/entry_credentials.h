/**
 * @file entry_credentials.h
 * @brief Command-line argument routing for credential updates.
 */
#pragma once

#include <istream>
#include <optional>
#include <string>
#include <string_view>

namespace args {
  /**
   * @brief The password a `--creds` call gives.
   *
   * The operand itself, or for "-" the first line of `input`: a script passes
   * the password that way so it is in no process list. Nothing when that line
   * is missing or empty.
   */
  inline std::optional<std::string> password_operand(std::string_view operand, std::istream &input) {
    if (operand != "-") {
      return std::string {operand};
    }
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return std::nullopt;
    }
    return line;
  }

  /**
   * @brief Dispatch the production credential CLI path through its callbacks.
   *
   * Keeping the callbacks injectable lets the argument contract be exercised
   * without opening or modifying the credential store.
   */
  template<typename ShowHelp, typename SaveCredentials>
  int creds(
    const char *name,
    int argc,
    char *argv[],
    ShowHelp show_help,
    SaveCredentials save_credentials
  ) {
    if (
      argc < 2 || !argv || !argv[0] || !argv[1] ||
      std::string_view {argv[0]} == "help" || std::string_view {argv[1]} == "help"
    ) {
      return show_help(name);
    }

    return save_credentials(argv[0], argv[1]);
  }
}  // namespace args
