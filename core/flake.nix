{
  description = "loam_core — the Loam transport FACADE core module: a stable, bearer-agnostic API over delivery_module (and later ble_mesh / lora) with fan-out + dedup. ADR 0015.";

  inputs = {
    # port/0.3: builder 0.3.1 (Basecamp 0.3.x) + UPSTREAM delivery_module v0.3.0 — no more fork.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.3.0";
    # ble_mesh: sibling module in this monorepo (pinned to the port/0.3 commit that ships it).
    ble_mesh.url = "github:vpavlin/loam-basecamp/0521263563e4e48270d0449864396cd4900969cc?dir=ble_mesh";
    # keycard: universal-interface port of Alisher's module (vpavlin/keycard-basecamp port/0.3).
    keycard.url = "github:vpavlin/keycard-basecamp/5e89fdf6ad226d3b0710d057b840f19f6d0b80e7";
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
