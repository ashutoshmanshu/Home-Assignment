# Order Matching Engine (short version)

A price-time (FIFO) limit order book. Reads order messages on `stdin`, matches
crossing orders, writes trades/fills to `stdout` and errors to `stderr`. C++17,
Linux, no third-party deps.

More: [README.md](README.md) for full detail, [ARCHITECTURE.md](ARCHITECTURE.md)
for diagrams, [how-it-works.html](how-it-works.html) for a visual walkthrough.

## Build and run

```bash
make            # -> build/matching_engine
make all-checks # unit tests + dataset golden-file checks
./build/matching_engine < data/example.in
```

A prebuilt static Linux binary is at [bin/matching_engine](bin/matching_engine).

## Protocol

```
in   0,id,side,qty,price   (add)        side: 0=buy 1=sell
     1,id                  (cancel)
out  2,qty,price           (trade)
     3,id                  (fully filled)     4,id,qty   (partially filled)
```

Per matched pair the order is: trade, then aggressive fill, then resting fill.
Trades happen at the resting order's price.

## How it works

- No heap allocation on the hot path: orders come from a pool, levels are
  intrusive linked lists.
- Best price is O(1): price levels are a flat array, with an occupancy bitmap
  bit-scanned to find the best non-empty one. Out-of-band prices use an ordered
  map fallback.
- Cancel is O(1) average via an open-addressed `OrderId -> slot` index.
- The book is single-threaded so price-time priority stays deterministic. The
  only lock-free structure is an SPSC ring at the I/O edge (`--pipeline`).

## Numbers (dev container)

```
add ~74ns, cancel ~57ns, match ~159ns (amortized)
~1.12M msg/s single, ~1.85M msg/s with --pipeline
```
