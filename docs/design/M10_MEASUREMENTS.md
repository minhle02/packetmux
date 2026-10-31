# M10 — Measurements

Status: done. Plan: `docs/requirements/IMPROVEMENTS.md`, requests 2 and 6.

Measured on 2026-10-31 on an Apple M4 with a clean Release build, and a Samsung SM-X133 over USB. Each
loopback row is the median of 3 alternating rounds, and the SDK cost is the median of 5. Scripts:
`bindings/python/packetmux/bench/`.

## 1. Results

- **Credit is required, not just useful.** `adb push` through the tunnel buffered the whole 200 MB file
  in the server's read buffer, in all 3 rounds. With the default 16 MiB `max_read_buffer`, the same push
  failed. This is the check D17 asked for, and the default does not pass it.
- **The latency comes from kernel socket buffers.** When the pumps make the receiver the bottleneck,
  TCP queues about 2.6 MB between the two sides, which is about 3.3 ms at 790 MB/s. Credit bounds it.
- **USB users do not see that latency.** Over real adb, the tunnel matches direct adb on speed, and adds
  0.2 ms or less beside a transfer.
- **The SDK costs about 12% of throughput** against a thin host. It is still well above native.

## 2. Latency during a transfer

A 64-byte echo on a second tunnelled connection, beside an 800 MB transfer. Kernel queue is the client's
receive buffer plus the server's send buffer.

| Setup | MB/s | Round trip p50 (ms) | Kernel queue (KB) |
|---|---|---|---|
| Idle | - | 0.93 | 0 |
| Download, full speed | 791 | 4.91 | 2,581 |
| Download, capped at 360 MB/s | 360 | 0.57 | 18 |
| Download, 256 KiB sockets | 755 | 2.60 | 672 |
| Upload, full speed | 768 | 1.98 | 364 |

Other causes were ruled out or are small:

- **Sender polling:** the sender never waited, in any setup. The poll and drain variants behave the
  same, so their differences, up to 7% of throughput and 1.6 ms, show the noise in these runs.
- **Event loop:** 0.4 ms of lag at full speed, so it is a small part of the delay.
- **The core and SDK alone:** with no socket pumps, the round trip stays at 0.48 ms at 857 MB/s,
  because the receiver keeps up.

## 3. Window size

Credit simulated with acks between the two pumps, granted at half a window, on one download.

| Window | MB/s | Round trip p50 (ms) | p99 (ms) |
|---|---|---|---|
| 64 KiB | 171 | 0.51 | 0.72 |
| 128 KiB | 337 | 0.59 | 1.12 |
| **256 KiB** | **496** | **0.78** | **1.44** |
| 512 KiB | 661 | 1.11 | 1.89 |
| 1 MiB | 754 | 1.66 | 2.57 |
| 2 MiB | 770 | 2.46 | 3.83 |
| 4 MiB | 763 | 3.78 | 4.49 |
| None | 791 | 4.91 | 5.52 |

**Proposed default: 256 KiB.** It is native's value, and it is the largest window here that meets the
1.0 ms target. On this run it also beats the 362 MB/s native reached in the report, on a different
harness. It must stay configurable, because a stream's speed is capped at window / round trip, or
12.8 MB/s at 20 ms.

For the M11 design: the first version of this simulation deadlocked. It held back a whole chunk while
the receiver held back a small grant, and both waited forever. M11 prevents this by capping frames at
half the peer's window.

## 4. Real device

`adb` direct against `adb` through the tunnel, 200 MB, mean of 3 rounds. The round trip is a line sent
to `cat` on the device and back.

| Load | Direct MB/s | Tunnel MB/s | Direct p50 (ms) | Tunnel p50 (ms) |
|---|---|---|---|---|
| Idle | - | - | 1.70 | 2.41 |
| `adb pull` | 41.7 | 41.8 | 41.09 | 41.33 |
| `adb push` | 35.7 | 36.2 | 9.28 | 9.34 |

The 41 ms during a pull is in adb and USB, with or without the tunnel.

## 5. Cost of the SDK

A thin host over the sync core against the SDK, with no probes or pings on either.

| Direction | Thin MB/s | SDK MB/s | SDK against thin | Client CPU (s/GB), thin and SDK |
|---|---|---|---|---|
| Download | 974 | 853 | 0.88x | 0.97, 1.11 |
| Upload | 847 | 755 | 0.89x | 1.01, 1.15 |

This confirms the report's lead, and it applies in both directions. The thin host has no keepalive,
timers or read buffer, so part of the gap is features. The rest was not broken down, and it is not
worth tuning before M11 changes this path.

## 6. Limits

Loopback has no real network delay. The window sweep is a simulation in one direction. Browser and
WebRTC paths were not measured. The 16 MiB failure was one run, and it stopped the push before its
error was captured, so the reset code was inferred from the buffer figures and not read.

## 7. After M11: real credit

The same harness on M11, with the default 256 KiB window. Each row is the median of 3 rounds. The device
figures are a single round.

| Metric | Before credit | With credit |
|---|---|---|
| Download | 791 MB/s | 726 MB/s |
| Round trip beside a download, p50 | 4.91 ms | **0.73 ms** |
| Round trip beside a download, p99 | 5.52 ms | 1.12 ms |
| Upload | 768 MB/s | 679 MB/s |
| Round trip beside an upload, p50 | 1.98 ms | 0.74 ms |
| `adb push` of 200 MB, default settings | Failed | Passes |
| Read buffer peak during that push | 200 MB, at a 1 GiB limit | 262 KB |

The real window does far better than the simulation in §3, which gave 496 MB/s at this size. Real grants
are smaller and cost less than the acks there.

| Window | MB/s | Round trip p50 (ms) | p99 (ms) |
|---|---|---|---|
| 256 KiB | 726 | 0.73 | 1.12 |
| 512 KiB | 861 | 1.05 | 1.86 |
| 1 MiB | 913 | 1.51 | 2.00 |

256 KiB stays the default: it meets the 1.0 ms target, and 512 KiB narrowly misses it.

The SDK against the thin host is 0.85x on download and 0.91x on upload, 831 against 978 MB/s and 802
against 884 MB/s.

The device round showed adb itself slower than in §4, with direct pull at 35.5 MB/s against 41.7. Beside a
transfer the tunnel still matches direct adb. Idle, it read 4.48 ms against 2.12 ms, a bigger gap than the
0.71 ms in §4. With one round this is unexplained, and it needs a repeat before anyone relies on it.
