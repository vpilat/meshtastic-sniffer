#!/usr/bin/env bash
# Per-SF x CFO matrix probe.
#
# Tests whether the SF9 |cfo_frac| > ~0.35 cliff is unique to SF9 or
# shared across SFs at the corresponding CFO Hz values (different SFs
# have different bin widths, so the same CFO Hz lands at different
# cfo_frac magnitudes per SF):
#
#   SF7  BW250 -> 1 bin = 1953 Hz
#   SF8  BW250 -> 1 bin =  977 Hz
#   SF9  BW250 -> 1 bin =  488 Hz
#   SF10 BW250 -> 1 bin =  244 Hz
#   SF11 BW250 -> 1 bin =  122 Hz
#
# For each SF, sweep CFO values that span |cfo_frac| from 0 to ~0.45
# at that SF's bin width. If the cliff is a shared chirp/twiddle
# numerical bug, it should appear at the corresponding |cfo_frac|
# range for every SF -- just at different CFO Hz numbers.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOL="$REPO/build/test_oversample_self"
GEN="$REPO/tools/gen_meshtastic_iq.py"
[ -x "$TOOL" ] || { echo "build test_oversample_self first" >&2; exit 1; }

WORK="$(mktemp -d -t allsfcfo-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

BW=250000
CR=5
OS=4
SAMP_RATE=$((OS * BW))   # post-DDC rate (= channel_rate at --no-ddc)

# Short payload (fast to generate + decode). 30-byte text -> ~50 byte total.
TEXT="HEAD-ABCDEFGHIJKLMNOPQRSTUVWXYZ"

# cfo_frac targets to probe. Same fractional bin offsets across SFs;
# the actual Hz value differs by SF because bin_width = BW / 2^SF.
FRACS=(0 0.10 0.20 0.30 0.35 0.40 0.45)

# Per-SF Hz values for each cfo_frac target, plus negative side. We
# pick CFO = frac * bin_width. We want both signs because the SF9
# matrix showed asymmetric thresholds (positive failed at frac>0.38,
# negative at >0.43 -ish).

echo
printf "  %-3s | %4s |" "SF" "bin"
for fr in "${FRACS[@]}"; do printf " %+6s |" "+${fr}"; done
echo
for fr in "${FRACS[@]:1}"; do printf "          |" ; printf " %+6s |" "-${fr}"; done
echo
echo "  ---------+$(printf -- "--------+%.0s" "${FRACS[@]}")"

any_fail=0
for SF in 7 8 9 10 11; do
    BIN_HZ=$(python3 -c "print(int($BW / 2**$SF))")
    # Generate the synthetic for this SF once (CFO is injected at the
    # decoder, so one synthetic file per SF is enough).
    OUT="$WORK/synth_sf${SF}.cf32"
    python3 "$GEN" --out "$OUT" --text "$TEXT" --channel LongFast \
        --sf "$SF" --cr "$CR" --bw "$BW" --samp-rate "$SAMP_RATE" \
        > "$WORK/gen_sf${SF}.log" 2>&1 \
        || { echo "gen failed at SF=$SF"; cat "$WORK/gen_sf${SF}.log"; exit 1; }

    printf "  SF%-2d  | %4d |" "$SF" "$BIN_HZ"
    for fr in "${FRACS[@]}"; do
        cfo=$(python3 -c "print(int(round($fr * $BIN_HZ)))")
        out=$("$TOOL" --file="$OUT" --fmt=cf32 --rate="$SAMP_RATE" --no-ddc \
            --bw="$BW" --sf="$SF" --cr="$CR" --os="$OS" --duration=0 \
            --cfo="$cfo" 2>&1)
        crcp=$(echo "$out" | awk -v sf="$SF" '/payload_crc_pass/  {
            col = sf - 7 + 2 ; print $col
        }')
        crcf=$(echo "$out" | awk -v sf="$SF" '/payload_crc_fail/  {
            col = sf - 7 + 2 ; print $col
        }')
        if [ "${crcp:-0}" = "1" ] && [ "${crcf:-0}" = "0" ]; then
            printf "   OK   |"
        else
            printf "  fail  |"
            any_fail=1
        fi
    done
    echo
    # Negative side
    printf "         |      |"
    for fr in "${FRACS[@]:1}"; do
        cfo=$(python3 -c "print(-int(round($fr * $BIN_HZ)))")
        out=$("$TOOL" --file="$OUT" --fmt=cf32 --rate="$SAMP_RATE" --no-ddc \
            --bw="$BW" --sf="$SF" --cr="$CR" --os="$OS" --duration=0 \
            --cfo="$cfo" 2>&1)
        crcp=$(echo "$out" | awk -v sf="$SF" '/payload_crc_pass/  {
            col = sf - 7 + 2 ; print $col
        }')
        crcf=$(echo "$out" | awk -v sf="$SF" '/payload_crc_fail/  {
            col = sf - 7 + 2 ; print $col
        }')
        if [ "${crcp:-0}" = "1" ] && [ "${crcf:-0}" = "0" ]; then
            printf "   OK   |"
        else
            printf "  fail  |"
            any_fail=1
        fi
    done
    echo
done

echo
echo "  Each cell: CFO injection at (frac * bin_width_hz) for the row's SF."
echo "  +0 column verifies the SF decodes at zero CFO."
echo
if [ "$any_fail" -eq 0 ]; then
    echo "  All SFs decode across the |cfo_frac| sweep. Bug not reproducible at these settings."
else
    echo "  At least one SF/CFO cell fails. Pattern in the table tells us if the cliff is shared."
fi
exit $any_fail
