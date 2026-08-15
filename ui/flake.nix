{
  description = "loam_ui — pure-QML metrics + control panel over loam_core (ADR 0015).";
  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
    loam_core.url = "github:vpavlin/loam-basecamp?dir=core";
    loam_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
