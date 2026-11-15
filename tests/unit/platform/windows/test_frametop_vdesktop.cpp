/**
 * @file tests/unit/platform/windows/test_frametop_vdesktop.cpp
 * @brief Test src/platform/windows/frametop_vdesktop.cpp: desktop ids, the calls that can't reach
 * the shell, and one that asks it (where a shell answers: run in a signed-in session). Moving
 * windows between desktops changes the user's desktop: not tested here.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/frametop_vdesktop.h>

namespace vdesktop = platf::frametop_vdesktop;

namespace {
  // As Vibepollo when it can't get the signed-in user's token.
  bool refuses(const std::function<void()> &) {
    return false;
  }

  // As Vibepollo when it runs as the user already.
  bool directly(const std::function<void()> &job) {
    job();
    return true;
  }
}  // namespace

TEST(FrametopVdesktop, DesktopIdRoundTrip) {
  constexpr GUID id {0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
  const auto text = vdesktop::to_string(id);
  EXPECT_EQ(text, "{53F5CA0B-158F-4124-900C-057158060B27}");
  const auto back = vdesktop::parse(text);
  ASSERT_TRUE(back.has_value());
  EXPECT_TRUE(IsEqualGUID(*back, id));
}

TEST(FrametopVdesktop, ParseRejectsWhatIsNoId) {
  EXPECT_FALSE(vdesktop::parse("").has_value());
  EXPECT_FALSE(vdesktop::parse("53F5CA0B-158F-4124-900C-057158060B27").has_value());
  EXPECT_FALSE(vdesktop::parse("{53F5CA0B-158F-4124-900C-05715806ZZ27}").has_value());
}

TEST(FrametopVdesktop, NoWindowIsCloaked) {
  EXPECT_FALSE(vdesktop::shell_cloaked(nullptr));
}

TEST(FrametopVdesktop, NothingToAskAboutAsksNothing) {
  bool asked = false;
  const auto away = vdesktop::elsewhere({}, [&](const std::function<void()> &) {
    asked = true;
    return true;
  });
  EXPECT_TRUE(away.empty());
  EXPECT_FALSE(asked);
}

TEST(FrametopVdesktop, WithoutTheUserNothingMoves) {
  const auto hwnd = GetDesktopWindow();
  EXPECT_TRUE(vdesktop::elsewhere({hwnd}, refuses).empty());
  std::string error;
  EXPECT_FALSE(vdesktop::bring_here(hwnd, refuses, &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(vdesktop::send_back(hwnd, GUID {}, refuses));
}

// The calls through the shell's vtables are made (an -O3 build once compiled them to a jump to
// nowhere). The desktop's own window is on no virtual desktop, so nothing is elsewhere.
TEST(FrametopVdesktop, AskingTheShellAboutAWindowReturns) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  EXPECT_TRUE(vdesktop::elsewhere({GetDesktopWindow()}, directly).empty());
  if (SUCCEEDED(com)) CoUninitialize();
}
#else
TEST(FrametopVdesktop, WindowsOnly) {
  GTEST_SKIP() << "virtual desktops are Windows'";
}
#endif
