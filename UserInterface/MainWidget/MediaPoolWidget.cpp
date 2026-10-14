#include "MediaPoolWidget.hpp"
#include "Backend/EditorSession.hpp"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kRefRole = Qt::UserRole;
constexpr int kDetailsRole = Qt::UserRole + 1;
constexpr int kOfflineRole = Qt::UserRole + 2;
constexpr int kSupportedRole = Qt::UserRole + 3;
constexpr int kTypeRole = Qt::UserRole + 4;

class MediaTileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {150, 126};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect tile = option.rect.adjusted(4, 4, -4, -4);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool offline = index.data(kOfflineRole).toBool();
        painter->setBrush(QColor(selected ? "#30343c" : "#26272c"));
        painter->setPen(selected ? QColor("#8abbd6") : QColor("#33343a"));
        painter->drawRoundedRect(tile, 5, 5);
        const QRect imageRect(tile.left() + 3, tile.top() + 3, tile.width() - 6, 72);
        painter->fillRect(imageRect, QColor("#111216"));
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) icon.paint(painter, imageRect, Qt::AlignCenter);
        else {
            painter->setPen(QColor(offline ? "#e57a7a" : "#77848e"));
            painter->drawText(imageRect, Qt::AlignCenter,
                              offline ? tr("OFFLINE") : index.data(kTypeRole).toString());
        }
        QFont titleFont = option.font;
        titleFont.setPixelSize(11);
        painter->setFont(titleFont);
        painter->setPen(QColor("#e9eaed"));
        painter->drawText(QRect(tile.left() + 7, tile.top() + 79, tile.width() - 14, 15),
            Qt::AlignVCenter, painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight, tile.width() - 14));
        titleFont.setPixelSize(9);
        painter->setFont(titleFont);
        painter->setPen(QColor(offline ? "#e57a7a" : "#999da8"));
        painter->drawText(QRect(tile.left() + 7, tile.top() + 96, tile.width() - 14, 14),
            Qt::AlignVCenter, painter->fontMetrics().elidedText(
                index.data(kDetailsRole).toString(), Qt::ElideRight, tile.width() - 14));
        painter->restore();
    }
};
}

MediaListWidget::MediaListWidget(heisenberg::ui::EditorSession* session, QWidget* parent)
    : QListWidget(parent), session_(session) {
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setGridSize(QSize(150, 126));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setItemDelegate(new MediaTileDelegate(this));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setObjectName("mediaList");
}

QStringList MediaListWidget::mimeTypes() const { return {heisenberg::ui::kMediaMimeType}; }
Qt::DropActions MediaListWidget::supportedDropActions() const { return Qt::CopyAction; }

QMimeData* MediaListWidget::mimeData(const QList<QListWidgetItem*>& items) const {
    auto* mime = new QMimeData;
    if (items.isEmpty() || !items.front()->data(kSupportedRole).toBool() ||
        items.front()->data(kOfflineRole).toBool()) return mime;
    const QJsonObject payload{{"session", session_->token()},
        {"mediaRef", items.front()->data(kRefRole).toString()}};
    mime->setData(heisenberg::ui::kMediaMimeType, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return mime;
}

void MediaListWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else event->ignore();
}

void MediaListWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else event->ignore();
}

void MediaListWidget::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasUrls()) { event->ignore(); return; }
    QStringList files;
    for (const auto& url : event->mimeData()->urls())
        if (url.isLocalFile()) files << url.toLocalFile();
    if (!files.isEmpty()) { emit filesDropped(files); event->acceptProposedAction(); }
    else event->ignore();
}

MediaPoolWidget::MediaPoolWidget(heisenberg::ui::EditorSession* session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("mediaPool");
    setMinimumWidth(196);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("Media Pool"), this);
    title->setStyleSheet("font-weight: 600;");
    auto* importButton = new QPushButton(tr("Import..."), this);
    importButton->setObjectName("importMedia");
    header->addWidget(title);
    header->addStretch();
    header->addWidget(importButton);
    layout->addLayout(header);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search media"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    list_ = new MediaListWidget(session, this);
    layout->addWidget(list_, 1);
    empty_ = new QLabel(tr("Import video/audio or drop files here.\nDrag media onto its timeline lane to edit."), this);
    empty_->setWordWrap(true);
    empty_->setStyleSheet("color: #999da8; padding: 4px;");
    layout->addWidget(empty_);
    count_ = new QLabel(this);
    count_->setStyleSheet("color: #999da8; font-size: 10px;");
    layout->addWidget(count_);
    auto* addButton = new QPushButton(tr("Add Selected at Playhead"), this);
    addButton->setObjectName("addSelectedMedia");
    addButton->setEnabled(false);
    layout->addWidget(addButton);

    thumbnailProcess_ = new QProcess(this);
    thumbnailTimer_ = new QTimer(this);
    thumbnailTimer_->setSingleShot(true);
    connect(thumbnailProcess_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finishThumbnail(code == 0 && status == QProcess::NormalExit);
    });
    connect(thumbnailProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishThumbnail(false);
    });
    connect(thumbnailTimer_, &QTimer::timeout, this, [this]() { thumbnailProcess_->kill(); });
    connect(importButton, &QPushButton::clicked, this, &MediaPoolWidget::chooseFiles);
    connect(list_, &MediaListWidget::filesDropped, this, &MediaPoolWidget::importFiles);
    connect(search_, &QLineEdit::textChanged, this, &MediaPoolWidget::filterMedia);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item->data(kOfflineRole).toBool()) emit importFailed(tr("This media is offline."));
        else if (!item->data(kSupportedRole).toBool()) emit importFailed(tr("Preview supports video and audio media."));
        else emit previewRequested(item->data(kRefRole).toString());
    });
    connect(addButton, &QPushButton::clicked, this, [this]() {
        if (!selectedMedia().isEmpty()) emit placeRequested(selectedMedia());
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this, addButton]() {
        addButton->setEnabled(!selectedMedia().isEmpty());
    });
    connect(session, &heisenberg::ui::EditorSession::mediaChanged, this, &MediaPoolWidget::refreshMedia);
    connect(session, &heisenberg::ui::EditorSession::projectChanged, this, [this]() {
        thumbnailQueue_.clear();
        thumbnailId_.clear();
        thumbnailTimer_->stop();
        thumbnailProcess_->kill();
        thumbnailProcess_->waitForFinished(1000);
        attemptedThumbnails_.clear();
        thumbnails_.clear();
        search_->clear();
    });
    refreshMedia();
}

MediaPoolWidget::~MediaPoolWidget() {
    thumbnailProcess_->disconnect(this);
    thumbnailProcess_->kill();
    thumbnailProcess_->waitForFinished(1000);
}

QString MediaPoolWidget::selectedMedia() const {
    const auto* item = list_->currentItem();
    if (!item || item->isHidden() || !item->data(kSupportedRole).toBool() || item->data(kOfflineRole).toBool()) return {};
    return item->data(kRefRole).toString();
}

void MediaPoolWidget::chooseFiles() {
    const auto files = QFileDialog::getOpenFileNames(this, tr("Import Media"), QString(),
        tr("Media Files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.mts *.m2ts *.mp3 *.wav *.aac *.m4a *.flac *.ogg);;"
           "Video Files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.mts *.m2ts);;"
           "Audio Files (*.mp3 *.wav *.aac *.m4a *.flac *.ogg);;All Files (*)"));
    if (!files.isEmpty()) importFiles(files);
}

void MediaPoolWidget::importFiles(const QStringList& paths) {
    const auto errors = session_->importFiles(paths);
    if (!errors.isEmpty()) emit importFailed(errors.join('\n'));
}

void MediaPoolWidget::refreshMedia() {
    const QString selected = selectedMedia();
    list_->clear();
    const auto assets = session_->editor().assets();
    for (const auto& asset : assets) {
        const QString id = QString::fromUtf8(asset.id.c_str());
        const QString path = QString::fromUtf8(asset.path.c_str());
        const auto* entry = session_->editor().manifest().find(asset.id);
        auto* item = new QListWidgetItem(QString::fromUtf8(asset.name.c_str()), list_);
        item->setData(kRefRole, id);
        item->setData(kOfflineRole, asset.missing);
        const bool supported = asset.type == heisenberg::ClipType::Video || asset.type == heisenberg::ClipType::Audio;
        item->setData(kSupportedRole, supported);
        item->setData(kTypeRole, QString::fromUtf8(heisenberg::toString(asset.type)).toUpper());
        QString detail = asset.missing ? tr("Offline") : tr("%1 s").arg(asset.duration, 0, 'f', 2);
        if (asset.type == heisenberg::ClipType::Audio) detail = tr("Audio  %1").arg(detail);
        if (entry && entry->sourceWidth && entry->sourceHeight)
            detail += QString("  %1 x %2").arg(*entry->sourceWidth).arg(*entry->sourceHeight);
        if (!supported) detail += tr("  Preview unavailable");
        item->setData(kDetailsRole, detail);
        item->setToolTip(item->text() + '\n' + detail + '\n' + QDir::toNativeSeparators(path));
        if (asset.missing || !supported)
            item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
        if (thumbnails_.contains(id)) item->setIcon(QIcon(thumbnails_.value(id)));
        else if (!asset.missing && asset.type == heisenberg::ClipType::Video && !attemptedThumbnails_.contains(id)) {
            attemptedThumbnails_.insert(id);
            thumbnailQueue_.enqueue({id, path});
        }
        if (id == selected) list_->setCurrentItem(item);
    }
    filterMedia();
    startNextThumbnail();
}

void MediaPoolWidget::filterMedia() {
    int visible = 0;
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        const bool match = item->text().contains(search_->text(), Qt::CaseInsensitive);
        item->setHidden(!match);
        if (match) ++visible;
    }
    empty_->setVisible(list_->count() == 0);
    if (list_->currentItem() && list_->currentItem()->isHidden()) list_->setCurrentRow(-1);
    count_->setText(tr("%1 of %2 assets").arg(visible).arg(list_->count()));
}

void MediaPoolWidget::startNextThumbnail() {
    if (!thumbnailId_.isEmpty() || thumbnailQueue_.isEmpty()) return;
    const auto job = thumbnailQueue_.dequeue();
    thumbnailId_ = job.first;
    thumbnailProcess_->setProgram(QCoreApplication::applicationDirPath() + "/ffmpeg.exe");
    thumbnailProcess_->setArguments({"-hide_banner", "-loglevel", "error", "-i", job.second,
        "-map", "0:v:0", "-frames:v", "1", "-vf",
        "scale=272:144:force_original_aspect_ratio=decrease,pad=272:144:(ow-iw)/2:(oh-ih)/2",
        "-threads", "1", "-f", "image2pipe", "-vcodec", "png", "pipe:1"});
    thumbnailTimer_->start(15000);
    thumbnailProcess_->start();
}

void MediaPoolWidget::finishThumbnail(bool success) {
    thumbnailTimer_->stop();
    QPixmap thumbnail;
    const QByteArray bytes = thumbnailProcess_->readAllStandardOutput();
    thumbnailProcess_->readAllStandardError();
    if (success && !thumbnailId_.isEmpty() && thumbnail.loadFromData(bytes, "PNG")) {
        thumbnails_.insert(thumbnailId_, thumbnail);
        for (int i = 0; i < list_->count(); ++i)
            if (list_->item(i)->data(kRefRole).toString() == thumbnailId_)
                list_->item(i)->setIcon(QIcon(thumbnail));
    }
    thumbnailId_.clear();
    QTimer::singleShot(0, this, &MediaPoolWidget::startNextThumbnail);
}
