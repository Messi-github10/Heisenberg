//
// MediaPoolPane.qml — 左上方媒体池面板
//
// 显示已导入媒体文件的缩略图列表，
// 提供导入 / 导入文件夹 / 清空操作。
//

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Heisenberg 1.0

Rectangle {
    id: root
    color: "#222222"
    border.color: "#333333"

    // 外部传入的模型和回调
    property var model: null
    property var onFileSelected: function(filePath) {}

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        // ---- 标题栏 ----
        Label {
            text: "Media Pool"
            font.bold: true
            font.pixelSize: 13
            color: "#cccccc"
        }

        // ---- 操作按钮 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Button {
                text: "Import"
                onClicked: fileDialog.open()
            }

            Button {
                text: "Folder"
                onClicked: folderDialog.open()
            }

            Button {
                text: "Clear"
                onClicked: { if (root.model) root.model.clear() }
            }
        }

        // ---- 媒体列表（缩略图 + 文件名 + 时长） ----
        ListView {
            id: mediaList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.model

            delegate: Rectangle {
                width: mediaList.width
                height: 40
                color: index % 2 === 0 ? "#2a2a2a" : "#252525"

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 8

                    // 缩略图占位
                    Rectangle {
                        width: 32; height: 32
                        color: "#3a3a3a"
                        border.color: "#444444"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            text: model.name || "Untitled"
                            color: "#dddddd"
                            font.pixelSize: 11
                        }
                        Label {
                            text: model.duration || ""
                            color: "#888888"
                            font.pixelSize: 10
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.onFileSelected(model.filePath)
                }
            }

            // 空状态提示
            Label {
                anchors.centerIn: parent
                text: "No media imported"
                color: "#555555"
                visible: mediaList.count === 0
            }
        }
    }

    // ---- 文件对话框（纯 QML，不依赖 QWidget） ----
    FileDialog {
        id: fileDialog
        title: "Import Media File(s)"
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            "Video Files (*.mp4 *.mov *.mkv *.avi *.webm *.mxf *.m4v *.mpg *.mpeg *.wmv *.flv *.ts *.mts *.m2ts *.3gp *.ogv *.vob *.asf)",
            "All Files (*.*)"
        ]
        onAccepted: {
            if (!root.model) return
            var files = fileDialog.selectedFiles
            for (var i = 0; i < files.length; i++) {
                root.model.importFile(files[i])
            }
        }
    }

    // ---- 文件夹对话框 ----
    FolderDialog {
        id: folderDialog
        title: "Import Media Folder"
        onAccepted: {
            if (root.model)
                root.model.importFolder(folderDialog.selectedFolder)
        }
    }
}
