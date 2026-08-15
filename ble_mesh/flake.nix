{
  description = "ble_mesh — Loam BLE offline-mesh bearer as a reusable logos-core module (ADR 0015).";

  inputs = {
    # released tooling baseline (same as loam_core). ble_mesh has NO module dependencies.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
