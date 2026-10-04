# Testing the 0.3 port (Basecamp 0.3.1)

Everything below is on `port/0.3` branches; nothing is merged or published to the normal repos.

## Setup (keeps your Basecamp 0.2.3 untouched)

```sh
mkdir -p ~/basecamp-0.3-home
HOME=~/basecamp-0.3-home \
SSL_CERT_FILE=/home/vpavlin/lan-cert.pem \
MESH_PROTOCOL=meshcore \
/path/to/LogosBasecamp-Desktop-v0.3.1-aeb819-x86_64.AppImage
```
Use absolute paths after `HOME=` (`~` would point into the new folder). The profile starts empty.

In the package manager add ONLY this repo (don't add the official catalog for this test — it has
other versions of some modules):

    https://jimmy-crib.office.mesh:8444/basecamp-0.3/logos-repo.json

| Package | Version | Notes |
|---|---|---|
| delivery_module | 0.3.0 | UPSTREAM, no fork; RLN off via loam_core config |
| loam_core | 0.5.4 | upstream delivery, format unwrap, URL-safe base64, nodeStarted, random ports |
| ble_mesh | 0.2.0 | QtBluetooth bundled by the build |
| keycard / keycard-ui | 1.1.0 | universal port — needs your card reader |
| scala / scala_ui | 0.10.4 / 0.9.0 | storage 3.0 (host-owned), identity param fix |
| qaku_core / qaku | 0.2.1 / 0.2.0 | |
| kym_core / kym | 0.8.0 / 0.7.0 | plain-relay topic fix (loam_core 0.5.4) |
| kith / kith_ui | 0.3.1 / 0.3.0 | identity param fix |
| perun_core / perun_analytics | 0.10.1 / 0.9.0 | |
| loam_ui | 0.2.0 | |

## Checklist

1. **Install**: Scala (pulls loam_core, delivery 0.3.0, ble_mesh, keycard, storage is the host's).
   No missing-library or "blocked by dependency" popups. Each view opens (icons show).
2. **Connect**: Scala's status reaches Connected within ~30 s (not stuck at "Connecting…").
   If it shows "Delivery error: …", send me the text.
3. **Scala sync**: create a calendar, share it to a phone (phone app as today, old library) — the
   phone joins and sees events; an event made on the phone appears on the desktop, and vice versa.
   (Tests old<->new across the format change.)
4. **Identity**: create/join a calendar with a non-default identity — it's actually used now.
5. **Attachments**: attach a small file. Expected today: upload OK; another device can only fetch it
   if a reachable node has it (see Storage below) — a phone/desktop behind NAT won't reach you
   directly. Not a regression to report, but tell me what you see.
6. **Qaku / kith / kym / Perun**: open each, pair/join with a phone or a second desktop, check one
   item syncs each way. kym desktop<->desktop over plain relay was broken before (fixed in 0.5.4).
7. **Keycard** (card reader attached): keycard-ui sees the reader + card; enrol + one signed write.
8. **Restart Basecamp**: data and identities persist; everything reconnects without manual steps.

## Known limitations

- **Storage**: Basecamp 0.3 owns the Storage node (it initializes it from
  `~/.logos_storage/config.json` on the PUBLIC logos.test network); Scala adopts it as-is. A NAT-ed
  node announces no addresses, so attachments/snapshots between home users need a reachable hub
  that is a relay + AutoNAT server and caches (proven with the test hub at 128.140.55.128:8299:
  NAT-ed A -> hub cache -> NAT-ed B). Clients must BOOTSTRAP from that hub (bootstrap-node in the
  host's config file); a runtime connect isn't enough. Open question for the Logos team.
- **Phones** stay on the old library; they only need the loam-transport unwrap update
  (`loam-transport` port/0.3) for desktop->phone channel messages. Not yet rolled out.
- Every new-library message carries an ~18.7 KB SDS bloom filter (~25 KB/msg) — ask Logos.
- macOS / Linux ARM builds of the 0.3 stack not made yet.
