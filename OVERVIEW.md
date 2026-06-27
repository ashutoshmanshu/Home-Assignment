# Order Matching Engine — TL;DR

Price–time (FIFO) limit order book. Reads order messages on `stdin`, matches
crossing orders, writes trades/fills to `stdout`, errors to `stderr`. C++17,
Linux, no third-party deps.

📖 Full details: [README.md](README.md) · 🖼️ Diagrams: [ARCHITECTURE.md](ARCHITECTURE.md)

## Build & run

```bash
make            # -> build/matching_engine
make all-checks # unit tests + dataset golden-file checks
./build/matching_engine < data/example.in
```

Prebuilt static Linux binary: [bin/matching_engine](bin/matching_engine).

## Protocol

```
in   0,id,side,qty,price   (add)        side: 0=buy 1=sell
     1,id                  (cancel)
out  2,qty,price           (trade)
     3,id                  (fully filled)     4,id,qty   (partially filled)
```
Per matched pair, in order: **Trade → aggressive fill → resting fill**; trade at the resting price.

## How it works (one breath)

- **Allocation-free hot path** — order pool + intrusive FIFO lists (no `new`/`delete`).
- **O(1) best price** — flat array of price levels + bit-scanned occupancy bitmap (no tree); ordered-map fallback for out-of-band prices.
- **O(1) cancel** — open-addressed `OrderId → slot` index.
- **Single-threaded book** for deterministic price-time priority; **lock-free SPSC ring** only at the I/O edge (`--pipeline`).

## Numbers (dev container)

```
add ~74ns · cancel ~57ns · match ~159ns (amortized)
~1.12M msg/s single  ·  ~1.85M msg/s --pipeline
```
