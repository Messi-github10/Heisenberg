//
// ColorWheel.qml — 色轮交互组件
//
// 圆形 HSL 色盘，用户可拖拽选择 Lift/Gamma/Gain 偏移。
// Phase 1: 静态色轮背景 + 指示点。
// Phase 2: MouseArea 拖拽交互。
//

import QtQuick
import QtQuick.Controls

Item {
    id: root

    implicitWidth: 100
    implicitHeight: 100

    property color wheelColor: "#888888"
    property real  hue: 0.5
    property real  saturation: 0.5

    Canvas {
        id: canvas
        anchors.fill: parent

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            let ctx = getContext("2d")
            let w = width
            let h = height
            if (w <= 0 || h <= 0) return

            let cx = w / 2
            let cy = h / 2
            let r = Math.min(cx, cy) - 4
            if (r <= 0) return

            ctx.clearRect(0, 0, w, h)

            // ---- 使用径向渐变绘制 HSL 色盘（性能远优于逐像素填充） ----
            // 从中心到边缘：白色 → 纯色 → 黑色（模拟 sat=0 → sat=max）
            // 360° 锥形渐变通过分段绘制近似
            let segments = 72  // 每 5° 一段
            for (let i = 0; i < segments; i++) {
                let h1 = i / segments
                let h2 = (i + 1) / segments
                let midHue = (h1 + h2) / 2

                let a1 = h1 * 2 * Math.PI - Math.PI / 2
                let a2 = h2 * 2 * Math.PI - Math.PI / 2

                ctx.beginPath()
                ctx.moveTo(cx, cy)
                ctx.arc(cx, cy, r, a1, a2)
                ctx.closePath()

                // 径向渐变：中心灰白 → 边缘纯色 hue
                let grad = ctx.createRadialGradient(cx, cy, 0, cx, cy, r)
                grad.addColorStop(0, "rgba(128,128,128,0.6)")
                grad.addColorStop(0.5, hslToRgb(midHue, 0.5, 0.5))
                grad.addColorStop(1, hslToRgb(midHue, 1.0, 0.4))
                ctx.fillStyle = grad
                ctx.fill()
            }

            // ---- 外圈边框 ----
            ctx.beginPath()
            ctx.arc(cx, cy, r, 0, 2 * Math.PI)
            ctx.strokeStyle = "#555555"
            ctx.lineWidth = 2
            ctx.stroke()

            // ---- 十字参考线 ----
            ctx.save()
            ctx.globalAlpha = 0.3
            ctx.beginPath()
            ctx.moveTo(cx - r * 0.85, cy)
            ctx.lineTo(cx + r * 0.85, cy)
            ctx.moveTo(cx, cy - r * 0.85)
            ctx.lineTo(cx, cy + r * 0.85)
            ctx.strokeStyle = "#ffffff"
            ctx.lineWidth = 0.5
            ctx.stroke()
            ctx.restore()

            // ---- 指示点 ----
            let angle = root.hue * 2 * Math.PI - Math.PI / 2
            let dist  = root.saturation * r * 0.95
            let dx = dist * Math.cos(angle)
            let dy = dist * Math.sin(angle)

            // 指示点外圈（白底黑边）
            ctx.beginPath()
            ctx.arc(cx + dx, cy + dy, 5, 0, 2 * Math.PI)
            ctx.fillStyle = "#ffffff"
            ctx.fill()
            ctx.strokeStyle = "#000000"
            ctx.lineWidth = 1.5
            ctx.stroke()

            // 指示点内圈（选中颜色预览）
            ctx.beginPath()
            ctx.arc(cx + dx, cy + dy, 3, 0, 2 * Math.PI)
            ctx.fillStyle = root.wheelColor
            ctx.fill()
        }

        function hslToRgb(h, s, l) {
            let r, g, b
            if (s === 0) {
                r = g = b = l
            } else {
                let hue2rgb = function(p, q, t) {
                    if (t < 0) t += 1
                    if (t > 1) t -= 1
                    if (t < 1/6) return p + (q - p) * 6 * t
                    if (t < 1/2) return q
                    if (t < 2/3) return p + (q - p) * (2/3 - t) * 6
                    return p
                }
                let q = l < 0.5 ? l * (1 + s) : l + s - l * s
                let p = 2 * l - q
                r = hue2rgb(p, q, h + 1/3)
                g = hue2rgb(p, q, h)
                b = hue2rgb(p, q, h - 1/3)
            }
            let ri = Math.round(r * 255)
            let gi = Math.round(g * 255)
            let bi = Math.round(b * 255)
            return "rgb(" + ri + "," + gi + "," + bi + ")"
        }
    }

    // TODO: Phase 2 — MouseArea 拖拽交互
}
