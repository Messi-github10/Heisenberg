//
// ScopesPane.qml — 右下方示波器面板
//
// 三个示波器垂直排列：
//   1. Waveform    — 波形图（亮度分布）
//   2. Vectorscope — 矢量图（UV 分量）
//   3. Histogram   — 直方图（RGB 通道）
//
// 数据由 C++ ScopeDataProvider 计算，QML 仅做显示。
//

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Heisenberg 1.0

Rectangle {
    id: root
    color: "#222222"
    border.color: "#333333"

    property var provider: null   // ScopeDataProvider

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            text: "Scopes"
            font.bold: true
            font.pixelSize: 13
            color: "#cccccc"
        }

        // ---- Waveform ----
        ScopeView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Waveform"
            image: root.provider ? root.provider.waveform : null
        }

        // ---- Vectorscope ----
        ScopeView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Vectorscope"
            image: root.provider ? root.provider.vectorscope : null
        }

        // ---- Histogram ----
        ScopeView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Histogram"
            image: root.provider ? root.provider.histogram : null
        }
    }

    // ---- 单个示波器视图 ----
    component ScopeView: ColumnLayout {
        property string title: ""
        property var image: null

        spacing: 2
        Label {
            text: title
            color: "#888888"
            font.pixelSize: 10
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0a0a0a"
            border.color: "#333333"

            // 示波器图像占位（Phase 2 使用 Image { source: scopeImage }）
            Label {
                anchors.centerIn: parent
                text: parent.parent ? parent.parent.title : "—"
                color: "#444444"
                font.pixelSize: 14
            }
        }
    }
}
