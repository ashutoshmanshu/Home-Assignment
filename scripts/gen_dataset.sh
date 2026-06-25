#!/usr/bin/env bash
# Generate a pseudo-random stream of order messages for load/perf testing.
# Prices sit in a narrow band so a large fraction of orders cross and match,
# exercising the matching path rather than just inserts.
#
# Usage: gen_dataset.sh [count] [seed]
set -euo pipefail

count="${1:-100000}"
seed="${2:-1}"

awk -v n="$count" -v seed="$seed" 'BEGIN {
    srand(seed);
    id = 1;
    live = 0;
    for (i = 0; i < n; i++) {
        if (rand() < 0.10 && live > 0) {
            # Cancel a previously added order id.
            print "1," ids[int(rand() * live)];
        } else {
            side = (rand() < 0.5) ? 0 : 1;
            qty = 1 + int(rand() * 100);
            price = 950 + int(rand() * 100);   # band 950..1049 -> frequent crossing
            print "0," id "," side "," qty "," price;
            ids[live++] = id;
            id++;
        }
    }
}'
