/*
 * WebSocket (RFC 6455) support.
 *
 * This implementation is very simple-minded at the moment, and doesn't fully
 * conform to various pertinent standards.
 */

#include "copyrite.h"

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include "conf.h"
#include "externs.h"
#include "log.h"
#include "ansi.h"
#include "parse.h"
#include "confmagic.h"
#include "strutil.h"

#ifndef WITHOUT_WEBSOCKETS
#include "websock.h"

/* Length of 16 bytes, Base64 encoded (with padding). */
#define WEBSOCKET_KEY_LEN 24

/* Length of magic value. */
#define WEBSOCKET_KEY_MAGIC_LEN 36

/* Length of 20 bytes, Base64 encoded (with padding). */
#define WEBSOCKET_ACCEPT_LEN 28

/* Escaped characters. */
#define WEBSOCKET_ESCAPE_IAC ((char)255) /* introduces escape sequence */
#define WEBSOCKET_ESCAPE_NUL 'n' /* \0 not allowed within a string */
#define WEBSOCKET_ESCAPE_END 't' /* TAG_END not allowed within a tag */

/* WebSocket opcodes. */
enum WebSocketOp {
  WS_OP_CONTINUATION = 0x0,
  WS_OP_TEXT = 0x1,
  WS_OP_BINARY = 0x2,
  /* 0x3 - 0x7 reserved for control frames */
  WS_OP_CLOSE = 0x8,
  WS_OP_PING = 0x9,
  WS_OP_PONG = 0xA
  /* 0xB - 0xF reserved for control frames */
};

int queue_newwrite(DESC *d, const unsigned char *b, int n);

/* Base64 encoder. PennMUSH's version uses the heavyweight OpenSSL API. */
static void
encode64(char *dst, const char *src, size_t srclen)
{
  static const char enc[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/'
  };

  // Encode 3-byte units. Manually unrolled for performance.
  while (3 <= srclen) {
    *dst++ = enc[src[0] >> 2 & 0x3F];
    *dst++ = enc[(src[0] << 4 & 0x30) | (src[1] >> 4 & 0xF)];
    *dst++ = enc[(src[1] << 2 & 0x3C) | (src[2] >> 6 & 0x3)];
    *dst++ = enc[src[2] & 0x3F];

    src += 3;
    srclen -= 3;
  }

  // Encode residue (0, 1, or 2 bytes).
  switch (srclen) {
  case 1:
    *dst++ = enc[src[0] >> 2 & 0x3F];
    *dst++ = enc[src[0] << 4 & 0x30];
    *dst++ = '=';
    *dst++ = '=';
    break;

  case 2:
    *dst++ = enc[src[0] >> 2 & 0x3F];
    *dst++ = enc[(src[0] << 4 & 0x30) | (src[1] >> 4 & 0xF)];
    *dst++ = enc[src[1] << 2 & 0x3C];
    *dst++ = '=';
    break;
  }
}

/* RFC 6455, section 4.2.2, step 5.4: Sec-WebSocket-Accept */
static void
compute_websocket_accept(char *dst, const char *key)
{
  static const char *const MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  char combined[WEBSOCKET_KEY_LEN + WEBSOCKET_KEY_MAGIC_LEN];
  char hash[20];

  /* Concatenate key with magic value. */
  memcpy(combined, key, WEBSOCKET_KEY_LEN);
  memcpy(combined + WEBSOCKET_KEY_LEN, MAGIC, WEBSOCKET_KEY_MAGIC_LEN);

  /** Compute SHA-1 hash of combined value. */
  SHA1((unsigned char *)combined, sizeof(combined), (unsigned char *)hash);

  /* Encode using Base64. dst must have at least 28 bytes of space. */
  encode64(dst, hash, sizeof(hash));
}

static void
abort_handshake(DESC *d)
{
  static const char *const RESPONSE =
    "HTTP/1.1 426 Upgrade Required\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
  ;

  static size_t RESPONSE_LEN = 0;

  if (!RESPONSE_LEN) {
    RESPONSE_LEN = strlen(RESPONSE);
  }

  queue_newwrite(d, (unsigned char *)RESPONSE, RESPONSE_LEN);
}

static void
complete_handshake(DESC *d)
{
  static const char *const RESPONSE =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: "
  ;

  static size_t RESPONSE_LEN = 0;

  char buf[BUFFER_LEN];
  char *bp = buf;

  if (!RESPONSE_LEN) {
    RESPONSE_LEN = strlen(RESPONSE);
  }

  memcpy(bp, RESPONSE, RESPONSE_LEN);
  bp += RESPONSE_LEN;

  compute_websocket_accept(bp, d->checksum);
  bp += WEBSOCKET_ACCEPT_LEN;

  memcpy(bp, "\r\n\r\n", 4);
  bp += 4;

  queue_newwrite(d, (unsigned char *)buf, bp - buf);

  /*
   * Switch on WebSockets frame processing.
   *
   * One unfortunate side effect of how PennMUSH processes input is that
   * incoming characters are cooked and queued before commands are processed.
   * This means that in our current implementation, WebSockets frames won't be
   * processed if they're in the input queue.
   *
   * Fortunately, the WebSockets client will presumably wait until it gets the
   * response back from the server before switching, so we're probably OK.
   */
  d->conn_flags &= ~CONN_WEBSOCKETS_REQUEST;
  d->conn_flags |= CONN_WEBSOCKETS;

  d->checksum[0] = 4;

  do_rawlog(LT_CONN, "[%d/%s/%s] Switching to Websocket mode.", d->descriptor,
            d->addr, d->ip);
}

int
is_websocket(const char *command)
{
  static const char *const REQUEST_LINE = "GET /wsclient HTTP/1.1";

  static size_t REQUEST_LINE_LEN = 0;

  if (!REQUEST_LINE_LEN) {
    REQUEST_LINE_LEN = strlen(REQUEST_LINE);
  }

  /* TODO: Full implementation should verify entire request line. */
  return strncmp(command, REQUEST_LINE, REQUEST_LINE_LEN) == 0;
}

int
process_websocket_request(DESC *d, const char *command)
{
  static const char *const KEY_HEADER = "Sec-WebSocket-Key:";

  static size_t KEY_HEADER_LEN = 0;

  if (!KEY_HEADER_LEN) {
    KEY_HEADER_LEN = strlen(KEY_HEADER);
  }

  /* TODO: Full implementation should verify entire request. */
  if (*command == '\0') {
    if (!d->checksum[0]) {
      abort_handshake(d);
      return 0;
    }

    complete_handshake(d);
    return 1;
  }

  if (strncasecmp(command, KEY_HEADER, KEY_HEADER_LEN) == 0) {
    /* Re-using Pueblo checksum field for storing WebSockets key. */
    char *value = skip_space(command + KEY_HEADER_LEN);
    if (value && strlen(value) == WEBSOCKET_KEY_LEN) {
      memcpy(d->checksum, value, WEBSOCKET_KEY_LEN + 1);
    }
  }

  return 1;
}

int
process_websocket_frame(DESC *d, char *tbuf1, int got)
{
  char mask[1 + 4 + 1 + 1];
  unsigned char state, type, first, channel;
  uint64_t len;
  char *wp;
  const char *cp, *end;
  enum WebSocketOp op;

  wp = tbuf1;

  /* Restore state. */
  memcpy(mask, d->checksum, sizeof(mask));
  state = mask[0];
  type = mask[5];
  first = mask[6];
  len = d->ws_frame_len;

  /* Process buffer bytes. */
  for (cp = tbuf1, end = tbuf1 + got; cp != end; ++cp) {
    const unsigned char ch = *cp;

    switch (state++) {
    case 4:
      /* Received frame type. */
      op = ch & 0x0F;

      switch (op) {
      case WS_OP_CONTINUATION:
        /* Continue the previous opcode. */
        /* TODO: Error handling (only data frames can be continued). */
        first = 0;
        op = type & 0x0F;
        break;

      case WS_OP_TEXT:
        /* First frame of a new message. */
        first = 1;
        break;

      default:
        /* Ignore unrecognized opcode. */
        first = 2;
        break;
      }

      type = (ch & 0xF0) | op;
      break;

    case 5:
      /* Mask bit (required to be 1) and payload length (7 bits). */
      /* TODO: Error handling (check for the mask bit). */
      switch (ch & 0x7F) {
      case 126:
        /* 16-bit payload length. */
        break;

      case 127:
        /* 64-bit payload length. */
        state = 8;
        break;

      default:
        /* 7-bit payload length. */
        len = ch & 0x7F;
        state = 16;
        break;
      }
      break;

    case 6:
      /* 16-bit payload length. */
      len = ch & 0xFF;
      break;

    case 7:
      len = (len << 8) | (ch & 0xFF);
      state = 16;
      break;

    case 8:
      /* 64-bit payload length. */
      /* TODO: Error handling (first bit must be 0). */
      len = ch & 0x7F;
      break;

    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
      len = (len << 8) | (ch & 0xFF);
      break;

    case 16:
    case 17:
    case 18:
      /* 32-bit mask key. */
      mask[state - 16] = ch;
      break;

    case 19:
      mask[4] = ch;

      if (len) {
        /* Begin payload. */
        state = 0;
     } else {
        /* Empty payload. */
        state = 4;

        /* TODO: Handle end of frame. */
      }
      break;

    default:
      /* Payload data; handle according to opcode. */
      switch (first) {
      case 0:
        /* Continue frame. */
        *wp++ = ch ^ mask[state];
        break;

      case 1:
        /* Channel byte. */
        first = 0;
        channel = ch ^ mask[state];

        if (channel != WEBSOCKET_CHANNEL_TEXT) {
          /* TODO: Support other channel types later. */
          first = 2;
        }
        break;

      case 2:
        /* Ignore channel. */
        break;
      }

      if (--len) {
        /* More payload bytes. */
        state &= 0x3;
      } else {
        /* Last payload byte. */
        state = 4;

        /* TODO: Handle end of frame. */
      }
      break;
    }
  }

  /* Preserve state. */
  mask[0] = state;
  mask[5] = type;
  mask[6] = first;
  memcpy(d->checksum, mask, sizeof(mask));
  d->ws_frame_len = len;

  return wp - tbuf1;
}

static char *
write_message(
	char *dst, char *const dstend,
	const char *src, const char *const srcend,
	char channel
)
{
  size_t dstlen = dstend - dst;
  size_t srclen = srcend - src;
  enum WebSocketOp op;

  /* Check bounds. */
  dstlen = dstend - dst;
  srclen = srcend - src;

  if (dstlen < 11) {
    /* Maximum header size is 1 + 1 + 8 + 1; drop if not enough space. */
    /* TODO: Can be more precise about this, but not much need. */
    return dst;
  }

  dstlen -= 11;

  if (srclen > dstlen) {
    /* Silently truncate excess source. */
    /* TODO: Future implementation could stream using framing. */
    srclen = dstlen;
  }

  /* Write frame header. */
  if (channel == WEBSOCKET_CHANNEL_BINARY) {
    op = WS_OP_BINARY;
    dstlen = srclen;
  } else {
    op = WS_OP_TEXT;
    dstlen = 1 + srclen;
  }

  *dst++ = 0x80 | op;

  if (dstlen < 126) {
    *dst++ = dstlen;
  } else if (dstlen < 65536) {
    *dst++ = 126;

    *dst++ = (dstlen >> 8) & 0xFF;
    *dst++ = dstlen & 0xFF;
  } else {
    /* Probably never going to need this code path for typical BUFFER_LEN. */
    int ii;

    *dst++ = 127;

    for (ii = 56; ii >= 0; ii -= 8) {
      *dst++ = (dstlen >> ii) & 0xFF;
    }
  }

  /* Write frame payload. Note server doesn't mask. */
  if (op == WS_OP_TEXT) {
    *dst++ = channel;
  }

  memcpy(dst, src, srclen);
  dst += srclen;

  return dst;
}

static char *
write_message_encoded(
	char *dst, char *const dstend,
	const char *src, const char *const srcend,
	char channel
)
{
  char decoded[BUFFER_LEN], *wp;
  int escaped = 0;

  /* Decode source. */
  for (wp = decoded; src != srcend; ++src) {
    char ch = (char)*src;

    if (escaped) {
      switch (ch) {
      case WEBSOCKET_ESCAPE_NUL:
        ch = '\0';
        break;

      case WEBSOCKET_ESCAPE_END:
        ch = TAG_END;
        break;

      case WEBSOCKET_ESCAPE_IAC:
        ch = WEBSOCKET_ESCAPE_IAC;
        break;
      }

      escaped = 0;
    } else if (ch == WEBSOCKET_ESCAPE_IAC) {
      escaped = 1;
      continue;
    }

    if (safe_chr(ch, decoded, &wp)) {
      /* Not enough buffer space... which shouldn't ever happen... */
      return dst;
    }
  }

  /* Write decoded. */
  return write_message(
    dst, dstend, decoded, wp, channel
  );
}

void
to_websocket_frame(const char **bp, int *np, char channel)
{
  /* TODO: Not sure what the largest possible buffer is yet. */
  static char buf[4 * BUFFER_LEN];

  char *dst = buf;
  char *const dstend = dst + sizeof(buf);

  if (channel == WEBSOCKET_CHANNEL_AUTO) {
    /* Scan for markup boundaries. */
    /* TODO: Comes from render_string, so should never be unterminated. */
    const char *start, *tag, *end;
    int suppress;

    start = *bp;
    tag = NULL;
    suppress = 0;

    for (end = start, tag = NULL; *end; ++end) {
      switch (*end) {
      case TAG_START:
        if (tag) {
          /* Ignore TAG_START inside of a tag. */
          continue;
        }

        if (!suppress && start != end) {
          dst = write_message(dst, dstend, start, end, WEBSOCKET_CHANNEL_TEXT);
        }

        tag = end + 1;
        break;

      case TAG_END:
        if (tag) {
          channel = TAG_END;

          switch (*tag++) {
          case MARKUP_HTML:
            channel = WEBSOCKET_CHANNEL_PUEBLO;
            break;

          case MARKUP_WS:
            channel = *tag++;
            break;

          case MARKUP_WS_ALT:
            suppress = 1;
            break;

          case MARKUP_WS_ALT_END:
            suppress = 0;
            break;

          default:
            break;
          }

          switch (channel) {
          case TAG_END:
            /* Premature end of tag. */
            break;

          case WEBSOCKET_CHANNEL_BINARY:
            /* Encoded tag. */
            dst = write_message_encoded(dst, dstend, tag, end, channel);
            break;

          default:
            /* Unencoded tag. */
            dst = write_message(dst, dstend, tag, end, channel);
            break;
          }

          tag = NULL;
        }

        start = end + 1;
        break;
      }
    }

    /* Send tail. */
    if (!suppress && start != end && !tag) {
      dst = write_message(dst, dstend, start, end, WEBSOCKET_CHANNEL_TEXT);
    }
  } else {
    /* Send entire buffer on specified channel. */
    dst = write_message(dst, dstend, *bp, *bp + *np, channel);
  }

  /* Replace old arguments. */
  *bp = buf;
  *np = dst - buf;
}

int
markup_websocket(
  char *buff, char **bp, char *data, int datalen, char *alt, int altlen,
  char channel
)
{
  char *saved = *bp;

  /* Mark up channel text. */
  if (*data) {
    safe_chr(TAG_START, buff, bp);
    safe_chr(MARKUP_WS, buff, bp);
    safe_chr(channel, buff, bp);
    safe_strl(data, datalen, buff, bp);
    if (safe_chr(TAG_END, buff, bp)) {
      /* Not enough space in buffer. */
      *bp = saved;
      return 1;
    }
  }

  /* Mark up fallback text. */
  if (alt && *alt) {
    safe_chr(TAG_START, buff, bp);
    safe_chr(MARKUP_WS_ALT, buff, bp);
    safe_chr(TAG_END, buff, bp);

    safe_strl(alt, altlen, buff, bp);

    safe_chr(TAG_START, buff, bp);
    safe_chr(MARKUP_WS_ALT_END, buff, bp);
    if (safe_chr(TAG_END, buff, bp)) {
      /* Not enough space in buffer. */
      *bp = saved;
      return 1;
    }
  }

  return 0;
}

int
encode_websocket_binary(char *buff, char **bp, const char *src, int len)
{
  const char *end;
  char *saved = *bp;

  for (end = src + len; src != end; ++src) {
    char ch = *src;

    switch (ch) {
    case '\0':
      safe_chr(WEBSOCKET_ESCAPE_IAC, buff, bp);
      ch = WEBSOCKET_ESCAPE_NUL;
      break;

    case TAG_END:
      safe_chr(WEBSOCKET_ESCAPE_IAC, buff, bp);
      ch = WEBSOCKET_ESCAPE_END;
      break;

    case WEBSOCKET_ESCAPE_IAC:
      safe_chr(WEBSOCKET_ESCAPE_IAC, buff, bp);
      ch = WEBSOCKET_ESCAPE_IAC;
      break;
    }

    if (safe_chr(ch, buff, bp)) {
      /* Not enough space in buffer. */
      *bp = saved;
      return 1;
    }
  }

  return 0;
}

static void
do_fun_markup_websocket(
  char *buff, char **bp, int nargs, char *args[], int arglens[],
  dbref executor, char channel
)
{
  char *arg1;
  int arglen1;

  if (!Can_Pueblo_Send(executor)) {
    /* TODO: May want to restrict this to wizards. */
    safe_str(e_perm, buff, bp);
    return;
  }

  if (strchr(args[0], TAG_END)) {
    safe_str("#-1 NESTED TAG", buff, bp);
    return;
  }

  if (nargs > 1) {
    arg1 = args[1];
    arglen1 = arglens[1];
  } else {
    arg1 = NULL;
    arglen1 = 0;
  }

  markup_websocket(buff, bp, args[0], arglens[0], arg1, arglen1, channel);
}

FUNCTION(fun_websocket_json)
{
  do_fun_markup_websocket(
    buff, bp, nargs, args, arglens, executor, WEBSOCKET_CHANNEL_JSON
  );
}

FUNCTION(fun_websocket_html)
{
  do_fun_markup_websocket(
    buff, bp, nargs, args, arglens, executor, WEBSOCKET_CHANNEL_HTML
  );
}

FUNCTION(fun_websocket_binary)
{
  char encoded[BUFFER_LEN], bytes[BUFFER_LEN], *wp, *arg0, *arg1;
  int arglen1;

  if (!Can_Pueblo_Send(executor)) {
    /* TODO: May want to restrict this to wizards. */
    safe_str(e_perm, buff, bp);
    return;
  }

  /* Convert binary description to raw bytes. */
  wp = bytes;

  for (arg0 = args[0]; *arg0;) {
    int nbytes, type, ii;
    char *end;

    union {
      char bytes[8];
      long val;
      unsigned long uval;
      float fval;
      double dval;
    } tmp;

    /*
     * Parse type description. Available types restricted to DataView; codes
     * based on IA-64 C++ ABI.
     */
    switch (*arg0++) {
    case ' ': /* skip space */
      continue;

    case 'a': /* signed integer (8-bit) */
      nbytes = 1;
      type = 0;
      break;

    case 'h': /* unsigned integer (8-bit) */
      nbytes = 1;
      type = 1;
      break;

    case 's': /* signed integer (16-bit) */
      nbytes = 2;
      type = 0;
      break;

    case 't': /* unsigned integer (16-bit) */
      nbytes = 2;
      type = 1;
      break;

    case 'i': /* signed integer (32-bit) */
      nbytes = 4;
      type = 0;
      break;

    case 'j': /* unsigned integer (32-bit) */
      nbytes = 4;
      type = 1;
      break;

    case 'f': /* floating point (32-bit) */
      nbytes = 4;
      type = 2;
      break;

    case 'd': /* floating point (64-bit) */
      nbytes = 8;
      type = 3;
      break;

    default:
      /* Invalid type code. */
      safe_str(e_argrange, buff, bp);
      return;
    }

    /* Parse value. Note we won't handle range errors. */
    switch (type) {
    case 0: /* signed integer */
      tmp.val = strtol(arg0, &end, 0);
      break;

    case 1: /* unsigned integer */
      tmp.uval = strtoul(arg0, &end, 0);
      break;

    case 2: /* single precision floating point */
      tmp.fval = strtod(arg0, &end);
      break;

    case 3: /* double precision floating point */
      tmp.dval = strtod(arg0, &end);
      break;
    }

    if (arg0 == end) {
      /* No conversion was performed. */
      safe_str(e_argrange, buff, bp);
      return;
    }

    arg0 = end;

    /* Write bytes. (Little endian, because that's most clients.) */
    /* TODO: This isn't strictly portable, but should work in many cases. */
    for (ii = 0; ii < nbytes; ++ii) {
      char bval;

#ifdef WORDS_BIGENDIAN
      bval = tmp.bytes[7 - ii];
#else /* undef WORDS_BIGENDIAN */
      bval = tmp.bytes[ii];
#endif /* undef WORDS_BIGENDIAN */

      if (safe_chr(bval, bytes, &wp)) {
        /* Not enough space in buffer. */
        return;
      }
    }
  }

  /* Encode raw bytes to escape special characters. */
  arg0 = wp;
  wp = encoded;
  if (encode_websocket_binary(encoded, &wp, bytes, arg0 - bytes)) {
    /* Not enough space in buffer. */
    return;
  }

  *wp = '\0';

  /* Mark up the encoded bytes. */
  if (nargs > 1) {
    arg1 = args[1];
    arglen1 = arglens[1];
  } else {
    arg1 = NULL;
    arglen1 = 0;
  }

  markup_websocket(
    buff, bp, encoded, wp - encoded, arg1, arglen1, WEBSOCKET_CHANNEL_BINARY
  );
}

#endif /* undef WITHOUT_WEBSOCKETS */