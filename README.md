# loam-basecamp

The **Loam modules for Basecamp / desktop** — bringing Loam's local-first transport (and, later,
the BLE offline mesh) **and its identity/signing service (incl. Keycard)** to Logos-core apps as
composable modules. The desktop counterpart of the mobile
[loam-transport](https://github.com/vpavlin/loam-transport). Design: transport **ADR 0015** (in
loam-transport `docs/adr/0015-…`); identity **[loam ADR 0004](https://github.com/vpavlin/loam/blob/master/docs/adr/0004-identity-as-a-loam-service.md)**.

## Modules

| dir | module | what it is |
|-----|--------|------------|
| [`core/`](core/) | **`loam_core`** (0.3.0) | Two services behind one module. **Transport facade:** one stable, bearer-agnostic API (`start`/`join`/`sendSealed`/`received`) + control/metrics (`setBearerEnabled`/`setBearerPriority`/`forceMesh`/`setNodeMode`/`metricsJson`) over a MultiBearer with `frameId` dedup. **Identity service (loam ADR 0004):** key custody + signing (`listIdentities`/`addSoftIdentity`/`bindContainer`/`identityForContainer`/`signDigest`) so apps sign through loam and never hold a private key — plus **Keycard** (`enrollKeycard`/`keycardSign` + the `keycardSignResult` event) delegating on-card signing to Alisher's `keycard` module at `m/43'/60'/1582'` (== mobile, one card = one identity across phone + desktop). Declares deps `delivery_module`, `ble_mesh`, **`keycard`**; `lora` slots in behind the facade with zero app edits. |
| [`ui/`](ui/) | **`loam_ui`** | A pure-QML metrics + control panel driving `loam_core` via `logos.callModule` — live per-bearer stats + controls (enable/priority, force-mesh, node mode). Builds to a Basecamp `.lgx`; the desktop counterpart of the Android Loam panel / `LoamDebug`. |
| [`ble_mesh/`](ble_mesh/) | **`ble_mesh`** | The BLE offline-mesh bearer as its own reusable module. **Portable flood-gossip + frame codec done & tested** (`frameId` parity-checked against `bearer.ts`, 28 checks), builds/loads as a module with a stub radio; the Qt Bluetooth radio is Phase 3 (needs hardware). |

## Architecture (why a facade)

Apps depend **only** on `loam_core` and program against one transport API; `loam_core` fans each
sealed write to every bearer and dedups receives, so a write arriving over Waku **and** BLE folds
once. Adding a bearer is a `loam_core` change with **zero app edits**. See ADR 0015 for the full
rationale, the Android↔desktop parity strategy, and the phased plan.

The same shape holds for **identity** (loam ADR 0004): apps ask `loam_core` to sign and never hold a
private key; the identity/Keycard custody is audited once in one module instead of smeared across every
app's core. This does mean `keycard` is a **mandatory dependency** of `loam_core` — so every app that
uses `loam_core` (kym, qaku, perun…) installs the `keycard` module too; it sits idle without a reader.

## Tooling baseline

Released logos-core tooling: **module-builder 0.2.6**, **delivery `v0.2.0`** (the released
channel/SDS API), **cpp-sdk 0.2.0**. Apps that depend on `loam_core` must share this SDK ABI.

## Build

Each module is its own Nix flake:

```sh
cd core && nix build .#packages.x86_64-linux.default -o result
# → result/lib/loam_core_plugin.so  +  result/include/loam_core_api.{h,cpp}
```
