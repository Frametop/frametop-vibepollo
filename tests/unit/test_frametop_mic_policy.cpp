#include "../tests_common.h"

#include "src/frametop_mic_policy.h"

#include <string>

namespace {
  using namespace frametop_mic::policy;

  std::string payload(std::uint16_t sequence, std::string_view opus, std::uint8_t channel = CHANNEL, std::uint8_t flags = FLAGS) {
    std::string bytes {static_cast<char>(sequence >> 8), static_cast<char>(sequence & 0xff), static_cast<char>(channel), static_cast<char>(flags)};
    return bytes.append(opus);
  }
}  // namespace

TEST(FrametopMicPolicy, ParsesTheHeaderAndKeepsTheOpusPacket) {
  const auto bytes = payload(0x1234, "opus");
  const auto packet = parse(bytes);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->sequence, 0x1234);
  EXPECT_EQ(packet->opus, "opus");
}

TEST(FrametopMicPolicy, RejectsPacketsItCannotPlay) {
  EXPECT_FALSE(parse(""));
  EXPECT_FALSE(parse(payload(1, "")));  // a header with no audio
  EXPECT_FALSE(parse(payload(1, "x", 2)));  // another channel
  EXPECT_FALSE(parse(payload(1, "x", CHANNEL, 1)));  // flags this host doesn't know
  EXPECT_FALSE(parse(payload(1, std::string(MAX_OPUS_BYTES + 1, 'x'))));
  EXPECT_TRUE(parse(payload(1, std::string(MAX_OPUS_BYTES, 'x'))));
}

TEST(FrametopMicPolicy, StartsOverOnTheFirstPacket) {
  sequence_t sequence;
  EXPECT_EQ(sequence.next(500), (step_t {.play = true, .reset = true}));
  EXPECT_EQ(sequence.next(501), (step_t {.play = true}));
}

TEST(FrametopMicPolicy, DropsRepeatedAndLatePackets) {
  sequence_t sequence;
  sequence.next(10);
  sequence.next(11);
  EXPECT_EQ(sequence.next(11), step_t {});
  EXPECT_EQ(sequence.next(9), step_t {});
  EXPECT_EQ(sequence.next(12), (step_t {.play = true}));
}

TEST(FrametopMicPolicy, FillsInShortGapsAndStartsOverAfterLongOnes) {
  sequence_t sequence;
  sequence.next(10);
  EXPECT_EQ(sequence.next(12), (step_t {.play = true, .conceal = 1}));
  EXPECT_EQ(sequence.next(13 + MAX_CONCEALED), (step_t {.play = true, .conceal = MAX_CONCEALED}));
  EXPECT_EQ(sequence.next(13 + MAX_CONCEALED + MAX_CONCEALED + 2), (step_t {.play = true, .reset = true}));
}

TEST(FrametopMicPolicy, CountsAcrossTheWrap) {
  sequence_t sequence;
  sequence.next(65535);
  EXPECT_EQ(sequence.next(0), (step_t {.play = true}));
  EXPECT_EQ(sequence.next(2), (step_t {.play = true, .conceal = 1}));
}

TEST(FrametopMicPolicy, StartsOverWhenTheClientCountsAgain) {
  sequence_t sequence;
  sequence.next(30000);
  EXPECT_EQ(sequence.next(0), (step_t {.play = true, .reset = true}));
  EXPECT_EQ(sequence.next(1), (step_t {.play = true}));
}
