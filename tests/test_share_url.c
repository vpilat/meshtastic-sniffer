/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * Regression tests for the meshtastic.org/e/ channel-share URL
 * decoder in web.c.
 *
 * Wire format (upstream firmware channel.proto):
 *   ChannelSet {
 *     repeated ChannelSettings settings = 1;
 *     LoRaConfig               lora_config = 2;
 *   }
 *   ChannelSettings {
 *     uint32  channel_num     = 1 [deprecated];
 *     bytes   psk             = 2;
 *     string  name            = 3;
 *     fixed32 id              = 4;
 *     bool    uplink_enabled  = 5;
 *     bool    downlink_enabled = 6;
 *     ModuleSettings module_settings = 7;
 *   }
 *
 * Field 1 of ChannelSet is repeated ChannelSettings directly -- no
 * intermediate Channel wrapper. The earlier parser assumed the wrapper
 * and dropped every channel because it treated the psk bytes as a
 * nested submessage. Issue #11 (hrusakcha: "imports 0 channels, should
 * be 4"); still-broken URL reported later by NeoChen1024.
 */

#include "../keyset.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../share_url.h"

static int fails = 0;
#define CHECK(cond, fmt, ...) do {                                    \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL [%s:%d]  " fmt "\n",                    \
                __FILE__, __LINE__, ##__VA_ARGS__);                   \
        fails++;                                                      \
    }                                                                 \
} while (0)

/* Base64URL-encode `n` bytes from `in` into `out` (NUL-terminated).
 * `out` needs 4*((n+2)/3) + 1 bytes. Returns bytes written. */
static size_t b64url_encode(const uint8_t *in, size_t n, char *out)
{
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t w = 0, i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[w++] = A[(v >> 18) & 0x3f];
        out[w++] = A[(v >> 12) & 0x3f];
        out[w++] = A[(v >>  6) & 0x3f];
        out[w++] = A[ v        & 0x3f];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        out[w++] = A[(v >> 18) & 0x3f];
        out[w++] = A[(v >> 12) & 0x3f];
        if (i + 1 < n) out[w++] = A[(v >> 6) & 0x3f];
    }
    out[w] = 0;
    return w;
}

/* Emit a varint into `buf`, return bytes written. */
static size_t pb_varint(uint8_t *buf, uint64_t v)
{
    size_t w = 0;
    while (v >= 0x80) { buf[w++] = (uint8_t)(v | 0x80); v >>= 7; }
    buf[w++] = (uint8_t)v;
    return w;
}
static size_t pb_tag(uint8_t *buf, uint32_t field, uint32_t wt)
{
    return pb_varint(buf, ((uint64_t)field << 3) | wt);
}

/* Emit one length-delimited (wire-type 2) field: tag + length + bytes.
 * Returns bytes written. */
static size_t pb_ld(uint8_t *buf, uint32_t field,
                    const void *data, size_t data_len)
{
    size_t w = pb_tag(buf, field, 2);
    w += pb_varint(buf + w, data_len);
    memcpy(buf + w, data, data_len);
    return w + data_len;
}

/* Build ONE ChannelSettings blob (psk + optional name). Returns
 * bytes written into `out`. */
static size_t build_channel_settings(uint8_t *out,
                                     const uint8_t *psk, size_t psk_len,
                                     const char *name)
{
    size_t w = 0;
    w += pb_ld(out + w, 2, psk, psk_len);
    if (name && name[0]) {
        w += pb_ld(out + w, 3, name, strlen(name));
    }
    return w;
}

/* Wrap a settings blob as a top-level ChannelSet entry (field 1).
 * Returns bytes written. */
static size_t append_settings(uint8_t *out, const uint8_t *settings, size_t n)
{
    return pb_ld(out, 1, settings, n);
}

/* ============================================================== */

static void test_single_default_channel(void)
{
    /* One channel, simpleN (1-byte psk = 1 = default LongFast). */
    uint8_t cs[16];
    uint8_t psk = 1;
    size_t cs_n = build_channel_settings(cs, &psk, 1, NULL);
    uint8_t frame[64];
    size_t  frame_n = append_settings(frame, cs, cs_n);
    char b64[128];
    b64url_encode(frame, frame_n, b64);

    char url[192];
    snprintf(url, sizeof(url), "https://meshtastic.org/e/?add=true#%s", b64);

    keyset_t *ks = keyset_create();
    int rc = decode_channel_share(ks, url);
    CHECK(rc == 1, "single-default: rc=%d want 1", rc);
    keyset_destroy(ks);
}

static void test_four_channel_import(void)
{
    /* Structure matches the URL NeoChen1024 posted on issue #11:
     * one simpleN default channel followed by three named
     * AES-256 channels. Locally-constructed test PSKs -- we don't
     * want to embed the reporter's actual channel keys in the tree. */
    uint8_t frame[512];
    size_t  frame_n = 0;
    uint8_t simple_psk = 1;
    uint8_t cs[80];

    /* Channel 1: primary, simpleN default (no name -> "LongFast"). */
    size_t n = build_channel_settings(cs, &simple_psk, 1, NULL);
    frame_n += append_settings(frame + frame_n, cs, n);

    /* Channels 2-4: 32-byte PSKs, distinct names. */
    struct { uint8_t psk[32]; const char *name; } ch[3] = {
        { {0}, "MeshTW"      },
        { {0}, "SignalTest"  },
        { {0}, "Emergency!"  },
    };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 32; ++j) ch[i].psk[j] = (uint8_t)(0x10 + i*32 + j);
        n = build_channel_settings(cs, ch[i].psk, 32, ch[i].name);
        frame_n += append_settings(frame + frame_n, cs, n);
    }

    /* Also tack on a LoRaConfig submessage (field 2) so the parser
     * exercises the skip path for non-Channel top-level fields. */
    uint8_t lora_cfg[8] = { 0x08, 0x01, 0x10, 0x04 };  /* two token varints */
    frame_n += pb_ld(frame + frame_n, 2, lora_cfg, 4);

    char b64[1024];
    b64url_encode(frame, frame_n, b64);
    char url[1152];
    snprintf(url, sizeof(url), "https://meshtastic.org/e/?add=true#%s", b64);

    keyset_t *ks = keyset_create();
    int rc = decode_channel_share(ks, url);
    CHECK(rc == 4,
          "four-channel: rc=%d want 4 (issue #11: 0 channels imported)",
          rc);
    keyset_destroy(ks);
}

static void test_channelset_prefix_form(void)
{
    /* URL variant seen in the wild: fragment prefixed with
     * "CHANNELSET=" before the base64url payload. */
    uint8_t cs[16], frame[64];
    uint8_t psk = 1;
    size_t cs_n = build_channel_settings(cs, &psk, 1, NULL);
    size_t frame_n = append_settings(frame, cs, cs_n);
    char b64[128];
    b64url_encode(frame, frame_n, b64);

    char url[192];
    snprintf(url, sizeof(url),
             "https://meshtastic.org/e/#CHANNELSET=%s", b64);

    keyset_t *ks = keyset_create();
    int rc = decode_channel_share(ks, url);
    CHECK(rc == 1, "CHANNELSET= prefix: rc=%d want 1", rc);
    keyset_destroy(ks);
}

static void test_reject_garbage(void)
{
    /* Not base64url -- must return -1, not silently succeed. */
    keyset_t *ks = keyset_create();
    int rc = decode_channel_share(ks, "https://meshtastic.org/e/#@@@");
    CHECK(rc == -1, "garbage: rc=%d want -1", rc);
    keyset_destroy(ks);
}

static void test_empty_fragment(void)
{
    keyset_t *ks = keyset_create();
    int rc = decode_channel_share(ks, "https://meshtastic.org/e/#");
    CHECK(rc == -1, "empty fragment: rc=%d want -1", rc);
    keyset_destroy(ks);
}

int main(void)
{
    test_single_default_channel();
    test_four_channel_import();
    test_channelset_prefix_form();
    test_reject_garbage();
    test_empty_fragment();

    if (fails == 0) { printf("OK\n"); return 0; }
    fprintf(stderr, "%d test(s) failed\n", fails);
    return 1;
}
