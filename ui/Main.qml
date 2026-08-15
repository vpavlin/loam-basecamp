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
  function callCore(method, args) {
    if (typeof logos === "undefined" || !logos.callModule) return "";
    var r = logos.callModule("loam_core", method, args || []);
    // returns may come back double-encoded through the bridge — peel up to twice
    for (var i = 0; i < 2 && typeof r === "string"; i++) { var t = r.trim(); if (t.charAt(0) !== '"') break; try { r = JSON.parse(r); } catch (e) { break; } }
    return String(r);
  }
  function parseObj(raw) {
    var s = String(raw || "").trim();
    if (s.charAt(0) === '"') { try { s = String(JSON.parse(s)).trim(); } catch (e) { return null; } }
    if (s.charAt(0) !== "{") return null;
    try { return JSON.parse(s); } catch (e) { return null; }
  }
  function refresh() {
    var o = parseObj(callCore("metricsJson", []));
    if (o && o.bearers !== undefined) root.metrics = o;
    root.statusText = callCore("status", []);
  }
  function setBearer(name, on) { callCore("setBearerEnabled", [name, on ? "1" : "0"]); Qt.callLater(root.refresh); }
  function setForceMesh(on)    { callCore("forceMesh", [on ? "1" : "0"]); Qt.callLater(root.refresh); }
  function setMode(m)          { root.mode = m; callCore("setNodeMode", [m]); }

  Timer { interval: 2500; running: true; repeat: true; onTriggered: root.refresh() }
  Component.onCompleted: Qt.callLater(root.refresh)   // defer — a sync callModule during construction freezes the view

  // ── layout ──────────────────────────────────────────────────────────────
  Rectangle { anchors.fill: parent; color: Theme.palette.background }

  ColumnLayout {
    anchors.fill: parent
    anchors.margins: Theme.spacing.large
    spacing: Theme.spacing.medium

    // header + connection
    RowLayout {
      Layout.fillWidth: true
      LogosText { text: "Loam"; font.pixelSize: Theme.typography.sizeXLarge; font.bold: true; color: Theme.palette.text }
      Item { Layout.fillWidth: true }
      Rectangle {
        width: 10; height: 10; radius: 5; Layout.alignment: Qt.AlignVCenter
        color: root.metrics.connected ? Theme.palette.success : Theme.palette.warning
      }
      LogosText {
        text: (root.metrics.connected ? "connected" : "connecting…") +
              (root.metrics.peers >= 0 ? "  ·  " + root.metrics.peers + " peers" : "")
        color: Theme.palette.textTertiary
      }
    }
    LogosText { text: root.statusText; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall }

    // bearers
    LogosText { text: "BEARERS"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.topMargin: Theme.spacing.small }
    Repeater {
      model: root.metrics.bearers
      Rectangle {
        Layout.fillWidth: true
        radius: Theme.spacing.radiusSmall
        color: Theme.palette.surface
        border.color: Theme.palette.border; border.width: 1
        implicitHeight: brow.implicitHeight + Theme.spacing.medium * 2
        RowLayout {
          id: brow
          anchors.fill: parent; anchors.margins: Theme.spacing.medium; spacing: Theme.spacing.medium
          ColumnLayout {
            spacing: 2
            RowLayout {
              spacing: Theme.spacing.small
              LogosText { text: modelData.name; font.bold: true; color: Theme.palette.text }
              LogosText {
                text: modelData.ready ? "ready" : "down"
                color: modelData.ready ? Theme.palette.success : Theme.palette.textTertiary
                font.pixelSize: Theme.typography.sizeSmall
              }
            }
            LogosText {
              text: "peers " + modelData.peers + "   rx " + modelData.rx + "   tx " + modelData.tx + "   prio " + modelData.priority
              color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall
            }
          }
          Item { Layout.fillWidth: true }
          LogosButton {
            text: modelData.enabled ? "On" : "Off"
            onClicked: root.setBearer(modelData.name, !modelData.enabled)
          }
        }
      }
    }

    // controls
    LogosText { text: "CONTROLS"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.topMargin: Theme.spacing.small }
    RowLayout {
      spacing: Theme.spacing.small
      LogosButton { text: "Force mesh"; onClicked: root.setForceMesh(true) }
      LogosButton { text: "Mesh auto"; onClicked: root.setForceMesh(false) }
    }
    RowLayout {
      spacing: Theme.spacing.small
      LogosText { text: "Node mode:"; color: Theme.palette.textTertiary; Layout.alignment: Qt.AlignVCenter }
      LogosButton { text: "Core"; onClicked: root.setMode("Core") }
      LogosButton { text: "Edge"; onClicked: root.setMode("Edge") }
      LogosText { text: root.mode + " (applies on restart)"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall; Layout.alignment: Qt.AlignVCenter }
    }

    Item { Layout.fillHeight: true }
    LogosText {
      text: "One shared node per phone. loam_core fans each sealed write to every bearer and dedups by frame id, so a write over Waku and the same over BLE fold to one."
      color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.sizeSmall
      wrapMode: Text.WordWrap; Layout.fillWidth: true
    }
  }
}
