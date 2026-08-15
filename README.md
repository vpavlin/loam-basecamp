# loam-basecamp

The **Loam modules for Basecamp / desktop** — bringing Loam's local-first transport (and, later,
the BLE offline mesh) to Logos-core apps as composable modules. The desktop counterpart of the
mobile [loam-transport](https://github.com/vpavlin/loam-transport). Design: **ADR 0015** (in
loam-transport `docs/adr/0015-…`).

## Modules

| dir | module | what it is |
|-----|--------|------------|
| [`core/`](core/) | **`loam_core`** | The transport **facade**: one stable, bearer-agnostic API (`start`/`join`/`sendSealed`/`received`) + control/metrics (`setBearerEnabled`/`setBearerPriority`/`forceMesh`/`setNodeMode`/`metricsJson`) over a MultiBearer with `frameId` dedup. Depends on `delivery_module` today; `ble_mesh` / `lora` slot in behind it with zero app edits. |
| `ui/` | **`loam_ui`** *(planned, Phase 2)* | A pure-QML metrics + control panel driving `loam_core` via `logos.callModule` — the desktop counterpart of the Android Loam app's panel / `LoamDebug`. |
| [`ble_mesh/`](ble_mesh/) | **`ble_mesh`** | The BLE offline-mesh bearer as its own reusable module. **Portable flood-gossip + frame codec done & tested** (`frameId` parity-checked against `bearer.ts`, 28 checks), builds/loads as a module with a stub radio; the Qt Bluetooth radio is Phase 3 (needs hardware). |

## Architecture (why a facade)

Apps depend **only** on `loam_core` and program against one transport API; `loam_core` fans each
sealed write to every bearer and dedups receives, so a write arriving over Waku **and** BLE folds
once. Adding a bearer is a `loam_core` change with **zero app edits**. See ADR 0015 for the full
rationale, the Android↔desktop parity strategy, and the phased plan.

## Tooling baseline

Released logos-core tooling: **module-builder 0.2.6**, **delivery `v0.2.0`** (the released
channel/SDS API), **cpp-sdk 0.2.0**. Apps that depend on `loam_core` must share this SDK ABI.

## Build

Each module is its own Nix flake:

```sh
cd core && nix build .#packages.x86_64-linux.default -o result
# → result/lib/loam_core_plugin.so  +  result/include/loam_core_api.{h,cpp}
```
