/**
 * @file src/platform/windows/clipboard_read.cpp
 * @brief Frametop shared clipboard: reading the Windows clipboard's text or image.
 */
#include "clipboard_read.h"

#include "image_convert.h"

#include <cstring>

#include <Windows.h>

namespace platf::clipboard_read {
  namespace {
    /// Password managers put this format next to a password, for clipboard history and
    /// clipboard managers to leave it alone. Asked with the clipboard open, so a copy can't come
    /// in between the check and the read. Listed as well as asked for, in case a process can
    /// list a format it can't look up.
    bool is_secret() {
      static const UINT secret = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
      if (!secret) {
        return false;
      }
      if (IsClipboardFormatAvailable(secret)) {
        return true;
      }
      for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
        if (format == secret) {
          return true;
        }
      }
      return false;
    }

    bool is_png(const std::string &data) {
      static constexpr unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      return data.size() > sizeof signature && !std::memcmp(data.data(), signature, sizeof signature);
    }

    /// A format's data on the open clipboard, empty when it has none or it's too big.
    std::string bytes_of(UINT format) {
      HANDLE data = format && IsClipboardFormatAvailable(format) ? GetClipboardData(format) : nullptr;
      const SIZE_T size = data ? GlobalSize(data) : 0;
      if (!size || size > max_bytes) {
        return {};
      }
      const void *bytes = GlobalLock(data);
      if (!bytes) {
        return {};
      }
      std::string out {static_cast<const char *>(bytes), size};
      GlobalUnlock(data);
      return out;
    }

    /// The open clipboard's text as UTF-8, empty when it has none or it's too big.
    std::string text() {
      HANDLE data = IsClipboardFormatAvailable(CF_UNICODETEXT) ? GetClipboardData(CF_UNICODETEXT) : nullptr;
      const SIZE_T size = data ? GlobalSize(data) : 0;
      const auto *wide = size ? static_cast<const wchar_t *>(GlobalLock(data)) : nullptr;
      if (!wide) {
        return {};
      }
      // Up to its terminator, which the memory may lack.
      const std::size_t most = size / sizeof(wchar_t);
      std::size_t length = 0;
      while (length < most && wide[length]) {
        ++length;
      }
      std::string out;
      if (length && length <= max_bytes) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (needed > 0 && static_cast<std::size_t>(needed) <= max_bytes) {
          out.resize(static_cast<std::size_t>(needed));
          WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), out.data(), needed, nullptr, nullptr);
        }
      }
      GlobalUnlock(data);
      return out;
    }
  }  // namespace

  result read(bool image, std::string &out) {
    out.clear();
    if (!OpenClipboard(nullptr)) {
      return result::busy;
    }
    if (is_secret()) {
      CloseClipboard();
      return result::secret;
    }
    if (!image) {
      out = text();
      CloseClipboard();
      return result::ok;
    }
    // The app's own PNG keeps transparency; else the bitmap every image copy has (Windows makes
    // a CF_DIB from a CF_BITMAP or CF_DIBV5).
    static const UINT png_format = RegisterClipboardFormatW(L"PNG");
    std::string png = bytes_of(png_format);
    std::string dib;
    if (!is_png(png)) {
      png.clear();
      dib = bytes_of(CF_DIB);
    }
    CloseClipboard();
    if (png.empty() && !dib.empty() && !img::png_from_clipboard_dib(dib.data(), dib.size(), png)) {
      return result::failed;
    }
    if (png.size() > max_bytes) {
      png.clear();
    }
    out = std::move(png);
    return result::ok;
  }
}  // namespace platf::clipboard_read
