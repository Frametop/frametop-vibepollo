/**
 * @file src/platform/windows/frametop_vdesktop.cpp
 * @brief Frametop windows on Windows' other virtual desktops (see frametop_vdesktop.h).
 */
#include "frametop_vdesktop.h"

#include <dwmapi.h>
#include <objbase.h>
#include <objectarray.h>
#include <servprov.h>

#include <cstdio>
#include <iterator>

namespace platf::frametop_vdesktop {
  namespace {
#ifndef DWM_CLOAKED_SHELL
    constexpr DWORD DWM_CLOAKED_SHELL = 0x2;
#endif

    // The shell's interfaces, declared here with their ids: the documented one so no SDK version
    // is needed, the others as MScholtes/VirtualDesktop (VirtualDesktop11.cs, V1.21, 2025-08)
    // has them for Windows 11 23H2 and 24H2. Only the leading methods, the ones called: a
    // vtable's later slots may move between builds, these haven't since 23H2.
    constexpr CLSID CLSID_ImmersiveShell {0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
    constexpr CLSID CLSID_DesktopManager {0xAA509086, 0x5CA9, 0x4C25, {0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A}};
    constexpr CLSID CLSID_DesktopManagerInternal {0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};
    constexpr IID IID_DesktopManager {0xA5CD92FF, 0x29BE, 0x454C, {0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B}};
    constexpr IID IID_ViewCollection {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};
    constexpr IID IID_Desktop {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};
    constexpr IID IID_DesktopManagerInternal {0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};

    // shobjidl.h's IVirtualDesktopManager.
    struct desktop_manager_t: IUnknown {
      virtual HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND hwnd, BOOL *here) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND hwnd, GUID *desktop) = 0;
      virtual HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND hwnd, REFGUID desktop) = 0;  // its own windows only
    };

    // IApplicationViewCollection. A view (IApplicationView) is only handed back to the shell.
    struct view_collection_t: IUnknown {
      virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray **views) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray **views) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR id, IObjectArray **views) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND hwnd, IUnknown **view) = 0;
    };

    // IVirtualDesktop.
    struct desktop_t: IUnknown {
      virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown *view, BOOL *visible) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetId(GUID *id) = 0;
    };

    // IVirtualDesktopManagerInternal.
    struct desktop_manager_internal_t: IUnknown {
      virtual HRESULT STDMETHODCALLTYPE GetCount(UINT *count) = 0;
      virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IUnknown *view, desktop_t *desktop) = 0;
      virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IUnknown *view, BOOL *can) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(desktop_t **desktop) = 0;
      virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray **desktops) = 0;
    };

    // Holds one reference.
    template<class T>
    class ref_t {
    public:
      ref_t() = default;
      ref_t(const ref_t &) = delete;
      ref_t &operator=(const ref_t &) = delete;

      ~ref_t() {
        if (p) p->Release();
      }

      T *get() const {
        return p;
      }

      T *operator->() const {
        return p;
      }

      explicit operator bool() const {
        return p != nullptr;
      }

      void **put() {
        if (p) p->Release();
        p = nullptr;
        return reinterpret_cast<void **>(&p);
      }

    private:
      T *p = nullptr;
    };

    // GUID_NULL, without uuid.lib.
    constexpr GUID no_desktop {};

    std::string hresult(HRESULT hr) {
      char text[16];
      std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
      return text;
    }

    // The shell's side of moving windows between desktops, opened for one job.
    struct shell_t {
      ref_t<desktop_manager_internal_t> desktops;
      ref_t<view_collection_t> views;

      // False with *error set: an older Windows (its own interface ids) answers no to these.
      bool open(std::string *error) {
        ref_t<IServiceProvider> shell;
        HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_IServiceProvider, shell.put());
        if (FAILED(hr)) {
          *error = "the Windows shell didn't answer (" + hresult(hr) + ")";
          return false;
        }
        hr = shell->QueryService(CLSID_DesktopManagerInternal, IID_DesktopManagerInternal, desktops.put());
        if (SUCCEEDED(hr)) hr = shell->QueryService(IID_ViewCollection, IID_ViewCollection, views.put());
        if (FAILED(hr)) {
          *error = "this Windows can't move windows between desktops for Frametop (" + hresult(hr) + "): Windows 11 23H2 or later can";
          return false;
        }
        return true;
      }

      // The desktop with that id, or nothing.
      bool find(const GUID &id, ref_t<desktop_t> &out) {
        ref_t<IObjectArray> all;
        UINT count = 0;
        if (FAILED(desktops->GetDesktops(reinterpret_cast<IObjectArray **>(all.put()))) || FAILED(all->GetCount(&count))) return false;
        for (UINT i = 0; i < count; ++i) {
          GUID its {};
          if (SUCCEEDED(all->GetAt(i, IID_Desktop, out.put())) && SUCCEEDED(out->GetId(&its)) && IsEqualGUID(its, id)) return true;
        }
        out.put();
        return false;
      }

      // Moves the window's view onto that desktop.
      HRESULT move(HWND hwnd, desktop_t *desktop) {
        ref_t<IUnknown> view;
        HRESULT hr = views->GetViewForHwnd(hwnd, reinterpret_cast<IUnknown **>(view.put()));
        if (FAILED(hr)) return hr;
        BOOL can = FALSE;
        if (SUCCEEDED(desktops->CanViewMoveDesktops(view.get(), &can)) && !can) return E_ACCESSDENIED;
        return desktops->MoveViewToDesktop(view.get(), desktop);
      }
    };
  }  // namespace

  bool shell_cloaked(HWND hwnd) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value == DWM_CLOAKED_SHELL;
  }

  std::vector<HWND> elsewhere(const std::vector<HWND> &windows, const as_user_t &as_user) {
    std::vector<HWND> out;
    if (windows.empty()) return out;
    as_user([&] {
      ref_t<desktop_manager_t> manager;
      if (FAILED(CoCreateInstance(CLSID_DesktopManager, nullptr, CLSCTX_ALL, IID_DesktopManager, manager.put()))) return;
      for (const auto hwnd : windows) {
        // A suspended Store app's frame is cloaked too, but on no desktop, or on the one shown.
        GUID desktop {};
        BOOL here = TRUE;
        if (SUCCEEDED(manager->GetWindowDesktopId(hwnd, &desktop)) && !IsEqualGUID(desktop, no_desktop) &&
            SUCCEEDED(manager->IsWindowOnCurrentVirtualDesktop(hwnd, &here)) && !here) {
          out.push_back(hwnd);
        }
      }
    });
    return out;
  }

  std::optional<GUID> bring_here(HWND hwnd, const as_user_t &as_user, std::string *error) {
    std::optional<GUID> from;
    std::string why = "Frametop couldn't ask Windows as the signed-in user";
    as_user([&] {
      ref_t<desktop_manager_t> manager;
      GUID desktop {};
      HRESULT hr = CoCreateInstance(CLSID_DesktopManager, nullptr, CLSCTX_ALL, IID_DesktopManager, manager.put());
      if (SUCCEEDED(hr)) hr = manager->GetWindowDesktopId(hwnd, &desktop);
      if (FAILED(hr) || IsEqualGUID(desktop, no_desktop)) {
        why = "Windows didn't say which desktop it's on (" + hresult(hr) + ")";
        return;
      }
      shell_t shell;
      if (!shell.open(&why)) return;
      ref_t<desktop_t> current;
      hr = shell.desktops->GetCurrentDesktop(reinterpret_cast<desktop_t **>(current.put()));
      if (SUCCEEDED(hr)) hr = shell.move(hwnd, current.get());
      if (FAILED(hr)) {
        why = "Windows didn't move it from its desktop (" + hresult(hr) + ")";
        return;
      }
      from = desktop;
    });
    if (!from) *error = why;
    return from;
  }

  bool send_back(HWND hwnd, const GUID &desktop, const as_user_t &as_user) {
    bool back = false;
    as_user([&] {
      shell_t shell;
      std::string ignored;
      ref_t<desktop_t> home;
      back = shell.open(&ignored) && shell.find(desktop, home) && SUCCEEDED(shell.move(hwnd, home.get()));
    });
    return back;
  }

  std::string to_string(const GUID &desktop) {
    wchar_t wide[40] {};
    const int n = StringFromGUID2(desktop, wide, int(std::size(wide)));
    std::string out;
    for (int i = 0; i + 1 < n; ++i) out.push_back(char(wide[i]));  // hex digits, dashes and braces
    return out;
  }

  std::optional<GUID> parse(const std::string &text) {
    if (text.size() != 38) return std::nullopt;
    const std::wstring wide(text.begin(), text.end());
    GUID out {};
    if (FAILED(CLSIDFromString(wide.c_str(), &out))) return std::nullopt;
    return out;
  }
}  // namespace platf::frametop_vdesktop
