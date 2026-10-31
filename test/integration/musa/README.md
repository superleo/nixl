# MUSA_UCX hardware tests

Hardware (GPU) validation entry points for the MUSA_UCX backend plugin. These
tests need a visible MUSA device and a UCX build whose memory domain publishes
the `musa` memory type; without them the tests **FAIL** — they never report
SKIP, because a skipped hardware test cannot count as acceptance evidence.

Build (debug, so the in-tree `pluginlist` discovery is active):

```bash
meson setup build-musa --buildtype=debug \
  -Dmusa_path="$MUSA_HOME" -Ducx_path="$UCX_HOME" \
  -Denable_plugins=MUSA_UCX -Dmusa_hardware_tests=true \
  -Dbuild_docs=false -Drust=false -Dbuild_examples=false
meson compile -C build-musa MUSA_UCX musa_ucx_e2e musa_ucx_negative
```

The registered Meson case (`--suite musa-hardware`) covers only the documented
minimum: single host, dual process, device/device, 64 KiB, background
progress, device 0, WRITE + notification and READ back. All other matrix
combinations use the same binaries with explicit arguments.

## `musa_ucx_e2e` — data correctness matrix

```
build-musa/test/unit/plugins/musa/musa_ucx_e2e <memA> <memB> <bytes> <post-threads> <devA> <devB>
```

- `memA`/`memB`: `host` or `device` (sender A, receiver B)
- `bytes`: transfer size; `post-threads`: backend `num_threads` (0 = shared progress)
- `devA`/`devB`: MUSA device ids (ignored for host memory)

The process forks after argument parsing; both peers touch MUSA/UCX only
**after** the fork. The NIXL backend (and therefore `ucp_init`) is created
**before** any MUSA allocation: UCX's memtype cache only tracks allocations
made after its hooks are installed, and VRAM allocated before `ucp_init`
would be detected as host memory and rejected by the MUSA memory contract.
Each run is guarded by a 90 second watchdog — expiry is exit code 124 and
a failure. The sender fills a pattern that distinguishes
rank, operation, descriptor index and offset, transfers with a notification,
and reads the data back for a byte-exact comparison; the receiver verifies
immediately upon seeing the WRITE notification (H-09) and both sides release
requests, deregister memory and invalidate remote metadata in order (H-10).

Environment extensions:

| Variable | Meaning |
| --- | --- |
| `NIXL_MUSA_E2E_ITERS=<n>` | repeat register/post/release cycles (H-06) |
| `NIXL_MUSA_E2E_NDESC=<n>` | split into n deliberately unaligned descriptors (H-05, ≤64) |
| `NIXL_MUSA_E2E_PROG_MODE=caller` | caller-only progress instead of background thread; the caller must poll `getXferStatus()` for its requests and `getNotifs()` on the target side |
| `NIXL_MUSA_E2E_KILL_RECV=1` | receiver exits after metadata exchange; sender must observe a bounded failure |
| `NIXL_MUSA_E2E_CHECK_RES=1` | report VmRSS and device free memory before/after |
| `NIXL_MUSA_E2E_TCP_LISTEN=ip:port` / `_TCP_CONNECT=ip:port` | two-host mode (receiver listens, sender connects) |

Matrix examples (single host):

```bash
build-musa/test/unit/plugins/musa/musa_ucx_e2e host host 4096 0 0 0
build-musa/test/unit/plugins/musa/musa_ucx_e2e host device 65536 0 0 0
build-musa/test/unit/plugins/musa/musa_ucx_e2e device host 65536 2 0 0
build-musa/test/unit/plugins/musa/musa_ucx_e2e device device 1048576 2 0 1   # needs 2 devices
NIXL_MUSA_E2E_ITERS=100 build-musa/test/unit/plugins/musa/musa_ucx_e2e device device 1048576 0 0 0
NIXL_MUSA_E2E_NDESC=8 build-musa/test/unit/plugins/musa/musa_ucx_e2e device device 65536 0 0 0
NIXL_MUSA_E2E_PROG_MODE=caller build-musa/test/unit/plugins/musa/musa_ucx_e2e host device 65536 0 0 0
NIXL_MUSA_E2E_KILL_RECV=1 build-musa/test/unit/plugins/musa/musa_ucx_e2e host host 4096 0 0 0
```

For the 1000-cycle resource check, combine `NIXL_MUSA_E2E_ITERS=1000` with
`NIXL_MUSA_E2E_CHECK_RES=1` on a small buffer and confirm neither VmRSS nor
device memory grows across the run.

## `musa_ucx_negative` — negative paths

Each case runs in a forked child (90 second watchdog) and must be rejected
with a clear error; an unexpected success fails the case. Run with
`NIXL_LOG_LEVEL=info` to capture backend failure messages:

- TLS/memory domain without musa capability → engine creation fails, no silent fallback
- out-of-range VRAM device id, host pointer as VRAM, device pointer as DRAM
- zero-length / null / overflowing ranges, unregistered local ranges
- corrupted remote metadata, runtime-invalid device pointers

```bash
build-musa/test/integration/musa/musa_ucx_negative
```

## Two-host mode (H-04)

The driver exchanges descriptor tables, UCX metadata and the done marker over
the same TCP channel, so a two-host run exercises exactly the same
register → WRITE → notification → READ → release lifecycle as the fork mode.
`devA` selects a device on the sender host, `devB` on the receiver host; each
host only needs its own devices visible.

Preparation on both hosts: identical NIXL/UCX builds (same `build-musa` tree
copied or rebuilt from the same commits), `NIXL_PLUGIN_DIR` and
`LD_LIBRARY_PATH` exported as for the single-host runs, and the chosen TCP
port open in the firewall. Pin the UCX transports explicitly so the default
iface search cannot pick an unintended one, e.g.
`UCX_TLS=tcp,musa_copy,self` (add `UCX_NET_DEVICES=<nic>` when several NICs
are present; use the IB pair only after `ucx_info -t` shows an IB tl that
accepts `musa` memory).

Run the receiver first (the sender retries the connect for ~40 s, so a short
startup skew is fine):

```bash
# receiver host
NIXL_MUSA_E2E_TCP_LISTEN=0.0.0.0:4999 UCX_TLS=tcp,musa_copy,self \
  build-musa/test/unit/plugins/musa/musa_ucx_e2e <memA> <memB> 65536 0 <devA> <devB>
# sender host — the exact same command line:
NIXL_MUSA_E2E_TCP_CONNECT=<recv-ip>:4999 UCX_TLS=tcp,musa_copy,self \
  build-musa/test/unit/plugins/musa/musa_ucx_e2e <memA> <memB> 65536 0 <devA> <devB>
```

Both sides parse the identical positional arguments: the receiver uses only
`memB`/`devB` (its own buffer), the sender only `memA`/`devA` (source and
readback buffers) — no mirroring is needed. Listen/connect specs are IPv4
literals only (`ip:port`; `0.0.0.0` is valid for listening; hostnames/IPv6
are not accepted by the driver). The receiver verifies; the sender does the
WRITE and readback. Both peers still run under the 90 second watchdog, so a
lost route is a bounded failure, not a hang.

The H-04 matrix is the four memory combinations, both with a single
descriptor and repeated rounds:

```bash
for combo in "host host" "host device" "device host" "device device"; do
  set -- $combo
  # both hosts, identical arguments:
  # receiver: NIXL_MUSA_E2E_TCP_LISTEN=0.0.0.0:4999 musa_ucx_e2e $1 $2 65536 0 0 0
  # sender:   NIXL_MUSA_E2E_TCP_CONNECT=<recv-ip>:4999 \
  #           NIXL_MUSA_E2E_ITERS=100 musa_ucx_e2e $1 $2 65536 0 0 0
done
```

`NIXL_MUSA_E2E_KILL_RECV=1` on the receiver additionally covers the
peer-exit path of U-11 across hosts. Record for each run: the transport
chosen by UCX (`UCX_LOG_LEVEL=info`), the staging/direct path classification,
the byte-exact verdict and both logs.
