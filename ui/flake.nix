{
  description = "loam_ui — pure-QML metrics + control panel over loam_core (ADR 0015).";
  inputs = {
    # port/0.3: builder 0.3.1, loam_core from the same branch.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    loam_core.url = "github:vpavlin/loam-basecamp/f66ad0ac8973a314f17eca12e4ce0b9939b1a19a?dir=core";
  };
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
