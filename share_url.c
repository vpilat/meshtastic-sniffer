/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * The wire format described below is the Meshtastic on-air channel
 * share protocol, from the upstream firmware at
 * https://github.com/meshtastic/firmware (GPL-3.0-or-later). The
 * implementation here is original.
 *
 * meshtastic-sniffer: meshtastic.org/e/ channel-share URL decoder.
 *
 * URLs are of the form:
 *   meshtastic.org/e/?#BASE64URL
 *   meshtastic.org/e/?#CHANNELSET=BASE64URL (also seen)
 *
 * The base64-url payload is a protobuf ChannelSet:
 *
 *   ChannelSet {
 *     repeated ChannelSettings settings = 1;
 *     LoRaConfig               lora_config = 2;
 *   }
 *   ChannelSettings {
 *     uint32 channel_num = 1 [deprecated];
 *     bytes  psk = 2; string name = 3;
 *     fixed32 id = 4; bool uplink_enabled = 5; bool downlink_enabled = 6;
 *     ModuleSettings module_settings = 7;
 *   }
 *
 * Field 1 is repeated ChannelSettings directly -- NOT wrapped in a
 * Channel submessage. The firmware's internal `Channel` type (index +
 * settings + Role) is a different message and is not what share URLs
 * carry on the wire.
 *
 * For each ChannelSettings found, calls keyset_add(name, psk, psk_len).
 * Returns the number of channels added, or -1 on parse error.
 */

#include "share_url.h"
#include "keyset.h"
#include "meshtastic.h"

#include <stdint.h>
#include <string.h>

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Advance past one field of the given wire type, or return -1 if the
 * buffer runs out. Keeps the walker in sync when we hit fields we
 * don't care about (id, uplink_enabled, module_settings, ...). */
static int share_skip(const uint8_t **pp, const uint8_t *end, uint32_t wt)
{
    const uint8_t *p = *pp;
    if (wt == 0) {
        while (p < end && (*p++ & 0x80)) {}
    } else if (wt == 1) {
        if (end - p < 8) return -1;
        p += 8;
    } else if (wt == 2) {
        uint64_t l = 0; int sh = 0;
        while (p < end) {
            uint8_t b = *p++;
            l |= (uint64_t)(b & 0x7f) << sh;
            if (!(b & 0x80)) break;
            sh += 7;
        }
        if ((uint64_t)(end - p) < l) return -1;
        p += (size_t)l;
    } else if (wt == 5) {
        if (end - p < 4) return -1;
        p += 4;
    } else {
        return -1;
    }
    *pp = p;
    return 0;
}

int decode_channel_share(keyset_t *ks, const char *url_or_b64)
{
    if (!ks || !url_or_b64) return -1;
    const char *p = url_or_b64;
    const char *hash = strchr(p, '#');
    if (hash) p = hash + 1;
    const char *eq = strchr(p, '=');
    if (eq) p = eq + 1;

    uint8_t buf[512]; size_t out = 0;
    uint32_t accum = 0; int bits = 0;
    for (; *p && out < sizeof(buf); ++p) {
        if (*p == '=' || *p == '&' || *p == ' ' || *p == '\r' || *p == '\n') break;
        int v = b64v(*p);
        if (v < 0) return -1;
        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[out++] = (uint8_t)((accum >> bits) & 0xff);
        }
    }
    if (out == 0) return -1;

    int added = 0;
    const uint8_t *bp = buf, *bend = buf + out;
    while (bp < bend) {
        uint64_t tag = 0; int shift = 0;
        while (bp < bend) {
            uint8_t b = *bp++;
            tag |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        uint32_t fld = (uint32_t)(tag >> 3);
        uint32_t wt  = (uint32_t)(tag & 0x7);
        if (wt != 2) {
            if (share_skip(&bp, bend, wt) < 0) break;
            continue;
        }
        uint64_t l = 0; shift = 0;
        while (bp < bend) {
            uint8_t b = *bp++;
            l |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        if ((uint64_t)(bend - bp) < l) break;
        const uint8_t *cp = bp, *cend = bp + l;
        bp += l;

        if (fld != 1) continue;  /* not a ChannelSettings */

        /* Walk the ChannelSettings fields directly. psk = field 2,
         * name = field 3; everything else gets share_skip'd. */
        uint8_t  psk[32]; size_t psk_len = 0;
        char     name[32]; name[0] = 0;
        while (cp < cend) {
            uint64_t t2 = 0; int s2 = 0;
            while (cp < cend) {
                uint8_t b = *cp++;
                t2 |= (uint64_t)(b & 0x7f) << s2;
                if (!(b & 0x80)) break;
                s2 += 7;
            }
            uint32_t f2 = (uint32_t)(t2 >> 3);
            uint32_t w2 = (uint32_t)(t2 & 0x7);
            if (w2 != 2) {
                if (share_skip(&cp, cend, w2) < 0) break;
                continue;
            }
            uint64_t l2 = 0; s2 = 0;
            while (cp < cend) {
                uint8_t b = *cp++;
                l2 |= (uint64_t)(b & 0x7f) << s2;
                if (!(b & 0x80)) break;
                s2 += 7;
            }
            if ((uint64_t)(cend - cp) < l2) break;
            if (f2 == 2 && l2 <= sizeof(psk)) {
                memcpy(psk, cp, l2); psk_len = (size_t)l2;
            } else if (f2 == 3 && l2 < sizeof(name)) {
                memcpy(name, cp, l2); name[l2] = 0;
            }
            cp += l2;
        }

        if (psk_len > 0) {
            /* simpleN expansion: a one-byte psk N means the stock
             * 16-byte MESH_DEFAULT_PSK with its last byte replaced. */
            if (psk_len == 1) {
                uint8_t expanded[16];
                memcpy(expanded, MESH_DEFAULT_PSK, 16);
                expanded[15] = psk[0];
                if (keyset_add(ks, name[0] ? name : NULL, expanded, 16) == 0) ++added;
            } else if (psk_len == 16 || psk_len == 32) {
                if (keyset_add(ks, name[0] ? name : NULL, psk, psk_len) == 0) ++added;
            }
        }
    }
    return added;
}
