# Integrating with Loam on Basecamp (desktop) — the `loam_core` facade

This is the **desktop/Basecamp** counterpart to [`loam/INTEGRATE.md`](https://github.com/vpavlin/loam)
(which covers the **Android** shared-node service + its owner-approval flow). On Basecamp
there is **no binder approval step**: `loam_core` is a normal **module dependency** your app
calls in-process. It owns the Logos Delivery node (+ future BLE bearer), dedups across
bearers, moves your **opaque sealed bytes** over SDS Reliable Channels, and (loam ADR 0004)
can sign through Loam identities/Keycard so keys never leave Loam.

Reference implementations: **perun** (`perun_analytics`), **kym** (`kym_core`), **qaku**
(`qaku_core`), **scala** (`scala`).

## What you get, and the trust model

- **One node, owned by loam_core.** You never `createNode`/`subscribe` directly; you hand
  loam_core a config and sealed payloads, and consume a `received` event.
- **You own end-to-end crypto.** loam_core moves **opaque** bytes — seal/open with your own
  household key (it never sees plaintext). Payloads cross the boundary as **base64 text**.
- **No approval prompt** (that's the Android AIDL service). In-process on desktop, the only
  gate is "is the node Connected yet" — surfaced via `statusChanged`.

## The wiring (declare the dependency)

`metadata.json` — add the dependency:

```json
"dependencies": ["loam_core"]
```

`flake.nix` — add the input and make its builder follow yours (one SDK ABI):

```nix
loam_core.url = "github:vpavlin/loam-basecamp?dir=core";
logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
loam_core.inputs.logos-module-builder.follows = "logos-module-builder";
```

Then call it via the generated proxy: `modules().loam_core.<method>()` and
`modules().loam_core.on("<event>", cb)`.

## The transport API (what apps call)

```cpp
std::string start(std::string cfgJson);          // "" ok / error text. Async node bringup.
std::string setSenderId(std::string id);          // set BEFORE join() — the SDS sender id.
std::string join(std::string topic);             // subscribe the content topic + open its SDS channel.
std::string sendSealed(std::string topic, std::string sealedB64);  // send opaque sealed bytes (base64 TEXT).
```

Events (`logos_events:`):

```cpp
void received(std::string topic, std::string senderId, std::string payloadB64);  // a frame, once-decoded + re-base64'd
void statusChanged(std::string status);          // "Connected" once the node is actually up
```

Also present (loam_ui drives / ADR 0004): `metricsJson()`, `status()`, a pull-based receive
drain, bearer control (`setBearerEnabled`/`priority`/`forceMesh`/`setNodeMode`), and the
identity/Keycard surface (`signWith…`, `keycardEnrol`, `keycardSign` → `keycardSignResult`).

## The bring-up sequence (get this order right)

```cpp
// 1) Subscribe BEFORE start so you never miss the first frames.
modules().loam_core.on("received", [this](const QVariantList &d){
  if (d.size() >= 3) ingestSealed(d.at(2).toByteArray());   // d[2] = payloadB64 (text)
});

// 2) Readiness is an EVENT, not start()'s return: start() returns early (async bringup).
//    The node is up only when statusChanged == "Connected" — THEN join your topic.
modules().loam_core.on("statusChanged", [this](const QVariantList &d){
  if (!d.isEmpty() && d.at(0).toString() == "Connected" && !m_ready) {
    m_ready = true;
    modules().loam_core.join(m_topic);              // subscribe + open the SDS channel
  }
});

// 3) setSenderId BEFORE start/join so the SDS channel uses your id.
modules().loam_core.setSenderId("myapp-desktop");
modules().loam_core.start(cfgJson);                 // one call owns createNode + start
```

## The config (delivery v0.2.0 — the gotchas)

Use the **LAYERED** shape. The `logos.test` preset supplies the fleet's discv5 bootstrap, so
do **not** pin bare `entryNodes` (v0.2.0's strict parser rejects top-level `WakuNodeConf`
keys). **`discv5-udp-port` is REQUIRED** or the node sits at **0 peers**. `useChannels` /
`hubMode` are loam-only flags loam_core strips before forwarding the rest to delivery verbatim.

```json
{
  "mode": "Core",
  "preset": "logos.test",
  "messagingOverrides": { "logLevel": "INFO", "tcp-port": 30303, "discv5-udp-port": 9000 },
  "useChannels": true,
  "hubMode": false
}
```

- `mode`: `Core` (relay/full) vs `Edge` (light — the battery/mobile switch).
- `hubMode: true` only for a headless always-on hub (never a normal app).

## Payload & topic conventions

- **Seal your own payload** (AEAD, household key, AAD = topic) and pass the **base64 of the
  sealed bytes** to `sendSealed`. On `received`, `payloadB64` is the base64 **text** of the
  once-decoded bytes — decode → open with your key.
- **Wire base64 depth varies by peer** (the phone's loam-transport double-base64s; legacy
  relay single). Keep a candidate-decoding receive (try raw / single / double-base64) so
  every peer opens — see perun `ingestSealed`, kym `payloadCandidates`.
- Derive one content **topic per shared room/household** from the shared secret; never
  surface the raw topic (secret-adjacent) — show a fingerprint instead.

## Checklist

- [ ] `metadata.json` deps include `loam_core`; flake input added + builder `follows` yours.
- [ ] `on("received")` and `on("statusChanged")` subscribed **before** `start()`.
- [ ] `setSenderId(...)` **before** `start()`; `join(topic)` only **after** `statusChanged == "Connected"`.
- [ ] Config is the layered `{mode, preset, messagingOverrides:{…discv5-udp-port…}}` shape — not bare keys.
- [ ] Payloads sealed by you; receive peels raw/single/double base64.
- [ ] Never leak the derived topic; surface a fingerprint.

## See also

- Android shared node + owner approval: [`loam/INTEGRATE.md`](https://github.com/vpavlin/loam/blob/master/INTEGRATE.md), loam ADR 0002 (consent), the `loam-integrate-app` skill.
- SDS channel mechanics + the silent-failure gates: the `logos-reliable-channels` skill.
- Building/publishing the module: the `logos-basecamp-module` + `logos-publish-artifacts` skills.
- House rules (why we do all this): the `logos-app-charter` skill.
