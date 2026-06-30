import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import Heisenberg 1.0

Window {
    id: root
    title: "Heisenberg — Color Grading"
    visible: true
    width: 1920
    height: 1080
    color: "#1a1a1a"

    // ---- Backend 实例 ----
    MediaPoolModel     { id: mediaPoolModel }
    PlayerController   { id: playerController }
    MediaInfoProvider  { id: mediaInfoProvider }
    ColorGradeModel    { id: colorGradeModel }
    ScopeDataProvider  { id: scopeDataProvider }

    // ================================================================
    // 主布局：使用原生 Column + Row（避免 QML Layouts 递归重排）
    // ================================================================
    Column {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 4

        // ============================================
        // 上方区域（40%）：媒体池 | 播放器 | 媒体信息
        // ============================================
        Row {
            width: parent.width
            height: parent.height * 0.4
            spacing: 4

            // --- 左上：媒体池（15%） ---
            MediaPoolPane {
                width: parent.width * 0.15
                height: parent.height
                model: mediaPoolModel
                onFileSelected: function(filePath) { /* TODO: Phase 2 */ }
            }

            // --- 上方中间：播放器（填充） ---
            ViewerPane {
                width: parent.width * 0.65
                height: parent.height
                controller: playerController
            }

            // --- 右上：媒体信息（20%） ---
            MediaInfoPane {
                width: parent.width * 0.20
                height: parent.height
                provider: mediaInfoProvider
            }
        }

        // ============================================
        // 下方区域（60%）：调色 | 示波器
        // ============================================
        Row {
            width: parent.width
            height: parent.height * 0.6 - 4   // 扣除 spacing
            spacing: 4

            // --- 左下：调色（55%） ---
            ColorGradingPane {
                width: parent.width * 0.55
                height: parent.height
                model: colorGradeModel
            }

            // --- 右下：示波器（填充） ---
            ScopesPane {
                width: parent.width * 0.45
                height: parent.height
                provider: scopeDataProvider
            }
        }
    }
}
