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

  Timer { interval: 2500; running: true; repeat: true; onTriggered: root.refresh() }
  Component.onCompleted: Qt.callLater(root.refresh)

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
