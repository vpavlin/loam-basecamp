{
  description = "ble_mesh — Loam BLE offline-mesh bearer as a reusable logos-core module (ADR 0015).";

  inputs = {
    # builder 0.3.1 (Basecamp 0.3.x). ble_mesh has NO module dependencies.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    nixpkgs.follows = "logos-module-builder/nixpkgs";
    # lgx CLI: re-adds a variant with extra files so the manifest hashes stay correct.
    logos-package.url = "github:logos-co/logos-package/c25a1167578aef5cbd9a9b6f822ffe2ae4fd6a89";
  };

  outputs = inputs@{ logos-module-builder, nixpkgs, logos-package, ... }:
    let
      base = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };

      # The portable bundler never packages Qt libraries (the host provides Qt), but Basecamp
      # (0.2.3 .. 0.3.1, Qt 6.9.2) ships no QtBluetooth, so ble_mesh failed to load with
      # "libQt6Bluetooth.so.6: cannot open shared object file" — and took loam_core and every app
      # on it down too. Bundle the library next to the plugin, built from the SAME Qt the module
      # links against, with RUNPATH=$ORIGIN; it needs only Core/DBus/Network, which the host has.
      # Linux only: macOS frameworks need a separate check.
      withQtBluetooth = system: lgxDrv:
        let
          pkgs = import nixpkgs { inherit system; };
          lgx = "${logos-package.packages.${system}.all}/bin/lgx";
          bt = "${pkgs.qt6.qtconnectivity}/lib/libQt6Bluetooth.so.6";
        in if !pkgs.stdenv.isLinux then lgxDrv else
          pkgs.runCommand lgxDrv.name { nativeBuildInputs = [ pkgs.jq pkgs.patchelf pkgs.gnutar pkgs.gzip ]; } ''
            mkdir -p $out
            LGX=$(ls ${lgxDrv}/*.lgx)
            BNAME=$(basename "$LGX")
            cp "$LGX" "$out/$BNAME"; chmod u+w "$out/$BNAME"
            tmpdir=$(mktemp -d)
            tar -xzf "$LGX" -C "$tmpdir"
            for vdir in "$tmpdir"/variants/*/; do
              v=$(basename "$vdir")
              case "$v" in linux-*) ;; *) continue ;; esac
              cp -L ${bt} "$vdir/libQt6Bluetooth.so.6"
              chmod u+w "$vdir/libQt6Bluetooth.so.6"
              patchelf --set-rpath '$ORIGIN' "$vdir/libQt6Bluetooth.so.6"
              main=$(jq -r --arg v "$v" '.main[$v]' "$tmpdir/manifest.json")
              ${lgx} add "$out/$BNAME" --variant "$v" --files "$vdir" --main "$main" -y
            done
            ${lgx} verify "$out/$BNAME" | grep -v -i 'unsigned' | grep -i -E 'mismatch|error' && exit 1 || true
            rm -rf "$tmpdir"
          '';
    in base // {
      packages = builtins.mapAttrs (system: sysPkgs:
        sysPkgs // { lgx-portable = withQtBluetooth system sysPkgs.lgx-portable; }
      ) base.packages;
    };
}
