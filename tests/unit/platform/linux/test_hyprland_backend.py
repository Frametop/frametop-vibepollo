"""Execute the complete production backend selector with inert capture/session edges."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else 'g++'
source = (root / 'src/platform/linux/display_backend.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.lstrip().startswith('#include'))
# Keep the production virtual interface and capability shape; only external
# data structures/capture discovery and KScreen manager are deterministic.
interface = (root / 'src/platform/linux/display_backend.h').read_text()
interface = '\n'.join(line for line in interface.splitlines() if not line.lstrip().startswith(('#include', '#pragma')))
program = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <iostream>
#include <variant>
#include "src/hdr_stream_policy.h"
#include "src/platform/linux/kms_outputs.h"
#define SUNSHINE_BUILD_DRM
namespace display_device {
enum class DeviceEnumerationDetail { Full };
struct Resolution { unsigned int m_width{}, m_height{}; };
struct Rational { unsigned int m_numerator{}, m_denominator{}; };
using FloatingPoint = std::variant<double, Rational>;
struct EnumeratedDevice {
 struct Info { Resolution m_resolution{}; FloatingPoint m_resolution_scale{}; FloatingPoint m_refresh_rate{}; bool m_primary{}; };
 std::string m_device_id, m_display_name, m_friendly_name;
 std::optional<Info> m_info;
};
using EnumeratedDeviceList = std::vector<EnumeratedDevice>;
std::string toJson(const EnumeratedDeviceList&, std::nullopt_t) { return "[]"; }
}
namespace config {
struct video_t {
 enum class virtual_display_mode_e { disabled, per_client, shared };
 virtual_display_mode_e virtual_display_mode=virtual_display_mode_e::disabled;
 std::string output_name;
 bool rtx_hdr=false, runtime_rtx_hdr_override=false;
 struct { std::vector<std::string> virtual_display_outputs={"Reserved-1"}; } dd;
};
inline video_t video;
}
namespace rtsp_stream {
struct launch_session_t {
 bool virtual_display=false, virtual_display_failed=false, client_requests_virtual_display=false;
 bool virtual_display_recreated_on_demand=true, virtual_display_needs_resume_apply=true;
 bool hdr=false, force_sdr=false, prefer_sdr_10bit=false;
 std::optional<bool> client_virtual_display_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::string virtual_display_device_id="old";
 std::optional<int> virtual_display_ready_since=3;
 std::optional<bool> virtual_display_hdr_enabled=true;
 std::optional<std::string> output_name_override;
};
bool effective_hdr_requested(const launch_session_t& session) {
 return session.hdr && !session.force_sdr && !session.prefer_sdr_10bit;
}
bool hdr_capture_required(const launch_session_t& session, const config::video_t& video_config) {
 return video::hdr_stream_policy::requires_hdr_source(
   effective_hdr_requested(session), video_config.runtime_rtx_hdr_override && video_config.rtx_hdr);
}
}
namespace platf {
std::vector<std::string> outputs={"/dev/dri/card0:Virtual-1", "DP-1", "Reserved-1", "DP-2", "DP-3"};
std::vector<std::string> display_names() { return outputs; }
// KMS lists only outputs with an active plane, so a real output always has a
// size; the zero size here checks the backend doesn't claim info without one.
std::vector<kms_output_t> scanout={{"DP-1",5120,1440,239999},{"DP-2",0,0,0},{"Reserved-1",1920,1080,60000},{"DP-3",1920,1080,0}};
std::vector<kms_output_t> kms_outputs() {
 std::vector<kms_output_t> result;
 for(const auto& name : outputs) {
  const auto found=std::ranges::find(scanout,name,&kms_output_t::name);
  result.push_back(found==scanout.end() ? kms_output_t{name} : *found);
 }
 return result;
}
namespace hyprland_windows {
struct monitor_t { std::string name, description; int width=0, height=0; double refresh=0; bool window=false; };
std::vector<monitor_t> monitors={{"DP-2","Microstep MPG491CX OLED",5120,1440,240.0,false},{"FT-W1","",1000,700,60.0,true},{"HDMI-A-1","",0,0,0,false}};
std::vector<monitor_t> monitor_list() { return monitors; }
}
namespace linux_private_display {
int mutations=0;
struct prepared { bool active; std::string output_name, error; };
prepared prepare_session(rtsp_stream::launch_session_t&, bool, bool) { ++mutations; return {true,"Virtual-1",{}}; }
bool is_kernel_output(const std::string& name) { return name=="Virtual-1"; }
bool capable() { return true; } bool ready() { return true; } bool hdr_capable() { return true; }
std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) { return {}; }
bool initialize() { ++mutations; return true; }
bool apply_session(rtsp_stream::launch_session_t&) { ++mutations; return true; }
bool revert() { ++mutations; return true; }
bool reset_persistence() { ++mutations; return true; }
void schedule_revert(std::chrono::milliseconds,std::string) { ++mutations; }
void cancel_scheduled_revert() { ++mutations; }
}
}
'''
program += interface + '\n' + (root / 'src/platform/linux/session_backend.h').read_text().replace('#pragma once', '') + '\n' + source
program += r'''
int main() {
 using namespace platf;
 unsetenv("VIBEPOLLO_MACHINE_HOST");
 setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-kms",1);
 assert(!linux_display::hyprland_kms_session());
 assert(std::string(linux_display::backend().capabilities().backend_name)=="kscreen-vkms");
 {
  // Frametop windows: the user session's own instance lists Hyprland's outputs, window ones too.
  setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-wlr",1);
  assert(linux_display::hyprland_wlr_session());
  auto& wlr=linux_display::backend();
  assert(std::string(wlr.capabilities().backend_name)=="hyprland-wlr-sdr");
  const auto listed=wlr.enumerate_devices(display_device::DeviceEnumerationDetail::Full);
  assert(listed && listed->size()==2);  // one without a size isn't listed
  assert((*listed)[0].m_friendly_name=="Microstep MPG491CX OLED" && (*listed)[1].m_friendly_name=="Frametop window FT-W1");
  const auto& window=(*listed)[1].m_info;
  assert(window && window->m_resolution.m_width==1000 && window->m_resolution.m_height==700);
  assert(std::get<display_device::Rational>(window->m_refresh_rate).m_numerator==60000);
  rtsp_stream::launch_session_t s; s.output_name_override="FT-W1";
  const auto prepared=wlr.prepare_session(s,true,true);
  assert(prepared.error.empty() && prepared.output_name=="FT-W1" && !prepared.owns_output);
  s={}; s.output_name_override="FT-W9";
  assert(!wlr.prepare_session(s,true,true).error.empty());
  s={}; s.output_name_override="FT-W1"; s.virtual_display=true;
  assert(!wlr.prepare_session(s,true,true).error.empty());
  s={}; assert(!wlr.prepare_session(s,true,true).error.empty());  // nothing named
  assert(wlr.capture_target("FT-W1")=="FT-W1");
  setenv("VIBEPOLLO_MACHINE_HOST","1",1);  // the machine service is never the wlr one
  assert(!linux_display::hyprland_wlr_session());
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-kms",1);
  assert(!linux_private_display::mutations);
 }
 setenv("VIBEPOLLO_MACHINE_HOST","1",1);
 auto& backend=linux_display::backend();
 assert(!backend.capabilities().independent_outputs);
 assert(std::string(backend.capabilities().backend_name)=="hyprland-kms-physical-sdr");
 const auto devices=backend.enumerate_devices(display_device::DeviceEnumerationDetail::Full);
 assert(devices && devices->size()==3 && devices->front().m_device_id=="DP-1");
 // An active scanout's size and refresh are its display info; nothing else is claimed.
 const auto& info=devices->front().m_info;
 assert(info && info->m_resolution.m_width==5120 && info->m_resolution.m_height==1440 && !info->m_primary);
 assert(std::get<double>(info->m_resolution_scale)==1.0);
 const auto refresh=std::get<display_device::Rational>(info->m_refresh_rate);
 assert(refresh.m_numerator==239999 && refresh.m_denominator==1000);
 assert(!(*devices)[1].m_info);
 // A mode with no valid refresh still gives the display's size.
 const auto& unknown_refresh=devices->back().m_info;
 assert(unknown_refresh && unknown_refresh->m_resolution.m_width==1920 && std::get<double>(unknown_refresh->m_refresh_rate)==0.0);
 assert(backend.capture_target("")=="__unsupported-hyprland-output__");
 assert(backend.capture_target("DP-2")=="DP-2");
 for(const auto& name : {"0","Virtual-1","/dev/dri/card0:Virtual-1","Reserved-1","missing"})
   assert(backend.capture_target(name)=="__unsupported-hyprland-output__");
 rtsp_stream::launch_session_t session;
 assert(!backend.prepare_session(session,true,true).error.empty());
 session.output_name_override="DP-1";
 auto result=backend.prepare_session(session,true,true);
 assert(result.error.empty() && !result.owns_output && result.output_name=="DP-1");
 assert(session.output_name_override=="DP-1" && session.virtual_display_device_id.empty());
 assert(!session.virtual_display_ready_since && !session.virtual_display_hdr_enabled);
 assert(!session.virtual_display_needs_resume_apply);
 config::video.output_name="DP-2"; session={};
 assert(backend.prepare_session(session,true,true).output_name=="DP-2");
 config::video.output_name.clear();
 assert(backend.initialize() && backend.apply_session(session) && backend.revert());
 backend.schedule_revert({},"disconnect"); backend.cancel_scheduled_revert();
 assert(!backend.reset_persistence() && !linux_private_display::mutations);
 for(int trigger=0;trigger<4;++trigger) {
   session={};
   if(trigger==0) session.virtual_display=true;
   if(trigger==1) session.client_requests_virtual_display=true;
   if(trigger==2) session.virtual_display_mode_override=config::video_t::virtual_display_mode_e::shared;
   if(trigger==3) session.hdr=true;
   assert(!backend.prepare_session(session,true,true).error.empty());
 }
 config::video.rtx_hdr=true;
 session={}; session.hdr=true; session.output_name_override="DP-1";
 assert(!backend.prepare_session(session,true,true).error.empty());
 config::video.runtime_rtx_hdr_override=true;
 session={}; session.hdr=true; session.output_name_override="DP-1";
 assert(backend.prepare_session(session,true,true).error.empty());
 assert(!session.virtual_display_hdr_enabled);
 config::video.rtx_hdr=false;
 session={}; session.output_name_override="missing";
 assert(!backend.prepare_session(session,true,true).error.empty());
 outputs={"/dev/dri/card0:Virtual-1"}; session={};
 assert(!backend.prepare_session(session,true,true).error.empty());
 assert(backend.capture_target("")=="__unsupported-hyprland-output__");
 setenv("VIBEPOLLO_SESSION_BACKEND","kwin",1);
 assert(linux_display::backend().capabilities().independent_outputs);
 assert(linux_display::backend().initialize() && linux_private_display::mutations==1);
 std::cout << "PASS: production Hyprland physical output selection, the user session's wlr outputs, unsupported requests, no topology mutation and KWin fallback\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'backend.cpp').write_text(program)
    subprocess.run([compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(path / 'backend.cpp'), '-o', str(path / 'backend')], check=True)
    subprocess.run([str(path / 'backend')], check=True)
