#!/usr/bin/env bash
# Run the engine against every dataset in data/ and diff stdout (and stderr,
# where a .err fixture exists) against the expected output. Exits non-zero on
# any mismatch so it can gate CI / the Makefile.
set -uo pipefail

bin="${1:-./build/matching_engine}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
data_dir="$script_dir/../data"

if [[ ! -x "$bin" ]]; then
    echo "error: engine binary not found/executable at '$bin' (build it first)" >&2
    exit 2
fi

fail=0
for infile in "$data_dir"/*.in; do
    name="$(basename "$infile" .in)"
    exp_out="$data_dir/$name.out"
    exp_err="$data_dir/$name.err"
    got_out="$(mktemp)"
    got_err="$(mktemp)"

    "$bin" < "$infile" > "$got_out" 2> "$got_err"

    if diff -u "$exp_out" "$got_out" >/dev/null 2>&1; then
        echo "  ok   $name (stdout)"
    else
        echo "  FAIL $name (stdout)"
        diff -u "$exp_out" "$got_out" || true
        fail=1
    fi

    if [[ -f "$exp_err" ]]; then
        if diff -u "$exp_err" "$got_err" >/dev/null 2>&1; then
            echo "  ok   $name (stderr)"
        else
            echo "  FAIL $name (stderr)"
            diff -u "$exp_err" "$got_err" || true
            fail=1
        fi
    fi

    rm -f "$got_out" "$got_err"
done

if [[ "$fail" -eq 0 ]]; then
    echo "All dataset checks passed."
fi
exit "$fail"
