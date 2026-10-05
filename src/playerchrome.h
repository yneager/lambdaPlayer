#pragma once

#include <QList>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QWidget>

class QWebEngineView;
class WindowBridge;

class PlayerChrome final : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerChrome(QWidget *parent = nullptr);

    WindowBridge *windowBridge() const { return windowBridge_; }

    void setLoading(bool loading);
    void setFinished(bool finished);
    void setHasNext(bool hasNext);
    void setFullscreenFill(bool fill);
    void setBuffered(double seconds);
    void toast(const QString &message, bool warning = false);
    void closeSettings();
    void playLeaveAnimation();
    void playEnterAnimation();

    void setMediaLoaded(bool loaded);
    void setMediaTitle(const QString &title, const QString &eyebrow);
    void setPlaybackState(bool paused, bool muted);
    void setTimeline(double position, double duration);
    void setVolume(int volume);
    void setSpeed(double speed);
    void setQuality(const QString &primary, const QString &secondary);
    void setFrcDiagnostics(const QJsonObject &stats);
    void setInterpolationShortcut(const QString &shortcut);
    void setSubtitleAppearance(const QJsonObject &appearance);
    void setPictureSettings(const QJsonObject &picture);
    void openShortcutEditor(const QString &action = {});
    void setChapter(const QString &index, const QString &title);
    void setChromeVisible(bool visible);
    void setSettings(const QStringList &audio, int audioIndex,
                     const QStringList &subtitles, int subtitleIndex,
                     const QStringList &interpolation, const QList<bool> &interpolationEnabled,
                     int interpolationIndex, const QStringList &subtitleGroups = {}, int fastQuality = 1,
                     const QStringList &interpolationReasons = {});
    void setActive(bool active);

signals:
    void ready();
    void openRequested();
    void openPathRequested(const QString &path);
    void homeRequested();
    void togglePauseRequested();
    void nextRequested();
    void muteRequested();
    void fullscreenRequested();
    void fullscreenFillRequested(bool fill);
    void activityRequested();
    void loadSubtitleRequested();
    void miniRequested();
    void configureInterpolationShortcutRequested();
    void seekRequested(double ratio);
    void volumeRequested(int value);
    void speedRequested(double value);
    void audioTrackRequested(int index);
    void subtitleTrackRequested(int index);
    void subtitleAppearanceRequested(const QJsonObject &appearance);
    void pictureRequested(const QJsonObject &picture);
    void interpolationRequested(int index);
    void fastQualityRequested(int quality);
    void videoRectChanged(int x, int y, int width, int height, int radius);
    void subtitleInsetChanged(int pixels);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void pushState();
    void flushState();
    void pushSettings();
    void runScript(const QString &script);

    QWebEngineView *view_ = nullptr;
    WindowBridge *windowBridge_ = nullptr;
    bool ready_ = false;
    bool statePending_ = false;
    QByteArray lastState_;
    bool loaded_ = false;
    bool loading_ = false;
    bool finished_ = false;
    bool hasNext_ = false;
    bool fullscreenFill_ = true;
    double buffered_ = 0.0;
    bool paused_ = false;
    bool muted_ = false;
    bool chromeVisible_ = true;
    QString title_ = "Open or drop a video";
    QString eyebrow_ = "READY";
    double position_ = 0.0;
    double duration_ = 0.0;
    int volume_ = 80;
    double speed_ = 1.0;
    QString qualityPrimary_ = "VIDEO";
    QString qualitySecondary_ = "ORIGINAL";
    QString chapterIndex_ = "--";
    QString chapterTitle_ = "No chapters";
    QString interpolationShortcut_;
    QStringList audio_;
    QStringList subtitles_;
    QStringList subtitleGroups_;
    QStringList interpolation_;
    QList<bool> interpolationEnabled_;
    QStringList interpolationReasons_;
    int audioIndex_ = 0;
    int subtitleIndex_ = 0;
    int interpolationIndex_ = 0;
    int fastQuality_ = 1;
    QJsonObject subtitleAppearance_;
    QJsonObject picture_;
};
