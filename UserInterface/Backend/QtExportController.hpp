#pragma once

#include <Export/ExportEngine.hpp>
#include <QObject>
#include <QPointer>
#include <atomic>
#include <memory>

class QWidget;
class QProgressDialog;
namespace heisenberg::ui {
class EditorSession;
class PlayerController;

class QtExportController final : public QObject {
    Q_OBJECT
public:
    QtExportController(EditorSession* session, PlayerController* player, QObject* parent = nullptr);
    ~QtExportController() override;
    bool start(ExportRequest request, QString* error = nullptr);
    bool running() const {
        return engine_.running();
    }
    void chooseAndExport(QWidget* parent);
    void cancel();
    void shutdown();
signals:
    void progressChanged(double fraction);
    void finished(bool succeeded, const QString& message);
private:
    EditorSession* session_;
    PlayerController* player_;
    ExportEngine engine_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    QPointer<QProgressDialog> progressDialog_;
    bool shutdown_ = false;
};
} // namespace heisenberg::ui
