# Basecamp 0.3.x + logos-module-builder 0.3.1 (analysis, 2026-10-02)

VERIFIED = checked against source at the cited commit or run here; else UNVERIFIED.
Sources: builder afe4430 (≈0.2.4), 2b59cb8 (0.2.6), 16e2f6b (0.3.1; 0.3.2 = +rust-sdk bump).
logos-protocol 0.2.0 (old) → 0.9.0 (builder 0.3.1 + Basecamp 0.3.1, 8bbc027). Code generator old 2e31eeb,
new 3f34c0b (both built + run on our headers). liblogos db45024. view runtime 049978f → 91dc59b.
Scratch: scratchpad/{lmb,lidl031,lidlold,cw031,cwold,ctl/sess1,ctl/sess2}.

## Key findings
- **Our contracts don't change.** afe4430 already derived LIDL from `src/<name>_impl.h`; old vs new
  generator output identical for all 8 cores (except scala's `void` methods written without `-> void`).
  No hand-written .lidl needed for our cores. Counts: loam_core 25/4, ble_mesh 5/1, scala 45/5, kith 21/3,
  qaku_core 33/2, kym_core 34/2, perun_core 12/3, whisperbox_core 16/2 (methods/events).
- **BLOCKER keycard:** 0.3.1 refuses deps without a LIDL contract and core modules without `interface`
  (`lib/common.nix:167-213`, `lib/modulePreConfigure.nix:236-250`). keycard is a legacy Qt plugin →
  `nix eval` loam_core: "lists dependencies that publish no LIDL contract: keycard" (VERIFIED).
  Fix: port keycard to universal `keycard_impl.h`, or commit `keycard.lidl` + `dependency_overrides`,
  or `optional_dependencies` / `modules().dynamic`. Fork has an uncommitted `"interface":"universal"`
  edit with no impl header — won't build as-is.
- **Old-built modules LOAD in 0.3.1 runtime (logosctl 0.3.1), but scala's startup calls to other modules
  were rejected** ("capability_module: rejecting requestModule — auth token not recognized", then
  setSenderId/start/init rejected; 12× incl. retries; ctl/sess2/logs/daemon.log). loam_core stayed
  Ready (transport never started). Calls after load worked. → rebuild everything; first test after rebuild =
  are onContextReady calls accepted (UNVERIFIED).
- **BLOCKER icons:** ui_qml needs a 256×256 PNG at manifest ≥0.4.0 (logos-package `package.cpp:54-121`).
  Only perun's is right; others are 512²/1024².
- **Version ranges:** `dependencies` entries may be objects `{name, version:"^x", signer}`; enforced at
  load, fail closed (`module_manager.cpp:851-868`, `dependency_gate`); no hyphen ranges; prereleases never
  satisfy caret. Installed version read from the plugin's EMBEDDED metadata — hand-repacked ble 0.1.1
  reports 0.1.0 (VERIFIED). Bump versions in metadata.json, not only the manifest.
- **Default arguments silently dropped** from derived contracts (old and new, `impl_header_parser.cpp:893-905`):
  scala `createCalendar`/`handleShareLink` lose `identityId`; kith `createBook`/`exportVcard`/`handleShareLink`.
  Trailing `//` comment still drops a method (6 kym_core methods hidden).
- **Caller API source-compatible:** `fooAsync` unchanged; new `fooAsyncResult(…, cb(AsyncResult<T>), timeout)`;
  `onEvent()` returns `logos::SubHandle` (implicit bool; doesn't own the sub); new onSubscriptionStatus,
  setRestartPolicy, rearmSubscriptions, `modules().dynamic(name)`; raw LpClient subscribe unchanged.
  StdLogosResult unchanged; LogosModuleContext adds aboutToUnload/unloadFinished/moduleName.
- **QML `logos`:** same functions/signatures; results still JSON-quoted. `callModule` waits ≤1.5 s for an
  unreachable module then `{"error":"Module not reachable yet"}`; `callModuleAsync` waits for the module
  (bounded by timeout, default 30000). `onModuleEvent` non-blocking, auto re-arms, events before arming
  are lost → pair with a state read. Views now call under their own identity (capability_module still
  fail-open for undeclared targets); declare every module a view calls (scala_ui, kith_ui → loam_core).
  Remove `"interface":"universal"` from views (scala_ui, kith_ui). whisperbox view never calls onModuleEvent.
- **Protocol 0.2 → 0.9:** host-services grant, per-identity token stores, teardown, caller identity,
  isolated identities start empty (0.7, breaking for isolated identities), in/out tokens, subscription
  continuity. Protocol gate refuses only a different MAJOR → old modules allowed.
- **Packages:** builder 0.3.1 packager CURRENT_VERSION 0.5.0 (UNVERIFIED by build); Basecamp reads
  manifest 0.2–0.6. Top-level allowed: manifest.json, manifest.sig, variants, assets, docs, licenses —
  keep `assets/` (icon + LIDL contracts) in repacks. 0.3.1 ships contracts in `assets/lidl/`; `lgx merge`
  refuses differing contract bytes → same builder on every platform. Unsigned = WARN. Index format
  unchanged; new optional `urls[]` (incl. `logos:<net>:<CID>`), `signature`, `includesUrl`.
  `LOGOS_USER_DIR` overrides the user dir.
- **Headless:** `logoscore` gone → `logosctl` (logos-logoscore-cli release, not in the AppImage);
  bundles capability_module, modules_state, package_manager, package_downloader, **storage_module 3.0.0
  (autoloaded)**. Session = dir (`LOGOSCTL_CONFIG_DIR`); `daemon start --detach|stop|status`,
  `install X.lgx -y`, `module load NAME`, `call M F args`, `watch M --event E`, `catalog add`. Arg typing:
  ints/bools auto; `str:` / `json:` / `@file` / `json:{"_bytes":…}`. Set HOME per session (scala writes
  `$HOME/.scala-core`, storage `~/.logos_storage`).
- **Storage 3.0.0 bundled by Basecamp 0.3.1** — scala's calls break (table in checklist).
- **ble_mesh:** bundler never packages Qt libs; both hosts Qt 6.9.2, neither ships libQt6Bluetooth;
  hand-bundling works under 0.3.1 (VERIFIED). Cleaner: lgx-portable override, external lib, or dlopen +
  StubRadio fallback. macOS same (UNVERIFIED).

## Checklist
**All modules:** pin builder 0.3.1/0.3.2; remove local/unpinned inputs (qaku_core unpinned builder;
absolute path: loam_core in kym_core, perun_core; relative path: in kym, kith_ui, whisperbox views; stale
delivery_module follows in qaku/kym views); same builder on all platforms before merge; keep assets/;
bump embedded version; hubs → logosctl.
**Cores:** keep universal; keycard decision; loam_core → upstream delivery 0.3.0; scala → storage 3.0.0:
| Method | 2.1.3 | 3.0.0 |
|---|---|---|
| uploadUrl | (path, chunk) | + advertise |
| uploadInit | (name, chunk) | + advertise |
| downloadToUrl / downloadChunks | (…, local, chunk) | + isPrivate, advertise |
| fetch / downloadManifest | (cid) | (cid, isPrivate, advertise) |
| togglePrivateQueries | present | removed |
(`scala_impl.cpp:1137,1187,1348`); remove default args (scala, kith); check onContextReady calls.
**Views:** 256² icons (all but perun); drop `interface` on scala_ui/kith_ui; declare called modules;
callModuleAsync at startup; onModuleEvent + state read; kym view → Logos.Theme/Controls.
**ble_mesh:** bundle QtBluetooth via override; macOS too.

## Unverified
Rebuilt modules vs the startup auth race; manifest 0.5.0 emission; views end-to-end in the 0.3.1 GUI;
perun_analytics `.rep` backend build; user-installed storage_module precedence over bundled 3.0.0.
