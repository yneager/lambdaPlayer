#pragma once

#include <QJsonArray>
#include <QString>
#include <QWidget>

class QWebEngineView;
class WindowBridge;
class LocalLibrary;

class HomePage final : public QWidget
{
    Q_OBJECT

public:
    explicit HomePage(QObject *addonsBridge, QWidget *parent = nullptr);

    LocalLibrary *library() const { return library_; }
    WindowBridge *windowBridge() const { return windowBridge_; }

    // Recently played files for "Continue watching" (see MainWindow::recentsJson).
    void setRecents(const QJsonArray &recents);
    // Current libmpv session (a file is loaded and can be resumed).
    void setCurrentMedia(const QString &displayName, const QString &path, bool available);

    void openOnline(const QJsonObject &item);
    void toast(const QString &message, bool warning = false);
    void playLeaveAnimation();
    void playEnterAnimation();

signals:
    void openVideoRequested();
    void openFolderRequested();
    void resumeRequested();
    void openPathRequested(const QString &path);
    void openLicensesRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void pushState();
    void runScript(const QString &script);

    LocalLibrary *library_ = nullptr;
    QWebEngineView *view_ = nullptr;
    WindowBridge *windowBridge_ = nullptr;
    QJsonArray recents_;
    QString currentMediaName_;
    QString currentMediaPath_;
    bool currentMediaAvailable_ = false;
    bool ready_ = false;
};
