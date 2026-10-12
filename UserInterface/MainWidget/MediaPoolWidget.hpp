#pragma once

#include <QListWidget>
#include <QQueue>
#include <QSet>
#include <QHash>
#include <QPixmap>

namespace heisenberg::ui { class EditorSession; }
class QLineEdit;
class QLabel;
class QProcess;
class QTimer;

class MediaListWidget final : public QListWidget {
    Q_OBJECT
public:
    MediaListWidget(heisenberg::ui::EditorSession* session, QWidget* parent);
signals:
    void filesDropped(const QStringList& files);
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    Qt::DropActions supportedDropActions() const override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    heisenberg::ui::EditorSession* session_;
};

class MediaPoolWidget final : public QWidget {
    Q_OBJECT
public:
    explicit MediaPoolWidget(heisenberg::ui::EditorSession* session, QWidget* parent = nullptr);
    ~MediaPoolWidget() override;
    void importFiles(const QStringList& paths);
    void chooseFiles();
signals:
    void previewRequested(const QString& mediaRef);
    void placeRequested(const QString& mediaRef);
    void importFailed(const QString& message);
private:
    void refreshMedia();
    void filterMedia();
    void startNextThumbnail();
    void finishThumbnail(bool success);
    QString selectedMedia() const;

    heisenberg::ui::EditorSession* session_;
    MediaListWidget* list_;
    QLineEdit* search_;
    QLabel* count_;
    QLabel* empty_;
    QProcess* thumbnailProcess_;
    QTimer* thumbnailTimer_;
    QQueue<QPair<QString, QString>> thumbnailQueue_;
    QSet<QString> attemptedThumbnails_;
    QHash<QString, QPixmap> thumbnails_;
    QString thumbnailId_;
};
