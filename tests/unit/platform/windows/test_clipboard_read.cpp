/**
 * @file tests/unit/platform/windows/test_clipboard_read.cpp
 * @brief Frametop shared clipboard: src/platform/windows/clipboard_read.cpp on this process's clipboard.
 */
#include "../../../tests_common.h"

#include <cstring>
#include <string>

#ifdef _WIN32
  #include <src/platform/windows/clipboard_read.h>
  #include <src/platform/windows/image_convert.h>
  #include <Windows.h>

namespace {
  using platf::clipboard_read::read;
  using platf::clipboard_read::result;

  HGLOBAL global_of(const void *data, std::size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    void *bytes = memory ? GlobalLock(memory) : nullptr;
    if (bytes) {
      std::memcpy(bytes, data, size);
      GlobalUnlock(memory);
    }
    return memory;
  }

  struct format_t {
    UINT format;
    std::string data;
  };

  // Puts the formats on the clipboard, as an app would. False when this session has no
  // clipboard to use (a service session, say).
  bool put(std::initializer_list<format_t> formats) {
    for (int tries = 0; tries < 20; ++tries) {
      if (OpenClipboard(nullptr)) {
        bool ok = EmptyClipboard() != FALSE;
        for (const auto &f : formats) {
          HGLOBAL memory = global_of(f.data.data(), f.data.size());
          if (!memory || !SetClipboardData(f.format, memory)) {
            ok = false;
            if (memory) {
              GlobalFree(memory);
            }
          }
        }
        CloseClipboard();
        return ok;
      }
      Sleep(50);
    }
    return false;
  }

  std::string wide(const wchar_t *text) {
    return std::string(reinterpret_cast<const char *>(text), (std::wcslen(text) + 1) * sizeof(wchar_t));
  }

  // A 2x1 CF_DIB, 32 bits a pixel, bottom-up.
  std::string dib() {
    BITMAPINFOHEADER header {};
    header.biSize = sizeof header;
    header.biWidth = 2;
    header.biHeight = 1;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::string out(reinterpret_cast<const char *>(&header), sizeof header);
    const unsigned char pixels[8] = {0, 0, 255, 0, 255, 0, 0, 0};
    out.append(reinterpret_cast<const char *>(pixels), sizeof pixels);
    return out;
  }

  UINT registered(const wchar_t *name) {
    return RegisterClipboardFormatW(name);
  }

  #define NEEDS_CLIPBOARD(...) \
    if (!put(__VA_ARGS__)) GTEST_SKIP() << "no clipboard in this session"
}  // namespace

TEST(FrametopClipboardRead, TextAsUtf8WithItsLineEnds) {
  NEEDS_CLIPBOARD({{CF_UNICODETEXT, wide(L"one\r\ntwo é")}});
  std::string out;
  EXPECT_EQ(read(false, out), result::ok);
  EXPECT_EQ(out, "one\r\ntwo \xc3\xa9");
  EXPECT_EQ(read(true, out), result::ok);
  EXPECT_TRUE(out.empty());
}

TEST(FrametopClipboardRead, TextWithoutItsTerminator) {
  const std::string unterminated(reinterpret_cast<const char *>(L"abc"), 3 * sizeof(wchar_t));
  NEEDS_CLIPBOARD({{CF_UNICODETEXT, unterminated}});
  std::string out;
  // Windows ends clipboard text within its memory (here "ab"): nothing past it is read.
  EXPECT_EQ(read(false, out), result::ok);
  EXPECT_LE(out.size(), 3u);
  EXPECT_EQ(std::string("abc").substr(0, out.size()), out);
}

TEST(FrametopClipboardRead, PasswordManagersCopyIsNotRead) {
  const UINT secret = registered(L"ExcludeClipboardContentFromMonitorProcessing");
  NEEDS_CLIPBOARD({{CF_UNICODETEXT, wide(L"hunter2")}, {secret, std::string(4, '\0')}});
  std::string out = "left";
  EXPECT_EQ(read(false, out), result::secret);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(read(true, out), result::secret);
}

TEST(FrametopClipboardRead, MarkFirstIsFoundToo) {
  const UINT secret = registered(L"ExcludeClipboardContentFromMonitorProcessing");
  NEEDS_CLIPBOARD({{secret, std::string(4, '\0')}, {registered(L"Frametop test"), "x"}, {CF_UNICODETEXT, wide(L"hunter2")}});
  std::string out;
  EXPECT_EQ(read(false, out), result::secret);
}

TEST(FrametopClipboardRead, AppsOwnPngComesAsItIs) {
  std::string png;
  ASSERT_TRUE(platf::img::png_from_clipboard_dib(dib().data(), dib().size(), png));
  NEEDS_CLIPBOARD({{registered(L"PNG"), png}, {CF_DIB, dib()}});
  std::string out;
  EXPECT_EQ(read(true, out), result::ok);
  EXPECT_EQ(out, png);
}

TEST(FrametopClipboardRead, BitmapOnlyIsMadeAPng) {
  NEEDS_CLIPBOARD({{CF_DIB, dib()}});
  std::string out;
  std::string expected;
  ASSERT_TRUE(platf::img::png_from_clipboard_dib(dib().data(), dib().size(), expected));
  EXPECT_EQ(read(true, out), result::ok);
  EXPECT_EQ(out, expected);
}

TEST(FrametopClipboardRead, NotAPngInThePngFormatFallsBackToTheBitmap) {
  NEEDS_CLIPBOARD({{registered(L"PNG"), "not a png at all"}, {CF_DIB, dib()}});
  std::string out;
  EXPECT_EQ(read(true, out), result::ok);
  ASSERT_GT(out.size(), 8u);
  EXPECT_EQ(out.substr(1, 3), "PNG");
}

TEST(FrametopClipboardRead, EmptyClipboardReadsAsNothing) {
  NEEDS_CLIPBOARD({});
  std::string out = "left";
  EXPECT_EQ(read(false, out), result::ok);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(read(true, out), result::ok);
  EXPECT_TRUE(out.empty());
}

TEST(FrametopClipboardRead, ExitCodesAreDistinct) {
  using platf::clipboard_read::exit_code;
  EXPECT_EQ(exit_code(result::ok), 0);
  EXPECT_EQ(exit_code(result::secret), 3);
  EXPECT_EQ(exit_code(result::busy), 4);
  EXPECT_EQ(exit_code(result::failed), 5);
}
#endif
