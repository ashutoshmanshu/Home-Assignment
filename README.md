# Order Matching Engine

A limit order book / matching engine with price-time (FIFO) priority. It reads
order messages from `stdin`, matches crossing orders, and writes the resulting
trade and order-state messages to `stdout`. Bad input is reported on `stderr`
and never crashes the process.

Implementation of the Vatic Labs order-matching home assignment
(`Vatic_Order_match_assignment.docx`). C++17, no third-party runtime
dependencies, Linux.

A few design points up front, because they drive most of the code:

* The hot path does no heap allocation (orders come from a pool, levels are
  intrusive linked lists).
* Best bid / best ask is O(1): price levels are a flat array and a small
  occupancy bitmap is bit-scanned to find the best non-empty one.
* The book runs on a single thread so price-time priority stays deterministic.
  The only lock-free structure is an SPSC ring at the I/O boundary.

More in [Architecture](#architecture) and [Performance](#performance).

---

## Quick start

```bash
make            # builds build/matching_engine
make test       # builds and runs the unit tests (engine + components)
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

There is also a prebuilt, fully static x86-64 Linux binary at
[bin/matching_engine](bin/matching_engine) (see [Prebuilt binary](#prebuilt-binary)).

### Requirements

A C++17 compiler (`g++` >= 7 or `clang++` >= 5) and `make`. The tests use a tiny
in-repo framework so there is nothing to fetch. To build without `make`:

```bash
g++ -std=c++17 -O2 -pthread src/main.cpp src/OrderBook.cpp src/MessageParser.cpp -o matching_engine
```

---

## Message protocol

Every line is a comma-separated message identified by an integer type id.

### Input

| Type | Name              | Format                                   | Example             |
|------|-------------------|------------------------------------------|---------------------|
| `0`  | AddOrderRequest   | `0,orderid,side,quantity,price`          | `0,123,0,9,1000`    |
| `1`  | CancelOrderRequest| `1,orderid`                              | `1,123`             |

`side` is `0` for Buy and `1` for Sell. `quantity` is a positive integer.
`price` is a positive decimal (see [Prices](#prices-are-fixed-point)).

### Output

| Type | Name                 | Format                  | Meaning                                |
|------|----------------------|-------------------------|----------------------------------------|
| `2`  | TradeEvent           | `2,quantity,price`      | A trade executed                       |
| `3`  | OrderFullyFilled     | `3,orderid`             | Order fully filled and removed         |
| `4`  | OrderPartiallyFilled | `4,orderid,quantity`    | Order partially filled; new open qty   |

For each matched pair the engine emits, in this order: the TradeEvent, then a
fill message for the aggressive order, then a fill message for the resting
order. Trades execute at the resting order's price.

### Worked example

`data/example.in` is the exact example from the assignment. The engine
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

The shape mirrors a real venue: a single-threaded deterministic matching core,
with lock-free queues only at the boundaries.

### Why the book is single-threaded (and what *is* lock-free)

Messages have to be processed in order. Price-time priority, and the atomicity
of a sweep that touches several resting orders, only hold if one message is
fully resolved before the next one starts. A "lock-free"/concurrent book would
throw that away, so the core is single-threaded; in production you pin it to an
isolated, busy-polling core.

Where concurrency does help is at the edges, and that is the one place a
lock-free structure shows up: [`SpscRing`](src/SpscRing.h), a single-producer /
single-consumer ring that hands parsed commands from the I/O thread to the
matching thread (and trades/market-data back out in a full system). It is
cache-line padded against false sharing and publishes its indices with
release/acquire ordering, so there is no mutex and no CAS loop. `--pipeline`
turns it on:

```
stdin -> [parse + validate thread] -> SpscRing -> [single-writer matching thread] -> stdout
```

Only the matcher writes `stdout`, so output order is the same as the
single-threaded path. I check that byte-for-byte in the tests.

### The hot path is allocation-free

Allocation and cache misses are what actually cost nanoseconds, so the book
([src/OrderBook.h](src/OrderBook.h), [src/OrderBook.cpp](src/OrderBook.cpp))
avoids both on the steady-state path.

- **Order pool + intrusive FIFOs.** Orders live in one pre-grown `std::vector`
  (`nodes_`). Each price level is a FIFO threaded through `prev`/`next` slot
  indices stored in the nodes themselves, so resting, matching and cancelling
  never touch `new`/`delete`. Filled or cancelled slots go back on a free list
  and get reused, so at steady state there is no allocation at all.
- **Array price levels + occupancy bitmap.** Levels are a flat array indexed by
  price tick. A 3-level [`OccupancyBitmap`](src/OccupancyBitmap.h) records which
  levels are non-empty, so best bid / best ask is a couple of hardware bit-scans
  (O(1)) rather than an O(log L) walk down a red-black tree. The array covers a
  price band around the market (an instrument's intraday range); anything outside
  it falls back to an ordered map. Correctness never depends on the band, only
  speed does.
- **Open-addressed id index.** [`IdIndex`](src/IdIndex.h) maps `OrderId` to a
  pool slot with linear probing and backward-shift deletion. One flat array, no
  per-entry node allocation, cache-friendly probes, O(1) average cancel lookup.

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

The one abstraction worth calling out is `MessageSink`: the engine emits events
through it instead of writing to `stdout` directly. That keeps the matching code
free of I/O, and lets the tests assert on the exact event sequence with a
`RecordingSink`.

### Prices are fixed-point

The spec calls prices "decimal numbers." Treating them as `double` is a trap:
values like `0.1` are not exactly representable, so two prices that should be
equal can compare unequal and quietly break price priority. [`Price`](src/Price.h)
instead stores an `int64_t` count of ticks (1 unit = 10^4 ticks, i.e. 4 decimal
places). Comparison is plain integer arithmetic, which is exact and also happens
to be the price-level array index. A price with more than 4 fractional digits is
rejected at parse time instead of being silently rounded.

---

## Performance

Complexity, with `L` = distinct active price levels on a side and `m` = resting
orders an incoming order matches against:

| Operation | Cost | Why |
|-----------|------|-----|
| Does an AddOrderRequest match?     | **O(1)** | Best opposite level is one bitmap bit-scan; compare it to the incoming price. |
| Executing a match                  | **O(m)** | Each fill pops a FIFO head in O(1); an emptied level clears one bitmap bit. |
| Removing a filled order            | **O(1)** | Pop the head, return the slot to the pool, erase from the id index. |
| Removing a cancelled order         | **O(1)** avg | id-index lookup, unlink via stable slot indices, clear the bit if the level empties. |
| Resting a non-matching order       | **O(1)** in-band | Pool alloc (amortized), push the FIFO tail, set a bitmap bit. Out-of-band is O(log L). |

Memory is O(N) live orders, plus a fixed per-side band array (about 8 MB/side at
the default `kBandTicks = 2^20`).

### Measured numbers

These come from the dev container, which is virtualized and not tuned hardware
(see the caveats below). Reproduce with `make latency` and `make bench`.

Per-operation latency (`make latency COUNT=2000000`):

```
add (resting, no match)  amortized= 74 ns/op | p50=150 p99=308 p99.9=476 ns
cancel                   amortized= 57 ns/op | p50=148 p99=319 p99.9=501 ns
add (matches 1 resting)  amortized=159 ns/op | p50=170 p99=391 p99.9=605 ns
```

`amortized` is total wall time / N with no per-op timer, so it is the truest
per-op cost. The p50/p99/p99.9 figures come from a separate timed loop and
include the timer's own cost (two `clock` calls, roughly 70 ns), so read them as
an upper bound. The multi-millisecond `max` values are OS preemption; a pinned
isolated core removes them.

End-to-end throughput (`make bench COUNT=5000000`, mixed adds/cancels, heavy
crossing):

```
single-threaded   ~1.12M messages/s
--pipeline         ~1.85M messages/s   (the lock-free ring overlaps parse/I/O with matching)
```

### Trade-offs

- All three paths the assignment asks about (match detection, fill removal,
  cancel) are O(1) or amortized O(1) at the same time. I did not trade one off
  against another, because a venue gets all three at rate. The only non-O(1)
  cost is the out-of-band fallback map, which only applies to prices outside the
  active band where latency matters least.
- Array + bitmap instead of a tree. The array gives O(1) best price and
  contiguous, prefetch-friendly memory, at the cost of a bounded band and ~8
  MB/side. Real instruments have a defined tick size and price band, so this is
  the normal production structure; the ordered-map fallback keeps arbitrary
  prices correct without paying for them on the hot path.
- Single-threaded core, not a "lock-free book." Deterministic priority and
  atomic sweeps need serialization. You scale by running one book per symbol
  across cores, not by parallelizing a single book. Lock-free goes where it is
  actually safe, the ingress/egress rings.
- Pool + intrusive lists instead of `std::list`/`std::map`. This removes
  per-order allocation and pointer chasing, in exchange for managing slots and a
  free list by hand.

### Measurement caveats

The numbers were taken in a container on a laptop: user space, no core pinning
or isolation, no huge pages, no kernel-bypass NIC, virtualized clock. They show
the algorithmic shape (flat tail, O(1) ops), not production hardware latency. A
real measurement would use isolated/pinned cores (`isolcpus`, `taskset`),
`perf`, and an `rdtscp`-based timer.

---

## Robustness

No input should be able to crash the process:

- The parser never throws and never half-applies a message. A line is either a
  valid request, a blank/comment line to skip, or an error with a reason.
- It validates unknown/garbage message types, wrong field counts, bad side,
  non-positive quantity, malformed/negative/over-precise prices, out-of-range
  integers, duplicate ids on add, and cancels of unknown ids.
- Every rejection goes to `stderr` and processing carries on. `data/errors.in`
  exercises these paths and `data/errors.err` pins the expected messages.
- For hand-written datasets the parser also skips blank lines and `#` / `//`
  comments. The canonical protocol input contains none of those.

---

## Testing

`make all-checks` runs both layers:

1. **Unit tests** (`tests/`, 29 cases):
   - *Engine*: the assignment example, multi-level sweeps, price-then-time
     priority, an aggressive remainder that rests, trades at the resting price,
     cancel, duplicate-id rejection, id reuse, and the band vs. out-of-band
     fallback boundary.
   - *Price / parser*: parsing, formatting, ordering, and each rejection path.
   - *Components*: `OccupancyBitmap` extremes, `IdIndex` through a rehash and
     backward-shift deletion under heavy collisions, and `SpscRing` FIFO plus a
     1M-item multithreaded producer/consumer ordering check.
2. **Dataset / golden-file tests** (`data/`, `scripts/run_datasets.sh`): each
   `*.in` is fed to the binary and its `stdout`/`stderr` diffed against committed
   fixtures, including the assignment example.

`scripts/gen_dataset.sh` produces the randomized input `make bench` runs on.

---

## Prebuilt binary

[bin/matching_engine](bin/matching_engine) is a statically linked, non-PIE
x86-64 ELF built against musl:

```
$ readelf -h bin/matching_engine | grep Type   #  Type: EXEC (Executable file)
$ ldd bin/matching_engine                       #  not a dynamic executable
```

Because it is fully static (no interpreter, no libc dependency) it runs on any
x86-64 Linux. I verified it on Alpine (musl) and Debian 13 / glibc 2.41. Rebuild
with:

```bash
g++ -std=c++17 -O2 -pthread -static -no-pie -s \
    src/main.cpp src/OrderBook.cpp src/MessageParser.cpp -o bin/matching_engine
```

---

## What I would add for production

- **Pin and isolate.** Run each book on an isolated, busy-polled core
  (`isolcpus`, `SCHED_FIFO`), keep NIC interrupts and other threads off it, and
  measure with `perf` and an `rdtscp` timer.
- **Kernel-bypass I/O.** Swap line-based CSV over stdio for a fixed-width binary
  wire format on a kernel-bypass NIC (Solarflare/ef_vi, DPDK) and busy-poll the
  ring, to get syscalls and copies off the critical path.
- **NUMA-aware, huge-page pools.** Put the order pool and level arrays on the
  local NUMA node with 2 MB / 1 GB pages to cut TLB misses, and size the band per
  instrument from its tick size and price limits so it is purely O(1).
- **Scale by sharding.** One single-writer book per symbol across cores, fed by
  per-shard SPSC ingress rings, with market-data fan-out over a lock-free
  broadcast ring.
- **More order semantics.** IOC / FOK / market / stop orders, cancel-replace
  (quantity modify), self-trade prevention, per-message sequence numbers.
- **Durability and observability.** A replayable input journal for recovery and
  bit-exact replay, latency histograms and fill/cancel-rate metrics off the hot
  path, and property-based / fuzz testing of the parser and matcher.
- **Numeric hardening.** Make the fixed-point scale and tick size configurable
  policy, and bound aggregate quantities so adversarial input cannot overflow.
