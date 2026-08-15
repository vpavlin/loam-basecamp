# ble_mesh

The **Loam BLE offline-mesh bearer** as a standalone, reusable logos-core module (ADR 0015). It
floods sealed bytes over a short-range Bluetooth mesh so nearby devices keep syncing with **no
internet at all** — then heal back to the fleet when it returns. `loam_core` (and any app) depends
on `ble_mesh` and adds it to its MultiBearer beside `delivery`; a write flooded over BLE and the
same write over Waku **dedup by `frameId`** (identical hash on both sides), so BLE needs no
ordering/reliability of its own — convergence is loam-sync's job.

## Status

- **Portable gossip — DONE & verified** (`src/mesh.hpp`). Frame codec, `frameId`, seen-set, and
  the flood / carry-forward / TTL gossip, ported from loam-transport's `bearer.ts`. Unit tests
  (`test/mesh_test.cpp`, 28 checks) cover codec round-trip, dedup, multi-hop carry-forward, TTL
  exhaustion, and N-node convergence over an in-memory `MockRadio` — **no hardware**. The
  `frameId` is **byte-identical to `bearer.ts`** (parity vectors in the test), so a desktop and a
  phone mesh together.
  ```sh
  g++ -std=c++17 -I src test/mesh_test.cpp -lcrypto -o /tmp/mesh_test && /tmp/mesh_test
  ```
- **Module — builds & loads** (`src/ble_mesh_impl.{h,cpp}`, `metadata.json`, `flake.nix`). Exposes
  `start` / `stop` / `flood(topic, payloadB64)` / `reachablePeers` / `status` and a
  `frameReceived(topic, senderId, payloadB64, ts)` event. `dependencies: []` — it depends on
  nothing. Runs today with a **no-op `StubRadio`** (0 peers) so it loads without a BLE device.
- **Qt Bluetooth radio — Phase 3 (needs hardware).** Swap `StubRadio` for a `QtBleRadio`
  implementing `MeshRadio` with `QLowEnergyController` (peripheral **and** central). One-line swap
  in `onContextReady()`; the gossip layer is unchanged.

## Radio wire-compat contract (Phase 3 must match the Android Loam mesh)

The Qt radio has to be byte-compatible with `LoamMeshModule.kt` so desktop↔phone meshing works:

| | value |
|---|---|
| Service UUID | `10a11052-0000-4c6f-616d-6d6573680001` ("Loammesh") — app-agnostic, one device-wide mesh |
| Characteristic UUID | `10a11052-0000-4c6f-616d-6d6573680002` (`WRITE_NO_RESPONSE` + `NOTIFY`) |
| CCCD | `00002902-0000-1000-8000-00805f9b34fb` |
| MTU | request `512`, handle negotiated-down |
| GATT payload type byte (ADR 0014) | `0x41` `'A'` = announce (payload = **stable node id**), `0x46` `'F'` = fragment |
| Fragmentation | one fragment at a time (each after the previous write completes); reassembly keyed by `addr/msgId`, 30s timeout |
| Identity (ADR 0014) | peers / routing / dedup / count keyed by **node id**, not the (rotating) BLE address |
| Roles | advertise the service **and** scan-filter on it + dial; lower node-id dials (tiebreak) |

The **bearer frame** above the radio is `mesh.hpp`'s `[ ver(1) | hop(1) | topicLen(2 BE) | topic |
payload ]` with `frameId = sha256(topic‖0x00‖payload)[:16]` — already parity-checked against
`bearer.ts`.

## Wiring into loam_core (Phase 4)

Add `ble_mesh` to `loam_core`'s `dependencies` and register it as a second bearer in the
MultiBearer. App cores stay unchanged — the new bearer just appears in `metricsJson`, and BLE
frames funnel into the same dedup'd receive stream as Waku frames.
