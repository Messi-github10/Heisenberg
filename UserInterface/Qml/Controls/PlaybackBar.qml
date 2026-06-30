//
// PlaybackBar.qml — 播放控制条
//
// 包含：播放/暂停 | 逐帧步进 | 时间显示 | 时间轴拖拽
//

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Heisenberg 1.0

Rectangle {
    id: root
    color: "#2a2a2a"
    border.color: "#333333"

    property var controller: null   // PlayerController

    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 6

        // ---- 播放控制按钮 ----
        ControlButton { text: "◀◀"; tooltip: "Go to start"; onClicked: if (root.controller) root.controller.goToStart() }
        ControlButton { text: "◀";  tooltip: "Step back";   onClicked: if (root.controller) root.controller.stepBackward() }
        ControlButton {
            // TODO: Phase 2 — 根据 controller.isPlaying 切换 ▶ / ⏸
            text: root.controller && root.controller.isPlaying ? "⏸" : "▶"
            tooltip: root.controller && root.controller.isPlaying ? "Pause" : "Play"
            onClicked: if (root.controller) root.controller.togglePlayPause()
        }
        ControlButton { text: "▶";  tooltip: "Step forward"; onClicked: if (root.controller) root.controller.stepForward() }
        ControlButton { text: "▶▶"; tooltip: "Go to end";    onClicked: if (root.controller) root.controller.goToEnd() }

        // ---- 时间码显示 ----
        Label {
            text: {
                if (!root.controller) return "00:00:00.000"
                var t = root.controller.currentTime
                var h = Math.floor(t / 3600)
                var m = Math.floor((t % 3600) / 60)
                var s = Math.floor(t % 60)
                var ms = Math.floor((t % 1) * 1000)
                return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}.${ms.toString().padStart(3, '0')}`
            }
            color: "#dddddd"
            font.pixelSize: 12
            font.family: "monospace"
        }

        // ---- 时间轴滑块 ----
        Slider {
            Layout.fillWidth: true
            from: 0
            to: root.controller ? root.controller.duration : 1
            value: root.controller ? root.controller.currentTime : 0
            enabled: root.controller && root.controller.isSeekable

            onValueChanged: {
                if (root.controller && pressed) {
                    root.controller.seek(value)
                }
            }
        }
    }

    // ---- 控制按钮组件 ----
    component ControlButton: Rectangle {
        width: 28; height: 28
        radius: 4
        color: mouseArea.containsMouse ? "#444444" : "transparent"

        property string text: ""
        property string tooltip: ""
        signal clicked()

        Label {
            anchors.centerIn: parent
            text: parent.text
            color: "#cccccc"
            font.pixelSize: 14
        }

        MouseArea {
            id: mouseArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: parent.clicked()
        }

        ToolTip {
            visible: mouseArea.containsMouse
            text: parent.tooltip
            delay: 500
        }
    }
}
