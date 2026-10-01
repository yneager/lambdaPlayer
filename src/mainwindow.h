#pragma once

#include <QByteArray>
#include <QHash>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QKeySequence>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <mpv/client.h>

#include "stremiobackend.h"

#include <optional>

class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QLabel;
class QKeyEvent;
class QPropertyAnimation;
class QPushButton;
class QResizeEvent;
class QSequentialAnimationGroup;
class QSlider;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QWidget;
class HomePage;
class PlayerChrome;
class InterpolationController;
class MpvVideoWidget;
class QShowEvent;
class WindowBridge;
class AddonsBridge;
class UpdateChecker;

namespace frc {
class ScreenInterpolationController;
}

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void openPath(const QString &path);
    // Plays a resolved addon stream through the same libmpv pipeline.
    void openStream(const AddonPlayback &playback);

signals:
    void mpvWakeup();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    void openFile();
    void openFolder();
    void openLicenses();
    void playNextInFolder();
    void toggleMiniPlayer();
    void applyNativeFrame();
    void applyFullscreenFrame();
    void loadSubtitle();
    void togglePause();
    void toggleMute();
    void toggleFullscreen();
    void processMpvEvents();
    void seekReleased();
    void volumeChanged(int value);
    void speedChanged(int index);
    void audioTrackChanged(int index);
    void subtitleTrackChanged(int index);
    void interpolationModeChanged(int index);
    void configureInterpolationShortcut();
    void toggleInterpolation();
    void interpolationDeactivated(const QString &reason);
    void updateInterpolationLabels();
    void toggleSettingsPanel();
    void showHome();
    void resumeFromHome();

private:
    static void wakeup(void *ctx);

    void buildUi();
    void initMpv();
    void handleEvent(mpv_event *event);
    void refreshTracks();
    QString mpvStringProperty(const QByteArray &name) const;
    bool mpvInt64Property(const QByteArray &name, qint64 &value) const;
    bool mpvFlagProperty(const QByteArray &name, bool &value) const;

    void layoutOverlayWidgets();
    void raiseOverlayWidgets();
    void setFullscreenChromeVisible(bool visible);
    void showTrackPicker(QComboBox *combo);
    void updatePlaybackUi();
    void updateCenterState();
    void updateChapterInfo();
    void updateQualityBadge();

    void enterFullscreenControlsMode();
    void leaveFullscreenControlsMode();
    void showFullscreenControls();
    void hideFullscreenControls();
    QRect fullscreenControlsShownRect() const;
    QRect fullscreenControlsHiddenRect() const;

    void showPlayer(bool resumePlayback);
    void transitionTo(QWidget *target);
    bool isPlayerVisible() const;

    void setMpvPropertyFlag(const char *name, bool value);
    void setMpvPropertyDouble(const char *name, double value);
    void setMpvPropertyInt64(const char *name, qint64 value);
    void command(const QStringList &args);
    void updateTimeLabel();
    void syncChromeSettings();
    void showInterpolationError(const QString &message);
    void showToast(const QString &message, bool warning = false);
    void updateSubtitleMargin();
    bool keyEventOwnerIsThisWindow(QObject *watched) const;

    QString dialogStartDirectory() const;
    static QStringList videosInFolder(const QString &directory);
    static QString findNextInFolder(const QString &path);
    void setAlwaysOnTop(bool onTop);

    void loadRecents();

    // Addon playback: request headers and subtitle addons.
    void setMpvStringList(const char *name, const QStringList &values);
    void clearAddonSession();
    void fetchAddonSubtitles();
    void addAddonSubtitles(const QList<stremio::Subtitles> &subtitles, const QString &addonName);
    void selectAddonSubtitle(int index);
    QString currentMediaTitle() const;
    void saveRecents();
    int recentIndex(const QString &path) const;
    void addRecent(const QString &path);
    void removeRecent(const QString &path);
    void rememberCurrentProgress();
    void publishRecents();

    WindowBridge *activeWindowBridge() const;
    void connectWindowBridge(WindowBridge *bridge);
    void updateWindowState();
    bool isWindowMaximized() const;
    void toggleMaximized();
    void maximizeWindow();

    static constexpr QSize kMinimumWindowSize{760, 480};
    static constexpr QSize kMiniMinimumSize{320, 180};
    static constexpr int kResizeBorder = 6;   // logical px
    static constexpr int kMaxRecents = 12;

    static QString formatTime(double seconds);
    static QString formatFps(double fps);

    mpv_handle *mpv_ = nullptr;
    UpdateChecker *updates_ = nullptr;
    InterpolationController *interpolation_ = nullptr;
    frc::ScreenInterpolationController *frc_ = nullptr;
    int frcMenuIndex_ = 7; // selected AMD FRC entry
    int fastQuality_ = 1;
    QKeySequence interpolationShortcut_;
    int lastInterpolationIndex_ = 3;
    QJsonObject fpsDiagnostics_;
    quint64 fpsSwaps_ = 0;
    QElapsedTimer fpsSample_;

    QWidget *appRoot_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    HomePage *homePage_ = nullptr;
    PlayerChrome *playerChrome_ = nullptr;
    QWidget *root_ = nullptr;
    QTimer *transitionTimer_ = nullptr;
    QWidget *transitionTarget_ = nullptr;

    MpvVideoWidget *video_ = nullptr;
    QVBoxLayout *mainLayout_ = nullptr;
    QString pendingPath_;

    QWidget *topBar_ = nullptr;
    QWidget *sideRail_ = nullptr;
    QWidget *qualityBadge_ = nullptr;
    QWidget *centerState_ = nullptr;
    QWidget *controls_ = nullptr;
    QWidget *settingsPanel_ = nullptr;

    QLabel *mediaEyebrow_ = nullptr;
    QLabel *mediaTitle_ = nullptr;
    QLabel *qualityPrimary_ = nullptr;
    QLabel *qualitySecondary_ = nullptr;
    QLabel *centerKicker_ = nullptr;
    QLabel *centerText_ = nullptr;
    QLabel *chapterIndexLabel_ = nullptr;
    QLabel *chapterTitleLabel_ = nullptr;
    QLabel *timelinePositionLabel_ = nullptr;
    QLabel *timelineDurationLabel_ = nullptr;
    QLabel *timeLabel_ = nullptr;

    QPushButton *homeButton_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QPushButton *railPlayerButton_ = nullptr;
    QPushButton *centerPlayButton_ = nullptr;
    QPushButton *muteButton_ = nullptr;
    QPushButton *fullscreenButton_ = nullptr;
    QPushButton *loadSubtitleButton_ = nullptr;
    QPushButton *settingsButton_ = nullptr;

    QSlider *seek_ = nullptr;
    QSlider *volume_ = nullptr;
    QComboBox *speed_ = nullptr;
    QComboBox *audioTrack_ = nullptr;
    QComboBox *subtitleTrack_ = nullptr;
    QComboBox *interpolationMode_ = nullptr;

    QTimer *fullscreenControlsTimer_ = nullptr;
    QPropertyAnimation *controlsSlide_ = nullptr;

    QString currentPath_;
    QString progressKey_;
    double pendingResume_ = 0.0;
    QElapsedTimer progressSaveTimer_;

    struct AddonSubtitle
    {
        stremio::Subtitles subtitle;
        QString addonName;
    };
    StremioBackend *stremio_ = nullptr;
    AddonsBridge *addonsBridge_ = nullptr;
    std::optional<AddonPlayback> addonPlayback_;
    QList<AddonSubtitle> addonSubtitles_;
    QHash<QString, QString> addonSubtitleLabels_; // url -> label once loaded into mpv
    int subtitleGeneration_ = 0;
    QString nextPath_;
    QVariantList recents_;
    QRect miniRestoreGeometry_;
    bool miniMode_ = false;
    bool fullscreenMode_ = false;
    bool fullscreenFill_ = true;
    QElapsedTimer interpolationWarmup_;
    QElapsedTimer interpolationSample_;
    qint64 interpolationLastDrops_ = -1;
    int interpolationSlowSamples_ = 0;
    bool miniRestoreMaximized_ = false;
    bool maximizedBeforeFullscreen_ = false;
    bool eofReached_ = false;
    int subtitleInset_ = 0;
    int lastSubtitleMargin_ = -1;
    int controlsHeight_ = 0;
    double position_ = 0.0;
    double duration_ = 0.0;
    bool paused_ = false;
    bool muted_ = false;
    bool seeking_ = false;
    bool mediaLoaded_ = false;
    bool settingsVisible_ = false;
    bool fullscreenControlsVisible_ = true;
};
