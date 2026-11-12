#include "../tests_common.h"

#include "src/frametop_together_wire.h"

#include <string>

namespace {
  using namespace frametop_together::wire;
  using namespace std::literals;

  template<class T>
  std::optional<T> parsed(std::string_view line) {
    const auto said = parse(line);
    if (!said || !std::holds_alternative<T>(*said)) {
      return std::nullopt;
    }
    return std::get<T>(*said);
  }
}  // namespace

TEST(FrametopTogetherWire, APictureIsItsLengthItsKindItsPanelItsFlagsAndItsBytes) {
  EXPECT_EQ(record(3, true, "abc"), "\x06\x00\x00\x00N\x03\x01"s + "abc");
  EXPECT_EQ(record(6, false, "x"), "\x04\x00\x00\x00N\x06\x00x"s);
  const std::string big(0x1f3f0, 'p');
  const auto bytes = record(4, false, big);
  ASSERT_EQ(bytes.size(), 7 + big.size());
  EXPECT_EQ(bytes.substr(0, 7), "\xf3\xf3\x01\x00N\x04\x00"s);  // 0x1f3f0 + 3, little-endian
  EXPECT_EQ(bytes.substr(7), big);
}

TEST(FrametopTogetherWire, OnlyAPcsPanelsAndOnlyPicturesARoomTakes) {
  EXPECT_TRUE(record(2, true, "abc").empty());  // the Frame's own
  EXPECT_TRUE(record(7, true, "abc").empty());
  EXPECT_TRUE(record(-1, true, "abc").empty());
  EXPECT_TRUE(record(3, true, "").empty());
  EXPECT_FALSE(record(3, true, std::string(MAX_PICTURE, 'x')).empty());
  EXPECT_TRUE(record(3, true, std::string(MAX_PICTURE + 1, 'x')).empty());
}

TEST(FrametopTogetherWire, ReadsWhatAPanelIsToBeEncodedAt) {
  EXPECT_EQ(parsed<told_t>("panel 3 R 2000000"), (told_t {3, 'R', 2000000}));
  EXPECT_EQ(parsed<told_t>("panel 4 F 30"), (told_t {4, 'F', 30}));
  EXPECT_EQ(parsed<told_t>("panel 5 P 10"), (told_t {5, 'P', 10}));
  EXPECT_EQ(parsed<told_t>("panel 6 K 0"), (told_t {6, 'K', 0}));
  EXPECT_EQ(parsed<told_t>("panel 3 W 1"), (told_t {3, 'W', 1}));
  EXPECT_EQ(parsed<sent_t>("panel 3 sent 240"), (sent_t {3, 240}));
}

TEST(FrametopTogetherWire, ReadsWhatBecameOfTheCall) {
  EXPECT_EQ(parsed<called_t>("called 100.101.102.103"), (called_t {"100.101.102.103"}));
  EXPECT_EQ(parsed<called_t>("called fd7a:115c:a1e0::1"), (called_t {"fd7a:115c:a1e0::1"}));
  EXPECT_EQ(parsed<ended_t>("refused: that code isn't good"), (ended_t {"that code isn't good"}));
  EXPECT_EQ(parsed<ended_t>("the link to the headset ended"), (ended_t {"the link to the headset ended"}));
  EXPECT_EQ(parsed<ended_t>("the headset said what isn't a message"), (ended_t {"the headset said what isn't a message"}));
}

TEST(FrametopTogetherWire, IgnoresEverythingElse) {
  for (const auto *line : {
         "",
         "panel",
         "panel 3",
         "panel 3 R",
         "panel 3 R x",
         "panel 3 R 5 6",
         "panel 3 R -5",
         "panel 3 R 99999999999",
         "panel 2 R 5",  // not a PC's panel
         "panel 7 W 1",
         "panel x W 1",
         "panel 3 Q 1",  // a word it doesn't know
         "panel 3 s 5",
         "panel 3 RF 1",
         "panel 3 sent",
         "panel 3 sent x",
         "panel  3 W 1",
         "Panel 3 W 1",
         "called",
         "called somewhere else",
         "called 100.1.2.3; rm",
         "refused",
         "usage: ft-pcsend HOST PORT CODE",
       }) {
    EXPECT_FALSE(parse(line)) << line;
  }
}

TEST(FrametopTogetherWire, AnAddressIsNumbersAndACodeEightLettersOrDigits) {
  EXPECT_TRUE(is_address("100.64.0.1"));
  EXPECT_TRUE(is_address("fd7a:115c:a1e0::ABCD"));
  EXPECT_FALSE(is_address(""));
  EXPECT_FALSE(is_address("1"));
  EXPECT_FALSE(is_address("--loopback"));
  EXPECT_FALSE(is_address("--feed"));  // (nothing ft-pcsend could take for an option)
  EXPECT_FALSE(is_address("frame.tailnet.ts.net"));
  EXPECT_FALSE(is_address("100.64.0.1 "));
  EXPECT_FALSE(is_address(std::string(46, '1')));
  EXPECT_TRUE(is_address(std::string(45, '1')));

  EXPECT_TRUE(is_code("ABCDEFGH"));
  EXPECT_TRUE(is_code("abc23456"));
  EXPECT_FALSE(is_code("ABCDEFG"));
  EXPECT_FALSE(is_code("ABCDEFGHJ"));
  EXPECT_FALSE(is_code("ABCDEFG-"));
  EXPECT_FALSE(is_code("ABCD EFG"));
  EXPECT_FALSE(is_code(""));
}
