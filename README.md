# Order Matching Engine

A limit order book / matching engine implementing **price–time (FIFO) priority**.
It reads a stream of order messages from `stdin`, matches crossing orders, and
writes the resulting trade and order-state messages to `stdout`. Malformed or
rejected input is reported to `stderr` and never crashes the process.

This is an implementation of the Vatic Labs order-matching home assignment
(`Vatic_Order_match_assignment.docx`). C++17, no third-party runtime
dependencies, builds and runs on Linux.

---

## Quick start

```bash
make            # builds build/matching_engine
make test       # builds and runs the unit-test suite
make datasets   # runs the engine against data/*.in and diffs expected output
make all-checks # test + datasets
make bench      # throughput benchmark (override size with: make bench COUNT=5000000)
```

Run it directly:

```bash
./build/matching_engine < data/example.in
```

A prebuilt, **fully static** x86-64 Linux binary is committed at
[bin/matching_engine](bin/matching_engine) (see [Prebuilt binary](#prebuilt-binary)):

```bash
./bin/matching_engine < data/example.in
```

### Requirements

A C++17 compiler (`g++` ≥ 7 or `clang++` ≥ 5) and `make`. The unit tests use a
tiny in-repo framework, so there is nothing to download. To build without
`make`:

```bash
g++ -std=c++17 -O2 src/main.cpp src/OrderBook.cpp src/MessageParser.cpp -o matching_engine
```

---

## Message protocol

Each line is a comma-separated message identified by an integer type id. Two
input types are accepted; three output types are produced.

### Input

| Type | Name              | Format                                   | Example             |
|------|-------------------|------------------------------------------|---------------------|
| `0`  | AddOrderRequest   | `0,orderid,side,quantity,price`          | `0,123,0,9,1000`    |
| `1`  | CancelOrderRequest| `1,orderid`                              | `1,123`             |

- `side`: `0` = Buy, `1` = Sell
- `quantity`: positive integer
- `price`: positive decimal (see [Prices](#prices-are-fixed-point))

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

`data/example.in` is the exact example from the assignment. The aggressive buy
`0,1000008,0,3,1050` sweeps the 1025 ask level (oldest first):

```
$ ./build/matching_engine < data/example.in
Unknown message type: BADMESSAGE      # <- stderr
2,2,1025                              # trade 2 @ 1025 vs resting 1000005
4,1000008,1                          # aggressive now has 1 left
3,1000005                            # resting fully filled
2,1,1025                             # trade 1 @ 1025 vs resting 1000007
3,1000008                            # aggressive fully filled
4,1000007,4                          # resting reduced to 4
```

---

## Design

### Components

```
src/
  Types.h          OrderId / Quantity / Side primitives
  Price.h          fixed-point price (parse, compare, format)
  Order.h          a single order
  Message.h        AddOrderRequest / CancelOrderRequest + the type enum
  MessageParser.*  stdin line -> validated request (never throws)
  MessageSink.h    abstract output interface (Trade / Fully / Partially filled)
  StreamSink.h     MessageSink that writes the CSV protocol to an ostream
  OrderBook.*      the matching engine
  main.cpp         stdin/stdout/stderr driver
tests/             tiny test framework + unit tests
data/              input datasets with expected .out / .err fixtures
scripts/           dataset runner, dataset generator, benchmark
```

The key abstraction is **`MessageSink`**: the engine emits structured events
through this interface instead of touching `stdout`. Production uses
`StreamSink`; the tests use a `RecordingSink` to assert on the exact event
sequence. This keeps the matching logic completely free of I/O.

### Data structures

The order book is built from three structures:

```cpp
std::map<Price, std::list<Order>, std::greater<Price>> bids_;  // best (highest) at begin()
std::map<Price, std::list<Order>, std::less<Price>>    asks_;  // best (lowest)  at begin()
std::unordered_map<OrderId, OrderRef>                  index_; // id -> exact location
```

- **Per side, a `std::map` keyed by price.** Bids sort descending and asks
  ascending, so the best price is always `begin()` — *O(1)* to find — while every
  other level stays sorted for the occasional deep walk through the book.
- **Per price level, a `std::list<Order>` in arrival order** (front = oldest).
  This gives O(1) append, O(1) pop of the matched head, and — crucially —
  *stable iterators*, so an order in the middle can be cancelled without
  disturbing the others.
- **`index_`: `OrderId → {side, price, list iterator}`.** Lets cancellations and
  lookups jump straight to an order in O(1) average time instead of scanning.

### Prices are fixed-point

The spec says prices are "decimal numbers." Comparing them as `double` is unsafe:
values such as `0.1` are not representable in binary floating point, so two
prices that should be equal can compare unequal and silently corrupt price
priority. `Price` therefore stores an `int64_t` count of ticks
(`1 unit = 10^4 ticks`, i.e. 4 decimal places). All comparisons are exact integer
operations — also faster than floating point. Inputs with more than 4 fractional
digits are **rejected at parse time** rather than rounded, so precision is never
lost silently. The scale is a single named constant and trivial to widen.

---

## Performance

Complexity, where `L` = number of distinct price levels on a side and `m` = number
of resting orders an incoming order matches against:

| Operation | Cost | Why |
|-----------|------|-----|
| **Does an AddOrderRequest match?** | **O(1)** | Compare the incoming price to the best opposite level (`map.begin()`). No search. |
| **Executing a match** | O(m + k·log L) | Each of the `m` fills is O(1) (pop list head); `k` is the number of price levels fully consumed, each removed from the map in O(log L). |
| **Removing a filled order** | **O(1)** amortized | Pop the list front and erase from `index_`; emptied level removed in O(log L). |
| **Removing a cancelled order** | **O(1)** average | `index_` lookup → erase from its list in O(1) (stable iterator); emptied level removed in O(log L). |
| **Resting a non-matching order** | O(log L) | One map insert + one `index_` insert. |

Memory is O(N) in the number of live orders.

### Measured throughput

End-to-end (`make bench`), generating a mixed add/cancel workload in a tight
price band (≈90% adds, ≈10% cancels, heavy crossing) on the development
container:

```
Processed 1,000,000 messages in 1.12s (~890,000 msg/s)
Processed 5,000,000 messages in 5.60s (~892,000 msg/s)
```

Throughput stays flat from 1M to 5M messages, confirming the per-message cost is
effectively constant (no degradation as the book grows). Numbers are indicative
and hardware-dependent; reproduce with `make bench COUNT=...`.

### Trade-offs and request-path priorities

- **All three required paths are kept fast simultaneously** — match-detection,
  fill removal, and cancellation are all O(1)/O(1)-amortized. I deliberately did
  *not* favour one path at the expense of another, because a real venue sees all
  three at high rates. The only logarithmic factor is touching a *price level*
  (insert / remove a level), not an *order*.
- **`std::map` vs. a hash map for price levels.** A hash map would make level
  access O(1) but loses sorted iteration, which the matcher needs to walk levels
  best-first and to find the next-best level when one empties. The `log L` factor
  is tiny (`L` is the count of distinct prices, not orders) and only paid when a
  level is created or destroyed, so the tree is the right call here.
- **`std::list` per level.** Chosen for stable iterators (O(1) cancel from the
  middle) over a `std::deque`/ring buffer that would be more cache-friendly but
  would not allow arbitrary-position erase. See production notes for how I'd
  remove the node-per-order allocation cost.
- **Correctness over micro-optimization.** Fixed-point prices and clear
  abstractions were prioritized; the structures chosen don't preclude the
  optimizations below.

---

## Robustness

The driver is built so **no input can crash it**:

- The parser never throws and never half-applies a message — a line is a valid
  request, a blank/comment line to skip, or an `Error` carrying a reason.
- Validation covers: unknown/garbage message types, wrong field counts, invalid
  side, non-positive quantity, malformed/negative/over-precise prices,
  out-of-range integers, duplicate order ids on add, and cancels of unknown ids.
- Every rejection is logged to `stderr` and processing continues with the next
  line. `data/errors.in` exercises these paths and `data/errors.err` pins the
  expected diagnostics.
- For convenience with hand-written datasets the parser also skips blank lines
  and `#` / `//` comments; canonical protocol input simply contains none.

---

## Testing

Two complementary layers, both run by `make all-checks`:

1. **Unit tests** (`tests/`, 19 cases) — `Price` parsing/formatting/ordering,
   message parsing and every rejection path, and the matching engine: the
   assignment example, multi-level sweeps, price-then-time priority, aggressive
   remainder resting, trades at the resting price, cancellation, duplicate-id
   rejection, and id reuse after a full fill. They assert on the engine's exact
   event sequence via a `RecordingSink`.
2. **Dataset / golden-file tests** (`data/`, run by `scripts/run_datasets.sh`) —
   each `*.in` is fed to the binary and its `stdout`/`stderr` are diffed against
   committed `*.out`/`*.err` fixtures, including a faithful reproduction of the
   assignment example.

`scripts/gen_dataset.sh` produces randomized load-test input and underlies
`make bench`.

---

## Prebuilt binary

[bin/matching_engine](bin/matching_engine) is a **statically linked, non-PIE
x86-64 ELF** built against musl:

```
$ readelf -h bin/matching_engine | grep Type
  Type:  EXEC (Executable file)
$ ldd bin/matching_engine
  not a dynamic executable
```

Because it is fully static (no interpreter, no libc dependency) it runs on any
x86-64 Linux — verified on both Alpine (musl) and Debian 13 / glibc 2.41. Rebuild
it with:

```bash
g++ -std=c++17 -O2 -static -no-pie -s \
    src/main.cpp src/OrderBook.cpp src/MessageParser.cpp -o bin/matching_engine
```

---

## What I would add for production

The current design is intentionally clean and dependency-free; for a
production-grade venue I would pursue:

- **Eliminate per-order allocations.** Replace `std::list` nodes and map nodes
  with a pre-allocated object pool / arena and intrusive linked lists, and an
  open-addressing id index. This removes the dominant cost (allocator traffic and
  cache misses) and makes latency deterministic.
- **Array-indexed price levels.** For instruments with a known tick size and
  bounded range, index levels by `(price - floor) / tick` into a flat array (plus
  a bitset of occupied levels), turning the `O(log L)` level operations into O(1)
  and improving cache locality dramatically.
- **Multi-instrument + sharding.** A book per symbol, sharded across threads with
  one writer per book (lock-free SPSC ingress queues) to scale horizontally while
  keeping each book single-threaded and contention-free.
- **Richer order semantics.** Order types (IOC, FOK, market, stop), self-trade
  prevention, per-order quantity *modify* (cancel/replace), and sequence numbers.
- **Determinism, durability & observability.** A replayable input journal for
  crash recovery and exact replay, structured metrics (latency histograms,
  fill/cancel rates), and property-based / fuzz testing of the parser and matcher
  to complement the example-based tests.
- **Binary protocol & zero-copy I/O.** A fixed-width binary wire format with
  batched, memory-mapped or `io_uring` I/O instead of line-based CSV parsing to
  cut per-message overhead.
- **Numeric hardening.** Make the fixed-point scale a configurable policy and add
  explicit handling/limits for aggregate quantities to rule out overflow under
  adversarial input.
