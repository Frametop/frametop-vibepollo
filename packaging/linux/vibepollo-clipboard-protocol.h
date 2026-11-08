/*
 * Frametop: what Vibepollo and vibepollo-clipboard say to each other over the helper's stdin
 * and stdout. The helper is the desktop session's clipboard (Wayland data control); Vibepollo
 * asks it for the clipboard's text or PNG image, or gives it one, for /actions/clipboard.
 *
 * Vibepollo to the helper, one request at a time:
 *   "get text\n"  "get image\n"
 *   "set text N\n" then N bytes  "set image N\n" then N bytes (N from 1 to the maximum)
 * The helper to Vibepollo: "ready\n" once it has the clipboard, else "fail WHY\n" and it
 * exits. Then one reply to each request:
 *   "ok N\n" then N bytes (a get's content; a set's N is 0)
 *   "none\n" (the clipboard holds no text, or no PNG image, or a password)
 *   "fail WHY\n" (WHY is plain words, never the clipboard's content)
 * Header-only and plain C, so the helper, Vibepollo and their tests share one parser.
 */
#ifndef VIBEPOLLO_CLIPBOARD_PROTOCOL_H
#define VIBEPOLLO_CLIPBOARD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The largest text or image either way. */
#define VIBEPOLLO_CLIPBOARD_MAX ((size_t) 64 << 20)
/* The longest header line, its "\n" included. */
#define VIBEPOLLO_CLIPBOARD_LINE 128

/* N in "set KIND N" and "ok N": decimal digits only, no sign or leading zero, at most max. */
static inline bool vibepollo_clipboard_parse_length(const char *text, size_t max, size_t *length) {
  if (!text[0] || (text[0] == '0' && text[1])) return false;
  size_t value = 0;
  for (const char *c = text; *c; ++c) {
    if (*c < '0' || *c > '9') return false;
    const size_t digit = (size_t) (*c - '0');
    if (value > (max - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *length = value;
  return true;
}

/* A request line, without its "\n". *length is 0 for a get. */
static inline bool vibepollo_clipboard_parse_request(const char *line, bool *set, bool *image, size_t *length) {
  const char *kind;
  if (!strncmp(line, "get ", 4)) *set = false, kind = line + 4;
  else if (!strncmp(line, "set ", 4)) *set = true, kind = line + 4;
  else return false;
  const char *rest;
  if (!strncmp(kind, "text", 4)) *image = false, rest = kind + 4;
  else if (!strncmp(kind, "image", 5)) *image = true, rest = kind + 5;
  else return false;
  if (!*set) return *length = 0, !*rest;
  return rest[0] == ' ' && vibepollo_clipboard_parse_length(rest + 1, VIBEPOLLO_CLIPBOARD_MAX, length) && *length;
}

enum vibepollo_clipboard_reply { VIBEPOLLO_CLIPBOARD_OK, VIBEPOLLO_CLIPBOARD_NONE, VIBEPOLLO_CLIPBOARD_FAIL };

/* A reply line, without its "\n". OK's *length is its content's; FAIL's *why is its words. */
static inline bool vibepollo_clipboard_parse_reply(const char *line, enum vibepollo_clipboard_reply *reply,
                                                   size_t *length, const char **why) {
  *length = 0, *why = "";
  if (!strcmp(line, "none")) return *reply = VIBEPOLLO_CLIPBOARD_NONE, true;
  if (!strncmp(line, "fail ", 5) && line[5]) return *reply = VIBEPOLLO_CLIPBOARD_FAIL, *why = line + 5, true;
  if (!strncmp(line, "ok ", 3)) {
    *reply = VIBEPOLLO_CLIPBOARD_OK;
    return vibepollo_clipboard_parse_length(line + 3, VIBEPOLLO_CLIPBOARD_MAX, length);
  }
  return false;
}

/* A PNG file starts so. */
static inline bool vibepollo_clipboard_is_png(const void *data, size_t length) {
  static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  return length > sizeof signature && !memcmp(data, signature, sizeof signature);
}

#endif
