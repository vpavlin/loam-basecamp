# shrooms_mesh: a Loam bearer over the Shrooms mesh

`shrooms_mesh` moves Loam's sealed frames between your own devices over the Shrooms mesh: WireGuard-authenticated IPv6 between machines you enrolled. It sits beside `ble_mesh` and has the same shape (`start`, `flood(topic, payloadB64)`, event `frameReceived`).

`loam_core` ≥ 0.7.0 adds it as a bearer when an app's config says `useMesh: true`. With `useDelivery: false` it is the only bearer, and no Logos Delivery (Waku) node exists on the machine.

## How it works
- **Listening.** It listens on **TCP 7392** at this machine's mesh addresses only, never on all interfaces. It accepts inbound connections only from the meshes' prefixes.
- **Finding peers.** Peers come from the Shrooms daemon: `GET /status` on `/run/shrooms/shrooms.sock`, or `$SHROOMS_SOCKET`. Failing that, it asks the shrooms-agent (`GET /v1/peers` on `$XDG_RUNTIME_DIR/shrooms-agent/agent.sock`). The list is refreshed every minute, or on demand with `refresh`.
- **Flooding.** A flood goes to every online peer that answers on 7392. A peer that isn't a member of the calendar or room can't open the frame, because it's sealed with that space's key.
- **Frames.** Each frame is `u32 length · u8 version(1) · u16 topic length · topic · payload`, capped at 8 MB. A connection that sends anything else is dropped. The same frame arriving over two meshes counts once.
- **No ordering, store or retries.** The app's catch-up (Scala's RBSR) repairs whatever a peer missed while it was away.

## Running Scala on the mesh only (logosctl)
```sh
export HOME=<dir>/home LOGOSCTL_CONFIG_DIR=<dir>/cfg QT_QPA_PLATFORM=offscreen
export SCALA_DELIVERY_CFG='{"useDelivery":false,"useMesh":true}'   # loam_core: mesh bearer only
export SCALA_STORAGE=off                                             # scala ≥ 0.12.6: no Storage node
logosctl daemon start -d
logosctl package install -y <shrooms_mesh.lgx> <loam_core ≥0.7.0.lgx> <scala ≥0.12.6.lgx>   # delivery_module not needed
logosctl module load loam_core && logosctl call loam_core hdUnlock '<password>'
logosctl module load scala
logosctl module unload storage_module        # the host starts one itself; Scala no longer needs it
```
After that, the session's only listening socket is `[mesh address]:7392`.

**Bridge to the Logos network:** run one always-on member with `{"useDelivery":true,"useMesh":true}` (delivery_module 0.3.0). It takes in events from both sides, and Scala's catch-up re-serves them, so no keys are needed beyond the calendar's read key.

## Diagnostics
- `peers`: `{listening, peers:[{name,mesh,addr,connected}], rx, tx, dropped, status}`.
- `status`
- `reachablePeers`

## Tested (2026-10-19)
- jimmy-crib and the VPS, both mesh-only (no delivery_module installed, storage_module unloaded):
  - a closed calendar created on crib, joined from the VPS with an editor invite (the claim was redeemed);
  - live edits arrived in about 2 s both ways.
- With the VPS bridging (both bearers):
  - a delivery-only node got the whole calendar about 3 min after joining;
  - a live edit from the mesh-only node reached it in about 1 min.

## Known
- **One listener per address.** Two sessions on one machine can't both bind 7392; the second one only sends.
- **Reloading the module** (`module unload/load`) can leave the old process holding 7392. Kill that exact PID.
- **No phone bearer yet.** Phones sync through the bridge.
