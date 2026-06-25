#!/usr/bin/env bash
# Crude end-to-end throughput benchmark: generate N messages, feed them through
# the engine (output discarded), and report wall-clock time and message rate.
#
# Usage: bench.sh [engine_binary] [count]
set -euo pipefail

bin="${1:-./build/matching_engine}"
count="${2:-1000000}"
script_dir="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -x "$bin" ]]; then
    echo "error: engine binary not found/executable at '$bin' (build it first)" >&2
    exit 2
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

echo "Generating $count messages..."
bash "$script_dir/gen_dataset.sh" "$count" > "$tmp"

echo "Running engine (stdout + stderr discarded)..."
start="$(date +%s.%N)"
"$bin" < "$tmp" > /dev/null 2>/dev/null
end="$(date +%s.%N)"

awk -v s="$start" -v e="$end" -v c="$count" 'BEGIN {
    t = e - s;
    printf "Processed %d messages in %.3fs (~%.0f msg/s)\n", c, t, (t > 0 ? c / t : 0);
}'
