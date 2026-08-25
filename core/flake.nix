{
  description = "loam_core — the Loam transport FACADE core module: a stable, bearer-agnostic API over delivery_module (and later ble_mesh / lora) with fan-out + dedup. ADR 0015.";

  inputs = {
    # Released logos-core tooling (latest as of 2026-08): module-builder 0.2.6 + delivery v0.2.0
    # (the released channel/SDS API — supersedes the old feat-add-channel-api-support branch).
    # delivery follows OUR module-builder so loam_core and every app that depends on it build
    # against ONE SDK ABI (avoids IPC skew). The apps must be bumped to match this baseline.
    delivery_module.url = "path:/home/vpavlin/kym-hub/sds-build/dm-channels";
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
    delivery_module.inputs.logos-module-builder.follows = "logos-module-builder";
    # ble_mesh is a sibling module in this monorepo (loam-basecamp/ble_mesh). loam_core fans
    # each sealed write to it too and funnels its frames into the same dedup'd receive stream.
    ble_mesh.url = "github:vpavlin/loam-basecamp?dir=ble_mesh";
    ble_mesh.inputs.logos-module-builder.follows = "logos-module-builder";
    # keycard: Alisher's native smartcard module (PC/SC via keycard-qt). loam_core's
    # identity service delegates signDigest for a keycard-kind identity to it. Declaring
    # it makes keycard a HARD dependency of loam_core (Basecamp has no optional deps) —
    # so every app on loam_core requires the keycard module present; it loads idle
    # without a reader. See ADR 0004 (loam) / scala ADR 0016.
    keycard.url = "github:xAlisher/keycard-basecamp";
    keycard.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  # mkLogosModule (not mkLogosQmlModule): a headless CORE module — no QML view. The
  # plugin glue is generated from src/loam_core_impl.h (universal authoring). The
  # metrics + control panel lives in a separate loam_ui QML view module (Phase 2).
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
