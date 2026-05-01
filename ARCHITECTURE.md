# meshtastic-sniffer architecture

Single-binary wideband Meshtastic LoRa receiver. One SDR -> one wide IQ stream -> N parallel LoRa decoders -> AES-CTR + protobuf decode -> 5 output sinks.

## Pipeline

```
   SDR / VITA-49 / IQ file
            |
            v   sample_buf_t (int8 or float complex IQ)
        push_samples()
            |
            +-- channelizer_process_*()  (per-channel NCO+FIR DDC)
            |        |
            |        v   float complex baseband, rate = bw_hz
            |    on_channel_baseband() -> lora_decoder_feed()
            |        |
            |        v   2^SF samples per LoRa symbol
            |    [dechirp * conj(upchirp), N-point FFTW3f, argmax bin]
            |        |
            |        v   IDLE -> PREAMBLE_OK -> HEADER -> PAYLOAD -> DELIVER
            |    [Gray, diagonal deinterleave, Hamming(8,4), dewhiten, CRC16]
            |        |
            |        v   raw bytes (16-byte radio header + ciphertext)
            |    on_lora_frame() -> mesh_packet_decode_with_meta()
            |        |
            |        v   channel-hash dispatch -> 1 candidate key -> AES-CTR decrypt
            |    parse_data_envelope() (protobuf)
            |        |
            |        v   mesh_event_t (port, payload, RSSI/SNR, channel name)
            |    on_mesh_event() -> feed_publish_event()
            |        |
            |        +-- stdout JSON
            |        +-- UDP feed (--feed=HOST:PORT, repeatable)
            |        +-- MQTT (--mqtt=HOST[:PORT], topic meshtastic/<station>)
            |        +-- ZMQ PUB (--zmq=tcp://*:7008)
            |        +-- CoT XML multicast (--cot-multicast=GROUP:PORT)
            |        +-- Web SSE (--web=PORT, all events tee'd to /events)
            |
            +-- scanner_feed_*()  (when --scan-and-decode / --alert-off-grid / --web-spectrum)
                     |
                     v   N-point FFT, EWMA per bin, peakfind @ 4 Hz
                 OFF_GRID_LORA event when peak isn't on the grid
                 SPECTRUM event @ 1 Hz when --web-spectrum on (256-bin downsample)
```

## File map

| File | Lines | Purpose |
|---|---:|---|
| main.c | ~700 | entry point, channel-set builder, signal handling, stats heartbeat |
| options.{c,h} | ~400 | full CLI parser, shared runtime state |
| meshtastic.{c,h} | ~250 | regions, presets, port enum, channel-hash, default PSK |
| channelizer.{c,h} | ~210 | per-channel NCO+FIR DDC, atomic n_channels for runtime add |
| lora.{c,h} | ~440 | LoRa CSS demod (state machine + Gray + Hamming + dewhiten + CRC + FFTW3f dechirp) |
| keyset.{c,h} | ~270 | multi-key dispatch, channel-hash buckets, rwlock for runtime add |
| protobuf.{c,h} | ~120 | minimal varint/tag/wire-type reader |
| mesh_packet.{c,h} | ~270 | radio header, AES-CTR via OpenSSL, multi-key try, Data envelope |
| mesh_decoders.{c,h} | ~600 | 12 per-port decoders |
| node_db.{c,h} | ~70 | id -> name cache, used by CoT for callsigns |
| feed.{c,h} | ~430 | JSON serialiser, fanout to stdout/UDP/MQTT/ZMQ/CoT/web |
| mqtt.c | ~80 | libmosquitto sink (stub if not present) |
| zmq_pub.c | ~70 | libzmq PUB sink (stub if not present) |
| cot.{c,h} | ~210 | CoT XML for ATAK PLIs and POSITION packets, multicast, runtime endpoint |
| scanner.{c,h} | ~280 | wideband FFT, off-grid energy detector, spectrum snapshot |
| web.{c,h} | ~700 | HTTP+SSE server, embedded Leaflet dashboard, /api/* endpoints |
| sigmf.{c,h} | ~110 | .sigmf-meta reader for --file auto-config |
| file_src.{c,h} | ~110 | CI8/CI16/CF32 IQ replay |
| hackrf/bladerf/rtlsdr/soapysdr/sdrplay/airspy/usrp/vita49.{c,h} | ~2100 | 8 SDR backends |
| simd_*.{c,h} | ~1250 | AVX2/SSE4.2/NEON/generic kernels (vendored) |
| blocking_queue.h, fair_lock.h | ~850 | MIT-licensed primitives (vendored) |

Total: ~5500 LOC of own code + ~3000 LOC of vendored backends.

## Threading

```
main thread:
  -- options_parse()
  -- simd_init()
  -- build_channel_set() / keyset / channelizer / scanner / feed / web init
  -- create stats_thread (5s stderr heartbeat + 1Hz spectrum SSE)
  -- create input_thread (one of: hackrf_stream_thread, ..., file_src_thread, vita49_thread)
  -- poll loop: while (running) usleep(100000)
  -- pthread_join + cleanup

input_thread:
  drains the SDR (or file / VITA-49 socket) into sample_buf_t,
  calls push_samples() in this thread.

push_samples() (called from input_thread, not its own thread):
  -- channelizer_process_int8/float -> per-channel NCO+FIR -> on_channel_baseband
       -> lora_decoder_feed -> state machine -> on_lora_frame
            -> mesh_packet_decode_with_meta -> on_mesh_event -> feed_publish_event
                 -> stdout / UDP / MQTT / ZMQ / CoT / web SSE
  -- scanner_feed_int8/float -> wideband FFT -> peakfind -> on_off_grid_discovery

web thread:
  accept() loop, single-threaded request handling.
  Serves GET / (dashboard HTML), GET /events (SSE upgrade),
  POST /api/keys, POST /api/share-url, POST /api/extra-freq, POST /api/cot-multicast.
  SSE clients are kept in a small list (max 8) and broadcast to from
  feed_publish_event() via web_publish_line().

stats thread:
  every 100 ms wakes up, every 5s prints stderr heartbeat,
  every 1s pushes a SPECTRUM SSE event when --web-spectrum is on.
```

All cross-thread state uses one of:
- `volatile sig_atomic_t running` (set by signal handler, polled by every thread)
- `__atomic_*` for hot counters (g_samples_total, g_frames_total, etc.) and `channelizer.n_channels` (release/acquire ordering for race-free runtime channel addition)
- `pthread_rwlock_t` on `keyset_t` (write-lock around add, read-lock during decode lookup-and-decrypt)
- `pthread_mutex_t` on `web` SSE client list, `cot` endpoint state, `node_db`

## Multi-key dispatch (no per-packet brute-force)

Meshtastic frames carry a 1-byte `channel` field in the radio header equal to `xorHash(channel_name) ^ xorHash(psk)`. At keyset_add time we precompute the same hash for each loaded `(name, psk)` pair and bucket it into `buckets[256]`. Per packet we read `header.channel`, look up the bucket, and try the (typically 1, occasionally 2) candidate keys. A successful protobuf parse is the confirmation. **Adding more keys does not slow per-packet decode** -- steady-state cost is one AES-CTR op + one protobuf parse, regardless of how many keys are loaded.

Bucket capacity is 7 collisions per single-byte hash. With well-distributed keys you can load ~50 before the first bucket fills. Beyond that, bucket-full additions return -1 (the keyset_parse_csv response surfaces this as `{"added":N}` where N is what actually went in).

## Runtime mutability

The web Config tab can add keys, paste channel-share URLs, add extra-frequency decoder slots, and change the CoT multicast destination -- all without restarting the binary. The implementation:

- `keyset_t` has a `pthread_rwlock_t` -- `keyset_add` takes the write lock briefly, `keyset_lookup` callers hold the read lock for the duration of lookup-and-decrypt. New keys take effect on the very next packet.
- `channelizer_t::n_channels` is `__atomic_store_n` released after the new channel pointer is fully initialised; the hot loop does `__atomic_load_n` acquire. Newly-added channels start receiving samples on the next channelizer_process_* call.
- `cot.c` keeps the multicast destination behind a mutex; `cot_set_endpoint(host, port)` reopens the socket atomically. Empty body to `POST /api/cot-multicast` disables CoT entirely.
- `scanner_t` known-grid array is replaced wholesale on each `scanner_set_known_grid` call; new extra-freq slots immediately start being excluded from the off-grid alert path.

## What's verified vs what's deferred

**Verified** (smoke-tested in `tests/test_smoke.sh`):
- Channelizer routing: synthetic tone at 902.625 MHz lights up channel 2 at -2.13 dB
- AES + multi-key + protobuf round-trip: synthesized TEXT_MESSAGE packet decodes back to "Hello"
- SigMF auto-config picks up rate / freq / datatype from a `.sigmf-meta` sibling
- All 4 web `/api/*` endpoints round-trip
- Spectrum SSE event format and live FFT values
- 72-channel US LongFast stare on a 20 MHz file capture, clean shutdown

**Deferred** (none block v1):
- LoRa CSS demod inline TODOs (sync-word verification, header CRC, fractional CFO, DE for SF11/SF12). The structure is in place; tuning is gated on having real LoRa IQ ground-truth.
- Channelizer filter-tap sharpening (current 63-tap Hamming sinc has ~-6 dB adjacent-channel rejection). Will tune against real captures.
- TAK Server TCP/TLS sink. Multicast covers LAN ops; TLS+mTLS+cert-pinning is v2.
- APRS-IS output. v2.

## Self-test entry points

`./meshtastic-sniffer --selftest` runs two synthesized smoke checks (channelizer + AES end-to-end). `bash tests/test_smoke.sh` adds SigMF auto-config + `--list` + web `/api/*` round-trip.
