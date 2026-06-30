//
// MediaInfoPane.qml — 右上方媒体信息面板
//
// 显示当前选中媒体的元数据：
//   封装格式 / 编码格式 / 分辨率 / 帧率 / 码率 /
//   时长 / 像素格式 / 色彩空间 / 文件大小
//
// 所有字段通过绑定从 MediaInfoProvider 自动更新。
//

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Heisenberg 1.0

Rectangle {
    id: root
    color: "#222222"
    border.color: "#333333"

    property var provider: null   // MediaInfoProvider

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        Label {
            text: "Media Info"
            font.bold: true
            font.pixelSize: 13
            color: "#cccccc"
        }

        // 分隔线
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#3a3a3a"
        }

        // ---- 信息行列表 ----
        // 格式
        InfoRow { label: "Format";    value: root.provider ? root.provider.format : "-" }
        InfoRow { label: "Codec";     value: root.provider ? root.provider.codec : "-" }
        InfoRow { label: "Resolution";value: root.provider ? root.provider.resolution : "-" }
        InfoRow { label: "Frame Rate"; value: root.provider ? root.provider.frameRate : "-" }
        InfoRow { label: "Bit Rate";  value: root.provider ? root.provider.bitRate : "-" }
        InfoRow { label: "Duration";  value: root.provider ? root.provider.duration : "-" }
        InfoRow { label: "Pixel Fmt"; value: root.provider ? root.provider.pixelFormat : "-" }
        InfoRow { label: "Color Space";value: root.provider ? root.provider.colorSpace : "-" }
        InfoRow { label: "File Size"; value: root.provider ? root.provider.fileSize : "-" }

        Item { Layout.fillHeight: true }   // 弹性占位
    }

    // ---- 单行信息组件 ----
    component InfoRow: RowLayout {
        property string label: ""
        property string value: ""

        spacing: 6

        Label {
            text: label + ":"
            color: "#888888"
            font.pixelSize: 11
            Layout.preferredWidth: 80
        }

        Label {
            text: value
            color: "#dddddd"
            font.pixelSize: 11
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
    }
}
