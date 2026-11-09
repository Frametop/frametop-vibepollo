/**
 * @file tools/frametop_clipboard.cpp
 * @brief Frametop shared clipboard: reads the signed-in user's clipboard for the host.
 *
 *   frametop_clipboard.exe text|image
 *
 * Writes the clipboard's text (UTF-8) or its image (a PNG file) to stdout, nothing when it holds
 * none, and exits with platf::clipboard_read::exit_code: 0 read, 3 a password manager's copy
 * (nothing written), 4 another app has the clipboard open, 5 a bitmap that couldn't be made a
 * PNG, 1 a bad command line. The host runs it as the user (no window) for each read: started
 * by its service, the host itself can't see the formats other apps register.
 */
#include "src/platform/windows/clipboard_read.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>

#include <Windows.h>

int main(int argc, char **argv) {
  if (argc != 2 || (std::strcmp(argv[1], "text") && std::strcmp(argv[1], "image"))) {
    std::fputs("usage: frametop_clipboard text|image\n", stderr);
    return 1;
  }
  std::string out;
  const auto result = platf::clipboard_read::read(!std::strcmp(argv[1], "image"), out);
  if (result == platf::clipboard_read::result::ok && !out.empty()) {
    _setmode(_fileno(stdout), _O_BINARY);
    if (std::fwrite(out.data(), 1, out.size(), stdout) != out.size() || std::fflush(stdout)) {
      return 5;
    }
  }
  return platf::clipboard_read::exit_code(result);
}
