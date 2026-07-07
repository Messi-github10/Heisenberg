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
        // 视频画面区域
        // ============================================
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // 通过 FrameImageProvider 显示解码帧
            // frameRevision 变化时 URL 变化，驱动 QML 重新请求帧
            Image {
                id: videoPreview
                anchors.centerIn: parent
                width: parent.width * 0.9
                height: parent.height * 0.9
                fillMode: Image.PreserveAspectFit
                source: "image://frame/preview?" + (root.controller ? root.controller.frameRevision : 0)
                cache: false

                // 无帧时显示占位文字
                Rectangle {
                    anchors.fill: parent
                    color: "#0a0a0a"
                    border.color: "#333333"
                    z: -1
                }

                Label {
                    anchors.centerIn: parent
                    text: videoPreview.sourceSize.width > 0
                          ? "" : "Video Preview"
                    color: "#444444"
                    font.pixelSize: 18
                }
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
