# Order Matching Engine

A low-latency limit order book / matching engine implementing **price–time (FIFO)
priority**. It reads a stream of order messages from `stdin`, matches crossing
orders, and writes the resulting trade and order-state messages to `stdout`.
Malformed or rejected input is reported to `stderr` and never crashes the
process.

This is an implementation of the Vatic Labs order-matching home assignment
(`Vatic_Order_match_assignment.docx`). C++17, no third-party runtime
dependencies, builds and runs on Linux.

The hot path is **allocation-free** (object pool + intrusive lists), best-bid /
best-ask is **O(1)** (array of price levels + a bit-scanned occupancy bitmap),
and the only place concurrency lives is a **lock-free SPSC ring buffer** at the
I/O boundary — the book itself stays single-threaded for deterministic
price-time priority. See [Architecture](#architecture) and [Performance](#performance).

---

## Quick start

```bash
make            # builds build/matching_engine
make test       # builds and runs the unit-test suite (engine + components)
make datasets   # runs the engine against data/*.in and diffs expected output
make all-checks # test + datasets
make latency    # per-operation latency (p50/p99/p99.9); make latency COUNT=2000000
make bench      # end-to-end throughput; make bench COUNT=5000000
```

Run it directly:

```bash
./build/matching_engine            < data/example.in   # single-threaded (default)
./build/matching_engine --pipeline < data/example.in   # lock-free ingress pipeline
```

A prebuilt **fully static** x86-64 Linux binary is committed at
[bin/matching_engine](bin/matching_engine) (see [Prebuilt binary](#prebuilt-binary)).

### Requirements

A C++17 compiler (`g++` ≥ 7 or `clang++` ≥ 5) and `make`. Tests use a tiny
in-repo framework, so there is nothing to download. Without `make`:

```bash
g++ -std=c++17 -O2 -pthread src/main.cpp src/OrderBook.cpp src/MessageParser.cpp -o matching_engine
```

---

## Message protocol

Each line is a comma-separated message identified by an integer type id.

### Input

| Type | Name              | Format                                   | Example             |
|------|-------------------|------------------------------------------|---------------------|
| `0`  | AddOrderRequest   | `0,orderid,side,quantity,price`          | `0,123,0,9,1000`    |
| `1`  | CancelOrderRequest| `1,orderid`                              | `1,123`             |

`side`: `0` = Buy, `1` = Sell · `quantity`: positive integer · `price`: positive
decimal (see [Prices](#prices-are-fixed-point)).

### Output

| Type | Name                 | Format                  | Meaning                                |
|------|----------------------|-------------------------|----------------------------------------|
| `2`  | TradeEvent           | `2,quantity,price`      | A trade executed                       |
| `3`  | OrderFullyFilled     | `3,orderid`             | Order fully filled and removed         |
| `4`  | OrderPartiallyFilled | `4,orderid,quantity`    | Order partially filled; new open qty   |

For every matched pair the engine emits, in order: the **TradeEvent**, then the
fill message for the **aggressive** order, then the fill message for the
**resting** order. Trades execute at the **resting** order's price.

### Worked example

`data/example.in` is the exact example from the assignment; the engine
reproduces its expected output byte-for-byte:

```
$ ./build/matching_engine < data/example.in
Unknown message type: BADMESSAGE      # <- stderr
2,2,1025
4,1000008,1
3,1000005
2,1,1025
3,1000008
4,1000007,4
```

---

## Architecture

The design follows how a real venue is built: a **single-threaded,
deterministic matching core** with **lock-free queues at the boundaries**.

### Why the book is single-threaded (and what *is* lock-free)

A matching engine must process messages in a defined sequence — price-time
priority and the atomicity of a multi-leg trade (a sweep that touches several
resting orders) only make sense if one message is fully resolved before the
next begins. Making the book itself "lock-free"/concurrent would destroy that
determinism and fairness. So the book core runs on **one thread**; in production
it is pinned to an isolated, busy-polling core.

Concurrency belongs at the **edges**, and that is exactly where the lock-free
structure lives here: [`SpscRing`](src/SpscRing.h), a single-producer /
single-consumer ring buffer that hands parsed commands from the I/O thread to
the matching thread (and, in a full system, trades/market-data back out). It is
cache-line padded to avoid false sharing and publishes indices with
release/acquire ordering — no mutex, no CAS loop. `--pipeline` wires it in:

```
stdin ─▶ [parse+validate thread] ─▶ SpscRing ─▶ [single-writer matching thread] ─▶ stdout
```

`stdout` is written only by the matcher, so output order is identical to the
single-threaded path (verified byte-for-byte).

### The hot path is allocation-free

Heap allocation and cache misses are the real nanosecond killers, so the book
([src/OrderBook.h](src/OrderBook.h) / [.cpp](src/OrderBook.cpp)) uses none on the
steady-state path:

- **Order pool + intrusive FIFOs.** Orders live in one pre-grown `std::vector`
  (`nodes_`); each price level is a FIFO threaded through `prev`/`next` **slot
  indices** inside those nodes. Resting, matching, and cancelling never call
  `new`/`delete`. Filled/cancelled slots return to a free list for reuse — at
  steady state, zero allocation.
- **Array price levels + occupancy bitmap.** Levels are a flat array indexed by
  price tick. A 3-level [`OccupancyBitmap`](src/OccupancyBitmap.h) tracks which
  levels are non-empty, so best-bid / best-ask is a handful of hardware
  bit-scans (`O(1)`) instead of a red-black tree's `O(log L)` pointer walk. The
  array spans a price band centred on the market (the instrument's intraday
  range); prices outside it use an ordered-map fallback, so **any** price is
  handled correctly — correctness never depends on the band, only speed does.
- **Open-addressed id index.** [`IdIndex`](src/IdIndex.h) maps `OrderId → slot`
  with linear probing and backward-shift deletion: one flat array, no per-entry
  node allocation, cache-friendly probes, and `O(1)` average cancel lookup.

### Components

```
src/
  Types.h          OrderId / Quantity / Side / Slot primitives
  Price.h          fixed-point price (parse, compare, format)
  Order.h          a single order
  Message.h        AddOrderRequest / CancelOrderRequest + the type enum
  MessageParser.*  stdin line -> validated request (never throws)
  MessageSink.h    abstract output interface (Trade / Fully / Partially filled)
  StreamSink.h     MessageSink that writes the CSV protocol to an ostream
  OccupancyBitmap.h 3-level bitmap, O(1) lowest/highest occupied index
  IdIndex.h        open-addressing OrderId -> slot map
  SpscRing.h       lock-free single-producer/single-consumer ring buffer
  OrderBook.*      the matching engine core
  main.cpp         stdin/stdout driver (single-threaded or --pipeline)
bench/             latency harness
tests/             tiny framework + engine, parser, price, and component tests
data/              datasets with expected .out / .err fixtures
scripts/           dataset runner, dataset generator, throughput benchmark
```

The key abstraction is **`MessageSink`**: the engine emits structured events
through it instead of touching `stdout`, so the matching logic is I/O-free and
tests assert on the exact event sequence via a `RecordingSink`.

### Prices are fixed-point

The spec says prices are "decimal numbers." Comparing them as `double` is unsafe:
values such as `0.1` are not representable in binary floating point, so two
prices that should be equal can compare unequal and corrupt price priority.
[`Price`](src/Price.h) stores an `int64_t` count of ticks (`1 unit = 10^4
ticks`, 4 decimal places). Comparison is exact integer arithmetic — also faster
than floating point, and it doubles as the price-level array index. Inputs with
more than 4 fractional digits are rejected at parse time rather than rounded.

---

## Performance

Complexity, where `L` = distinct active price levels on a side and `m` = resting
orders an incoming order matches against:

| Operation | Cost | Why |
|-----------|------|-----|
| **Does an AddOrderRequest match?** | **O(1)** | Best opposite level = one bitmap bit-scan; compare to the incoming price. |
| **Executing a match** | **O(m)** | Each fill pops a FIFO head in O(1); an emptied level clears one bitmap bit in O(1). |
| **Removing a filled order** | **O(1)** | Pop list head, return slot to the pool, erase from the id index — all O(1). |
| **Removing a cancelled order** | **O(1)** avg | id-index lookup → unlink via stable slot indices → clear bit if level empties. |
| **Resting a non-matching order** | **O(1)** in-band | Pool alloc (amortized), push FIFO tail, set bitmap bit. Out-of-band: O(log L). |

Memory is O(N) live orders plus a fixed per-side band array (~8 MB/side at the
default `kBandTicks = 2^20`).

### Measured numbers

Indicative figures from the development container (virtualized, **not** tuned
hardware — see caveats). Reproduce with `make latency` and `make bench`.

Per-operation latency (`make latency COUNT=2000000`):

```
add (resting, no match)  amortized= 74 ns/op | p50=150 p99=308 p99.9=476 ns
cancel                   amortized= 57 ns/op | p50=148 p99=319 p99.9=501 ns
add (matches 1 resting)  amortized=159 ns/op | p50=170 p99=391 p99.9=605 ns
```

- **amortized** = total wall time / N (no per-op timer) — the truest per-op cost.
- **p50/p99/p99.9** come from a separate per-op timed loop and include the
  timer's own ~70 ns/op cost (two `clock` calls), so they are an upper bound; the
  multi-ms `max` values are OS preemption outliers that a pinned, isolated core
  eliminates.

End-to-end throughput (`make bench COUNT=5000000`, mixed adds/cancels, heavy
crossing):

```
single-threaded   ~1.12M messages/s
--pipeline         ~1.85M messages/s   (lock-free ring overlaps parse/I/O with matching)
```

### Trade-offs and request-path priorities

- **All three required paths are O(1)/O(1)-amortized simultaneously** —
  match-detection, fill removal, and cancellation. I did not favour one at the
  expense of another, because a venue sees all three at high rates. The only
  non-O(1) cost is the out-of-band fallback map (`O(log L)`), reserved for prices
  outside the active band where latency matters least.
- **Array + bitmap vs. a tree.** The array gives O(1) best-price and contiguous,
  prefetch-friendly memory, at the cost of a bounded price band and ~8 MB/side.
  Real instruments have an exchange-defined tick size and price band, so this is
  the standard production structure; the ordered-map fallback preserves
  correctness for arbitrary prices without paying for them on the hot path.
- **Single-threaded core over a "lock-free book."** Deterministic price-time
  priority and atomic sweeps require serialization; throughput is scaled by
  sharding **one book per symbol** across cores, not by parallelizing a single
  book. Lock-free is applied where it is actually correct — the ingress/egress
  rings.
- **Pool + intrusive lists over `std::list`/`std::map`.** Eliminates per-order
  allocation and pointer-chasing in exchange for managing slots and a free list.

### Measurement caveats

These numbers were taken in a container on a developer machine: user space, no
core pinning or CPU isolation, no huge pages, no kernel-bypass NIC, and a
virtualized clock. They show the **algorithmic shape** (flat tail, O(1) ops),
not production hardware latency. Real measurement would use isolated/pinned
cores (`isolcpus`, `taskset`), `perf`, and an `rdtscp`-based timer.

---

## Robustness

No input can crash the process:

- The parser never throws and never half-applies a message — a line is a valid
  request, a blank/comment line to skip, or an `Error` carrying a reason.
- Validation covers unknown/garbage message types, wrong field counts, invalid
  side, non-positive quantity, malformed/negative/over-precise prices,
  out-of-range integers, duplicate order ids on add, and cancels of unknown ids.
- Every rejection is logged to `stderr` and processing continues.
  `data/errors.in` exercises these paths; `data/errors.err` pins the diagnostics.
- For convenience with hand-written datasets the parser also skips blank lines
  and `#` / `//` comments; canonical protocol input contains none.

---

## Testing

Run by `make all-checks`:

1. **Unit tests** (`tests/`, 29 cases):
   - *Engine* — the assignment example, multi-level sweeps, price-then-time
     priority, aggressive remainder resting, trades at the resting price,
     cancellation, duplicate-id rejection, id reuse, and the array-band vs.
     out-of-band fallback boundary.
   - *Price / parser* — parsing, formatting, ordering, and every rejection path.
   - *Components* — `OccupancyBitmap` extremes, `IdIndex` with rehash and
     backward-shift deletion under heavy collisions, and `SpscRing` FIFO +
     a 1M-item multithreaded producer/consumer ordering check.
2. **Dataset / golden-file tests** (`data/`, `scripts/run_datasets.sh`) — each
   `*.in` is fed to the binary and its `stdout`/`stderr` diffed against committed
   fixtures, including a faithful reproduction of the assignment example.

`scripts/gen_dataset.sh` generates randomized load-test input for `make bench`.

---

## Prebuilt binary

[bin/matching_engine](bin/matching_engine) is a **statically linked, non-PIE**
x86-64 ELF (built against musl):

```
$ readelf -h bin/matching_engine | grep Type   #  Type: EXEC (Executable file)
$ ldd bin/matching_engine                       #  not a dynamic executable
```

Being fully static (no interpreter, no libc dependency) it runs on any x86-64
Linux — verified on Alpine (musl) and Debian 13 / glibc 2.41. Rebuild it with:

```bash
g++ -std=c++17 -O2 -pthread -static -no-pie -s \
    src/main.cpp src/OrderBook.cpp src/MessageParser.cpp -o bin/matching_engine
```

---

## What I would add for production

- **Pin and isolate.** Run each book on an isolated, busy-polled core
  (`isolcpus`, `SCHED_FIFO`), with NIC interrupts and other threads kept off it;
  measure with `perf` + an `rdtscp` timer.
- **Kernel-bypass I/O.** Replace line-based CSV over stdio with a fixed-width
  binary wire format on a kernel-bypass NIC (Solarflare/ef_vi, DPDK) and
  busy-poll the ring — removes syscalls and copies from the critical path.
- **NUMA-aware, huge-page pools.** Allocate the order pool and level arrays on
  the local NUMA node with 2 MB/1 GB pages to cut TLB misses; size the band per
  instrument from its tick size and price limits so it is pure O(1).
- **Scale by sharding, not locking.** One single-writer book per symbol,
  distributed across cores, fed by per-shard SPSC ingress rings; market-data
  fan-out via a lock-free broadcast ring.
- **Richer semantics.** Order types (IOC, FOK, market, stop), cancel/replace
  (quantity modify), self-trade prevention, and per-message sequence numbers.
- **Determinism, durability & observability.** A replayable input journal for
  crash recovery and bit-exact replay; latency histograms and fill/cancel-rate
  metrics off the hot path; property-based and fuzz testing of the parser and
  matcher.
- **Numeric hardening.** Make the fixed-point scale and tick size configurable
  policy, and bound aggregate quantities to rule out overflow under adversarial
  input.
