// loam_vault.hpp: BIP39 generate/validate agrees with the phones' list; the encrypted root round-trips.
//   g++ -std=c++17 -I core/src -I <nlohmann/include> core/test/vault_test.cpp -lcrypto -o /tmp/vault_test && /tmp/vault_test
#include "loam_vault.hpp"
#include "loam_hd.hpp"
#include <cstdio>
int main() {
    int fails = 0;
    auto check = [&](bool c, const char* what) { printf("  %s %s\n", c ? "ok  " : "FAIL", what); if (!c) ++fails; };
    const std::string abandon = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    check(loamhd::validMnemonic(abandon), "official test phrase is valid");
    check(loamhd::validMnemonic("  Abandon abandon ABANDON abandon abandon abandon abandon abandon abandon abandon abandon about\n"), "normalises case and spacing");
    check(!loamhd::validMnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon"), "bad checksum rejected");
    check(!loamhd::validMnemonic("abandon abandon abandon"), "too short rejected");
    check(!loamhd::validMnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon notaword"), "unknown word rejected");
    bool allValid = true; std::string m;
    for (int i = 0; i < 200; i++) { m = loamhd::newMnemonic(); if (!loamhd::validMnemonic(m)) allValid = false; }
    check(allValid, "200 generated phrases all validate");

    loamhd::VaultParams fast; fast.n = 1u << 12;   // quick for the test
    auto v = loamhd::sealMnemonic(abandon, "correct horse", fast);
    check(!v.is_null() && v.dump().find("abandon") == std::string::npos, "vault never contains the phrase in clear");
    std::string err;
    check(loamhd::openMnemonic(v, "correct horse", err) == abandon && err.empty(), "right password opens it");
    std::string bad = loamhd::openMnemonic(v, "wrong", err);
    check(bad.empty() && err == "wrong password", "wrong password is refused");
    auto v2 = loamhd::sealMnemonic(abandon, "correct horse", fast);
    check(v2["crypto"]["ciphertext"] != v["crypto"]["ciphertext"], "fresh salt/iv each time");
    // The opened phrase derives the same main identity as the shared vectors.
    auto d = loamhd::derive(loamhd::seedFromMnemonic(loamhd::openMnemonic(v, "correct horse", err)), "", "");
    check(d.address == "0x31b399ecdd7071c32225ec674b1ab841aa2776a7", "opened root derives the expected main address");
    printf(fails ? "VAULT: %d FAILED\n" : "VAULT GREEN\n", fails);
    return fails ? 1 : 0;
}
