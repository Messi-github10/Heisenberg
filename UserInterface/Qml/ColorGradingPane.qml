//
// ColorGradingPane.qml — 左下方调色面板
//
// Tab 切换：色轮 | 曲线 | 限定器（Phase 2 扩展）
// 色轮区：Lift / Gamma / Gain + Offset / Saturation / Contrast / Temp / Tint
//

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Heisenberg 1.0
import "Controls"

Rectangle {
    id: root
    color: "#222222"
    border.color: "#333333"

    property var model: null   // ColorGradeModel

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        // ---- 标题 + Tab 切换 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                text: "Color Grading"
                font.bold: true
                font.pixelSize: 13
                color: "#cccccc"
            }

            Item { Layout.fillWidth: true }

            // Tab 按钮组
            TabButton { text: "Wheels";  checked: true; /* TODO */ }
            TabButton { text: "Curves";  /* TODO: Phase 2 */ }
            TabButton { text: "Qualifier"; /* TODO: Phase 2 */ }

            component TabButton: Button {
                checkable: true
                autoExclusive: true
                flat: true

                contentItem: Label {
                    text: parent.text
                    color: parent.checked ? "#ffffff" : "#666666"
                    font.pixelSize: 11
                    horizontalAlignment: Text.AlignHCenter
                }

                background: Rectangle {
                    color: parent.checked ? "#3a3a3a" : "transparent"
                    radius: 3
                }
            }
        }

        // ---- 色轮区（Lift / Gamma / Gain） ----
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 160
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Label { text: "Lift"; color: "#aaaaaa"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                ColorWheel { Layout.fillWidth: true; Layout.fillHeight: true; wheelColor: root.model ? root.model.liftColor : "#000000" }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Label { text: "Gamma"; color: "#aaaaaa"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                ColorWheel { Layout.fillWidth: true; Layout.fillHeight: true; wheelColor: root.model ? root.model.gammaColor : "#000000" }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Label { text: "Gain"; color: "#aaaaaa"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                ColorWheel { Layout.fillWidth: true; Layout.fillHeight: true; wheelColor: root.model ? root.model.gainColor : "#000000" }
            }
        }

        // ---- 滑块区 ----
        GridLayout {
            Layout.fillWidth: true
            columns: 4
            rowSpacing: 4
            columnSpacing: 12

            // Offset
            Label { text: "Offset"; color: "#888888"; font.pixelSize: 10 }
            Slider { Layout.fillWidth: true; from: -1; to: 1; value: root.model ? root.model.offset : 0; onValueChanged: if (root.model) root.model.offset = value }
            Label { text: root.model ? root.model.offset.toFixed(2) : "0.00"; color: "#666666"; font.pixelSize: 10; Layout.preferredWidth: 36 }

            // Saturation
            Label { text: "Sat"; color: "#888888"; font.pixelSize: 10 }
            Slider { Layout.fillWidth: true; from: 0; to: 2; value: root.model ? root.model.saturation : 1; onValueChanged: if (root.model) root.model.saturation = value }
            Label { text: root.model ? root.model.saturation.toFixed(2) : "1.00"; color: "#666666"; font.pixelSize: 10; Layout.preferredWidth: 36 }

            // Contrast
            Label { text: "Contrast"; color: "#888888"; font.pixelSize: 10 }
            Slider { Layout.fillWidth: true; from: 0; to: 2; value: root.model ? root.model.contrast : 1; onValueChanged: if (root.model) root.model.contrast = value }
            Label { text: root.model ? root.model.contrast.toFixed(2) : "1.00"; color: "#666666"; font.pixelSize: 10; Layout.preferredWidth: 36 }

            // Temperature
            Label { text: "Temp"; color: "#888888"; font.pixelSize: 10 }
            Slider { Layout.fillWidth: true; from: -1; to: 1; value: root.model ? root.model.temperature : 0; onValueChanged: if (root.model) root.model.temperature = value }
            Label { text: root.model ? root.model.temperature.toFixed(2) : "0.00"; color: "#666666"; font.pixelSize: 10; Layout.preferredWidth: 36 }

            // Tint
            Label { text: "Tint"; color: "#888888"; font.pixelSize: 10 }
            Slider { Layout.fillWidth: true; from: -1; to: 1; value: root.model ? root.model.tint : 0; onValueChanged: if (root.model) root.model.tint = value }
            Label { text: root.model ? root.model.tint.toFixed(2) : "0.00"; color: "#666666"; font.pixelSize: 10; Layout.preferredWidth: 36 }
        }

        // ---- 重置按钮 ----
        Button {
            text: "Reset All"
            flat: true
            onClicked: if (root.model) root.model.resetAll()

            contentItem: Label {
                text: parent.text
                color: "#888888"
                font.pixelSize: 11
            }
        }

        Item { Layout.fillHeight: true }   // 弹性占位
    }
}
