#pragma once

#include <QList>
#include <QJsonObject>
#include <QObject>
#include <QRectF>
#include <QChronoTimer>
#include <QElapsedTimer>
#include <QVariantList>

// Exposed to each Vui web page through Qt WebChannel as "windowBridge".
//
// The pages draw LAMBDA Player's custom window controls and report which of
// their areas behave like a title bar ("drag regions") and which interactive
// elements sit inside those areas ("holes"). MainWindow uses these rectangles
// to answer Windows' WM_NCHITTEST, so dragging, Aero Snap, double-click to
// maximize and the system menu all stay native.
class WindowBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool maximized READ isMaximized NOTIFY stateChanged)
    Q_PROPERTY(bool fullscreen READ isFullscreen NOTIFY stateChanged)
    Q_PROPERTY(bool mini READ isMini NOTIFY stateChanged)
    Q_PROPERTY(QJsonObject update READ updateState NOTIFY updateChanged)
    Q_PROPERTY(bool uiFramePacing READ uiFramePacing CONSTANT)
    Q_PROPERTY(double uiRefreshRate READ uiRefreshRate NOTIFY stateChanged)
    Q_PROPERTY(QJsonObject preferences READ preferences NOTIFY preferencesChanged)
    Q_PROPERTY(QJsonObject shortcuts READ shortcuts NOTIFY shortcutsChanged)

public:
    explicit WindowBridge(QObject *parent = nullptr);

    bool isMaximized() const { return maximized_; }
    bool isFullscreen() const { return fullscreen_; }
    bool isMini() const { return mini_; }
    QJsonObject updateState() const { return update_; }
    bool uiFramePacing() const;
    double uiRefreshRate() const;
    QJsonObject preferences() const;
    QJsonObject shortcuts() const { return shortcuts_; }
    bool keyboardInputActive() const { return keyboardInputActive_; }
    void setUpdateState(const QJsonObject &state) { update_ = state; emit updateChanged(); }

    void setWindowState(bool maximized, bool fullscreen, bool mini);

    // Point in the page's CSS pixels (== Qt logical pixels of the window).
    bool isDragPoint(const QPointF &point) const;

public slots:
    void minimize() { emit minimizeRequested(); }
    void toggleMaximize() { emit toggleMaximizeRequested(); }
    void close() { emit closeRequested(); }
    void checkUpdates() { emit updateCheckRequested(); }
    void downloadUpdate() { emit updateDownloadRequested(); }
    void cancelUpdate() { emit updateCancelRequested(); }
    void dismissUpdate() { emit updateDismissRequested(); }
    void dismissWhatsNew() { emit whatsNewDismissRequested(); }
    void updateReleaseNotes() { emit updateNotesRequested(); }
    void setTestUpdates(bool enabled) { emit testUpdatesRequested(enabled); }
    void setDragRegions(const QVariantList &drag, const QVariantList &holes);
    // Demand-driven animation clock for the local UI, paced to its monitor.
    void requestUiFrame();
    void setPreference(const QString &key, const QVariant &value);
    QJsonObject setShortcut(const QString &action, const QString &sequence);
    void resetShortcuts();
    void setKeyboardInputActive(bool active) { keyboardInputActive_ = active; }

signals:
    void stateChanged();
    void updateChanged();
    void updateCheckRequested();
    void updateDownloadRequested();
    void updateCancelRequested();
    void updateDismissRequested();
    void whatsNewDismissRequested();
    void updateNotesRequested();
    void testUpdatesRequested(bool enabled);
    void minimizeRequested();
    void toggleMaximizeRequested();
    void closeRequested();
    void uiFrame();
    void preferencesChanged();
    void shortcutsChanged();

private:
    static QList<QRectF> toRects(const QVariantList &list);

    QList<QRectF> drag_;
    QJsonObject update_;
    QJsonObject preferences_;
    QJsonObject shortcuts_;
    bool keyboardInputActive_ = false;
    void broadcastShortcuts();
    QList<QRectF> holes_;
    bool maximized_ = false;
    bool fullscreen_ = false;
    bool mini_ = false;
    QChronoTimer uiFrameTimer_;
    QElapsedTimer uiFrameClock_;
    qint64 uiFrameDeadline_ = 0;
};
