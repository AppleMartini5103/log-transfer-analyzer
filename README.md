# log-transfer-analyzer

A client-server system for transferring and analyzing large log files. A client streams a ~500 MB
log file to a Linux C++17 server over TCP; the server parses the stream **while receiving it**,
keeps its whole process footprint far under 50 MB, skips corrupted lines without crashing, and
returns the statistics as `result.csv`.

Measured on the reference log (483 MB, 3,483,528 lines): **peak RSS 8.4 MB**, 26 corrupted lines
skipped, full round trip in 1.3 s over loopback.

---

## 1. Build

### Requirements

| | Linux (server) | Windows (client) |
|---|---|---|
| Compiler | GCC 9+ / Clang 10+ (C++17) | MSVC 2019+ (C++17) |
| Build system | CMake 3.16+ | CMake 3.16+ |
| Other | `tar`, `g++`, POSIX | "x64 Native Tools Command Prompt" |

No package installation is required. libuv and Catch2 ship as source tarballs under `3rdparty/`
and are built by the scripts below.

### One command

```bash
./build_project_linux.sh          # Linux
build_project_window.bat          # Windows (from the VS Native Tools prompt)
```

The script builds the third-party libraries from their tarballs if they are missing, configures
CMake in `build/`, compiles, and runs the unit tests. Outputs:

```
build/server/server        # server binary
build/tests/unit_tests     # unit test runner
```

### Manual build

```bash
(cd 3rdparty/libuv  && ./build_linux.sh)     # produces include/ and lib/
(cd 3rdparty/catch2 && ./build_linux.sh)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`3rdparty/*/include/` and `lib/` are generated, not committed — the tarball plus the build script
is the source of truth.

### Sanitizer build

```bash
cmake -B build-asan -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j && ./build-asan/tests/unit_tests
```

AddressSanitizer + UndefinedBehaviorSanitizer. The full suite passes clean; this configuration
caught a dangling `string_view` during development that ordinary tests did not.

---

## 2. Running the server

```bash
./build/server/server                      # foreground, port 23507
./build/server/server -d -p 23507          # daemon
kill $(cat server.pid)                     # graceful shutdown
```

| Flag | Meaning |
|---|---|
| `-d` | Daemonize (double-fork, detach, log to file, write `server.pid`) |
| `-p PORT` | Listen port (default 23507) |
| `-c CONFIG` | Config file (default `./server.conf`; absent file is not an error) |

Artifacts are written relative to the working directory: `result.csv`, `skip_report.txt`,
`logs/<YYYYMMDD>/server.log` (daemon mode), `server.pid` (daemon mode). The log lives under a
dated directory because a daemon runs for days: the active file keeps a fixed name so `tail -f`
stays predictable, rotated files get a numeric suffix, and directories older than the retention
window are pruned.

The optional config file is plain `key=value` with `#` comments. It exists for benchmarking and
log placement only — protocol constants are never configurable, so a config file can never make
the client and server disagree:

```ini
chunk_size=65536        # receive/ring slot size (4 KB - 1 MB)
snd_buf_size=0          # SO_SNDBUF; 0 keeps kernel autotuning
log_path=./server.log   # base directory + file name; the file lands in <dir>/logs/<YYYYMMDD>/
```

Precedence is *built-in defaults < config file < command line*, so a benchmark sweep can change
one value per run from the command line. Unknown keys are rejected rather than ignored: a typo
like `chunck_size` would otherwise silently invalidate a measurement.

---

## 3. Network architecture

### 3.1 Wire protocol

Every message starts with a fixed 8-byte preamble, and all integers are big-endian. Structures are
never `memcpy`'d onto the wire — each field is serialized explicitly with shift operations, because
MSVC and GCC do not agree on padding and alignment.

```
offset 0  magic     4B  'B''Y''D''A'
offset 4  version   1B  = 1
offset 5  type      1B  1..5
offset 6  reserved  2B  = 0 (ignored by the receiver)
```

| Type | Direction | Body |
|---|---|---|
| 1 `UploadHeader` | client → server | `fileSize u64`, `filenameLen u16`, `filename` (≤255 B, UTF-8) |
| 2 `UploadTrailer` | client → server | `crc32 u32` of the payload |
| 3 `Ack` | server → client | `status u8`, `receivedBytes u64` |
| 4 `ResultHeader` | server → client | `csvSize u64`, `crc32 u32` |
| 5 `DownloadDone` | client → server | (no body) |

One session is one conversation:

```
client:  UploadHeader → payload (fileSize bytes) → UploadTrailer → (wait)
server:  (receive + parse concurrently) → Ack → ResultHeader → result.csv → (wait)
client:  (verify CRC) → DownloadDone → both sides clean up
```

**Why the checksum sits in different places per direction.** For the upload, computing a CRC up
front would mean reading 500 MB twice, so both sides compute it incrementally over the chunks they
are already handling and the client sends it in a trailer. The result CSV is only a few kilobytes
and already complete in memory, so its CRC travels in the header.

Chunk-level acknowledgements were deliberately left out: TCP already guarantees delivery, so they
would only add round trips. A single final acknowledgement carries the received byte count and the
checksum verdict.

### 3.2 Session state machine

```
LISTENING → WAIT_HEADER → RECEIVING → VERIFYING → ANALYZING → SENDING_RESULT → WAIT_DONE
     ▲                                                                             │
     └────────────────────────────── CLEANUP ◀─────────────────────────────────────┘
```

Any error, timeout, or abrupt disconnect in any state converges on the same `CLEANUP` path. That
is deliberate: if each failure kind had its own teardown, the number of code paths to test would
be the number of failure kinds multiplied by the number of states.

**Timeouts are attached to states, not to the connection.** A timer only runs while the server is
waiting on the *peer*:

| State | Waiting on | Timer |
|---|---|---|
| `WAIT_HEADER` | peer (`UploadHeader`) | 120 s |
| `RECEIVING` | peer (payload + trailer) | 30 s idle, reset on activity |
| `VERIFYING` | itself (CRC compare) | none |
| `ANALYZING` | itself (statistics + CSV) | none |
| `SENDING_RESULT` | peer (consuming the stream) | 30 s idle |
| `WAIT_DONE` | peer (`DownloadDone`) | 120 s |

Putting a timer on the server's own CPU work would produce a server that kills itself on a slow
machine. The state→timeout mapping lives in exactly one function so that adding a state cannot
silently forget its timer. Total transfer time is intentionally unbounded — a slow link is not a
failure; a *stalled* one is.

A silent failure (a pulled cable, a frozen peer, a dropped NAT entry) produces no socket error at
all, which is precisely why application-level timers are required rather than relying on TCP
keepalive, whose kernel defaults are measured in hours.

### 3.3 Concurrency model

Two threads, fixed at startup:

| Thread | Owns | Sleeps on | Woken by |
|---|---|---|---|
| libuv loop | sockets, timers, state machine, receive CRC | epoll | kernel events, `uv_async_send` |
| parser (resident, 1) | reassembly buffer, statistics map, skip counters | `cv.wait` | condition variable |

Data moves one way through a bounded lock-free SPSC ring buffer. Ownership of a slot transfers
from producer to consumer on commit, so **no mutex protects the data itself** — the only
cross-thread signals are wake-ups:

```
loop → parser :  condition variable  (slot committed, upload complete, abort, shutdown)
parser → loop :  uv_async_send       (resume reading, analysis complete)
```

`uv_async_send` is the only libuv call that is safe from another thread, so it is the only one the
parser thread uses. Coalescing is assumed: several sends may collapse into one callback, so both
handlers are idempotent.

Because the statistics belong to the parser thread, the CSV is also built there and only the
finished buffer is handed to the loop. Keeping that boundary intact is what removes the need for
shared ownership at the end of a session.

### 3.4 One session at a time

The server serves a single session at a time. A connection that arrives mid-session is **not**
rejected — it simply is not accepted yet, so it waits in the kernel backlog and is served after
cleanup, with any bytes it sent in the meantime preserved. This removes the need for retry logic
in the client. The behaviour was verified experimentally before the design depended on it, and is
locked in by a regression test.

Single active session is by design and matches the assignment scenario; the session-encapsulated
architecture makes concurrent-session extension straightforward.

### 3.5 Daemon and signals

Initialization order is fixed — `signal defaults → argument parsing → daemonize → logger →
event loop → threads` — because `fork` clones only the calling thread: daemonizing after creating
threads would leave the child with locked mutexes and nobody to unlock them.

* `SIGPIPE` is ignored explicitly, so writing to a closed socket returns `EPIPE` through the normal
  error path instead of killing the process. This is exactly the path an abrupt disconnect takes.
* `SIGTERM`/`SIGINT` are received through `uv_signal` rather than a raw handler, which moves them
  onto the loop thread and out of async-signal-safe restrictions.
* During shutdown the termination signals are *blocked*, not merely ignored — `uv_signal_stop`
  restores the default disposition, and a second `SIGTERM` arriving in that window would kill the
  process mid-cleanup and leave a stale PID file.
* `SIGHUP` is ignored; there is no configuration reload.
* The conventional `chdir("/")` is deliberately skipped so that `result.csv` and the logs stay at
  predictable relative paths.

---

## 4. Memory optimization strategy

The requirement is a process-wide ceiling, so every buffer in the process is bounded — a bound on
the queue alone would be defeated by any unbounded buffer downstream.

| Buffer | Bound | Enforced by |
|---|---|---|
| Ring buffer | **4 MB budget** | slot count derived from the budget |
| Line reassembly | 64 KB | maximum line length |
| Header framing | 273 B | largest possible message |
| Skip report samples | 100 lines × 200 B | explicit caps |
| Statistics map | 10,000 entries | entry limit |
| Result CSV | a few KB | derived from bucket count |

**Zero-copy receive.** libuv asks the application for a buffer before every read. Instead of
allocating one, the session hands back an empty ring slot, so bytes land directly in the queue the
parser will read — there is no copy between the socket and the parser, and no allocation on the
data path at all. Two small exceptions exist, both bounded: the chunk that carries the upload
header may also carry the first payload bytes, which are copied into a ring slot once per session,
and the ≤12-byte trailer tail is copied out before the slot ownership transfers.

**Backpressure is the ceiling mechanism.** When the ring fills, the server stops reading; the
kernel receive buffer fills, the TCP window closes, and the sender throttles itself. When the
parser frees a slot it wakes the loop to resume. No unbounded queue can form, so memory does not
track the file size.

**The budget is in bytes, not slots.** An earlier version fixed the ring at 64 slots; a benchmark
sweep then showed that a 1 MB chunk setting produced a 64 MB ring and a 69 MB peak — a
configuration value could breach the process limit. Slot count is now computed from a fixed 4 MB
budget, and the same sweep now peaks at 9.4 MB with 1 MB chunks.

**No manual memory management.** `new`, `delete`, `malloc`, `free`, `calloc`, and `realloc` do not
appear anywhere in the sources; ownership is expressed with `std::unique_ptr`, STL containers, and
move semantics. Because `uv_close` is asynchronous, handle memory is owned by a `unique_ptr` whose
ownership transfers to the close callback if the wrapper dies first, which keeps teardown safe even
when a peer disappears mid-callback.

### Measured

Reference log (483 MB), loopback, median of 3 runs per setting:

| Chunk size | Throughput | Server CPU | Peak RSS |
|---|---|---|---|
| 32 KB | 375 MB/s | 2.31 s | 6.4 MB |
| **64 KB** (default) | 379 MB/s | 2.26 s | **8.4 MB** |
| 128 KB | 379 MB/s | 2.21 s | 8.6 MB |
| 1 MB | 378 MB/s | 2.19 s | 9.4 MB |

Throughput and CPU are flat across the range: system-call overhead has already converged below
32 KB, so a larger chunk buys nothing and only costs memory. Sweeping `SO_SNDBUF` from 64 KB to
4 MB was equally flat, which is the expected result on loopback where the bandwidth-delay product
is effectively zero — the socket buffer is not the bottleneck in this environment, so the final
configuration leaves it to kernel autotuning.

Both figures are relative comparisons: the driver is written in Python and runs over loopback, so
the absolute throughput reflects the harness as much as the server.

---

## 5. Corrupted data handling

The reference log contains deliberately damaged lines. The parser must skip them, log them, and
finish parsing every remaining line.

**The filter is a whitelist, not a blacklist.** Filtering out the specific damage patterns that
happen to be present would leave the parser open to the first unfamiliar one. Instead a line is
accepted only if it matches the known-good structure exactly; everything else is skipped with a
reason code. This was not a theoretical concern: the log turned out to contain a fifth damage type
that the initial analysis had missed, and the whitelist rejected it correctly with no code change.

### Validation pipeline

A line passes through five stages in order and stops at the first failure:

| Stage | Checks | Reason codes |
|---|---|---|
| 0 · bytes | length cap, empty/blank, control characters, `\r` handling | `LINE_TOO_LONG`, `EMPTY`, `CTRL_CHAR` |
| 1 · frame | `[ts][pid][tid][sess] BYDA::Module: message` by position | `BAD_FRAME` |
| 2 · timestamp | format, calendar validity, ±48 h from the first good line | `BAD_TIMESTAMP`, `TS_OUT_OF_RANGE` |
| 3 · numbers | `from_chars`, full-token consumption, `int64` range, `isfinite` | `BAD_NUMBER`, `NUM_OUT_OF_RANGE` |
| 4 · domain | bracket pairing, module whitelist, speed range | `BAD_BRACKET`, `UNKNOWN_MODULE` |
| 5 · resources | statistics map entry cap | `MAP_LIMIT` |

Details that matter in practice:

* **Position-based parsing, no regular expressions** on the hot path — a regex on adversarial input
  invites catastrophic backtracking.
* **`std::from_chars` only.** `stod`/`atoi`/`sscanf` are absent. Parsing success is not validity:
  one damaged line carries `spd[888888888888888888888.88]`, which `stod` accepts happily as
  8.9e20 and which would destroy the average. Every number is range-checked after parsing, and
  every double is checked with `isfinite` because `from_chars` accepts `inf` and `nan` as text.
* **Length-based string handling** (`string_view`) throughout, so an embedded null byte truncates
  nothing.
* **Timestamps are range-checked against the session.** A random far-future timestamp would parse
  successfully and create a new statistics bucket; without the range check that is an unbounded
  memory growth path, not merely a wrong number.
* **A damaged line never stops the run.** Line reassembly reports an over-long line once, discards
  to the next newline, and resumes.

### Skip reporting

Counters are `uint64` and unbounded in value; the raw text is not. The first 100 skipped lines are
recorded with their reason code and byte offset, capped at 200 bytes each and with control
characters escaped, and the report states how many further lines were omitted. A few million
damaged lines therefore cannot fill the disk — the log about bounded buffers is itself bounded.

`result.csv` also carries `skipped_lines`, so the evidence that skipping happened is visible in the
deliverable itself.

---

## 6. Output format

`result.csv` has two blocks separated by a blank line, each with its own header row, so ordinary
CSV readers can parse both:

```csv
module,hour,count
AntennaProfileSpec,2026-06-19 22,23502
...

metric,value
avg_speed,137500.000000
valid_spd_samples,580661
excluded_spd_samples,0
skipped_lines,26
```

The hour key includes the date because the reference log spans 24.7 hours, so hour 22 occurs on two
different days and would otherwise collide. Rows are sorted by module name, then chronologically.
A label line such as `[Task 1]` was rejected precisely because it breaks CSV parsers.

---

## 7. Tests

```bash
ctest --test-dir build --output-on-failure          # 165 unit tests
python3 tests/e2e/run_e2e.py --server build/server/server
python3 tests/perf/sweep.py  --server build/server/server
```

The end-to-end driver and the benchmark sweep use the Python standard library only — no
installation step.

**Unit tests (Catch2, 165 cases / 4,916 assertions)** cover the CRC vector
(`"123456789"` → `0xCBF43926`), codec round trips including every truncation point, framer
accumulation, SPSC ring behaviour under two-thread contention, each validation stage with its own
damaged-line fixture, and the session state machine driven over real loopback sockets.

**End-to-end scenarios (15)** run against the real server binary and speak the protocol from an
independent Python implementation — deliberately not reusing the server's own codec, and using
`zlib.crc32` so the CRC implementation is cross-checked rather than compared with itself. They
cover the happy path, an empty file, eleven kinds of damaged data (five observed in the reference
log plus six the design had never seen), CRC mismatch, protocol violations of both classes, a
header split byte by byte, abrupt disconnection during upload and during result delivery, backlog
behaviour with two clients, peak RSS under the limit, and daemon mode. Two further scenarios are
opt-in because they are slow: the full 483 MB log (`--log <path>`) and the 120-second idle timeout
(`--slow`).

Every scenario also asserts that the server exits cleanly afterwards. That check found a real
regression: a shutdown deadlock in which the process never left its event loop.

---

## 8. Repository layout

```
common/              shared by client and server (libuv + standard library only)
  protocol/          protocol constants, codec, message framer
  net/               socket interface, TCP socket, listener, timer
  util/              CRC32, SPSC ring buffer, logger
server/src/
  app/               entry point, CLI/config, daemonization, signals
  session/           session state machine, 1:1 session manager
  parser/            line reassembly, validation pipeline, parser thread, skip report
  stats/             per-module hourly counts, speed statistics
  csv/               result.csv builder
tests/               Catch2 unit tests, Python E2E driver, benchmark sweep
3rdparty/            libuv and Catch2 source tarballs + build scripts
```

`common/` depends on libuv and the standard library only — no OS headers and no `long`, so the same
sources compile under both MSVC and GCC.
