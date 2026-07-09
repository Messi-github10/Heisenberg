//
// ViewerPane.qml — 上方中间播放器窗口
//
// 视频预览画面 + 播放控制条。
// 画面内容由 C++ RenderEngine 通过 Vulkan 渲染到此区域。
//

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Heisenberg 1.0
import "Controls"

Rectangle {
    id: root
    color: "#111111"
    border.color: "#333333"

    property var controller: null   // PlayerController

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ============================================
        // 视频画面区域（Vulkan 渲染）
        // ============================================
        Item {
            id: videoContainer
            Layout.fillWidth: true
            Layout.fillHeight: true

            VideoOutputItem {
                id: videoOutput
                objectName: "videoOutput"
                anchors.fill: parent
            }

            // 无视频时显示占位文字
            Label {
                anchors.centerIn: parent
                text: root.controller && root.controller.currentFile
                      ? "" : "Video Preview"
                color: "#444444"
                font.pixelSize: 18
            }
        }

        // ============================================
        // 播放控制条
        // ============================================
        PlaybackBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            controller: root.controller
        }
    }
}
