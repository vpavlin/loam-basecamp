# Logos Storage (storage_module) → upstream, for the Basecamp 0.3.x / logos-module-builder 0.3.1 port

Date: 2026-10-02. Read-only research. Sources: local clones `~/logos-storage-module` (fetched origin, master = 93c60af),
`~/logos-storage-nim` (+ `vendor/nim-libp2p`, fetched), a scratch clone of logos-module-builder and logos-cpp-sdk,
`gh release view`, Scala `src/scala_impl.cpp` @ 541bde5 (core 0.9.42).
Items marked **UNVERIFIED** were not built or tested.

## 0. TL;DR

* The only release after our pin is **v3.0.0** (2026-09-30, tag a9c14b8, libstorage **v0.5.0**). master (93c60af, metadata **3.0.1**,
  untagged) moves to libstorage **v0.5.1** (2026-10-02: DHT startup/shutdown + private-address-leak fix + timeout fixes).
* **No release pins builder 0.3.1.** v3.0.0-rc1, v3.0.0, and master all lock logos-module-builder `fb8d551` (= 0.3.0+11, 2026-09-16,
  7 commits before the 0.3.1 tag). The input is unversioned, so `storage_module.inputs.logos-module-builder.follows` onto 0.3.1 is the
  intended way to consume it. Note: our shipped 2.1.3 was **not** built with fb8d551. Scala makes it follow builder **0.2.6**.
* v3.0.0 **requires builder ≥ 0.3.0**. It overrides `aboutToUnload()` / `LogosShutdown`, which arrived in logos-cpp-sdk 667990f
  (2026-08-20). That SDK is absent from builder 0.2.6 (cpp-sdk 2e31eeb) and present in fb8d551 (5001755) and 0.3.1 (3f34c0b).
  So the storage bump and the builder bump have to land together.
* **Breaking API for Scala:** `uploadUrl`, `downloadToUrl`, and `fetch` gained `advertise` / `isPrivate` bool params. Events and payloads
  are unchanged. Config keys are unchanged.
* **Hidden behaviour change:** `fetch()` and `downloadToUrl()` now block synchronously for up to **30 s** (FETCH_MANIFEST_TIMEOUT_MS; was
  3 s). That is longer than the 20 s IPC timeout, so Scala's cache-on-see / retry loop and `downloadAttachment` can stall the core.
* Our fixes: **(a) getProviders-local is NOT fixed upstream** (absent in nim-libp2p v2.3.5 pinned by v0.5.1 and absent on nim-libp2p
  master 68561de). **(b) IPv6 dial-bind is fixed upstream** (nim-libp2p #2952, 8d1312cc, in v2.4.0), but **not** in the v2.3.5
  that libstorage v0.5.1 pins. Both of our patches still `git apply --check` cleanly on 05e8dfea (v2.3.5).

## 1. Upstream releases after bcc29f0

`git log bcc29f0..origin/master` lists 36 commits. `gh release list --repo logos-co/logos-storage-module`:

| ref | date | metadata version | libstorage (logos-storage-nim) | nim-libp2p | builder lock |
|---|---|---|---|---|---|
| **bcc29f0** (ours; PR #79 windows-cross) | 2026-09-17 | 2.1.3 | e3225940 (untagged, after v0.4.5, "cross-compile for windows #1516") | 391e403c | fb8d551 (we override to 0.2.6) |
| v2.1.3 tag (59b494e, ancestor of bcc29f0) | 2026-09-03 | 2.1.3 | v0.4.5 | | |
| v3.0.0-rc1 (e679a87, pre-release) | 2026-09-25 / pub 09-29 | 3.0.0 | v0.5.0-rc1 (05926ed) | | fb8d551 |
| **v3.0.0** (a9c14b8, "Latest") | 2026-09-30 | 3.0.0 | **v0.5.0** (a827d9e) | 391e403c | fb8d551 |
| master 93c60af (untagged) | 2026-10-02 | **3.0.1** | **v0.5.1** (ee6e29a) | **05e8dfea (v2.3.5)** | fb8d551 |

* The v3.0.0 changelog (`gh release view v3.0.0`) covers #87 (remove doctests), #85 (total bytes in download progress), #86 (refresh
  config), #89 (bump nim), #82 (expose network config), #79 (windows), #92 (tests), **#95 node running (isRunning, nodeBusy,
  aboutToUnload)**, **#96 config managed in module (loadConfigOrDefault, init persists config)**, and **#97 feat!: v0.5.x libstorage
  API**. Then #98 and #99 (metadata, mix presets).
* After v3.0.0: f20ac68 bumps metadata to 3.0.1, 6fbe6d9 bumps libstorage to v0.5.1, 9c8fca1 updates the mix config, and #100
  fixes doctests.
* Unmerged branches: `feat/v0.5.x-api` (already merged as #97) and `fix/mix-config` (mix-config fixes, no-bootstrap doctest).
* **No GitHub release assets** (no .lgx) on v3.0.0 or v2.1.3. You build it yourself (flake `lgx-portable`), as we do today.
* libstorage v0.5.0 notes: Kad-DHT instead of discv5 (#1522; our e3225940 already uses Kad), per-dataset advertise flag (#1533),
  MixTransport for BlockExchange and manifests (#1524), advertiser gated on AutoNAT (#1529, already in e3225940), and Windows
  cross-compile. v0.5.1: #1542 DHT startup/shutdown + private address leaks, #1544 timeouts/batch length.
* **Builder:** fb8d551 = `git describe` 0.3.0-11-gfb8d551 (0.3.0 = 2026-09-13, 0.3.1 = 2026-09-22, 0.3.2 = 2026-09-23). The
  fb8d551→0.3.1 commits are design-system and logos-protocol relocks only (de169fd "unconfigured-replica UAF", 1d77ec6
  subscription-continuity). Following 0.3.1 should be safe. **UNVERIFIED** (not built).

## 2. API diff relevant to Scala

### 2a. Calls Scala makes (scala_impl.cpp) vs src/storage_module_plugin.h (bcc29f0 → master)

| Scala call (line) | old signature | new signature | action |
|---|---|---|---|
| `onStorageUploadDone/DownloadDone/Stop/Start` (1109-1112) | events | **unchanged** (the 9 events are identical, see `logos_events:`) | none |
| `init(cfg)` (1120, via ensureStorage) | bool init(string) | same signature. **New:** normalizes the cfg, stamps `config-version`, **persists cfg to `~/.logos_storage/config.json`**, returns false on a mistyped cfg, still refuses a 2nd init on a live ctx | none (note the file write; the hub's HOME is /root) |
| `start()` (1120) | bool | same. **New:** returns false if the node is busy (starting/stopping). If already running, it emits `storageStart` immediately | Scala ignores the return value. The restart path is OK |
| `uploadUrl(path, 65536)` (1137, 1187) | (string,int64) | **(string,int64,bool advertise)** | pass `true` |
| `stop()` (1229) | StdLogosResult | same. **New:** fails "Node is busy starting or stopping." | already handled (`!ok` keeps the node) |
| `destroy()` (1155, 1250) | StdLogosResult | same. **New:** fails while busy. Docs: stop before destroy | the 60 s stuck fallback (1155) calls destroy without stop. Add stop (or check `isRunning()`) first |
| `spr()` (1271) | unchanged | unchanged (timeout 1000 ms) | none |
| `exists(cid)` (1345, 1370) | unchanged | unchanged, value is a bool | none |
| `fetch(cid)` (1348, 1376) | (string), 3 s sync | **(string, bool isPrivate, bool advertise)**, **sync up to 30 s**. libstorage `fetch` awaits `fetchManifest` before spawning the dataset task (node_storage_request.nim:109-130) | pass `(cid,false,true)`. **Must not be called inline in loops on the core thread.** See checklist |
| `downloadToUrl(cid, path, false, 65536)` (1391) | (string,string,bool,int64). Manifest pre-fetch 3 s + init 1 s | **(string,string,bool,int64,bool isPrivate,bool advertise)**. Manifest pre-fetch **30 s** + download_init **30 s**, both synchronous | pass `false,true`. Expect IPC-timeout "rejected" on slow manifests. Treat a failure as possibly in flight (the poll path already exists) |
| `manifests()` (1434) | unchanged | unchanged output fields (`cid`, `datasetSize`, `filename`) | none |
| events payloads | `{sessionId, success, cid/error}` | unchanged (emitSessionResult, the same fields) | none |

Other changes Scala doesn't use today: `migrateConfig(cfg)` was **removed** and replaced by `loadConfigOrDefault()`.
`togglePrivateQueries` was removed. New: `isRunning()`, `getAdvertise(cid)`, `setAdvertise(cid,bool)`, and an `isPrivate/advertise`
pair on `downloadChunks`, `downloadManifest`, and `uploadInit`. `aboutToUnload()` now stops and destroys the node on host teardown.

### 2b. Config keys

The `conf.nim` option names at e3225940 and at v0.5.1 are **identical** (the extracted `name:` lists match exactly). Every key Scala
sets is still valid: `log-level`, `data-dir`, `listen-port`, `listen-ip`, `nat` (`extip:<ip>`), `bootstrap-node`, `no-bootstrap-node`,
`network`, `autonat-server`, and `relay-server`. **No renames.**
Module-side changes: when `data-dir` is missing the default is `~/.logos_storage/data` (Scala always sets it). Mix config is
auto-filled only when `mix-enabled` is true and the bootstrap is not custom. `hasCustomBootstrap` now also treats
`no-bootstrap-node:true` as custom. `mix-enabled` defaults to false in nim. Scala is unaffected.

### 2c. Behaviour changes in libstorage that matter to our topology

* **#1542 address policy (v0.5.1):** if any bootstrap node or the extip is public (`isPublicNetwork`), the Kad DHT uses
  `dialableMixAddressPolicy`, which drops non-global addresses (`isGlobal()`), and **ULA fd00::/8 mesh addresses are non-global**.
  Clients that bootstrap off the public VPS hub will therefore not exchange `fdb0:`/`fd3b:` mesh addresses via the DHT. The
  storage_mesh path (client announces a mesh IPv6 extip while bootstrapping from the public hub) may stop working, and the crib
  mesh hub (private-only) is unaffected. **UNVERIFIED** (code-read only). The switch-level `dialableAddressPolicy` already dropped
  private addresses before this change.
* **#1542:** extip `announcedAddrs` is set before switch start (good for the hub), and `switch.stop` is capped at 5 s, so
  stop→storageStop gets faster.
* **#1533 per-dataset advertise:** repo marks only *disabled* datasets (`metaDs.has(advertiseKey)` ⇒ not advertised). Existing
  datasets in the hub data-dir stay advertised and served. Manifest requests for non-advertised CIDs answer NotFound. Data-dir reuse
  from e3225940 → v0.5.1 looks additive. **UNVERIFIED** (not run against the live hub repo; back it up first).
* **Wire compat with the e3225940 desktops and the Android client (vpavlin/nim-codex 6af2049):** the codecs (blockexc, manifest) look
  unchanged. The Mix path is an add-on. **UNVERIFIED**. Test hub-v0.5.1 ↔ old desktop ↔ phone before cutting over.
* Android C API (if you ever rebase build-libstorage.sh onto v0.5.x): `storage_upload_init(+advertise)`,
  `storage_download_init(+isPrivate,+advertise)`, `storage_download_stream` **lost `local`**, `storage_fetch(+isPrivate,+advertise)`,
  `storage_download_manifest(+isPrivate,+advertise)`, plus `storage_get_advertise/set_advertise`
  (library/libstorage.h@v0.5.1). The JNI glue would need changes. Not required while wire compat holds.

## 3. Status of our two nim-libp2p fixes

| fix | upstream status | in libstorage v0.5.0 (v3.0.0)? | in v0.5.1 (master)? |
|---|---|---|---|
| (a) `kademlia/provider.nim getProviders` ignores `providerManager.knownKeys` (patch `0002-kad-getproviders-include-local-records.patch`) | **Not fixed.** nim-libp2p master 68561de (2026-10-01) still only adds self + remote replies (provider.nim:552-555). No matching PR (repo is now `iftech/nim-libp2p`; searched "getProviders", "knownKeys") | no (391e403c) | no (05e8dfea, provider.nim:508) |
| (b) TCP dial binds to `addrs[0]` (IPv4) → EINVAL to IPv6 when NotReachable (patch `0001-tcp-dial-bind-same-family.patch`) | **Fixed upstream:** 8d1312cc "fix(TcpTransport): dual-stack reuse bug (#2952)", 2026-08-17, `findAddressByFamily(self.addrs, ta.family)`. In tag v2.4.0 (2026-09-28) | no | **no** (v2.3.5 = 05e8dfea is not a descendant of 8d1312cc) |

Both patches apply cleanly to 05e8dfea (`git apply --check` rc=0 in a scratch worktree).

**How to carry them in a module build**, in order of preference:
1. **Fork-pinned input (reproducible off-box):** in `vpavlin/nim-codex`, create a branch `hub-v0.5.1` from tag v0.5.1. Point
   `vendor/nim-libp2p` at a `vpavlin/nim-libp2p` branch of 05e8dfea + 0002 (+ 0001, or cherry-pick 8d1312cc). Then build the
   storage module with
   `nix build .#lgx-portable --override-input logos-storage 'git+https://github.com/vpavlin/nim-codex?ref=hub-v0.5.1&submodules=1'`.
   libstorage's `nix/default.nix` asserts `src.submodules == true`, so the `submodules=1` flag is mandatory.
2. **Local patched path (what we do today):** copy the v0.5.1 tree with submodules, apply the patches in `vendor/nim-libp2p`, and run
   `--override-input logos-storage path:<dir>`. This is quick, but the build only reproduces on this box.
3. Builder-level patching: `externalLibInputs` resolves `input` + `packages.default` (builder lib/mkExternalLib.nix). It has no
   patches hook, so an overlay would mean a wrapper flake that re-exports `packages.<sys>.libstorage` with `overrideAttrs { postPatch =
   "patch -d vendor/nim-libp2p -p1 < …"; }`. Workable, but more glue than option 1. **UNVERIFIED**.

Only the **hub** needs (a). (b) matters for no-extip desktops dialing IPv6 peers (mesh) and for Android, which already carries it
via build-libstorage.sh. Upstreaming (a) is the long-term fix, but per memory, ask vpavlin before filing.

## 4. Platform coverage (master / v3.0.0)

* Builder systems (fb8d551 lib/common.nix:89): `aarch64-darwin`, `x86_64-darwin`, `aarch64-linux`, `x86_64-linux`, and the
  pseudo-system `x86_64-windows` (cross).
* libstorage flake (v0.5.1): x86_64/aarch64 linux, x86_64/aarch64 darwin, and x86_64-windows (cross from x86_64-linux).
* Storage-module CI: `ci.yml` builds `nix build` on **ubuntu-latest + macos-latest** (x86_64-linux, aarch64-darwin), and
  `windows.yml` builds `default lgx-portable` for **x86_64-windows** via logos-windows-ci with a smoke test. **aarch64-linux is not
  built in CI** (the flake exposes it; **UNVERIFIED**).
* The libstorage v0.5.1 GitHub release ships prebuilt `libstorage-{linux-amd64,linux-arm64,darwin-arm64}` (+static) and
  `windows-amd64` zips. The module does not use them: it builds from source.
* Bundled native libs (metadata `include`): `libstorage.so` / `.dylib` / `.dll`. Our current linux-amd64 lgx variant also carries
  `libboost_system.so.1.87.0`, `libcrypto.so.3`, `libssl.so.3`, and `storage_module_plugin.so`, plus `assets/lidl/storage_module.lidl`.
  Windows lgx-portable omits the DLLs Basecamp provides (windows.yml comment).

## 5. Checklist

### Scala core (`/home/vpavlin/scala`)
- [ ] `flake.nix`: move `logos-module-builder` to 0.3.1 (required, because the v3 module needs the aboutToUnload SDK). Pin
      `storage_module.url = "github:logos-co/logos-storage-module/v3.0.0"` (libstorage v0.5.0) **or** master 93c60af/3.0.1
      (v0.5.1, unreleased). Stop tracking unpinned master; today any `nix flake update` would silently pull the breaking API.
      Keep `storage_module.inputs.logos-module-builder.follows`.
- [ ] `metadata.json`: bump the dependency expectation (storage_module 3.x), and bump Scala's own version.
- [ ] `uploadUrl(tmpPath, 65536)` → `uploadUrl(tmpPath, 65536, true)` (lines 1137, 1187).
- [ ] `downloadToUrl(cid, sealedPath, false, 65536)` → `(…, 65536, false /*isPrivate*/, true /*advertise*/)` (1391).
- [ ] `fetch(cid)` → `fetch(cid, false, true)` (1348, 1376).
- [ ] **Blocking:** `fetch` and `downloadToUrl` can now take ≥ 30 s synchronously (over the 20 s IPC timeout). Do one of the following:
      cap `cacheAttachments` / `retryCacheFetches` to ONE fetch per tick, deferred via `onLoop`; or move cache-on-see to
      `downloadManifest(cid,false,true)` (async, `storageDownloadManifestDone`) followed by `fetch` only once the manifest is known.
      In `downloadAttachment`, treat an IPC timeout as "pending", not "rejected": keep the `m_pendDown` entry so
      `pollStorageSessions` can complete it.
- [ ] Restart path: the 60 s stuck fallback (1155) calls `destroy()` on a possibly running or busy node, and v3 refuses while busy.
      Call `stop()` first or gate on `isRunning()`, and retry destroy when it returns "busy". The normal path (stop → storageStop →
      destroy → init → start) is compatible because busy is cleared before the event is emitted.
- [ ] `ensureStorage` re-entry: `start()` on a running node now emits `storageStart` immediately. Make sure the
      `m_storageAwaitStart` logic tolerates an extra event.
- [ ] Do not use `migrateConfig` (removed). Optionally use `isRunning()` in diagnostics.
- [ ] Note that `init()` writes `~/.logos_storage/config.json` (harmless; Scala passes its own data-dir).
- [ ] Mesh mode under v0.5.1 #1542: re-test `storage_mesh=1` desktop ↔ crib with the public hub as bootstrap. Expect DHT records
      without ULA addresses. **UNVERIFIED**.
- [ ] Re-run the headless rigs (`stor-desk` / `stor-desk2`, including `SCALA_TEST_DROP_STORAGE_EVENTS=1`) against the new module.
      Check desk→hub→desk and phone fetch from the hub (the Android 0.4.x-era client against a v0.5.x hub).
- [ ] macOS / Windows lgx: rebuild via the multiplatform workflow. The v3 module has Windows support, but Scala's own deps decide
      the rest.

### VPS hub (`root@198.19.139.213`, scala-hub.service)
- [ ] Back up `/root/scala-hub` and the storage data-dir before the cutover (repo metadata gets the new advertise namespace; the
      change is additive, but **UNVERIFIED** on our data).
- [ ] Build the PATCHED module from libstorage **v0.5.1** (DHT startup/shutdown fixes; 05e8dfea) plus patch 0002 (and 0001, or
      cherry-pick 8d1312cc), using a fork-pinned `--override-input logos-storage git+…vpavlin/nim-codex?ref=hub-v0.5.1&submodules=1`.
      Build it with builder 0.3.1, matching the new logoscore runtime.
- [ ] Upgrade the whole hub stack together: logoscore/logos-hub runtime (0.3.x), scala core, loam_core, and delivery. The v3 module
      won't load in a 0.2.x-SDK host. **UNVERIFIED** (load test needed).
- [ ] Keep the hub cfg: `storage_root=1`, explicit public `storage_extip`, `autonat-server` + `relay-server` (key names unchanged).
      With a public extip, #1542 switches the hub's Kad to the dialable-only address policy, which is fine for the public hub.
- [ ] Verify with `rsync --checksum` plus an md5 of `libstorage.so` (nix mtime trap). Then confirm that the hub's own `fetch` of a
      NAT-ed client's CID succeeds; that proves 0002 is in. Confirm relay reservations, and confirm cache-on-see under the new 30 s
      fetch semantics.
- [ ] The hub's HOME gets `/root/.logos_storage/config.json` written on init. It is benign; don't confuse it with the Scala data-dir.
