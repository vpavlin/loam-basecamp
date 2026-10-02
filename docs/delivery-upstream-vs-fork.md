# Delivery: our fork vs upstream (2026-10-01)

Snapshot for the planned move of loam_core (and every Loam app) onto upstream `delivery_module`.

## What we run today ("0.1.4")

- **Module:** upstream `logos-co/logos-delivery-module`, branch `feat-add-channel-api-support` @ `0fb3a74`,
  version set to 0.1.4. Published as `github:vpavlin/logos-delivery-module/loam-0.1.4` (was a local path).
- **Library:** upstream `logos-messaging/logos-delivery` @ `8ad99f1` (`v0.38.1`) plus local-only patches in
  `~/kym-hub/sds-build/logos-delivery-patched` (not on GitHub): Android build (libc++_shared, staged librln,
  nims symlink, nimble overlay) and `online_monitor.nim` (assume online instead of the DNS check that
  deadlocks Android offline). The published Linux 0.1.4 `.lgx` was built from that patched tree
  (`liblogosdelivery` reports `v0.38.1-ge91aaa`); the macOS build uses plain upstream `8ad99f1`.

## Upstream now: delivery_module v0.3.0 (2026-09-30), library v0.39.0 (`ca28145`), +161 commits

| | ours (0.1.4) | upstream v0.3.0 |
|---|---|---|
| send / subscribe / channels | yes | same |
| `messageReceived` event | (hash, topic, payload, ts) | **+ `source`** ("live"/"history") — breaking |
| `storeQuery` (history) | not exposed | yes |
| `getConnectionStatus()` | — | yes |
| RLN | — | `rlnState`, `rlnStateChanged`, `messageQueued`; bridge to external RLN modules |
| `createNode` config | flat `{"preset","mode","entryNodes","tcpPort"}` | layered `{"preset","messagingOverrides":{…}}`; flat still accepted as "legacy" (any bare top-level key switches to it) |
| module builder | 0.2.6 (Linux, macOS) | 0.3.1 (+ **Windows** cross) |

Library behaviour changes: **RLN on for `logos.test`** (a node needs `liblogos_rln_module` 0.10.0 +
`liblogos_lez_rln_module` and an **active, funded membership** before `start`; proofs are attached, not
validated on receive); **store catch-up on by default** (replays missed messages, `source:"history"`);
**QUIC on by default** (UDP at the TCP port); mix sender anonymity (`anonymityLevel`); CBOR ABI.

Still missing upstream: the Android online-check fix (v0.39.0 still does the DNS check) and an Android
build target (only an iOS example) — the phones keep needing our patched library.

## Migration work (next week)

1. RLN membership for every `logos.test` node (desktops, VPS hub, crib hub) — the real blocker. Ask the
   Logos team whether v0.3.0 can run on `logos.test` without a funded membership while enforcement is off.
2. loam_core: rebuild against the new `messageReceived` signature (keep the flat config or move to layered).
3. All modules to builder 0.3.1 (one ABI across the stack) — also unlocks Windows (+ keycard PCSC port).
4. Phones: patched v0.39.0 Android build carrying our patches forward; native bridge for the CBOR ABI.
5. Interop test old ↔ new nodes (same wire protocol expected).

Related: loam-transport ADR 0018 (RLN), `docs/` of the platform builds workflow (`.github/workflows/macos-modules.yml`).

## Incident 2026-10-02: Basecamp replaced our fork with upstream

With both our repo and the official Logos repo added, Basecamp offers the highest `delivery_module` version
across repos, so it "upgraded" our 0.1.4 fork to upstream 0.3.x. The node still connected, but no message
reached loam_core (the `messageReceived` signature change, plus RLN on `logos.test`), so every app stopped
syncing. Workaround: reinstall 0.1.4 from our repo and decline the update. This makes the migration
above urgent; until then, our fork can be shadowed by upstream at any time.
