/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * meshtastic.org/e/ channel-share URL decoder.
 *
 * Extracted from web.c so tests can exercise the parser without
 * pulling in the socket / SSE / HTTP surface. The wire format is
 * the upstream firmware ChannelSet protobuf; see share_url.c for
 * details.
 */

#ifndef SHARE_URL_H
#define SHARE_URL_H

#include "keyset.h"

/* Decode a meshtastic.org/e/ URL (or a raw base64-url payload) and
 * add each parsed channel to the keyset. Returns the number of
 * channels added, or -1 on parse error. */
int decode_channel_share(keyset_t *ks, const char *url_or_b64);

#endif
