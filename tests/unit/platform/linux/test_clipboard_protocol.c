// Frametop shared clipboard: what Vibepollo and vibepollo-clipboard say to each other
// (packaging/linux/vibepollo-clipboard-protocol.h).
#include "../../../../packaging/linux/vibepollo-clipboard-protocol.h"

#include <stdio.h>

static int failed = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c), ++failed; \
  } while (0)

static bool request(const char *line, bool *set, bool *image, size_t *length) {
  *set = *image = false, *length = 99;
  return vibepollo_clipboard_parse_request(line, set, image, length);
}

int main(void) {
  bool set, image;
  size_t length;
  CHECK(request("get text", &set, &image, &length) && !set && !image && !length);
  CHECK(request("get image", &set, &image, &length) && !set && image && !length);
  CHECK(request("set text 5", &set, &image, &length) && set && !image && length == 5);
  CHECK(request("set image 67108864", &set, &image, &length) && set && image && length == VIBEPOLLO_CLIPBOARD_MAX);
  CHECK(!request("set image 67108865", &set, &image, &length));  // over the limit
  CHECK(!request("set text 0", &set, &image, &length));  // nothing to set
  CHECK(!request("set text 05", &set, &image, &length));
  CHECK(!request("set text -1", &set, &image, &length));
  CHECK(!request("set text 1 ", &set, &image, &length));
  CHECK(!request("set text", &set, &image, &length));
  CHECK(!request("set text 99999999999999999999999", &set, &image, &length));
  CHECK(!request("get text 5", &set, &image, &length));
  CHECK(!request("get texts", &set, &image, &length));
  CHECK(!request("get png", &set, &image, &length));
  CHECK(!request("GET text", &set, &image, &length));
  CHECK(!request("", &set, &image, &length));

  enum vibepollo_clipboard_reply reply;
  const char *why;
  CHECK(vibepollo_clipboard_parse_reply("ok 0", &reply, &length, &why) && reply == VIBEPOLLO_CLIPBOARD_OK && !length);
  CHECK(vibepollo_clipboard_parse_reply("ok 1234", &reply, &length, &why) && reply == VIBEPOLLO_CLIPBOARD_OK && length == 1234);
  CHECK(vibepollo_clipboard_parse_reply("none", &reply, &length, &why) && reply == VIBEPOLLO_CLIPBOARD_NONE);
  CHECK(vibepollo_clipboard_parse_reply("fail it is over the size limit", &reply, &length, &why) &&
        reply == VIBEPOLLO_CLIPBOARD_FAIL && !strcmp(why, "it is over the size limit"));
  CHECK(!vibepollo_clipboard_parse_reply("fail ", &reply, &length, &why));
  CHECK(!vibepollo_clipboard_parse_reply("ok", &reply, &length, &why));
  CHECK(!vibepollo_clipboard_parse_reply("ok 67108865", &reply, &length, &why));
  CHECK(!vibepollo_clipboard_parse_reply("ready", &reply, &length, &why));
  CHECK(!vibepollo_clipboard_parse_reply("nonesuch", &reply, &length, &why));

  static const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0};
  CHECK(vibepollo_clipboard_is_png(png, sizeof png));
  CHECK(!vibepollo_clipboard_is_png(png, 8));  // the signature alone isn't an image
  CHECK(!vibepollo_clipboard_is_png("GIF89a....", 10));

  if (failed) return fprintf(stderr, "%d failed\n", failed), 1;
  printf("clipboard protocol: all checks passed\n");
  return 0;
}
