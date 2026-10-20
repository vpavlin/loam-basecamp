#!/usr/bin/env bash
# Unit test for src/mesh_wire.hpp (shrooms_mesh frame codec + peer parsing).
set -euo pipefail
cd "$(dirname "$0")"
NLOHMANN=$(find /nix/store -maxdepth 5 -name json.hpp -path '*nlohmann*' 2>/dev/null | head -1)
SSL=$(find /nix/store -maxdepth 4 -name libcrypto.so -path '*openssl-3*' 2>/dev/null | head -1 | xargs -r dirname | xargs -r dirname)
g++ -std=c++17 -Wall -I../src -I"$(dirname "$(dirname "$NLOHMANN")")" ${SSL:+-I"$SSL/include" -L"$SSL/lib" -Wl,-rpath,"$SSL/lib"} -Wno-deprecated-declarations \
    wire_test.cpp -o /tmp/shrooms_mesh_wire_test -lcrypto
/tmp/shrooms_mesh_wire_test
