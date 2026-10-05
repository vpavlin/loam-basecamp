// loam_ui — the Loam control panel: live per-bearer metrics + controls over loam_core (ADR 0015).
// Pure QML (no C++ backend), calls loam_core via logos.callModule and renders its JSON. Polls
// metricsJson on a Timer (Basecamp 0.2.0 may not deliver module events to QML). Uses the Logos
// design system (Theme tokens + a version-safe LogosText/LogosButton baseline).
import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
  id: root
  property var metrics: ({ bearers: [], peers: -1, connected: false })
  property string statusText: ""
  property string mode: "Core"

  // ── bridge ────────────────────────────────────────────────────────────────
  // No blocking cross-module calls in a view: callModuleAsync when the host has it, else the old
  // callModule deferred to the next event-loop turn. cb gets the result, peeled of the bridge's
  // extra JSON-quoting (up to twice).
  function callCore(method, args, cb) {
    var a = args || [];
    var deliver = function (r) {
      for (var i = 0; i < 2 && typeof r === "string"; i++) { var t = r.trim(); if (t.charAt(0) !== '"') break; try { r = JSON.parse(r); } catch (e) { break; } }
      if (cb) { try { cb(r === undefined || r === null ? "" : String(r)); } catch (e2) { console.warn("loam view: callback error: " + e2); } }
    };
    if (typeof logos === "undefined" || logos === null) { Qt.callLater(function () { deliver(""); }); return; }
    if (typeof logos.callModuleAsync === "function") {
      try { logos.callModuleAsync("loam_core", method, a, deliver, 20000); }
      catch (e) { Qt.callLater(function () { deliver(""); }); }
      return;
    }
    Qt.callLater(function () {
      var r = "";
      try { r = (typeof logos.callModule === "function") ? logos.callModule("loam_core", method, a) : ""; } catch (e) {}
      deliver(r);
    });
  }
  function parseObj(raw) {
    var s = String(raw || "").trim();
    if (s.charAt(0) === '"') { try { s = String(JSON.parse(s)).trim(); } catch (e) { return null; } }
    if (s.charAt(0) !== "{") return null;
    try { return JSON.parse(s); } catch (e) { return null; }
  }
  property bool refreshBusy: false
  function refresh() {
    if (root.refreshBusy) return;          // single-flight: a slow core can't pile 2.5 s polls up
    root.refreshBusy = true;
    callCore("metricsJson", [], function (raw) {
      var o = parseObj(raw);
      if (o && o.bearers !== undefined) root.metrics = o;
      callCore("status", [], function (st) { root.statusText = st; root.refreshBusy = false; });
    });
  }
  // friendly name + a plain "when & why" per bearer — matches the mobile Loam app.
  function bearerInfo(name) {
    if (name === "delivery") return { label: "Logos network",
      why: "The internet path — reaches anyone on the Logos network, anywhere there's a connection. It's how apps sync by default." };
    if (name === "ble") return { label: "Bluetooth mesh",
      why: "No internet needed — nearby devices sync directly, device-to-device, over Bluetooth. Auto-arms when the Logos path drops and heals back when it returns." };
    if (name === "lora") return { label: "LoRa",
      why: "Long-range, low-power radio — kilometres, off-grid. Apps don't change; it's just another pipe Loam fans writes across." };
    return { label: name, why: "" };
  }
  function setBearer(name, on) { callCore("setBearerEnabled", [name, on ? "1" : "0"], function () { root.refresh(); }); }
  function setForceMesh(on)    { callCore("forceMesh", [on ? "1" : "0"], function () { root.refresh(); }); }
  function setMode(m)          { root.mode = m; callCore("setNodeMode", [m]); }

  // ── identity (loam-keycard ADR 0001): one root → separate identity per app/space + one main identity.
  // The 12 words are kept encrypted by loam_core; unlocked once per session with a password.
  property var hd: ({ exists: false, unlocked: false })
  property string hdWords: ""      // shown ONCE (after create / on reveal), then cleared
  property string hdError: ""
  property bool hdRestoring: false
  property bool hdBusy: false
  function hdRefresh() { callCore("hdStatus", [], function (raw) { var o = root.parseObj(raw); if (o) root.hd = o; }); }
  function hdRun(method, args, onOk) {
    root.hdError = ""; root.hdBusy = true;
    callCore(method, args, function (raw) {
      root.hdBusy = false;
      var o = root.parseObj(raw);
      if (!o) { root.hdError = "no answer from loam_core"; return; }
      if (o.error) { root.hdError = o.error; return; }
      if (onOk) onOk(o);
      root.hdRefresh();
    });
  }

  Timer { interval: 2500; running: true; repeat: true; onTriggered: { root.refresh(); root.hdRefresh() } }
  Component.onCompleted: { Qt.callLater(root.refresh); Qt.callLater(root.hdRefresh) }

  // ── layout ──────────────────────────────────────────────────────────────
  Rectangle { anchors.fill: parent; color: Theme.palette.background }

  ColumnLayout {
    anchors.fill: parent
    anchors.margins: Theme.spacing.large
    spacing: Theme.spacing.medium

    // header + connection
    RowLayout {
      Layout.fillWidth: true
      LogosText { textFormat: Text.PlainText; text: "Loam"; font.pixelSize: Theme.typography.sizeXLarge; font.bold: true; color: Theme.palette.text }
      Item { Layout.fillWidth: true }
      Rectangle {
        width: 10; height: 10; radius: 5; Layout.alignment: Qt.AlignVCenter
        color: root.metrics.connected ? Theme.palette.success : Theme.palette.warning
      }
      LogosText { textFormat: Text.PlainText;
        text: (root.metrics.connected ? "connected" : "connecting…") +
              (root.metrics.peers >= 0 ? "  ·  " + root.metrics.peers + " peers" : "")
        color: Theme.palette.textTertiary
      }
    }
    LogosText { textFormat: Text.PlainText; text: root.statusText; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall }

    // identity
    LogosText { textFormat: Text.PlainText; text: "IDENTITY"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.topMargin: Theme.spacing.small }
    Rectangle {
      Layout.fillWidth: true
      radius: Theme.spacing.radiusSmall
      color: Theme.palette.surface
      border.color: Theme.palette.border; border.width: 1
      implicitHeight: idcol.implicitHeight + Theme.spacing.medium * 2
      ColumnLayout {
        id: idcol
        anchors.fill: parent; anchors.margins: Theme.spacing.medium; spacing: Theme.spacing.small

        // the 12 words, shown once
        LogosText { textFormat: Text.PlainText; visible: root.hdWords.length > 0
          text: "Write these 12 words down, in order, and keep them safe. They restore every identity on a new computer or phone. Anyone who has them can act as you."
          color: Theme.palette.textTertiary; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        LogosText { textFormat: Text.PlainText; visible: root.hdWords.length > 0
          text: { var w = root.hdWords.split(" "), out = []; for (var i = 0; i < w.length; i++) out.push((i + 1) + ". " + w[i]); return out.join("    "); }
          font.bold: true; color: Theme.palette.text; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        LogosButton { visible: root.hdWords.length > 0; text: "I've written them down"; onClicked: root.hdWords = "" }

        // no root yet
        LogosText { textFormat: Text.PlainText; visible: !root.hd.exists && root.hdWords.length === 0
          text: "One set of 12 recovery words gives you a separate identity in every app and every shared calendar or room, so they can't be linked, plus one main identity you share with people you know. Choose a password to protect it on this computer."
          color: Theme.palette.textTertiary; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        LogosTextField { id: hdWordsIn; visible: !root.hd.exists && root.hdRestoring && root.hdWords.length === 0
          placeholderText: "your 12 recovery words"; Layout.fillWidth: true }
        LogosTextField { id: hdPass; visible: (!root.hd.exists || !root.hd.unlocked) && root.hdWords.length === 0
          placeholderText: root.hd.exists ? "password" : "new password (6+ characters)"; echoMode: TextInput.Password; Layout.fillWidth: true }
        RowLayout {
          visible: !root.hd.exists && root.hdWords.length === 0; spacing: Theme.spacing.small
          LogosButton { visible: !root.hdRestoring; enabled: !root.hdBusy; text: root.hdBusy ? "Creating…" : "Create"
            onClicked: root.hdRun("hdCreate", [hdPass.text], function (o) { root.hdWords = o.mnemonic || ""; hdPass.text = ""; }) }
          LogosButton { visible: !root.hdRestoring; text: "I have recovery words"; onClicked: root.hdRestoring = true }
          LogosButton { visible: root.hdRestoring; enabled: !root.hdBusy; text: root.hdBusy ? "Restoring…" : "Restore"
            onClicked: root.hdRun("hdImport", [hdWordsIn.text, hdPass.text], function () { hdWordsIn.text = ""; hdPass.text = ""; root.hdRestoring = false; }) }
          LogosButton { visible: root.hdRestoring; text: "Cancel"; onClicked: { root.hdRestoring = false; root.hdError = ""; } }
        }

        // root exists
        LogosText { textFormat: Text.PlainText; visible: root.hd.exists && root.hdWords.length === 0
          text: "Main identity — share it with family and friends so they can add you to calendars and contacts."
          color: Theme.palette.textTertiary; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        LogosText { textFormat: Text.PlainText; visible: root.hd.exists && root.hdWords.length === 0
          text: root.hd.mainAddress || ""; font.family: "monospace"; color: Theme.palette.text }
        LogosText { textFormat: Text.PlainText; visible: root.hd.exists && root.hdWords.length === 0
          text: root.hd.unlocked ? "Unlocked — your apps can sign." : "Locked — unlock so your apps can sign."
          color: root.hd.unlocked ? Theme.palette.success : Theme.palette.warning; font.pixelSize: Theme.typography.sizeSmall }
        RowLayout {
          visible: root.hd.exists && root.hdWords.length === 0; spacing: Theme.spacing.small
          LogosButton { visible: !root.hd.unlocked; enabled: !root.hdBusy; text: root.hdBusy ? "Unlocking…" : "Unlock"
            onClicked: root.hdRun("hdUnlock", [hdPass.text], function () { hdPass.text = ""; }) }
          LogosButton { visible: root.hd.unlocked; text: "Lock"; onClicked: root.hdRun("hdLock", []) }
          LogosButton { visible: !root.hd.unlocked; text: "Show recovery words"
            onClicked: root.hdRun("hdExport", [hdPass.text], function (o) { root.hdWords = o.mnemonic || ""; hdPass.text = ""; }) }
        }
        LogosText { textFormat: Text.PlainText; visible: root.hdError.length > 0; text: root.hdError; color: Theme.palette.error }
      }
    }

    // bearers
    LogosText { textFormat: Text.PlainText; text: "BEARERS"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.topMargin: Theme.spacing.small }
    Repeater {
      model: root.metrics.bearers
      Rectangle {
        Layout.fillWidth: true
        radius: Theme.spacing.radiusSmall
        color: Theme.palette.surface
        border.color: Theme.palette.border; border.width: 1
        implicitHeight: bcol.implicitHeight + Theme.spacing.medium * 2
        ColumnLayout {
          id: bcol
          anchors.fill: parent; anchors.margins: Theme.spacing.medium; spacing: Theme.spacing.small
          RowLayout {
            Layout.fillWidth: true; spacing: Theme.spacing.small
            Rectangle {
              width: 9; height: 9; radius: 4; Layout.alignment: Qt.AlignVCenter
              color: modelData.ready && modelData.peers > 0 ? Theme.palette.success
                   : modelData.ready ? Theme.palette.warning : Theme.palette.textTertiary
            }
            LogosText { textFormat: Text.PlainText; text: root.bearerInfo(modelData.name).label; font.bold: true; color: Theme.palette.text }
            LogosText { textFormat: Text.PlainText;
              text: modelData.ready ? "ready" : "down"
              color: modelData.ready ? Theme.palette.success : Theme.palette.textTertiary
              font.pixelSize: Theme.typography.sizeSmall
            }
            Item { Layout.fillWidth: true }
            LogosButton {
              text: modelData.enabled ? "On" : "Off"
              onClicked: root.setBearer(modelData.name, !modelData.enabled)
            }
          }
          LogosText { textFormat: Text.PlainText;
            text: "peers " + modelData.peers + "   rx " + modelData.rx + "   tx " + modelData.tx + "   prio " + modelData.priority
            color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall
          }
          LogosText { textFormat: Text.PlainText;
            text: root.bearerInfo(modelData.name).why
            visible: text.length > 0
            color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall
            wrapMode: Text.WordWrap; Layout.fillWidth: true
          }
        }
      }
    }

    // controls
    LogosText { textFormat: Text.PlainText; text: "CONTROLS"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.topMargin: Theme.spacing.small }
    RowLayout {
      spacing: Theme.spacing.small
      LogosButton { text: "Force mesh"; onClicked: root.setForceMesh(true) }
      LogosButton { text: "Mesh auto"; onClicked: root.setForceMesh(false) }
    }
    RowLayout {
      spacing: Theme.spacing.small
      LogosText { textFormat: Text.PlainText; text: "Node mode:"; color: Theme.palette.textTertiary; Layout.alignment: Qt.AlignVCenter }
      LogosButton { text: "Core"; onClicked: root.setMode("Core") }
      LogosButton { text: "Edge"; onClicked: root.setMode("Edge") }
      LogosText { textFormat: Text.PlainText; text: root.mode + " (applies on restart)"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.alignment: Qt.AlignVCenter }
    }

    Item { Layout.fillHeight: true }
    LogosText { textFormat: Text.PlainText;
      text: "One shared node per phone. loam_core fans each sealed write to every bearer and dedups by frame id, so a write over Waku and the same over BLE fold to one."
      color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall
      wrapMode: Text.WordWrap; Layout.fillWidth: true
    }
  }
}
