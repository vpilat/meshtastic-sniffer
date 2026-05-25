#!/usr/bin/env bash
# Payload-length x CFO sweep at SF9 (matches the live MediumFast traffic
# the B205 sees). Goal: determine whether payload-CRC failure scales
# with payload duration -- i.e. whether residual CFO/clock drift
# accumulates across long payloads after the header is locked in.
#
# Pre-fix (sign asymmetry) baseline:  expectation is all sweep cells pass
# on synthetic, since the synthetic has zero SFO and the post-CFO-fix
# decoder handles +/-15 kHz CFO. If any synthetic cell fails, it's a
# real bug we can chase deterministically.
#
# Then comparing to live B205 (which shows roughly 1 in 10 SF9 frames
# CRC-passing) tells us: synthetic clean -> the live failure is in the
# real-RF regime (SFO, multipath, dynamic CFO), not a static decoder bug.
#
# Usage:
#   tests/length_cfo_sweep.sh
#
# Copyright (c) 2026 CEMAXECUTER LLC
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOL="$REPO/build/test_oversample_self"
GEN="$REPO/tools/gen_meshtastic_iq.py"

[ -x "$TOOL" ] || { echo "build test_oversample_self first" >&2; exit 1; }
[ -f "$GEN"  ] || { echo "missing $GEN" >&2; exit 1; }

WORK="$(mktemp -d -t lensweep-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Payload lengths to test, in bytes of text (excluding 16-byte radio
# header + AES envelope overhead). Inner envelope = 2 (port tag+val) +
# 1 (payload tag) + 1-or-2 (varint len) + N (text). So text=8 -> total
# ~28 bytes; text=170 -> total ~190 bytes.
LENGTHS=(8 30 60 90 120 150 170)

# CFO values in Hz. Stay within the +/-15 kHz region the synthetic
# sweep already exercises. Negative side caught the prior sign bug.
CFOS=(-10000 0 10000)

# Preset: SF9/CR5/BW250 = MediumFast, what the connected node + the
# live B205 capture both saw.
SF=9
CR=5
BW=250000
OS=4
SAMP_RATE=$((OS * BW))

build_text() {
    local n="$1"
    python3 - "$n" <<'PY'
import sys
n = int(sys.argv[1])
parts = []
parts.append('HEAD-')
parts.append(''.join(chr(0x41 + i % 26) for i in range(40)))
parts.append('-MID1-')
parts.append(''.join(chr(0x30 + i % 10) for i in range(40)))
parts.append('-MID2-')
parts.append(''.join(chr(0x61 + i % 26) for i in range(40)))
parts.append('-T@!#$%^&*()_+=[]{}|;,./?-')
parts.append('END~')
text = ''.join(parts)
print(text[:n], end='')
PY
}

echo
printf "  %-9s |" "len-bytes"
for cfo in "${CFOS[@]}"; do printf " %+8d Hz" "$cfo"; done
echo
echo "  -----------+$(for _ in "${CFOS[@]}"; do printf -- "------------"; done)"

declare -i any_fail=0
for textlen in "${LENGTHS[@]}"; do
    TEXT="$(build_text "$textlen")"
    OUT="$WORK/synth_${textlen}.cf32"
    python3 "$GEN" --out "$OUT" --text "$TEXT" --channel LongFast \
        --sf "$SF" --cr "$CR" --bw "$BW" --samp-rate "$SAMP_RATE" \
        > "$WORK/gen_${textlen}.log" 2>&1 \
        || { echo "gen failed at textlen=$textlen"; cat "$WORK/gen_${textlen}.log"; exit 1; }
    # Total payload bytes from generator's "frame: N bytes" line.
    TOTAL="$(grep '^frame:' "$WORK/gen_${textlen}.log" | awk '{print $2}')"
    printf "  %5s/%-3s |" "$TOTAL" "$textlen"
    for cfo in "${CFOS[@]}"; do
        "$TOOL" --file="$OUT" --fmt=cf32 --rate="$SAMP_RATE" --no-ddc \
            --bw="$BW" --sf="$SF" --cr="$CR" --os="$OS" --duration=0 \
            --cfo="$cfo" > /dev/null 2> "$WORK/run_${textlen}_${cfo}.log"
        # Parse the SF9 column (field 4 after stage name -> column 5
        # because there are leading spaces; awk treats stage as $1).
        # The SF-column ordering is SF7 SF8 SF9 SF10 SF11 SF12.
        crcp=$(awk '/payload_crc_pass/  { print $4 }' "$WORK/run_${textlen}_${cfo}.log")
        crcf=$(awk '/payload_crc_fail/  { print $4 }' "$WORK/run_${textlen}_${cfo}.log")
        hdr=$(awk  '/header_checksum_pass/ { print $4 }' "$WORK/run_${textlen}_${cfo}.log")
        : "${crcp:=?}"; : "${crcf:=?}"; : "${hdr:=?}"
        if [ "$crcp" = "1" ] && [ "$crcf" = "0" ]; then
            printf "  %10s" "OK"
        elif [ "$hdr" = "0" ]; then
            printf "  %10s" "hdr-fail"
            any_fail+=1
        else
            printf "  %10s" "crc-fail"
            any_fail+=1
        fi
    done
    echo
done
echo
if [ "$any_fail" -eq 0 ]; then
    echo "  matrix all-OK: synthetic SF9 decodes at every (length, CFO) cell."
    echo "  -> long-payload + CFO is fine on clean IQ; live failures are"
    echo "     real-RF-only (SFO, multipath, dynamic carrier drift)."
    exit 0
else
    echo "  matrix has $any_fail failing cell(s) on clean synthetic IQ;"
    echo "  reproducible drift bug independent of real-RF conditions."
    exit 1
fi
