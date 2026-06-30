//
// CurvesEditor.qml — 曲线编辑器控件
//
// Phase 2 实现：RGB 通道独立曲线编辑。
// 类似达芬奇的 Custom Curves 面板。
//

import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    color: "transparent"

    // TODO: Phase 2 — 实现曲线编辑器
    //       - 贝塞尔 / 样条曲线编辑点
    //       - RGB 通道切换
    //       - 直方图背景叠加
    //       - 绑定到 ColorGradeModel 曲线参数

    Label {
        anchors.centerIn: parent
        text: "Curves Editor\n(Phase 2)"
        color: "#555555"
        font.pixelSize: 14
        horizontalAlignment: Text.AlignHCenter
    }
}
