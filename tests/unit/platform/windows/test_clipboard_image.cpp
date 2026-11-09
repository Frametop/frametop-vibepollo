/**
 * @file tests/unit/platform/windows/test_clipboard_image.cpp
 * @brief Frametop shared clipboard: src/platform/windows/image_convert.cpp's CF_DIB and PNG conversions.
 */
#include "../../../tests_common.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <src/platform/windows/image_convert.h>
  #include <Windows.h>

namespace {
  struct rgb {
    std::uint8_t r, g, b;
  };

  // A 3x2 picture, top row first.
  const rgb picture[2][3] = {
    {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}},
    {{10, 20, 30}, {200, 100, 50}, {255, 255, 255}},
  };

  // A CF_DIB of picture: 24 bits a pixel, bottom-up, rows padded to 4 bytes.
  std::string dib24() {
    BITMAPINFOHEADER header {};
    header.biSize = sizeof header;
    header.biWidth = 3;
    header.biHeight = 2;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    std::string out(reinterpret_cast<const char *>(&header), sizeof header);
    for (int y = 1; y >= 0; --y) {
      for (const rgb &p : picture[y]) {
        out += char(p.b), out += char(p.g), out += char(p.r);
      }
      out += std::string(3, '\0');  // 9 bytes a row, padded to 12
    }
    return out;
  }

  // The same as 32 bits a pixel, top-down, BI_BITFIELDS with its masks after the header.
  std::string dib32_bitfields() {
    BITMAPINFOHEADER header {};
    header.biSize = sizeof header;
    header.biWidth = 3;
    header.biHeight = -2;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_BITFIELDS;
    std::string out(reinterpret_cast<const char *>(&header), sizeof header);
    const DWORD masks[3] = {0x00FF0000, 0x0000FF00, 0x000000FF};
    out.append(reinterpret_cast<const char *>(masks), sizeof masks);
    for (const auto &row : picture) {
      for (const rgb &p : row) {
        out += char(p.b), out += char(p.g), out += char(p.r), out += char(0);
      }
    }
    return out;
  }

  bool is_png(const std::string &data) {
    return data.size() > 8 && std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
  }

  // clipboard_dib_from_png's DIB matches picture: 32 bits a pixel, bottom-up, opaque.
  void expect_picture(const std::string &dib) {
    ASSERT_GE(dib.size(), sizeof(BITMAPINFOHEADER) + 3 * 2 * 4);
    BITMAPINFOHEADER header {};
    std::memcpy(&header, dib.data(), sizeof header);
    EXPECT_EQ(header.biWidth, 3);
    EXPECT_EQ(header.biHeight, 2);
    EXPECT_EQ(header.biBitCount, 32);
    EXPECT_EQ(header.biCompression, DWORD(BI_RGB));
    EXPECT_EQ(dib.size(), sizeof header + 3 * 2 * 4);
    const auto *pixels = reinterpret_cast<const std::uint8_t *>(dib.data() + sizeof header);
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 3; ++x) {
        const std::uint8_t *p = pixels + ((1 - y) * 3 + x) * 4;  // bottom row first
        EXPECT_EQ(p[0], picture[y][x].b) << x << "," << y;
        EXPECT_EQ(p[1], picture[y][x].g) << x << "," << y;
        EXPECT_EQ(p[2], picture[y][x].r) << x << "," << y;
        EXPECT_EQ(p[3], 255) << x << "," << y;
      }
    }
  }
}  // namespace

TEST(ClipboardImage, BottomUpDibRoundTrips) {
  const std::string dib = dib24();
  std::string png;
  ASSERT_TRUE(platf::img::png_from_clipboard_dib(dib.data(), dib.size(), png));
  EXPECT_TRUE(is_png(png));
  std::string back;
  ASSERT_TRUE(platf::img::clipboard_dib_from_png(png, back));
  expect_picture(back);
}

TEST(ClipboardImage, TopDownBitfieldsDibRoundTrips) {
  const std::string dib = dib32_bitfields();
  std::string png;
  ASSERT_TRUE(platf::img::png_from_clipboard_dib(dib.data(), dib.size(), png));
  EXPECT_TRUE(is_png(png));
  std::string back;
  ASSERT_TRUE(platf::img::clipboard_dib_from_png(png, back));
  expect_picture(back);
}

TEST(ClipboardImage, RefusesWhatIsNoBitmap) {
  std::string png;
  const std::string dib = dib24();
  EXPECT_FALSE(platf::img::png_from_clipboard_dib(dib.data(), 10, png));  // shorter than its header
  std::string bad = dib;
  reinterpret_cast<BITMAPINFOHEADER *>(bad.data())->biSize = DWORD(bad.size() + 1);
  EXPECT_FALSE(platf::img::png_from_clipboard_dib(bad.data(), bad.size(), png));
  const std::string header_only = dib.substr(0, sizeof(BITMAPINFOHEADER));
  EXPECT_FALSE(platf::img::png_from_clipboard_dib(header_only.data(), header_only.size(), png));
}

TEST(ClipboardImage, RefusesTooBig) {
  // A header that says 20000 pixels wide: over the 16384 a side, whatever its data.
  std::string dib = dib24();
  reinterpret_cast<BITMAPINFOHEADER *>(dib.data())->biWidth = 20000;
  dib += std::string(20000 * 3 * 2, '\0');
  std::string png;
  EXPECT_FALSE(platf::img::png_from_clipboard_dib(dib.data(), dib.size(), png));
}

TEST(ClipboardImage, RefusesWhatIsNoPng) {
  std::string dib;
  EXPECT_FALSE(platf::img::clipboard_dib_from_png("GIF89a not a png at all", dib));
  EXPECT_FALSE(platf::img::clipboard_dib_from_png(std::string("\x89PNG\r\n\x1a\n", 8) + "broken", dib));
}
#endif
