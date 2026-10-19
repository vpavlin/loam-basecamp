{
  description = "shrooms_mesh — a Loam bearer over the Shrooms mesh (WireGuard-authenticated IPv6 between your own devices).";

  inputs = {
    # builder 0.3.1 (Basecamp 0.3.x). No module dependencies.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    nixpkgs.follows = "logos-module-builder/nixpkgs";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
