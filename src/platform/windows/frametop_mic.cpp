/**
 * @file src/platform/windows/frametop_mic.cpp
 * @brief Frametop: a client's microphone plays into the speaker side of Steam Streaming
 *        Microphone, which apps record from as "Microphone (Steam Streaming Microphone)".
 */
#define INITGUID

// standard includes
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

// platform includes
#include <WinSock2.h>
#include <Audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <newdev.h>

// local includes
#include "src/frametop_mic.h"
#include "src/logging.h"
#include "src/utility.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#endif

using namespace std::literals;

namespace platf::frametop_mic {
  namespace {
    /// The adapter of both its endpoints: what's in brackets in "Speakers (Steam Streaming Microphone)".
    constexpr std::wstring_view ADAPTER_NAME = L"Steam Streaming Microphone";
    /// PKEY_DeviceInterface_FriendlyName: the adapter's name.
    const PROPERTYKEY adapter_name_key {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
#ifdef STEAM_DRIVER_SUBDIR
    constexpr auto DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingMicrophone.inf";
#endif

    constexpr int SAMPLE_RATE = 48000;
    /// The buffer Windows keeps for this stream.
    constexpr REFERENCE_TIME BUFFER_DURATION = 2'000'000;  // 200 ms in 100 ns units
    /// Silence ahead of the audio whenever the device runs dry, so the next late packet doesn't
    /// make it run dry again.
    constexpr UINT32 CUSHION_FRAMES = SAMPLE_RATE * 40 / 1000;
    /// More than this waiting to be heard is too late; packets are dropped to catch up.
    constexpr UINT32 MAX_QUEUED_FRAMES = SAMPLE_RATE * 120 / 1000;
    /// How often the microphone side's level is looked at again: 50 packets, a second.
    constexpr int LEVEL_CHECK_WRITES = 50;

    constexpr std::array FLOWS {eRender, eCapture};
    constexpr std::array ROLES {eConsole, eMultimedia, eCommunications};

    template<class T>
    void release(T *p) {
      p->Release();
    }

    template<class T>
    using com_t = util::safe_ptr<T, release<T>>;

    std::atomic<bool> g_tried_install = false;

    class co_init_t {
    public:
      co_init_t():
          ok_ {SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))} {
      }

      ~co_init_t() {
        if (ok_) {
          CoUninitialize();
        }
      }

      co_init_t(const co_init_t &) = delete;
      co_init_t &operator=(const co_init_t &) = delete;

    private:
      bool ok_;
    };

    bool is_steam_mic(IMMDevice *device) {
      com_t<IPropertyStore> props;
      if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return false;
      }

      PROPVARIANT name;
      PropVariantInit(&name);
      const bool match = SUCCEEDED(props->GetValue(adapter_name_key, &name)) && name.vt == VT_LPWSTR && name.pwszVal && ADAPTER_NAME == name.pwszVal;
      PropVariantClear(&name);
      return match;
    }

    /// Steam Streaming Microphone's speaker side (or with eCapture, its microphone side) in any
    /// of @p states, or nullptr.
    com_t<IMMDevice> find_endpoint(IMMDeviceEnumerator *devices, DWORD states, EDataFlow flow = eRender) {
      com_t<IMMDeviceCollection> collection;
      if (FAILED(devices->EnumAudioEndpoints(flow, states, &collection))) {
        return nullptr;
      }

      UINT count = 0;
      collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        com_t<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device)) && is_steam_mic(device.get())) {
          return device;
        }
      }
      return nullptr;
    }

    std::wstring default_id(IMMDeviceEnumerator *devices, EDataFlow flow, ERole role) {
      com_t<IMMDevice> device;
      if (FAILED(devices->GetDefaultAudioEndpoint(flow, role, &device))) {
        return {};
      }

      LPWSTR id = nullptr;
      if (FAILED(device->GetId(&id))) {
        return {};
      }
      std::wstring result {id};
      CoTaskMemFree(id);
      return result;
    }

    /// Installs Steam's microphone driver, which comes with Steam, and puts back any default
    /// device Windows moved to it.
    bool install_driver(IMMDeviceEnumerator *devices) {
#ifdef STEAM_DRIVER_SUBDIR
      // MinGW's libnewdev.a lacks DiInstallDriverW(), so it's loaded at runtime, like audio.cpp does.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(error) << "Frametop mic: newdev.dll failed to load"sv;
        return false;
      }
      auto free_newdev = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });
      auto install = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!install) {
        BOOST_LOG(error) << "Frametop mic: DiInstallDriverW() is missing"sv;
        return false;
      }

      std::array<std::array<std::wstring, ROLES.size()>, FLOWS.size()> before;
      for (std::size_t f = 0; f < FLOWS.size(); ++f) {
        for (std::size_t r = 0; r < ROLES.size(); ++r) {
          before[f][r] = default_id(devices, FLOWS[f], ROLES[r]);
        }
      }

      WCHAR path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(DRIVER_PATH, path, ARRAYSIZE(path));
      if (!install(nullptr, path, 0, nullptr)) {
        const auto err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
          BOOST_LOG(warning) << "Frametop mic: Steam Streaming Microphone isn't installed, and Steam isn't here to install it from"sv;
        } else if (err == ERROR_ACCESS_DENIED) {
          BOOST_LOG(warning) << "Frametop mic: installing Steam Streaming Microphone needs administrator rights"sv;
        } else {
          BOOST_LOG(warning) << "Frametop mic: couldn't install Steam Streaming Microphone: "sv << err;
        }
        return false;
      }
      BOOST_LOG(info) << "Frametop mic: installed Steam Streaming Microphone"sv;

      // Let Windows set up the new endpoints before looking at them.
      Sleep(5000);

      com_t<IPolicyConfig> policy;
      if (FAILED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void **) &policy))) {
        return true;
      }
      for (std::size_t f = 0; f < FLOWS.size(); ++f) {
        for (std::size_t r = 0; r < ROLES.size(); ++r) {
          const auto now = default_id(devices, FLOWS[f], ROLES[r]);
          if (before[f][r].empty() || now == before[f][r]) {
            continue;
          }

          com_t<IMMDevice> device;
          if (SUCCEEDED(devices->GetDevice(now.c_str(), &device)) && is_steam_mic(device.get())) {
            policy->SetDefaultEndpoint(before[f][r].c_str(), ROLES[r]);
          }
        }
      }
      return true;
#else
      BOOST_LOG(warning) << "Frametop mic: Steam Streaming Microphone isn't installed, and there's no driver for this architecture"sv;
      return false;
#endif
    }

    class wasapi_device_t: public device_t {
    public:
      bool open(IMMDevice *endpoint, IMMDevice *microphone) {
        if (microphone) {
          microphone->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void **) &level_);
        }
        if (FAILED(endpoint->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **) &client_))) {
          BOOST_LOG(error) << "Frametop mic: couldn't open Steam Streaming Microphone"sv;
          return false;
        }

        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = SAMPLE_RATE;
        format.wBitsPerSample = 32;
        format.nBlockAlign = sizeof(float);
        format.nAvgBytesPerSec = SAMPLE_RATE * sizeof(float);

        auto status = client_->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          BUFFER_DURATION,
          0,
          &format,
          nullptr
        );
        if (FAILED(status)) {
          BOOST_LOG(error) << "Frametop mic: couldn't set up Steam Streaming Microphone: 0x"sv << util::hex(status).to_string_view();
          return false;
        }

        client_->GetBufferSize(&buffer_frames_);
        if (FAILED(client_->GetService(IID_IAudioRenderClient, (void **) &render_))) {
          BOOST_LOG(error) << "Frametop mic: Steam Streaming Microphone won't take audio"sv;
          return false;
        }
        return true;
      }

      ~wasapi_device_t() override {
        if (started_) {
          client_->Stop();
        }
      }

      bool write(const float *samples, int count) override {
        UINT32 queued = 0;
        if (FAILED(client_->GetCurrentPadding(&queued))) {
          return false;
        }

        if (writes_++ % LEVEL_CHECK_WRITES == 0) {
          check_level();
        }

        const UINT32 lead = queued == 0 ? CUSHION_FRAMES : 0;
        const UINT32 frames = lead + static_cast<UINT32>(count);
        const UINT32 limit = MAX_QUEUED_FRAMES < buffer_frames_ ? MAX_QUEUED_FRAMES : buffer_frames_;
        if (queued + frames > limit) {
          return true;  // too far behind: this packet goes, to catch up
        }

        BYTE *data = nullptr;
        if (FAILED(render_->GetBuffer(frames, &data))) {
          return false;
        }
        std::memset(data, 0, lead * sizeof(float));
        auto *out = reinterpret_cast<float *>(data) + lead;
        for (int i = 0; i < count; ++i) {
          out[i] = samples[i] * gain_;
        }
        if (FAILED(render_->ReleaseBuffer(frames, 0))) {
          return false;
        }

        if (!started_) {
          if (FAILED(client_->Start())) {
            return false;
          }
          started_ = true;
        }
        return true;
      }

    private:
      // Windows boosts "Microphone (Steam Streaming Microphone)" by up to 30 dB, the whole 30 at
      // a level of 100, which clips. Whatever its level adds above 0 dB comes off
      // here, so apps hear the client's own level. Levels below 0 dB are left to work as usual.
      void check_level() {
        float db = 0;
        if (!level_ || FAILED(level_->GetMasterVolumeLevel(&db))) {
          db = 0;
        }
        const float boost = db > 0 ? db : 0;
        if (boost != boost_) {
          if (boost > 0) {
            BOOST_LOG(info) << "Frametop mic: \"Microphone (Steam Streaming Microphone)\" adds "sv << boost << " dB; playing that much quieter"sv;
          } else {
            BOOST_LOG(info) << "Frametop mic: \"Microphone (Steam Streaming Microphone)\" adds nothing now; playing at the client's level"sv;
          }
          boost_ = boost;
          gain_ = std::pow(10.0f, -boost / 20);
        }
      }

      co_init_t co_init_;  // first: COM outlives the interfaces below
      com_t<IAudioClient> client_;
      com_t<IAudioRenderClient> render_;
      com_t<IAudioEndpointVolume> level_;  // of the microphone side
      UINT32 buffer_frames_ = 0;
      bool started_ = false;
      unsigned writes_ = 0;
      float boost_ = 0;  // dB
      float gain_ = 1;
    };
  }  // namespace

  std::unique_ptr<device_t> open_device() {
    auto device = std::make_unique<wasapi_device_t>();  // first: it starts COM on this thread

    com_t<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **) &devices))) {
      BOOST_LOG(error) << "Frametop mic: couldn't list audio devices"sv;
      return nullptr;
    }

    auto endpoint = find_endpoint(devices.get(), DEVICE_STATE_ACTIVE);
    if (!endpoint) {
      if (find_endpoint(devices.get(), DEVICE_STATE_DISABLED)) {
        BOOST_LOG(warning) << "Frametop mic: Steam Streaming Microphone is turned off in Windows' sound settings"sv;
        return nullptr;
      }
      if (g_tried_install.exchange(true) || !install_driver(devices.get())) {
        return nullptr;
      }
      endpoint = find_endpoint(devices.get(), DEVICE_STATE_ACTIVE);
      if (!endpoint) {
        BOOST_LOG(warning) << "Frametop mic: Steam Streaming Microphone didn't show up after installing it"sv;
        return nullptr;
      }
    }

    auto microphone = find_endpoint(devices.get(), DEVICE_STATE_ACTIVE, eCapture);
    if (!device->open(endpoint.get(), microphone.get())) {
      return nullptr;
    }
    BOOST_LOG(info) << "Frametop mic: playing into Steam Streaming Microphone; apps record it as \"Microphone (Steam Streaming Microphone)\""sv;
    return device;
  }
}  // namespace platf::frametop_mic
