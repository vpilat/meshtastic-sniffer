# meshtastic-sniffer architecture

Single-binary wideband Meshtastic LoRa receiver. One SDR -> one wide IQ stream -> polyphase channelizer -> N parallel LoRa decoders -> AES-CTR + protobuf decode -> 5 output sinks.

## Pipeline

```
   SDR / VITA-49 / IQ file
            |
            v   sample_buf_t (int8 or float complex IQ)
        push_samples()
            |
            +-- channelizer_process_*()
            |   one or more pfb_t instances (one per bw_hz/os_factor group),
            |   critically-sampled M-channel polyphase filterbank:
            |     1. pre-shift NCO aligns input bin grid to channel grid
            |     2. forward commutator distributes input across M branches
            |     3. per-branch length-L FIR (Hamming-windowed sinc prototype)
            |     4. forward M-point FFT
            |     5. emit Y[bin] -> each registered bin's sink list
            |        |
            |        v   per-channel baseband at Fs/M
            |    on_channel_baseband() -> lora_decoder_feed()
            |        |
            |        v   2^SF samples per LoRa symbol
            |    [dechirp * conj(upchirp), N-point FFTW3f, argmax bin]
            |        |
            |        v   IDLE -> PREAMBLE_OK -> HEADER -> PAYLOAD -> DELIVER
            |    [Gray, diagonal deinterleave, Hamming(8,4), dewhiten, CRC16]
            |        |
            |        v   raw bytes (16-byte radio header + ciphertext)
            |    on_lora_frame() -> dedup ring (PFB bin-leakage filter)
            |        |
            |        v   (best replica picked at window expiry by drainer)
            |    mesh_packet_decode_with_radio()
            |        |
            |        v   channel-hash dispatch -> 1..few candidate keys
            |        |   -> AES-CTR decrypt -> protobuf parse_data_envelope
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
            +-- scanner_feed_*()  (when --scan / --scan-and-decode / --alert-off-grid)
                     |
                     v   N-point FFT, EWMA per bin, peakfind @ 4 Hz
                 OFF_GRID_LORA event with occupied-BW estimate,
                 only when peak isn't on the configured grid
                 and bandwidth >= 50 kHz (LoRa minimum is 62.5 kHz)
```

## File map

| File | Lines | Purpose |
|---|---:|---|
| main.c | 1289 | entry point, channel-set builder, signal handling, dedup drainer, stats heartbeat |
| options.{c,h} | 528 | full CLI parser, shared runtime state, per-backend gain controls |
| meshtastic.{c,h} | 350 | regions, presets, port enum, channel-hash, default PSK, AES nonce layout |
| channelizer.{c,h} | 372 | groups channels by `(bw_hz, os_factor)`, one PFB per group, atomic n_channels for runtime add |
| pfb.{c,h} | 405 | critically-sampled M-channel polyphase filterbank: pre-shift NCO + forward commutator + per-branch FIR + forward FFT |
| lora.{c,h} | 1269 | LoRa CSS demod (state machine + Gray + Hamming + dewhiten + CRC + FFTW3f dechirp) -- bit-level stages ported from gr-lora_sdr |
| keyset.{c,h} | 341 | multi-key dispatch, channel-hash buckets, rwlock for runtime add |
| protobuf.{c,h} | 122 | minimal varint/tag/wire-type reader |
| mesh_packet.{c,h} | 379 | radio header, AES-CTR via OpenSSL, multi-key try, Data envelope |
| mesh_decoders.{c,h} | 867 | 12 per-port decoders |
| node_db.{c,h} | 95 | id -> name cache, used by CoT for callsigns |
| feed.{c,h} | 514 | JSON serialiser, fanout to stdout/UDP/MQTT/ZMQ/CoT/web |
| mqtt.c | 85 | libmosquitto sink (stub if not present) |
| zmq_pub.c | 73 | libzmq PUB sink (stub if not present) |
| cot.{c,h} | 293 | CoT XML for ATAK PLIs and POSITION packets, multicast, runtime endpoint |
| scanner.{c,h} | 369 | wideband FFT, off-grid energy detector with occupied-BW estimate, spectrum snapshot |
| web.{c,h} | 1587 | HTTP+SSE server, embedded Leaflet/Activity/Topology dashboard, /api/* endpoints |
| sigmf.{c,h} | 136 | .sigmf-meta reader for --file auto-config |
| file_src.{c,h} | 135 | CI8/CI16/CF32 IQ replay |
| hackrf/bladerf/rtlsdr/soapysdr/sdrplay/airspy/usrp/vita49.c | 2097 | 8 SDR backends |
| simd_*.{c,h} | 1649 | AVX2/SSE4.2/NEON/generic kernels (one per ISA tier, runtime-detected) |
| blocking_queue.h, fair_lock.h | 848 | MIT-licensed primitives (vendored, Felipe Kersting) |

Build is warning-free with `-Wall -Wextra -Werror=implicit-function-declaration`. AddressSanitizer + UndefinedBehaviorSanitizer + ThreadSanitizer all clean against the smoke-test suite.

## Polyphase channelizer

The channelizer in `pfb.c` is a textbook critically-sampled decimator-by-M PFB:

1. Design a prototype lowpass `h[0..L*M-1]` with cutoff `1/(2M)` (Hamming-windowed sinc, L = 12 typical, ~-43 dB sidelobes)
2. Decompose into M branches: `h_p[i][k] = h[k*M + i]`
3. Per cycle of M input samples: forward commutator distributes across branches, each branch FIRs against its polyphase row, then a single M-point FFT produces all M output bins at `Fs/M` rate

Per-input-sample cost is `O(L + log M)` ops, vs `O(M)` for the per-channel-cascade DDC alternative. At M=80, that's ~7 ops/sample vs 80 -- the source of the wideband throughput. AVX2 SIMD on the FIR + OpenMP parallel-for over PFB groups (one per `(bw_hz, os_factor)` combination) lets the binary process 30+ Msps real-time on a single i7 core.

Channels are bound to PFB output bins via `pfb_register_bin` -- multiple decoders may bind to the same bin (a bin's callback list is a tiny linked list). The pre-shift NCO multiplies input by `exp(-j*2*pi*pre_shift_hz/Fs * n)` so the FFT's bin-0 lines up exactly with the configured channel grid.

## PFB bin-leakage dedup

Each LoRa transmission lights up roughly 30 leakage replicas across adjacent PFB output bins. Per-replica bit errors are independent across bins; the cleanest decode is whichever bin had the highest SNR. Picking the first replica that arrives is random; picking the highest-SNR replica is deterministically the best-quality copy.

Implementation in `main.c`:

1. New replica arrives -> compute payload fingerprint (64-bit XOR-fold of payload bytes; bit-error replicas produce near-identical fingerprints, real transmissions produce uncorrelated ones)
2. Find an existing cluster within Hamming distance 14 (real transmissions typically differ by ~32 bits; bit-error replicas by 1-5)
3. If found and this replica has higher SNR: replace cluster's stored best
4. If not found: open a new cluster, schedule emit at `now + 30 ms`
5. Drainer thread polls every 5 ms; when a cluster's emit time passes, hand its best-stored replica to `mesh_packet_decode_with_radio` -- single attempt, with the cleanest SNR copy = best decrypt odds

Density-safe: 100 simultaneous distinct transmissions create 100 distinct clusters (random fingerprints don't collide within 14 Hamming bits). The 30 ms window swallows leakage cluster from a single chirp without merging adjacent unrelated transmissions.

Result: one JSON line per real transmission, regardless of how many leakage replicas the channelizer emitted.

## Threading

```
main thread:
  -- options_parse()
  -- simd_init()
  -- build_channel_set() / keyset / channelizer / scanner / feed / web init
  -- create dedup_drainer_thread (5 ms tick, batches expirations under one lock)
  -- create stats_thread (1 s tick + 5 s heartbeat to stderr + STATS SSE)
  -- create input_thread (one of: hackrf_stream_thread, ..., file_src_thread, vita49_thread)
  -- poll loop: while (running) usleep(100000)
  -- pthread_join + cleanup

input_thread:
  drains the SDR (or file / VITA-49 socket) into sample_buf_t,
  calls push_samples() in this thread.

push_samples() (called from input_thread, not its own thread):
  -- channelizer_process_int8/float
       -> pfb_process per group (parallel-for if multiple groups)
            -> per-bin sink callbacks -> on_channel_baseband
                 -> lora_decoder_feed -> state machine -> on_lora_frame
                      -> dedup ring insert (under g_dedup_mu)
  -- scanner_feed_int8/float -> wideband FFT -> peakfind -> on_off_grid_discovery

dedup_drainer_thread:
  every 5 ms wakes up, batches up to 64 expired clusters under one
  lock acquire, then for each: mesh_packet_decode_with_radio ->
  on_mesh_event -> feed_publish_event -> stdout / UDP / MQTT / ZMQ /
  CoT / web SSE.

web thread:
  accept() loop, single-threaded request handling.
  Serves GET / (dashboard HTML), GET /events (SSE upgrade),
  POST /api/keys, POST /api/share-url, POST /api/extra-freq, POST /api/cot-multicast.
  SSE clients are kept in a small list (max 8) and broadcast to from
  feed_publish_event() via web_publish_line().

stats thread:
  every 1 s wakes up; every 5 s prints stderr heartbeat and pushes a
  STATS SSE event so the dashboard's persistent header has live counts.
```

All cross-thread state uses one of:

- `volatile sig_atomic_t running` (set by signal handler, polled by every thread)
- `__atomic_*` for hot counters (g_samples_total, g_frames_total, ...) and `channelizer.n_channels` (release/acquire ordering for race-free runtime channel addition)
- `pthread_rwlock_t` on `keyset_t` (write-lock around add, read-lock during decode lookup-and-decrypt)
- `pthread_mutex_t` on web SSE client list, cot endpoint state, node_db, dedup ring

## Multi-key dispatch (no per-packet brute-force)

Meshtastic frames carry a 1-byte `channel` field in the radio header equal to `xorHash(channel_name) ^ xorHash(psk)`. At `keyset_add` time the same hash is precomputed for each loaded `(name, psk)` pair and bucketed into `buckets[256]`. Per packet `header.channel` reads, looks up the bucket, and tries the (typically 1, occasionally 2) candidate keys. A successful protobuf parse is the confirmation. **Adding more keys does not slow per-packet decode** -- steady-state cost is one AES-CTR op + one protobuf parse, regardless of how many keys are loaded.

Bucket capacity is 7 collisions per single-byte hash. With well-distributed keys ~50 entries fit before the first bucket fills.

## Runtime mutability

The web Config tab can add keys, paste channel-share URLs, add extra-frequency decoder slots, and change the CoT multicast destination -- all without restarting the binary.

- `keyset_t` has a `pthread_rwlock_t` -- `keyset_add` takes the write lock briefly, `keyset_lookup` callers hold the read lock for the duration of lookup-and-decrypt. New keys take effect on the very next packet.
- `channelizer_t::n_channels` is `__atomic_store_n`-released after the new channel pointer is fully initialised; the hot loop does `__atomic_load_n`-acquire. Newly-added channels start receiving samples on the next channelizer_process_* call.
- `cot.c` keeps the multicast destination behind a mutex; `cot_set_endpoint(host, port)` reopens the socket atomically. Empty body to `POST /api/cot-multicast` disables CoT entirely.
- `scanner_t` known-grid array is replaced wholesale on each `scanner_set_known_grid` call; new extra-freq slots immediately start being excluded from the off-grid alert path.

## Validation

What's verified:

- **Channelizer routing** (smoke-tested in `tests/test_smoke.sh`): synthetic tone at 902.625 MHz lights up channel 2 at -2.13 dB inside a 20 MHz capture
- **AES + multi-key + protobuf round-trip**: synthesized `TEXT_MESSAGE_APP` packet decodes back to "Hello"
- **LoRa bit-level stages**: hard-decode Hamming, deinterleave, gray, dewhiten verified bit-exact against gr-lora_sdr stage outputs (fixtures at `tests/fixtures/lora_stages/`)
- **End-to-end LoRa decode**: real-radio HackRF capture on US LongFast / ShortFast / ShortTurbo decrypts back to plaintext text+position+nodeinfo
- **SigMF auto-config**: rate / freq / datatype picked up from `.sigmf-meta` sibling
- **All 4 web `/api/*` endpoints**: round-trip via curl
- **STATS SSE event**: arrives at the dashboard within one heartbeat
- **AddressSanitizer + UndefinedBehaviorSanitizer**: zero findings on the smoke-test suite (selftest, file replay, web API hits, key adds)
- **ThreadSanitizer**: zero data races under concurrent `/api/keys` POSTs hitting while the demod thread is in `keyset_lookup`
- **Polyphase channelizer throughput**: 30+ Msps sustained on a single i7 core, with 41-channel + multi-preset US grid

Known runtime concerns deliberately not blocked-on:

- SDRplay's proprietary `libsdrplay_api.so.3` has a double-free in its `sdrplay_api_Close()` exit handler (third-party; surfaces only at process exit when `--list` enumerated SoapySDR drivers). Not actionable from our side.

## Self-test entry points

`./meshtastic-sniffer --selftest` runs two synthesized smoke checks (channelizer + AES end-to-end). `bash tests/test_smoke.sh` adds SigMF auto-config, `--list`, web `/api/*` round-trip, STATS SSE heartbeat, and stats heartbeat. Both run clean under sanitizers (`-fsanitize=address,undefined`).

`MESHTASTIC_LORA_TRACE=1` enables a per-symbol state-machine trace on stderr, useful for debugging decode against a known-good reference.
