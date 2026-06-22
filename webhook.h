/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * Opt-in webhook sink. POSTs the JSON for selected event types to an
 * operator-configured URL on a background thread so it cannot stall
 * decode or dashboard publishing.
 *
 * Enabled with --webhook-url=URL. Default event allowlist is a small
 * curated set (high-signal events: PSK_DISCOVERED, OFF_GRID_LORA,
 * GEOFENCE_ENTRY, GEOFENCE_EXIT). Override with --webhook-on=A,B,C.
 * Per-publish timeout is --webhook-timeout-ms=N (default 1000).
 *
 * Bounded queue: webhook_publish() drops when the queue is full and
 * bumps a counter the dashboard can read. The decode path never blocks
 * on HTTP.
 */

#ifndef WEBHOOK_H
#define WEBHOOK_H

#include <stddef.h>
#include <stdint.h>

/* Start the worker thread. `url` must outlive the program (we store
 * the pointer, no copy). `event_csv` is a comma-separated list of
 * event names to allow; NULL or "" means use the safe default
 * allowlist. `timeout_ms` is the per-POST timeout (clamped 100..30000).
 * Returns 0 on success. Safe to call once at startup. */
int  webhook_init(const char *url, const char *event_csv, int timeout_ms);

/* Stop the worker thread. Drains nothing; in-flight requests time
 * out per their existing timeout. Safe to call at shutdown. */
void webhook_stop(void);

/* Non-blocking enqueue. `event_name` is checked against the allowlist;
 * if it doesn't match, returns silently and counts nothing. If it
 * matches and the queue has room, copies the JSON line and bumps
 * webhook_queued_total(). Otherwise bumps webhook_dropped_total(). */
void webhook_publish(const char *event_name, const char *json, size_t len);

/* Counters for the dashboard / stats heartbeat. */
uint64_t webhook_queued_total(void);
uint64_t webhook_sent_total(void);
uint64_t webhook_dropped_total(void);
uint64_t webhook_failed_total(void);  /* enqueued but POST failed */

#endif
