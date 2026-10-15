#include "QtExportController.hpp"
#include "EditorSession.hpp"
#include "PlayerController.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QHBoxLayout>
#include <algorithm>

namespace heisenberg::ui {
QtExportController::QtExportController(EditorSession* session, PlayerController* player, QObject* parent)
    : QObject(parent), session_(session), player_(player) {
    // The worker is joined before QObject destruction. Queued functors are bound
    // to this QObject and independently guarded for an explicit early shutdown.
    engine_.onProgress = [this, alive = alive_](const ExportProgress& value) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, alive, value]() {
            if (!alive->load()) return;
            if (progressDialog_) {
                progressDialog_->setValue(static_cast<int>(std::clamp(value.fraction, 0.0, 1.0) * 1000));
                progressDialog_->setLabelText(tr("Exporting frozen timeline: %1 / %2 frames").arg(value.completedFrames).arg(value.totalFrames));
            }
            emit progressChanged(value.fraction);
        }, Qt::QueuedConnection);
    };
    engine_.onFinished = [this, alive = alive_](const ExportResult& result) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, alive, result]() {
            if (!alive->load()) return;
            if (progressDialog_) { progressDialog_->close(); progressDialog_->deleteLater(); progressDialog_ = nullptr; }
            QString message;
            if (result.succeeded()) message = tr("Export complete: %1").arg(QString::fromUtf8(result.outputPath.c_str()));
            else if (result.state == ExportState::Cancelled) message = tr("Export cancelled.");
            else message = tr("Export failed (%1): %2").arg(QString::fromUtf8(result.stage.c_str()), QString::fromUtf8(result.error.c_str()));
            for (const auto& warning : result.warnings) message += "\n" + QString::fromUtf8(warning.c_str());
            emit finished(result.succeeded(), message);
        }, Qt::QueuedConnection);
    };
}
QtExportController::~QtExportController() { shutdown(); }
bool QtExportController::start(ExportRequest request, QString* error) {
    if (shutdown_) { if (error) *error = tr("Export controller is shut down."); return false; }
    std::string reason;
    if (!engine_.start(std::move(request), &reason)) { if (error) *error = QString::fromUtf8(reason.c_str()); return false; }
    return true;
}
void QtExportController::cancel() { engine_.cancel(); }
void QtExportController::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    alive_->store(false);
    engine_.shutdown();
    if (progressDialog_) { progressDialog_->close(); progressDialog_->deleteLater(); progressDialog_ = nullptr; }
}

void QtExportController::chooseAndExport(QWidget* parent) {
    if (running()) { if (progressDialog_) progressDialog_->show(); return; }
    // Capture document and graph before opening the dialog. Source-preview mode
    // and subsequent editor revisions never alter this request.
    ExportRequest request;
    request.snapshot = session_->snapshot();
    if (player_) request.filterGraph = player_->filterGraphSnapshot();
    const auto* timeline = request.snapshot.activeTimeline();
    if (!timeline || timeline->totalFrames() == 0) { QMessageBox::information(parent, tr("Export Timeline"), tr("The timeline is empty.")); return; }
    QDialog dialog(parent);
    dialog.setObjectName("exportTimelineDialog");
    dialog.setWindowTitle(tr("Export Timeline"));
    auto* form = new QFormLayout(&dialog);
    auto* summary = new QLabel(tr("%1 — %2 × %3, %4 fps, %5 frames\n%6 / %7")
        .arg(QString::fromUtf8(timeline->name.c_str())).arg(timeline->width).arg(timeline->height).arg(timeline->fps).arg(timeline->totalFrames())
        .arg(QString::fromUtf8(request.snapshot.project().colorSettings.exportOutput.display.c_str()),
             QString::fromUtf8(request.snapshot.project().colorSettings.exportOutput.view.c_str())), &dialog);
    summary->setWordWrap(true); form->addRow(summary);
    auto* path = new QLineEdit(&dialog); path->setObjectName("exportPath");
    auto* browse = new QPushButton(tr("Browse..."), &dialog);
    auto* pathLayout = new QHBoxLayout; pathLayout->addWidget(path); pathLayout->addWidget(browse); form->addRow(tr("MP4 output"), pathLayout);
    connect(browse, &QPushButton::clicked, &dialog, [&]() {
        const auto file = QFileDialog::getSaveFileName(&dialog, tr("Export Timeline"), path->text().isEmpty() ? "Timeline.mp4" : path->text(), tr("MP4 video (*.mp4)"), nullptr, QFileDialog::DontConfirmOverwrite);
        if (!file.isEmpty()) path->setText(file);
    });
    auto* codec = new QComboBox(&dialog); codec->setObjectName("exportCodec"); codec->addItems({"H.264", "H.265"}); form->addRow(tr("Codec"), codec);
    auto* crf = new QSpinBox(&dialog); crf->setObjectName("exportCrf"); crf->setRange(0, 51); crf->setValue(20); form->addRow(tr("CRF"), crf);
    connect(codec, &QComboBox::currentIndexChanged, &dialog, [crf](int index) {
        // Follow codec defaults until the user has chosen a custom CRF.
        const int previousDefault = index == 0 ? 24 : 20;
        if (crf->value() == previousDefault) crf->setValue(index == 0 ? 20 : 24);
    });
    auto* preset = new QComboBox(&dialog); preset->setObjectName("exportPreset");
    preset->addItems({"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}); preset->setCurrentText("medium"); form->addRow(tr("Preset"), preset);
    auto* hint = new QLabel(tr("Software export uses this captured timeline. Editing can continue while export runs. Unsupported visual features fail with a diagnostic."), &dialog);
    hint->setWordWrap(true); form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); form->addRow(buttons);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(path, &QLineEdit::textChanged, &dialog, [=](const QString& text) { buttons->button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty()); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QString output = path->text().trimmed();
    if (!output.endsWith(".mp4", Qt::CaseInsensitive)) output += ".mp4";
    if (QFileInfo::exists(output)) {
        if (QMessageBox::question(parent, tr("Replace Output"), tr("Replace %1?").arg(output), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        request.overwrite = true;
    }
    request.outputPath = QFileInfo(output).absoluteFilePath().toUtf8().toStdString();
    request.videoCodec = codec->currentIndex() == 0 ? ExportVideoCodec::H264 : ExportVideoCodec::H265;
    request.crf = crf->value(); request.preset = preset->currentText().toStdString();
    QString error;
    if (!start(std::move(request), &error)) { QMessageBox::critical(parent, tr("Cannot Export"), error); return; }
    auto* progress = new QProgressDialog(tr("Preparing frozen timeline..."), tr("Cancel Export"), 0, 1000, parent);
    progress->setObjectName("exportProgress"); progress->setWindowTitle(tr("Export Timeline"));
    progress->setWindowModality(Qt::NonModal); progress->setAutoClose(false); progress->setAutoReset(false);
    progressDialog_ = progress;
    connect(progress, &QProgressDialog::canceled, this, &QtExportController::cancel);
    progress->show();
}
} // namespace heisenberg::ui
