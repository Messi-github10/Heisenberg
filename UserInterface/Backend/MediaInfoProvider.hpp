//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QString>

namespace heisenberg {
namespace ui {

class MediaInfoProvider : public QObject {
    Q_OBJECT

    // 所有属性只读，QML 直接绑定
    Q_PROPERTY(QString format      READ format      NOTIFY infoChanged)
    Q_PROPERTY(QString codec       READ codec       NOTIFY infoChanged)
    Q_PROPERTY(QString resolution  READ resolution  NOTIFY infoChanged)
    Q_PROPERTY(QString frameRate   READ frameRate   NOTIFY infoChanged)
    Q_PROPERTY(QString bitRate     READ bitRate     NOTIFY infoChanged)
    Q_PROPERTY(QString duration    READ duration    NOTIFY infoChanged)
    Q_PROPERTY(QString pixelFormat READ pixelFormat NOTIFY infoChanged)
    Q_PROPERTY(QString colorSpace  READ colorSpace  NOTIFY infoChanged)
    Q_PROPERTY(QString fileSize    READ fileSize    NOTIFY infoChanged)

public:
    explicit MediaInfoProvider(QObject* parent = nullptr);
    ~MediaInfoProvider() override;

    // 属性 getter
    QString format()      const { return format_; }
    QString codec()       const { return codec_; }
    QString resolution()  const { return resolution_; }
    QString frameRate()   const { return frameRate_; }
    QString bitRate()     const { return bitRate_; }
    QString duration()    const { return duration_; }
    QString pixelFormat() const { return pixelFormat_; }
    QString colorSpace()  const { return colorSpace_; }
    QString fileSize()    const { return fileSize_; }

signals:
    // 批量更新时只发一次信号
    void infoChanged();

private:
    QString format_;
    QString codec_;
    QString resolution_;
    QString frameRate_;
    QString bitRate_;
    QString duration_;
    QString pixelFormat_;
    QString colorSpace_;
    QString fileSize_;

    // TODO: Phase 2 — 当选择媒体文件时，从 Demuxer 填充这些字段
};

} // namespace ui
} // namespace heisenberg
