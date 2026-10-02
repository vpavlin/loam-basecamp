# Port to Basecamp 0.3 / builder 0.3.1 / delivery v0.3.0 / storage 3.0.0 — plan

Status 2026-10-02. Analyses: `analysis-basecamp-builder.md`, `analysis-delivery.md`, `analysis-storage.md`.

## The shape of it
- **Mixed stacks don't work.** Old-built modules load in the 0.3.1 runtime, but their startup calls to
  other modules are rejected (auth token not recognized) → loam_core never starts its transport. So the
  whole desktop stack moves together: every module rebuilt on builder 0.3.1, one release.
- **Phones are the hard constraint.** delivery v0.39 wraps reliable-channel payloads in a segment protobuf
  with an unchanged wire marker, so 0.39 desktops and 0.38.1 phones don't understand each other's
  channel messages. Fix = an unwrap shim on BOTH sides (desktop loam_core, phone loam-transport) that
  accepts both formats. It ships to phones BEFORE desktops move.
- **RLN:** without a membership a v0.3.0 node receives but cannot send. Needs a decision (below).

## Phases (each on `port/0.3` branches; nothing merged or published until you've tested)

**P0 — safe now, works on the current stack too**
1. loam_core: segment-unwrap shim (accept old raw SDS and new SegmentMessage; keccak check), read the
   timestamp from the last event argument (v0.3.0 inserts `source`), wait for `nodeStarted` before
   "ready". Must still pass with delivery 0.1.4.
2. loam-transport (phones): the same unwrap shim → app updates (scala, qaku, kym, perun, kith, Loam,
   whisperbox) via the usual F-Droid path. Phones keep the old library; they just learn the new format.
3. Unit tests for the shim with real frames from both library versions.

**P1 — desktop rebuild on builder 0.3.1 (private network, RLN off)**
4. Builder 0.3.1 everywhere; remove `path:` inputs; 256² icons; drop `interface` on views; declare
   view→module calls; remove default args (scala, kith); kym_core trailing-comment methods.
5. keycard: make it publish a contract (see decision 2).
6. loam_core → upstream delivery v0.3.0 (layered config, `source`, RLN state surfaced in status).
7. scala → storage 3.0.0 (new args; move fetch/download off the 20 s IPC path — they now wait up to
   30 s; fix the restart fallback); patched storage build carrying our libp2p fix for the hub.
8. ble_mesh: bundle QtBluetooth via the build (no hand repack); version in metadata.
9. whisperbox_core (Atlas's code): note for Atlas — same delivery changes + the channel-layer issue.

**P2 — test here**
10. `logosctl` 0.3.1 sessions (replaces logos-hub/logoscore): 3 nodes on a private cluster
    (preset "", no RLN): loam_core + scala + qaku sync between them; storage upload/fetch between two.
11. Mixed test: an old-library node (0.38.1, our current hub stack) ↔ new node, via the shim.
12. Basecamp 0.3.1 AppImage headless smoke where possible; GUI check is yours.

**P3 — for you**
13. LAN test repo (separate from the current one, so nothing auto-upgrades): `basecamp-0.3/`.
14. Checklist: install on Basecamp 0.3.1, what to click, what "working" looks like.

**Later (separate track)**
- Phone library v0.39 + JNI rewrite (new C ABI) + our Android patches re-applied.
- Hubs (VPS, crib) to logosctl + new stack.
- macOS/ARM rebuilds with the same builder, then merge.

## Decisions needed from you
1. **RLN on logos.test:** (a) fund a membership per node (two RLN modules + LEZ testnet wallet; one
   payer key can fund many nodes; 100 msgs / 10 min per node — every segment counts), or (b) run with
   RLN disabled via the presets env file / `preset:""` + cluster 2 for now (works only while the fleet
   doesn't validate proofs). Suggest: (b) for development, (a) before release; ask the Logos team
   whether validation will be turned on.
2. **keycard** (it's xAlisher's module): port it to the universal interface (cleanest, most work),
   or ship a hand-written `keycard.lidl` (quick), or make it optional in loam_core.
3. **Stop the auto-upgrade now:** rename our fork's module or bump its version above 0.3.x until the
   port ships? (P0 doesn't need it, but users can be broken again any day.)

## Known risks
- The startup auth rejection may also affect rebuilt modules (first thing P2 tests).
- Storage 3.0.0 DHT drops non-public addresses → mesh-mode Storage may break (untested).
- whisperbox_core uses the channel event, so phone↔desktop WhisperBox breaks until both sides move.
- Default-argument bug in the code generator (scala/kith lose a parameter over IPC today, not just after).
