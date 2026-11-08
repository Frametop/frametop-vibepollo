// WIC-based image conversion to PNG 96 DPI

#include "image_convert.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include <objbase.h>
#include <wincodec.h>
#include <Windows.h>
#include <winrt/base.h>

namespace platf::img {

  struct CoInitGuard {
    bool inited = false;

    CoInitGuard() {
      inited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    }

    ~CoInitGuard() {
      if (inited) {
        CoUninitialize();
      }
    }
  };

  bool convert_to_png_96dpi(const std::wstring &src_path, const std::wstring &dst_path) {
    CoInitGuard co;
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(src_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) {
      return false;
    }

    // Convert to a well-supported pixel format if needed
    GUID pf = GUID_WICPixelFormat32bppPBGRA;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) {
      return false;
    }
    if (FAILED(converter->Initialize(frame.get(), pf, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return false;
    }

    // Create output stream and encoder
    winrt::com_ptr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put()))) {
      return false;
    }
    if (FAILED(stream->InitializeFromFilename(dst_path.c_str(), GENERIC_WRITE))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))) {
      return false;
    }
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapFrameEncode> fenc;
    winrt::com_ptr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(fenc.put(), props.put()))) {
      return false;
    }
    if (FAILED(fenc->Initialize(props.get()))) {
      return false;
    }

    // Set 96 DPI to avoid scaling issues
    if (FAILED(fenc->SetResolution(96.0, 96.0))) {
      return false;
    }

    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h))) {
      return false;
    }
    if (FAILED(fenc->SetSize(w, h))) {
      return false;
    }
    WICPixelFormatGUID outFmt = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(fenc->SetPixelFormat(&outFmt))) {
      return false;
    }

    // Write pixels with no resampling (preserve pixel dimensions)
    const UINT stride = w * 4;
    const UINT bufSize = stride * h;
    std::unique_ptr<BYTE[]> buffer = std::make_unique<BYTE[]>(bufSize);
    if (FAILED(converter->CopyPixels(nullptr, stride, bufSize, buffer.get()))) {
      return false;
    }
    if (FAILED(fenc->WritePixels(h, stride, bufSize, buffer.get()))) {
      return false;
    }
    if (FAILED(fenc->Commit())) {
      return false;
    }
    if (FAILED(encoder->Commit())) {
      return false;
    }
    return true;
  }

  namespace {
    namespace fs = std::filesystem;

    // Wide resource-type ids (the RT_GROUP_ICON / RT_ICON macros resolve to the ANSI form
    // unless UNICODE is defined for this translation unit).
    const LPCWSTR kRtGroupIconW = MAKEINTRESOURCEW(14);
    const LPCWSTR kRtIconW = MAKEINTRESOURCEW(3);

    // Encode a WIC bitmap source to a 96 DPI PNG (straight alpha, 32bpp BGRA).
    bool encode_source_to_png(IWICImagingFactory *factory, IWICBitmapSource *src, const std::wstring &dst_path) {
      UINT w = 0, h = 0;
      if (FAILED(src->GetSize(&w, &h)) || w == 0 || h == 0) {
        return false;
      }
      winrt::com_ptr<IWICStream> stream;
      if (FAILED(factory->CreateStream(stream.put()))) {
        return false;
      }
      if (FAILED(stream->InitializeFromFilename(dst_path.c_str(), GENERIC_WRITE))) {
        return false;
      }
      winrt::com_ptr<IWICBitmapEncoder> encoder;
      if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))) {
        return false;
      }
      if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        return false;
      }
      winrt::com_ptr<IWICBitmapFrameEncode> fenc;
      winrt::com_ptr<IPropertyBag2> props;
      if (FAILED(encoder->CreateNewFrame(fenc.put(), props.put()))) {
        return false;
      }
      if (FAILED(fenc->Initialize(props.get()))) {
        return false;
      }
      fenc->SetResolution(96.0, 96.0);
      if (FAILED(fenc->SetSize(w, h))) {
        return false;
      }
      WICPixelFormatGUID outFmt = GUID_WICPixelFormat32bppBGRA;
      if (FAILED(fenc->SetPixelFormat(&outFmt))) {
        return false;
      }
      if (FAILED(fenc->WriteSource(src, nullptr))) {
        return false;
      }
      if (FAILED(fenc->Commit())) {
        return false;
      }
      if (FAILED(encoder->Commit())) {
        return false;
      }
      return true;
    }

    bool encode_hicon_to_png(HICON hicon, const std::wstring &dst_path) {
      CoInitGuard co;
      winrt::com_ptr<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) {
        return false;
      }
      winrt::com_ptr<IWICBitmap> bmp;
      if (FAILED(factory->CreateBitmapFromHICON(hicon, bmp.put()))) {
        return false;
      }
      // Normalize to straight-alpha BGRA so the PNG keeps a clean transparent background.
      winrt::com_ptr<IWICFormatConverter> conv;
      if (FAILED(factory->CreateFormatConverter(conv.put()))) {
        return false;
      }
      if (FAILED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
      }
      return encode_source_to_png(factory.get(), conv.get(), dst_path);
    }

    // Walk the RT_GROUP_ICON directories, tracking the largest icon and its RT_ICON id.
    struct IconScan {
      int best_dim = 0;  ///< Largest icon dimension found (px). 0 == none.
      WORD best_id = 0;  ///< RT_ICON resource id for the largest icon.
    };

    void scan_icon_group(HMODULE mod, LPCWSTR name, IconScan &scan) {
      HRSRC res = FindResourceW(mod, name, kRtGroupIconW);
      if (!res) {
        return;
      }
      HGLOBAL glob = LoadResource(mod, res);
      if (!glob) {
        return;
      }
      const BYTE *data = static_cast<const BYTE *>(LockResource(glob));
      const DWORD size = SizeofResource(mod, res);
      if (!data || size < 6) {
        return;
      }
      // GRPICONDIR: WORD reserved, WORD type, WORD count, then count * GRPICONDIRENTRY (14 bytes).
      const WORD count = *reinterpret_cast<const WORD *>(data + 4);
      const size_t needed = 6 + static_cast<size_t>(count) * 14;
      if (size < needed) {
        return;
      }
      for (WORD i = 0; i < count; ++i) {
        const BYTE *entry = data + 6 + static_cast<size_t>(i) * 14;
        int width = entry[0];  // 0 encodes 256
        int height = entry[1];
        if (width == 0) {
          width = 256;
        }
        if (height == 0) {
          height = 256;
        }
        const int dim = std::max(width, height);
        const WORD id = *reinterpret_cast<const WORD *>(entry + 12);
        if (dim > scan.best_dim) {
          scan.best_dim = dim;
          scan.best_id = id;
        }
      }
    }

    BOOL CALLBACK enum_icon_group_cb(HMODULE mod, LPCWSTR /*type*/, LPWSTR name, LONG_PTR param) {
      scan_icon_group(mod, name, *reinterpret_cast<IconScan *>(param));
      return TRUE;
    }

    IconScan scan_executable_icons(const std::wstring &exe_path) {
      IconScan scan;
      HMODULE mod = LoadLibraryExW(exe_path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
      if (!mod) {
        return scan;
      }
      EnumResourceNamesW(mod, kRtGroupIconW, enum_icon_group_cb, reinterpret_cast<LONG_PTR>(&scan));
      FreeLibrary(mod);
      return scan;
    }

    std::wstring to_lower(std::wstring s) {
      std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
      });
      return s;
    }

    bool has_exe_extension(const fs::path &p) {
      return to_lower(p.extension().wstring()) == L".exe";
    }

    // Store/front-end launchers whose icon is *not* the game's branding.
    bool is_launcher_exe(const std::wstring &filename_lower) {
      static const std::array<std::wstring_view, 22> kLaunchers = {
        L"steam.exe", L"steamservice.exe", L"steamwebhelper.exe",
        L"epicgameslauncher.exe", L"galaxyclient.exe", L"goggalaxy.exe",
        L"origin.exe", L"eadesktop.exe", L"easteamproxy.exe", L"link2ea.exe",
        L"battle.net.exe", L"battlenet.exe", L"upc.exe", L"uplay.exe",
        L"ubisoftconnect.exe", L"ubisoftgamelauncher.exe", L"bethesdanetlauncher.exe",
        L"itch.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe",
        L"cmd.exe", L"explorer.exe"
      };
      for (const auto &name : kLaunchers) {
        if (filename_lower == name) {
          return true;
        }
      }
      return false;
    }

    // Redistributables / crash handlers / installers bundled next to a game.
    bool is_helper_exe(const std::wstring &filename_lower) {
      static const std::array<std::wstring_view, 18> kHelpers = {
        L"crashreport", L"crashhandler", L"crashpad", L"unitycrashhandler",
        L"ueprereq", L"prereq", L"vcredist", L"vc_redist", L"dxsetup",
        L"directx", L"redist", L"unins", L"uninstall", L"dotnet",
        L"oalinst", L"easyanticheat", L"anticheat", L"touchup"
      };
      for (const auto &needle : kHelpers) {
        if (filename_lower.find(needle) != std::wstring::npos) {
          return true;
        }
      }
      return false;
    }

    // Locate the executable whose icon best represents a game: the explicit play-action exe when
    // usable, otherwise the largest plausible .exe inside the install directory. The directory walk
    // is deliberately robust -- a single unreadable entry (reparse point, locked file) must skip
    // only that directory, never abort the whole scan -- and tightly bounded so it stays cheap even
    // for very large game installs.
    fs::path find_primary_executable(const std::wstring &exe_hint, const std::wstring &install_dir) {
      std::error_code ec;
      if (!exe_hint.empty()) {
        fs::path candidate(exe_hint);
        if (has_exe_extension(candidate) && fs::is_regular_file(candidate, ec) &&
            !is_launcher_exe(to_lower(candidate.filename().wstring()))) {
          return candidate;
        }
      }

      if (install_dir.empty()) {
        return {};
      }
      fs::path root(install_dir);
      if (!fs::is_directory(root, ec)) {
        return {};
      }

      fs::path best;
      uintmax_t best_size = 0;
      int scanned = 0;
      constexpr int kMaxEntries = 3000;
      constexpr int kMaxDepth = 3;

      // Breadth-first so shallow directories (where the main executable usually lives) are scanned
      // first; the per-directory iterator isolates errors to a single directory.
      std::deque<std::pair<fs::path, int>> queue;
      queue.emplace_back(root, 0);
      while (!queue.empty() && scanned < kMaxEntries) {
        const auto [dir, depth] = queue.front();
        queue.pop_front();
        std::error_code dir_ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, dir_ec);
        const fs::directory_iterator end;
        for (; it != end; it.increment(dir_ec)) {
          if (dir_ec) {
            break;  // give up on this directory only, keep processing the queue
          }
          if (++scanned >= kMaxEntries) {
            break;
          }
          std::error_code entry_ec;
          const fs::directory_entry &entry = *it;
          if (entry.is_directory(entry_ec)) {
            if (!entry_ec && depth + 1 < kMaxDepth) {
              queue.emplace_back(entry.path(), depth + 1);
            }
            continue;
          }
          if (!entry.is_regular_file(entry_ec) || entry_ec) {
            continue;
          }
          const fs::path &p = entry.path();
          if (!has_exe_extension(p)) {
            continue;
          }
          const std::wstring fn = to_lower(p.filename().wstring());
          if (is_launcher_exe(fn) || is_helper_exe(fn)) {
            continue;
          }
          const uintmax_t sz = entry.file_size(entry_ec);
          if (entry_ec) {
            continue;
          }
          if (sz > best_size) {
            best_size = sz;
            best = p;
          }
        }
      }
      return best;
    }
  }  // namespace

  int probe_largest_icon_size(const std::wstring &exe_path) {
    return scan_executable_icons(exe_path).best_dim;
  }

  bool extract_largest_icon_png(const std::wstring &exe_path, const std::wstring &dst_path, int &out_native_size) {
    out_native_size = 0;
    HMODULE mod = LoadLibraryExW(exe_path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) {
      return false;
    }
    IconScan scan;
    EnumResourceNamesW(mod, kRtGroupIconW, enum_icon_group_cb, reinterpret_cast<LONG_PTR>(&scan));
    if (scan.best_dim == 0 || scan.best_id == 0) {
      FreeLibrary(mod);
      return false;
    }

    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(scan.best_id), kRtIconW);
    HGLOBAL glob = res ? LoadResource(mod, res) : nullptr;
    const BYTE *bytes = glob ? static_cast<const BYTE *>(LockResource(glob)) : nullptr;
    const DWORD bytes_size = res ? SizeofResource(mod, res) : 0;
    HICON hicon = nullptr;
    if (bytes && bytes_size) {
      // 0x00030000 selects the standard icon resource version. Passing the selected
      // resource dimension avoids Windows shrinking the icon to the default 32px system size.
      hicon = CreateIconFromResourceEx(const_cast<PBYTE>(bytes), bytes_size, TRUE, 0x00030000, scan.best_dim, scan.best_dim, LR_DEFAULTCOLOR);
    }
    FreeLibrary(mod);  // HICON keeps its own copy of the bits.
    if (!hicon) {
      return false;
    }

    const bool ok = encode_hicon_to_png(hicon, dst_path);
    DestroyIcon(hicon);
    if (ok) {
      out_native_size = scan.best_dim;
    }
    return ok;
  }

  int image_pixel_width(const std::wstring &src_path) {
    CoInitGuard co;
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) {
      return 0;
    }
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(src_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
      return 0;
    }
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0) {
      return 0;
    }
    int max_width = 0;
    for (UINT i = 0; i < frames; ++i) {
      winrt::com_ptr<IWICBitmapFrameDecode> frame;
      if (FAILED(decoder->GetFrame(i, frame.put()))) {
        continue;
      }
      UINT w = 0, h = 0;
      if (SUCCEEDED(frame->GetSize(&w, &h)) && static_cast<int>(w) > max_width) {
        max_width = static_cast<int>(w);
      }
    }
    return max_width;
  }

  bool resolve_best_app_icon_png(const std::wstring &icon_src, const std::wstring &exe_hint, const std::wstring &install_dir, const std::wstring &dst_path, IconResolutionInfo *info) {
    std::error_code ec;
    constexpr int kMinUsableIconPx = 96;

    // Fast path: an already high-resolution cache stays as-is (icons are effectively static).
    const int dst_width = fs::exists(fs::path(dst_path), ec) ? image_pixel_width(dst_path) : 0;
    if (dst_width >= kMinUsableIconPx) {
      if (info) {
        info->exe.clear();
        info->exe_size = 0;
        info->icon_size = 0;
      }
      return true;
    }

    const fs::path exe = find_primary_executable(exe_hint, install_dir);
    const int exe_size = exe.empty() ? 0 : probe_largest_icon_size(exe.wstring());
    const int icon_size = icon_src.empty() ? 0 : image_pixel_width(icon_src);
    if (info) {
      info->exe = exe.wstring();
      info->exe_size = exe_size;
      info->icon_size = icon_size;
    }

    const int best_source = std::max(exe_size, icon_size);
    if (best_source < kMinUsableIconPx) {
      return false;
    }
    if (dst_width >= best_source) {
      return true;
    }

    auto extract_usable_exe_icon = [&]() {
      if (exe.empty() || exe_size < kMinUsableIconPx) {
        return false;
      }
      int got = 0;
      if (!extract_largest_icon_png(exe.wstring(), dst_path, got) || got < kMinUsableIconPx) {
        return false;
      }
      return image_pixel_width(dst_path) >= kMinUsableIconPx;
    };

    // Prefer the executable's branded icon when it is at least as large as Playnite's.
    if (exe_size > 0 && exe_size >= icon_size) {
      if (extract_usable_exe_icon()) {
        return true;
      }
    }
    if (icon_size >= kMinUsableIconPx && convert_to_png_96dpi(icon_src, dst_path) && image_pixel_width(dst_path) >= kMinUsableIconPx) {
      return true;
    }
    if (extract_usable_exe_icon()) {
      return true;
    }
    return false;
  }

  namespace {
    constexpr UINT max_clipboard_side = 16384;
    constexpr UINT64 max_clipboard_pixels = 40'000'000;

    winrt::com_ptr<IWICImagingFactory> clipboard_wic() {
      winrt::com_ptr<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) {
        return nullptr;
      }
      return factory;
    }

    bool clipboard_size_ok(IWICBitmapSource *source, UINT &width, UINT &height) {
      return SUCCEEDED(source->GetSize(&width, &height)) && width && height && width <= max_clipboard_side &&
             height <= max_clipboard_side && UINT64(width) * height <= max_clipboard_pixels;
    }

    // The first frame of an image in memory, decoded as container (BMP or PNG).
    winrt::com_ptr<IWICBitmapFrameDecode> clipboard_decode(IWICImagingFactory *factory, const GUID &container, const void *data, std::size_t size, winrt::com_ptr<IWICStream> &stream) {
      winrt::com_ptr<IWICBitmapDecoder> decoder;
      winrt::com_ptr<IWICBitmapFrameDecode> frame;
      if (size > MAXDWORD || FAILED(factory->CreateStream(stream.put())) ||
          FAILED(stream->InitializeFromMemory(static_cast<BYTE *>(const_cast<void *>(data)), DWORD(size))) ||
          FAILED(factory->CreateDecoder(container, nullptr, decoder.put())) ||
          FAILED(decoder->Initialize(stream.get(), WICDecodeMetadataCacheOnDemand)) || FAILED(decoder->GetFrame(0, frame.put()))) {
        return nullptr;
      }
      return frame;
    }
  }  // namespace

  bool png_from_clipboard_dib(const void *dib, std::size_t size, std::string &png) {
    BITMAPINFOHEADER header {};
    if (size < sizeof header) {
      return false;
    }
    std::memcpy(&header, dib, sizeof header);
    if (header.biSize < sizeof header || header.biSize > size) {
      return false;
    }
    // Where the pixels start: after the header, BI_BITFIELDS' masks (in a plain
    // BITMAPINFOHEADER's DIB only; later headers hold them) and the colors.
    std::size_t pixels = header.biSize;
    if (header.biSize == sizeof header && header.biCompression == BI_BITFIELDS) {
      pixels += 3 * sizeof(DWORD);
    } else if (header.biSize == sizeof header && header.biCompression == 6 /* BI_ALPHABITFIELDS */) {
      pixels += 4 * sizeof(DWORD);
    }
    const DWORD colors = header.biClrUsed ? header.biClrUsed : header.biBitCount <= 8 ? 1u << header.biBitCount : 0;
    if (colors > 256) {
      return false;
    }
    pixels += std::size_t {colors} * sizeof(RGBQUAD);
    if (pixels >= size || size > MAXDWORD - sizeof(BITMAPFILEHEADER)) {
      return false;
    }
    // A .bmp file of it, for WIC's BMP decoder, which knows every DIB layout.
    std::string file(sizeof(BITMAPFILEHEADER) + size, '\0');
    BITMAPFILEHEADER file_header {};
    file_header.bfType = 0x4D42;  // "BM"
    file_header.bfSize = DWORD(file.size());
    file_header.bfOffBits = DWORD(sizeof file_header + pixels);
    std::memcpy(file.data(), &file_header, sizeof file_header);
    std::memcpy(file.data() + sizeof file_header, dib, size);

    CoInitGuard co;
    const auto factory = clipboard_wic();
    winrt::com_ptr<IWICStream> input;
    const auto frame = factory ? clipboard_decode(factory.get(), GUID_ContainerFormatBmp, file.data(), file.size(), input) : nullptr;
    UINT width = 0, height = 0;
    if (!frame || !clipboard_size_ok(frame.get(), width, height)) {
      return false;
    }
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::com_ptr<IStream> output;
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::com_ptr<IWICBitmapFrameEncode> encoded;
    winrt::com_ptr<IPropertyBag2> properties;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(factory->CreateFormatConverter(converter.put())) || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, output.put())) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
        FAILED(encoder->Initialize(output.get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(encoded.put(), properties.put())) ||
        FAILED(encoded->Initialize(properties.get())) || FAILED(encoded->SetSize(width, height)) || FAILED(encoded->SetPixelFormat(&format)) ||
        FAILED(converter->Initialize(frame.get(), format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(encoded->WriteSource(converter.get(), nullptr)) || FAILED(encoded->Commit()) || FAILED(encoder->Commit())) {
      return false;
    }
    STATSTG stat {};
    HGLOBAL memory = nullptr;
    if (FAILED(output->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(output.get(), &memory)) || !memory) {
      return false;
    }
    const void *bytes = GlobalLock(memory);
    if (!bytes) {
      return false;
    }
    png.assign(static_cast<const char *>(bytes), std::size_t(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return !png.empty();
  }

  bool clipboard_dib_from_png(const std::string &png, std::string &dib) {
    CoInitGuard co;
    const auto factory = clipboard_wic();
    winrt::com_ptr<IWICStream> input;
    const auto frame = factory ? clipboard_decode(factory.get(), GUID_ContainerFormatPng, png.data(), png.size(), input) : nullptr;
    UINT width = 0, height = 0;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (!frame || !clipboard_size_ok(frame.get(), width, height) || FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return false;
    }
    const UINT stride = width * 4;
    const std::size_t bytes = std::size_t {stride} * height;
    BITMAPINFOHEADER header {};
    header.biSize = sizeof header;
    header.biWidth = LONG(width);
    header.biHeight = LONG(height);  // bottom-up, as most apps expect
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = DWORD(bytes);
    dib.assign(sizeof header + bytes, '\0');
    std::memcpy(dib.data(), &header, sizeof header);
    auto *pixels = reinterpret_cast<BYTE *>(dib.data() + sizeof header);
    if (FAILED(converter->CopyPixels(nullptr, stride, UINT(bytes), pixels))) {
      dib.clear();
      return false;
    }
    // WIC gives rows top-down: turn them over in place.
    std::vector<BYTE> row(stride);
    for (UINT top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
      std::memcpy(row.data(), pixels + std::size_t {top} * stride, stride);
      std::memcpy(pixels + std::size_t {top} * stride, pixels + std::size_t {bottom} * stride, stride);
      std::memcpy(pixels + std::size_t {bottom} * stride, row.data(), stride);
    }
    return true;
  }
}  // namespace platf::img
