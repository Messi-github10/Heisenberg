//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QAbstractListModel>
#include <QUrl>
#include <QString>
#include <vector>

namespace heisenberg {
namespace ui {

class MediaPoolModel : public QAbstractListModel {
    Q_OBJECT

public:
    // 模型角色（QML 通过 role 名称访问）
    enum Roles {
        NameRole = Qt::UserRole + 1,
        ThumbnailRole,   // 缩略图路径
        DurationRole,    // 时长（秒）
        ResolutionRole,  // "768x432"
        FilePathRole,
    };

    explicit MediaPoolModel(QObject* parent = nullptr);
    ~MediaPoolModel() override;

    // QAbstractListModel 接口
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 操作接口（URL 由 QML 侧 FileDialog/FolderDialog 提供）
    Q_INVOKABLE void importFile(const QUrl& url);
    Q_INVOKABLE void importFolder(const QUrl& url);
    Q_INVOKABLE void removeItem(int index);
    Q_INVOKABLE void clear();

private:
    struct MediaItem {
        QString name;
        QString thumbnailPath;
        double  duration = 0.0;
        QString resolution;
        QString filePath;
    };

    std::vector<MediaItem> items_;
};

} // namespace ui
} // namespace heisenberg
