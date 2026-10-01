#include "mainwindow.h"
#include "addonsbridge.h"
#include "stremio/addonurl.h"
#include "stremio/language.h"
#include "homepage.h"
#include "locallibrary.h"
#include "playerchrome.h"
#include "interpolationcontroller.h"
#include "frc/screeninterpolationcontroller.h"
#include "mpvvideowidget.h"
#include "windowbridge.h"
#include "updatechecker.h"

#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainterPath>
#include <QRegion>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QSequentialAnimationGroup>
#include <QSignalBlocker>
#include <QSlider>
#include <QCollator>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QWindow>
#include <QStandardItemModel>
#include <QOpenGLContext>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace { QSettings appSettings(); }

// Interpolation modes: original/RIFE, Generic D3D11 2x/60/120/200, then AMD AMF 2x/60/120/200.
static constexpr int kGenericFrcModeIndex = 3;
static constexpr int kGeneric60FrcModeIndex = 4;
static constexpr int kGeneric120FrcModeIndex = 5;
static constexpr int kGeneric200FrcModeIndex = 6;
static constexpr int kAmfFrcModeIndex = 7;
static constexpr int kFast240FrcModeIndex = 11;
static constexpr int kFrcModeCount = 9;
static constexpr double kFrcTargets[] = {0.0, 60.0, 120.0, 200.0, 0.0, 60.0, 120.0, 200.0, 240.0};
static bool isFrcMode(int index) { return index >= kGenericFrcModeIndex && index < kGenericFrcModeIndex + kFrcModeCount; }
static double frcTargetFor(int index) { return isFrcMode(index) ? kFrcTargets[index - kGenericFrcModeIndex] : 0.0; }
static frc::ScreenInterpolationController::Backend frcBackendFor(int index)
{
    return index < kAmfFrcModeIndex || index == kFast240FrcModeIndex
                                    ? frc::ScreenInterpolationController::Backend::GenericD3D11
                                    : frc::ScreenInterpolationController::Backend::AmdAmf;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("LAMBDA Player");
    setAcceptDrops(true);
    // Custom-drawn title bar. The native frame styles (resize border, Aero
    // Snap, shadow, Windows 11 rounded corners) are restored in
    // applyNativeFrame(); see nativeEvent() for hit-testing.
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);

    // Connect BEFORE initMpv(): mpv_set_wakeup_callback() invokes the callback
    // immediately, and libmpv only calls it again after mpv_wait_event() has
    // drained the queue. A wakeup emitted before this connection existed was
    // lost, so no mpv event (file-loaded, time-pos, ...) was ever processed.
    connect(this, &MainWindow::mpvWakeup,
            this, &MainWindow::processMpvEvents,
            Qt::QueuedConnection);

    buildUi();
    initMpv();
    updates_ = new UpdateChecker(QStringLiteral(LAMBDA_RELEASE_TAG), this);
    connect(updates_, &UpdateChecker::restartRequested, this, [this] { close(); });
    connect(updates_, &UpdateChecker::notify, this, &MainWindow::showToast);
    connect(updates_, &UpdateChecker::changed, this, [this](const QJsonObject &state) {
        homePage_->windowBridge()->setUpdateState(state);
        playerChrome_->windowBridge()->setUpdateState(state);
    });
    homePage_->windowBridge()->setUpdateState(updates_->state());
    playerChrome_->windowBridge()->setUpdateState(updates_->state());
    updates_->start();
    connectWindowBridge(homePage_->windowBridge());
    connectWindowBridge(playerChrome_->windowBridge());
    loadRecents();
    connect(homePage_->library(), &LocalLibrary::playRequested, this, &MainWindow::openPath);
    connect(homePage_->library(), &LocalLibrary::notify, this, &MainWindow::showToast);
    progressSaveTimer_.start();

    applyNativeFrame();
    updateWindowState();

    qApp->installEventFilter(this);

    // Developer aid (inactive unless LAMBDA_DEBUG_GRAB=<dir> is set): writing
    // a file named "<name>.request" into <dir> makes the app save its own
    // composited window (video + web UI) as "<name>.png". Desktop screen
    // capture cannot read OpenGL content on some drivers.
    const QString grabDir = qEnvironmentVariable("LAMBDA_DEBUG_GRAB");
    if (!grabDir.isEmpty()) {
        auto *grabTimer = new QTimer(this);
        grabTimer->setInterval(250);
        connect(grabTimer, &QTimer::timeout, this, [this, grabDir] {
            const QDir dir(grabDir);
            const QStringList requests = dir.entryList({"*.request"}, QDir::Files);
            for (const QString &request : requests) {
                const QString name = QFileInfo(request).completeBaseName();
                QFile::remove(dir.filePath(request));
                // Opt-in runtime diagnostics alongside the composited capture.
                // No URLs, headers or other add-on credentials are recorded.
                const auto rectJson = [](const QRect &r) {
                    return QJsonArray{r.x(), r.y(), r.width(), r.height()};
                };
                QJsonObject state{{"fullscreen", fullscreenMode_},
                                  {"window", rectJson(geometry())},
                                  {"video", rectJson(video_->geometry())},
                                  {"root", rectJson(root_->rect())},
                                  {"videoMasked", !video_->mask().isEmpty()}};
                for (const char *property : {"estimated-vf-fps", "frame-drop-count", "decoder-frame-drop-count",
                                             "hwdec-current", "avsync", "time-pos", "panscan", "video-params/w",
                                             "video-params/h", "video-params/pixelformat", "osd-dimensions",
                                             "audio-delay", "container-fps", "vo-delayed-frame-count"}) {
                    state.insert(QLatin1String(property), mpvStringProperty(property));
                }
#ifdef Q_OS_WIN
                DWORD corners = 0;
                if (SUCCEEDED(::DwmGetWindowAttribute(reinterpret_cast<HWND>(winId()), 33, &corners, sizeof(corners)))) {
                    state.insert("nativeCornerPreference", int(corners));
                }
#endif

                if (frc_) state.insert("frc", frc_->stats());
                if (updates_) {
                    const auto update = updates_->state();
                    state.insert("updateCheck", QJsonObject{{"currentTag", update["currentTag"]},
                        {"status", update["status"]}, {"available", update["available"]}});
                }
                QFile diagnostic(dir.filePath(name + ".json"));
                if (diagnostic.open(QIODevice::WriteOnly)) diagnostic.write(QJsonDocument(state).toJson());
                // Capturing a large GL framebuffer can stall presentation;
                // *-stats.request collects timing without disturbing it.
                if (!name.endsWith("-stats")) grab().save(dir.filePath(name + ".png"));
            }
        });
        grabTimer->start();
    }
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);

    if (mpv_) {
        rememberCurrentProgress();
        // Resume position for "Continue watching" (mpv watch-later).
        if (mediaLoaded_ && !eofReached_) {
            const char *writeArgs[] = {"write-watch-later-config", nullptr};
            mpv_command(mpv_, writeArgs);
        }
        mpv_set_wakeup_callback(mpv_, nullptr, nullptr);
        // Restores audio-delay and stops the FRC worker before mpv goes away.
        if (frc_) {
            frc_->setEnabled(false);
        }
        // The render context must be freed before the mpv core.
        if (video_) {
            video_->shutdown();
        }
        mpv_terminate_destroy(mpv_);
    }
}

void MainWindow::buildUi()
{
    setMinimumSize(kMinimumWindowSize);

    appRoot_ = new QWidget(this);
    appRoot_->setObjectName("appRoot");
    auto *appLayout = new QVBoxLayout(appRoot_);
    appLayout->setContentsMargins(0, 0, 0, 0);
    appLayout->setSpacing(0);

    stack_ = new QStackedWidget(appRoot_);
    stack_->setObjectName("applicationStack");
    appLayout->addWidget(stack_);

    // Stremio addon client: shared by the Home page (catalogs, details,
    // sources) and this window (playback, subtitle addons).
    stremio_ = new StremioBackend(this);
    addonsBridge_ = new AddonsBridge(stremio_, this, this);
    connect(stremio_, &StremioBackend::playRequested, this, &MainWindow::openStream);
    connect(addonsBridge_, &AddonsBridge::cacheClearing, this, [this] {
        // The engine is stopped before its cache is deleted.
        if (!addonPlayback_ || !addonPlayback_->source.viaStreamingServer || !stremio_->streamingServer()->usesBuiltIn()) {
            return;
        }
        rememberCurrentProgress();
        command({"stop"});
        mediaLoaded_ = false;
        clearAddonSession();
        if (playerChrome_) {
            playerChrome_->setLoading(false);
            playerChrome_->setMediaLoaded(false);
            playerChrome_->setMediaTitle("Open or drop a video", "READY");
        }
        if (homePage_) homePage_->setCurrentMedia({}, {}, false);
    });

    homePage_ = new HomePage(addonsBridge_, stack_);
    root_ = new QWidget(stack_);
    root_->setObjectName("playerPage");
    stack_->addWidget(homePage_);
    stack_->addWidget(root_);
    stack_->setCurrentWidget(homePage_);

    connect(homePage_, &HomePage::openVideoRequested, this, &MainWindow::openFile);
    connect(homePage_, &HomePage::openFolderRequested, this, &MainWindow::openFolder);
    connect(homePage_, &HomePage::resumeRequested, this, &MainWindow::resumeFromHome);
    connect(homePage_, &HomePage::openLicensesRequested, this, &MainWindow::openLicenses);
    connect(homePage_, &HomePage::openPathRequested, this, [this](const QString &path) {
        if (path.startsWith("online:")) {
            const int i = recentIndex(path);
            if (i >= 0) homePage_->openOnline(QJsonObject::fromVariantMap(recents_[i].toMap()));
        } else openPath(path);
    });

    mainLayout_ = new QVBoxLayout(root_);
    mainLayout_->setContentsMargins(0, 0, 0, 0);
    mainLayout_->setSpacing(0);

    // libmpv renders into this OpenGL widget through its render API. It is a
    // normal (non-native) widget so Qt can composite the translucent web
    // chrome above it.
    video_ = new MpvVideoWidget(root_);
    connect(video_, &MpvVideoWidget::renderReady, this, [this] {
        // Runs inside initializeGL (context current): probe each GPU backend
        // so unavailable modes are disabled with the driver's reason.
        if (frc_ && interpolationMode_) {
            frc_->probe(QOpenGLContext::currentContext(), nullptr);
            if (auto *model = qobject_cast<QStandardItemModel *>(interpolationMode_->model())) {
                for (int i = kGenericFrcModeIndex; i < kGenericFrcModeIndex + kFrcModeCount; ++i) {
                    if (QStandardItem *item = model->item(i)) {
                        const auto backend = frcBackendFor(i);
                        const bool backendAvailable = frc_->isAvailable(backend);
                        item->setEnabled(backendAvailable);
                        item->setToolTip(backendAvailable
                                             ? QString("%1 on %2").arg(backend == frc::ScreenInterpolationController::Backend::GenericD3D11
                                                                              ? "Vendor-neutral D3D11 compute" : "AMD AMF FRC",
                                                                         frc_->adapterName())
                                             : QString("Not available: %1").arg(frc_->unavailableReason(backend)));
                    }
                }
            }
            updateInterpolationLabels();
        }
        if (!pendingPath_.isEmpty()) {
            const QString path = pendingPath_;
            pendingPath_.clear();
            command({"loadfile", path, "replace"});
        }
    });
    connect(video_, &MpvVideoWidget::renderFailed, this, [this](const QString &reason) {
        showInterpolationError(reason);
    });

    // Top glass bar: Vui's brand/title region plus two global action buttons.
    // The concept's share slot is mapped to LAMBDA Player's existing Open action
    // rather than inventing a new sharing service.
    topBar_ = new QWidget(root_);
    topBar_->setObjectName("topBar");
    auto *topLayout = new QHBoxLayout(topBar_);
    topLayout->setContentsMargins(14, 10, 12, 10);
    topLayout->setSpacing(12);

    auto *brand = new QLabel("◉  LAMBDA", topBar_);
    brand->setObjectName("brandMark");
    brand->setMinimumWidth(92);
    topLayout->addWidget(brand);

    auto *titleStack = new QWidget(topBar_);
    auto *titleLayout = new QVBoxLayout(titleStack);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(1);

    mediaEyebrow_ = new QLabel("READY", titleStack);
    mediaEyebrow_->setObjectName("eyebrow");
    mediaTitle_ = new QLabel("Open or drop a video", titleStack);
    mediaTitle_->setObjectName("mediaTitle");
    mediaTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLayout->addWidget(mediaEyebrow_);
    titleLayout->addWidget(mediaTitle_);
    topLayout->addWidget(titleStack, 1);

    auto makeButton = [](QWidget *parent, const QString &text, const QString &role,
                         const QString &tooltip) {
        auto *button = new QPushButton(text, parent);
        button->setProperty("uiRole", role);
        button->setToolTip(tooltip);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::StrongFocus);
        return button;
    };

    homeButton_ = makeButton(topBar_, "⌂", "icon", "Back to Vui home");
    auto *topOpenButton = makeButton(topBar_, "↥", "icon", "Open video");
    auto *moreButton = makeButton(topBar_, "•••", "icon", "More playback options");
    topLayout->addWidget(homeButton_);
    topLayout->addWidget(topOpenButton);
    topLayout->addWidget(moreButton);

    // Four-button Vui quick-action rail. Every button is retained and mapped
    // onto an existing LAMBDA Player action.
    sideRail_ = new QWidget(root_);
    sideRail_->setObjectName("sideRail");
    auto *railLayout = new QVBoxLayout(sideRail_);
    railLayout->setContentsMargins(8, 8, 8, 8);
    railLayout->setSpacing(7);

    railPlayerButton_ = makeButton(sideRail_, "▶", "rail", "Play / pause");
    railPlayerButton_->setProperty("active", true);
    auto *railAudioButton = makeButton(sideRail_, "♪", "rail", "Audio tracks");
    auto *railCaptionsButton = makeButton(sideRail_, "CC", "rail", "Subtitle tracks");
    auto *railCinemaButton = makeButton(sideRail_, "▣", "rail", "Cinema / fullscreen");

    railLayout->addWidget(railPlayerButton_);
    railLayout->addWidget(railAudioButton);
    railLayout->addWidget(railCaptionsButton);
    railLayout->addWidget(railCinemaButton);

    // Center play state from the Vui concept. It appears before a file is
    // loaded and while playback is paused.
    centerState_ = new QWidget(root_);
    centerState_->setAttribute(Qt::WA_TranslucentBackground);
    auto *centerLayout = new QVBoxLayout(centerState_);
    centerLayout->setContentsMargins(8, 8, 8, 8);
    centerLayout->setAlignment(Qt::AlignCenter);
    centerLayout->setSpacing(8);

    centerPlayButton_ = makeButton(centerState_, "▶", "playCore", "Play / pause");
    centerKicker_ = new QLabel("READY", centerState_);
    centerKicker_->setObjectName("centerKicker");
    centerKicker_->setAlignment(Qt::AlignCenter);
    centerText_ = new QLabel("Open or drop a video", centerState_);
    centerText_->setObjectName("mutedLabel");
    centerText_->setAlignment(Qt::AlignCenter);

    centerLayout->addWidget(centerPlayButton_, 0, Qt::AlignHCenter);
    centerLayout->addWidget(centerKicker_);
    centerLayout->addWidget(centerText_);

    // Truthful quality/status badge: resolution + current interpolation path.
    // We intentionally do not claim HDR unless we actually add verified HDR
    // metadata detection later.
    qualityBadge_ = new QWidget(root_);
    qualityBadge_->setObjectName("qualityBadge");
    auto *qualityLayout = new QHBoxLayout(qualityBadge_);
    qualityLayout->setContentsMargins(10, 6, 10, 6);
    qualityLayout->setSpacing(7);
    auto *statusDot = new QLabel("●", qualityBadge_);
    statusDot->setObjectName("statusDot");
    qualityPrimary_ = new QLabel("VIDEO", qualityBadge_);
    qualityPrimary_->setObjectName("qualityPrimary");
    auto *divider = new QLabel("│", qualityBadge_);
    divider->setObjectName("mutedLabel");
    qualitySecondary_ = new QLabel("ORIGINAL", qualityBadge_);
    qualitySecondary_->setObjectName("qualitySecondary");
    qualityLayout->addWidget(statusDot);
    qualityLayout->addWidget(qualityPrimary_);
    qualityLayout->addWidget(divider);
    qualityLayout->addWidget(qualitySecondary_);

    // Bottom Vui control deck.
    controls_ = new QWidget(root_);
    controls_->setObjectName("controlDeck");
    auto *controlsLayout = new QVBoxLayout(controls_);
    controlsLayout->setContentsMargins(16, 12, 16, 12);
    controlsLayout->setSpacing(8);

    auto *timelineLabels = new QHBoxLayout;
    timelineLabels->setContentsMargins(1, 0, 1, 0);
    timelinePositionLabel_ = new QLabel("00:00", controls_);
    timelinePositionLabel_->setObjectName("timelineLabel");
    timelineDurationLabel_ = new QLabel("00:00", controls_);
    timelineDurationLabel_->setObjectName("timelineLabel");
    timelineLabels->addWidget(timelinePositionLabel_);
    timelineLabels->addStretch();
    timelineLabels->addWidget(timelineDurationLabel_);
    controlsLayout->addLayout(timelineLabels);

    seek_ = new QSlider(Qt::Horizontal, controls_);
    seek_->setObjectName("timelineSlider");
    seek_->setRange(0, 1000);
    seek_->setCursor(Qt::PointingHandCursor);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, &MainWindow::seekReleased);
    controlsLayout->addWidget(seek_);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(7);

    playButton_ = makeButton(controls_, "▶", "strong", "Play / pause");
    auto *nextButton = makeButton(controls_, "⏭", "icon", "Next playlist item");
    muteButton_ = makeButton(controls_, "VOL", "text", "Mute / unmute");

    volume_ = new QSlider(Qt::Horizontal, controls_);
    volume_->setObjectName("volumeSlider");
    volume_->setRange(0, 100);
    volume_->setValue(80);
    volume_->setFixedWidth(82);

    timeLabel_ = new QLabel("00:00  /  00:00", controls_);
    timeLabel_->setObjectName("timecode");

    buttonRow->addWidget(playButton_);
    buttonRow->addWidget(nextButton);
    buttonRow->addWidget(muteButton_);
    buttonRow->addWidget(volume_);
    buttonRow->addWidget(timeLabel_);
    buttonRow->addStretch(1);

    auto *chapterPill = new QWidget(controls_);
    chapterPill->setObjectName("chapterPill");
    auto *chapterLayout = new QHBoxLayout(chapterPill);
    chapterLayout->setContentsMargins(8, 5, 9, 5);
    chapterLayout->setSpacing(8);

    chapterIndexLabel_ = new QLabel("--", chapterPill);
    chapterIndexLabel_->setObjectName("chapterIndex");

    auto *chapterCopy = new QWidget(chapterPill);
    auto *chapterCopyLayout = new QVBoxLayout(chapterCopy);
    chapterCopyLayout->setContentsMargins(0, 0, 0, 0);
    chapterCopyLayout->setSpacing(0);
    auto *chapterKicker = new QLabel("CHAPTER", chapterCopy);
    chapterKicker->setObjectName("chapterKicker");
    chapterTitleLabel_ = new QLabel("No chapters", chapterCopy);
    chapterTitleLabel_->setObjectName("chapterTitle");
    chapterCopyLayout->addWidget(chapterKicker);
    chapterCopyLayout->addWidget(chapterTitleLabel_);

    auto *wave = new QLabel("▂▅▇▃▆", chapterPill);
    wave->setObjectName("wave");

    chapterLayout->addWidget(chapterIndexLabel_);
    chapterLayout->addWidget(chapterCopy);
    chapterLayout->addWidget(wave);
    buttonRow->addWidget(chapterPill);
    buttonRow->addStretch(1);

    speed_ = new QComboBox(controls_);
    speed_->setFixedWidth(72);
    speed_->addItems({"0.50×", "0.75×", "1.00×", "1.25×", "1.50×", "2.00×"});
    speed_->setCurrentIndex(2);
    speed_->setToolTip("Playback speed");

    auto *captionsButton = makeButton(controls_, "CC", "icon", "Subtitle tracks");
    settingsButton_ = makeButton(controls_, "⚙", "icon", "Settings");
    auto *pipButton = makeButton(controls_, "▣", "icon", "Picture in picture");
    pipButton->setEnabled(false);
    pipButton->setToolTip("Picture in picture is not implemented in v0.2.1");
    fullscreenButton_ = makeButton(controls_, "⛶", "strong", "Fullscreen");

    buttonRow->addWidget(speed_);
    buttonRow->addWidget(captionsButton);
    buttonRow->addWidget(settingsButton_);
    buttonRow->addWidget(pipButton);
    buttonRow->addWidget(fullscreenButton_);
    controlsLayout->addLayout(buttonRow);

    // LAMBDA Player-specific controls live in a compact expandable settings row
    // instead of being lost when adopting Vui's cleaner deck.
    settingsPanel_ = new QWidget(controls_);
    settingsPanel_->setObjectName("settingsPanel");
    auto *settingsLayout = new QHBoxLayout(settingsPanel_);
    settingsLayout->setContentsMargins(9, 7, 9, 7);
    settingsLayout->setSpacing(7);

    auto *openButton = makeButton(settingsPanel_, "Open File", "text", "Open video");
    audioTrack_ = new QComboBox(settingsPanel_);
    audioTrack_->setMinimumWidth(170);
    audioTrack_->addItem("No audio tracks");
    audioTrack_->setEnabled(false);

    subtitleTrack_ = new QComboBox(settingsPanel_);
    subtitleTrack_->setMinimumWidth(170);
    subtitleTrack_->addItem("Subtitles Off", QVariant::fromValue<qlonglong>(-1));

    loadSubtitleButton_ = makeButton(settingsPanel_, "Load Subtitle", "text", "Load external subtitle");

    interpolationMode_ = new QComboBox(settingsPanel_);
    interpolationMode_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    interpolationMode_->addItem("Original");
    interpolationMode_->addItem("Double frame rate (RIFE)");
    interpolationMode_->addItem("60 fps (RIFE)");
    interpolationMode_->addItem("Double frame rate (Universal D3D11)");
    interpolationMode_->addItem("60 fps (Universal D3D11)");
    interpolationMode_->addItem("120 fps (Universal D3D11)");
    interpolationMode_->addItem("200 fps (Universal D3D11)");
    interpolationMode_->addItem("Double frame rate (AMD FRC)");
    interpolationMode_->addItem("60 fps (AMD FRC)");
    interpolationMode_->addItem("120 fps (AMD FRC)");
    interpolationMode_->addItem("200 fps (AMD FRC)");
    interpolationMode_->addItem("240 fps (Fast RIFE, experimental)");
    interpolationMode_->setCurrentIndex(0);
    interpolationMode_->setToolTip("Frame interpolation (RIFE v4.6 via VapourSynth, Vulkan GPU)");

    auto *audioLabel = new QLabel("AUDIO", settingsPanel_);
    audioLabel->setObjectName("mutedLabel");
    auto *subLabel = new QLabel("SUBTITLES", settingsPanel_);
    subLabel->setObjectName("mutedLabel");
    auto *rifeLabel = new QLabel("INTERPOLATION", settingsPanel_);
    rifeLabel->setObjectName("mutedLabel");

    settingsLayout->addWidget(openButton);
    settingsLayout->addSpacing(4);
    settingsLayout->addWidget(audioLabel);
    settingsLayout->addWidget(audioTrack_);
    settingsLayout->addWidget(subLabel);
    settingsLayout->addWidget(subtitleTrack_);
    settingsLayout->addWidget(loadSubtitleButton_);
    settingsLayout->addWidget(rifeLabel);
    settingsLayout->addWidget(interpolationMode_);
    settingsLayout->addStretch();

    settingsPanel_->setVisible(false);
    controlsLayout->addWidget(settingsPanel_);

    // The real Vui player is rendered by Chromium as CSS/HTML chrome, composited
    // by Qt above the OpenGL video widget. The native widgets above are only
    // kept as hidden state holders for tracks/speed/interpolation.
    for (QWidget *legacyOverlay : {topBar_, sideRail_, qualityBadge_, centerState_, controls_}) {
        legacyOverlay->hide();
    }

    playerChrome_ = new PlayerChrome(root_);
    playerChrome_->setGeometry(root_->rect());
    playerChrome_->show();
    playerChrome_->raise();

    connect(playerChrome_, &PlayerChrome::openRequested, this, &MainWindow::openFile);
    connect(playerChrome_, &PlayerChrome::openPathRequested, this, &MainWindow::openPath);
    connect(playerChrome_, &PlayerChrome::homeRequested, this, &MainWindow::showHome);
    connect(playerChrome_, &PlayerChrome::togglePauseRequested, this, &MainWindow::togglePause);
    connect(playerChrome_, &PlayerChrome::nextRequested, this, &MainWindow::playNextInFolder);
    connect(playerChrome_, &PlayerChrome::miniRequested, this, &MainWindow::toggleMiniPlayer);
    connect(playerChrome_, &PlayerChrome::muteRequested, this, &MainWindow::toggleMute);
    connect(playerChrome_, &PlayerChrome::fullscreenRequested, this, &MainWindow::toggleFullscreen);
    connect(playerChrome_, &PlayerChrome::fullscreenFillRequested, this, [this](bool fill) {
        fullscreenFill_ = fill;
        command({"set", "panscan", fullscreenMode_ && fill ? "1" : "0"});
        playerChrome_->setFullscreenFill(fill);
    });
    connect(playerChrome_, &PlayerChrome::activityRequested, this, [this] {
        if (fullscreenMode_) showFullscreenControls();
    });
    connect(playerChrome_, &PlayerChrome::loadSubtitleRequested, this, &MainWindow::loadSubtitle);
    connect(playerChrome_, &PlayerChrome::seekRequested, this, [this](double ratio) {
        seek_->setValue(qBound(0, qRound(ratio * 1000.0), 1000));
        seeking_ = true;
        seekReleased();
    });
    connect(playerChrome_, &PlayerChrome::volumeRequested, this, [this](int value) {
        volume_->setValue(qBound(0, value, 100));
    });
    connect(playerChrome_, &PlayerChrome::speedRequested, this, [this](double value) {
        static const double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
        int best = 2;
        double delta = 100.0;
        for (int i = 0; i < 6; ++i) {
            const double d = std::abs(speeds[i] - value);
            if (d < delta) {
                delta = d;
                best = i;
            }
        }
        if (speed_->currentIndex() == best) {
            setMpvPropertyDouble("speed", speeds[best]);
            updateInterpolationLabels();
            playerChrome_->setSpeed(speeds[best]);
        } else {
            speed_->setCurrentIndex(best);
        }
    });
    connect(playerChrome_, &PlayerChrome::audioTrackRequested, this, [this](int index) {
        if (index >= 0 && index < audioTrack_->count()) audioTrack_->setCurrentIndex(index);
    });
    connect(playerChrome_, &PlayerChrome::subtitleTrackRequested, this, [this](int index) {
        if (index >= 0 && index < subtitleTrack_->count()) subtitleTrack_->setCurrentIndex(index);
    });
    connect(playerChrome_, &PlayerChrome::interpolationRequested, this, [this](int index) {
        if (index >= 0 && index < interpolationMode_->count()) interpolationMode_->setCurrentIndex(index);
    });
    interpolationShortcut_ = QKeySequence::fromString(appSettings().value("player/interpolationShortcut").toString(), QKeySequence::PortableText);
    lastInterpolationIndex_ = appSettings().value("player/lastInterpolationIndex", 3).toInt();
    playerChrome_->setInterpolationShortcut(interpolationShortcut_.toString(QKeySequence::NativeText));
    connect(playerChrome_, &PlayerChrome::configureInterpolationShortcutRequested, this, &MainWindow::configureInterpolationShortcut);
    connect(playerChrome_, &PlayerChrome::fastQualityRequested, this, [this](int quality) {
        fastQuality_ = qBound(0, quality, 2);
        QSettings(QSettings::IniFormat, QSettings::UserScope, "LAMBDA", "LAMBDA Player")
            .setValue("frc/fastQuality", fastQuality_);
        if (frc_) frc_->setFastQuality(fastQuality_);
        syncChromeSettings();
    });
    connect(playerChrome_, &PlayerChrome::videoRectChanged, this,
            [this](int x, int y, int width, int height, int radius) {
        if (!video_ || width <= 0 || height <= 0) return;
        // Browser geometry can arrive late during fullscreen transitions.
        if (fullscreenMode_) {
            video_->clearMask();
            video_->setGeometry(root_->rect());
            return;
        }
        video_->setGeometry(x, y, width, height);
        if (radius > 0) {
            QPainterPath clip;
            clip.addRoundedRect(QRectF(video_->rect()), radius, radius);
            video_->setMask(QRegion(clip.toFillPolygon().toPolygon()));
        } else {
            video_->clearMask();
        }
        playerChrome_->raise();
        updateSubtitleMargin();
    });
    connect(playerChrome_, &PlayerChrome::subtitleInsetChanged, this, [this](int pixels) {
        subtitleInset_ = pixels;
        updateSubtitleMargin();
    });
    connect(playerChrome_, &PlayerChrome::ready, this, [this] {
        playerChrome_->setHasNext(!nextPath_.isEmpty());
        updatePlaybackUi();
        updateTimeLabel();
        updateCenterState();
        updateChapterInfo();
        updateQualityBadge();
        syncChromeSettings();
        layoutOverlayWidgets();
    });

    setCentralWidget(appRoot_);

    connect(homeButton_, &QPushButton::clicked, this, &MainWindow::showHome);
    connect(topOpenButton, &QPushButton::clicked, this, &MainWindow::openFile);
    connect(moreButton, &QPushButton::clicked, this, &MainWindow::toggleSettingsPanel);
    connect(openButton, &QPushButton::clicked, this, &MainWindow::openFile);
    connect(loadSubtitleButton_, &QPushButton::clicked, this, &MainWindow::loadSubtitle);

    connect(playButton_, &QPushButton::clicked, this, &MainWindow::togglePause);
    connect(railPlayerButton_, &QPushButton::clicked, this, &MainWindow::togglePause);
    connect(centerPlayButton_, &QPushButton::clicked, this, &MainWindow::togglePause);
    connect(nextButton, &QPushButton::clicked, this, &MainWindow::playNextInFolder);

    connect(railAudioButton, &QPushButton::clicked, this, [this] {
        showTrackPicker(audioTrack_);
    });
    connect(railCaptionsButton, &QPushButton::clicked, this, [this] {
        showTrackPicker(subtitleTrack_);
    });
    connect(captionsButton, &QPushButton::clicked, this, [this] {
        showTrackPicker(subtitleTrack_);
    });
    connect(railCinemaButton, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);

    connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::toggleSettingsPanel);
    connect(muteButton_, &QPushButton::clicked, this, &MainWindow::toggleMute);
    connect(fullscreenButton_, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);
    connect(volume_, &QSlider::valueChanged, this, &MainWindow::volumeChanged);
    connect(speed_, &QComboBox::currentIndexChanged, this, &MainWindow::speedChanged);
    connect(audioTrack_, &QComboBox::currentIndexChanged, this, &MainWindow::audioTrackChanged);
    connect(subtitleTrack_, &QComboBox::currentIndexChanged, this, &MainWindow::subtitleTrackChanged);
    connect(interpolationMode_, &QComboBox::currentIndexChanged, this, &MainWindow::interpolationModeChanged);

    fullscreenControlsTimer_ = new QTimer(this);
    fullscreenControlsTimer_->setSingleShot(true);
    fullscreenControlsTimer_->setInterval(2200);
    connect(fullscreenControlsTimer_, &QTimer::timeout,
            this, &MainWindow::hideFullscreenControls);

    controlsSlide_ = new QPropertyAnimation(controls_, "geometry", this);
    controlsSlide_->setDuration(500);
    controlsSlide_->setEasingCurve(QEasingCurve::OutCubic);
    connect(controlsSlide_, &QPropertyAnimation::finished, this, [this] {
        if (fullscreenMode_ && !fullscreenControlsVisible_) {
            controls_->hide();
            setCursor(Qt::BlankCursor);
        }
    });

    root_->setMouseTracking(true);
    for (QWidget *widget : root_->findChildren<QWidget *>()) {
        widget->setMouseTracking(true);
    }

    updatePlaybackUi();
    updateCenterState();
    updateQualityBadge();
    layoutOverlayWidgets();
}

void MainWindow::initMpv()
{
    // Before mpv_create(): mpv snapshots the environment on first use.
    InterpolationController::configureProcessEnvironment();

    mpv_ = mpv_create();
    if (!mpv_) {
        throw std::runtime_error("Could not create libmpv context.");
    }

    // Render through libmpv's render API into MpvVideoWidget (no native wid).
    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");
    // Qt paints on the GUI thread. Let mpv schedule frames at their target
    // time instead of sleeping inside paintGL (see render.h).
    mpv_set_option_string(mpv_, "video-timing-offset", "0");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "osc", "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "sub-visibility", "yes");
    mpv_set_option_string(mpv_, "embeddedfonts", "yes");
    mpv_set_option_string(mpv_, "demuxer-mkv-subtitle-preroll", "yes");
    // Letterbox colour matches the Vui player background.
    mpv_set_option_string(mpv_, "background-color", "#000000");

    // "Continue watching": mpv's own watch-later files store the resume
    // position. Only the position and track choices are restored - never the
    // video filter chain (the optional @novarife RIFE filter stays Off until
    // the user enables it).
    const QString profileDir = qEnvironmentVariable("LAMBDA_DATA_DIR");
    const QString watchLaterDir = QDir(profileDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : profileDir)
                                      .filePath("watch_later");
    QDir().mkpath(watchLaterDir);
    mpv_set_option_string(mpv_, "watch-later-dir", QDir::toNativeSeparators(watchLaterDir).toUtf8().constData());
    mpv_set_option_string(mpv_, "watch-later-options", "start,aid,sid");
    mpv_set_option_string(mpv_, "resume-playback", "yes");

    // Developer aid: LAMBDA_MPV_LOG=<file> writes mpv's own log.
    const QByteArray mpvLog = qgetenv("LAMBDA_MPV_LOG");
    if (!mpvLog.isEmpty()) {
        mpv_set_option_string(mpv_, "log-file", mpvLog.constData());
    }

    // Developer aid: LAMBDA_MPV_OPTIONS="name=value;name=value" overrides mpv
    // options for local experiments (applied last, before initialisation).
    for (const QString &pair : qEnvironmentVariable("LAMBDA_MPV_OPTIONS").split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const qsizetype eq = pair.indexOf(QLatin1Char('='));
        if (eq > 0) mpv_set_option_string(mpv_, pair.left(eq).toUtf8().constData(), pair.mid(eq + 1).toUtf8().constData());
    }

    if (mpv_initialize(mpv_) < 0) {
        throw std::runtime_error("Could not initialize libmpv.");
    }
    video_->setMpv(mpv_);

    mpv_observe_property(mpv_, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 4, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 5, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, 6, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 7, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 8, "video-params/h", MPV_FORMAT_INT64);

    // Error-level log messages are needed to notice when mpv disables the
    // RIFE VapourSynth filter after a script/runtime failure.
    mpv_request_log_messages(mpv_, "error");

    interpolation_ = new InterpolationController(mpv_, this);

    // Post-render AMD FRC (native resolution, no VapourSynth).
    frc_ = new frc::ScreenInterpolationController(mpv_, this);
    fastQuality_ = qBound(0, QSettings(QSettings::IniFormat, QSettings::UserScope, "LAMBDA", "LAMBDA Player")
                               .value("frc/fastQuality", 1).toInt(), 2);
    frc_->setFastQuality(fastQuality_);
    video_->setFrameInterpolation(frc_);
    connect(frc_, &frc::ScreenInterpolationController::diagnosticsUpdated,
            this, [this](const QJsonObject &stats) { fpsDiagnostics_ = stats; });
    fpsSample_.start();
    connect(video_, &QOpenGLWidget::frameSwapped, this, [this] { ++fpsSwaps_; });
    auto *fpsTimer = new QTimer(this);
    fpsTimer->setInterval(1000);
    connect(fpsTimer, &QTimer::timeout, this, [this] {
        const double seconds = qMax(0.001, fpsSample_.restart() / 1000.0);
        QJsonObject stats = frc_->isEnabled() ? fpsDiagnostics_ : QJsonObject{};
        stats.insert("enabled", frc_->isEnabled());
        if (frc_->isEnabled()) stats.insert("outputRate", frc_->outputRate());
        stats.insert("paused", paused_ || !mediaLoaded_ || eofReached_);
        stats.insert("presentationFps", (paused_ || !mediaLoaded_ || eofReached_) ? 0.0 : fpsSwaps_ / seconds);
        double sourceFps = 0, filterFps = 0;
        mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE, &sourceFps);
        mpv_get_property(mpv_, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &filterFps);
        stats.insert("sourceFps", sourceFps);
        stats.insert("filterFps", filterFps > 0 ? filterFps : sourceFps);
        fpsSwaps_ = 0;
        playerChrome_->setFrcDiagnostics(stats);
    });
    fpsTimer->start();
    connect(frc_, &frc::ScreenInterpolationController::failed, this, [this](const QString &reason) {
        const QSignalBlocker blocker(interpolationMode_);
        interpolationMode_->setCurrentIndex(0);
        updateQualityBadge();
        syncChromeSettings();
        showInterpolationError(reason + ". Playing at the original frame rate.");
    });
    connect(interpolation_, &InterpolationController::deactivated,
            this, &MainWindow::interpolationDeactivated);

    // Keep native detail when inference cannot keep up. Detect sustained
    // presentation loss / A/V drift, not a video's nominal filter FPS.
    auto *healthTimer = new QTimer(this);
    healthTimer->setInterval(1000);
    connect(healthTimer, &QTimer::timeout, this, [this] {
        auto reset = [this] {
            interpolationWarmup_.restart();
            interpolationSample_.restart();
            interpolationLastDrops_ = -1;
            interpolationSlowSamples_ = 0;
        };
        if (!mediaLoaded_ || paused_ || eofReached_ || !isPlayerVisible()
            || interpolation_->mode() == InterpolationController::Mode::Off) {
            reset();
            return;
        }
        int seeking = 0, buffering = 0;
        mpv_get_property(mpv_, "seeking", MPV_FORMAT_FLAG, &seeking);
        mpv_get_property(mpv_, "paused-for-cache", MPV_FORMAT_FLAG, &buffering);
        if (seeking || buffering) { reset(); return; }
        qint64 drops = 0;
        if (!mpvInt64Property("frame-drop-count", drops)) return;
        if (!interpolationWarmup_.isValid() || interpolationWarmup_.elapsed() < 3000
            || interpolationLastDrops_ < 0 || drops < interpolationLastDrops_) {
            if (!interpolationWarmup_.isValid()) interpolationWarmup_.start();
            interpolationLastDrops_ = drops;
            interpolationSample_.restart();
            return;
        }
        double fps = 0, speed = 1, avsync = 0;
        mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE, &fps);
        mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
        mpv_get_property(mpv_, "avsync", MPV_FORMAT_DOUBLE, &avsync);
        const double target = (interpolation_->mode() == InterpolationController::Mode::Rife60 ? 60 : fps * 2) * speed;
        const double seconds = qMax(0.001, interpolationSample_.restart() / 1000.0);
        const double lostPerSecond = (drops - interpolationLastDrops_) / seconds;
        interpolationLastDrops_ = drops;
        const bool overloaded = lostPerSecond > qMax(8.0, target * 0.25) || std::abs(avsync) > 0.5;
        interpolationSlowSamples_ = overloaded ? interpolationSlowSamples_ + 1 : 0;
        if (interpolationSlowSamples_ >= 3) {
            interpolationMode_->setCurrentIndex(0);
            showToast("Native-resolution interpolation cannot keep up on this video. "
                      "Original frame rate restored to keep playback smooth; resolution is unchanged.", true);
            reset();
        }
    });
    healthTimer->start();

    mpv_set_wakeup_callback(mpv_, &MainWindow::wakeup, this);

    // The Chromium player chrome is a sibling above the native libmpv HWND.
    // Do not recreate or replace the mpv render target.
    if (playerChrome_) {
        playerChrome_->raise();
    }

    setMpvPropertyDouble("volume", 80);
    layoutOverlayWidgets();
    raiseOverlayWidgets();
}

void MainWindow::wakeup(void *ctx)
{
    emit static_cast<MainWindow *>(ctx)->mpvWakeup();
}

void MainWindow::processMpvEvents()
{
    if (!mpv_) {
        return;
    }

    for (;;) {
        auto *event = mpv_wait_event(mpv_, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            break;
        }
        handleEvent(event);
    }
}

void MainWindow::handleEvent(mpv_event *event)
{
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        auto *property = static_cast<mpv_event_property *>(event->data);
        if (!property || !property->data) {
            return;
        }

        const QString name = QString::fromUtf8(property->name);

        if (name == "time-pos" && property->format == MPV_FORMAT_DOUBLE) {
            position_ = *static_cast<double *>(property->data);
            if (!seeking_ && duration_ > 0) {
                seek_->setValue(int(position_ / duration_ * 1000));
            }
            updateTimeLabel();
            if (mediaLoaded_ && progressSaveTimer_.elapsed() >= 5000) {
                rememberCurrentProgress();
                progressSaveTimer_.restart();
            }
        } else if (name == "duration" && property->format == MPV_FORMAT_DOUBLE) {
            duration_ = *static_cast<double *>(property->data);
            updateTimeLabel();
        } else if (name == "pause" && property->format == MPV_FORMAT_FLAG) {
            paused_ = *static_cast<int *>(property->data) != 0;
            if (frc_) frc_->setPaused(paused_);
            updatePlaybackUi();
            updateCenterState();
            if (paused_) {
                rememberCurrentProgress();
            }
        } else if (name == "mute" && property->format == MPV_FORMAT_FLAG) {
            muted_ = *static_cast<int *>(property->data) != 0;
            updatePlaybackUi();
        } else if (name == "chapter" && property->format == MPV_FORMAT_INT64) {
            updateChapterInfo();
        } else if (name == "eof-reached" && property->format == MPV_FORMAT_FLAG) {
            const bool eof = *static_cast<int *>(property->data) != 0;
            if (eof != eofReached_) {
                eofReached_ = eof;
                if (eof && mediaLoaded_) {
                    // Finished: start from the beginning next time.
                    command({"delete-watch-later-config"});
                    rememberCurrentProgress();
                }
                if (playerChrome_) playerChrome_->setFinished(eof && mediaLoaded_);
            }
        } else if (name == "demuxer-cache-time" && property->format == MPV_FORMAT_DOUBLE) {
            if (playerChrome_) playerChrome_->setBuffered(*static_cast<double *>(property->data));
        } else if (name == "video-params/h" && property->format == MPV_FORMAT_INT64) {
            updateQualityBadge();
            qint64 w = 0, h = 0;
            if (frc_ && mpvInt64Property("video-params/w", w) && mpvInt64Property("video-params/h", h)) {
                frc_->setSourceSize(QSize(int(w), int(h)));
            }
        }
    } else if (event->event_id == MPV_EVENT_SEEK) {
        // Never interpolate between frames from before and after a seek.
        if (frc_) frc_->resetTimeline();
        interpolationWarmup_.restart();
        interpolationLastDrops_ = -1;
        interpolationSlowSamples_ = 0;
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        if (frc_) frc_->resetTimeline();
        interpolationWarmup_.restart();
        interpolationLastDrops_ = -1;
        interpolationSlowSamples_ = 0;
        mediaLoaded_ = true;
        paused_ = false;
        eofReached_ = false;
        const QString fileName = currentMediaTitle();
        addRecent(progressKey_);
        if (pendingResume_ > 0.0) {
            command({"seek", QString::number(pendingResume_, 'f', 3), "absolute+exact"});
            pendingResume_ = 0.0;
        }
        if (homePage_ && !currentPath_.isEmpty()) {
            homePage_->setCurrentMedia(fileName, progressKey_, true);
        }
        const QString eyebrow = addonPlayback_ && !addonPlayback_->addonName.isEmpty()
            ? QStringLiteral("NOW PLAYING · %1").arg(addonPlayback_->addonName.toUpper())
            : QStringLiteral("NOW PLAYING");
        mediaEyebrow_->setText(eyebrow);
        if (playerChrome_) {
            playerChrome_->setLoading(false);
            playerChrome_->setFinished(false);
            playerChrome_->setMediaLoaded(true);
            playerChrome_->setMediaTitle(fileName, eyebrow);
        }
        nextPath_ = addonPlayback_ ? QString() : homePage_->library()->nextFile(currentPath_);
        if (!addonPlayback_ && nextPath_.isEmpty()) nextPath_ = findNextInFolder(currentPath_);
        if (addonPlayback_) {
            fetchAddonSubtitles();
        }
        if (playerChrome_) playerChrome_->setHasNext(!nextPath_.isEmpty());
        updatePlaybackUi();
        updateCenterState();
        QTimer::singleShot(0, this, &MainWindow::refreshTracks);
        QTimer::singleShot(0, this, &MainWindow::updateInterpolationLabels);
        QTimer::singleShot(0, this, &MainWindow::updateChapterInfo);
        QTimer::singleShot(0, this, &MainWindow::updateQualityBadge);
        // Benchmark-only entry point: the normal UI remains the sole control
        // path unless a local test explicitly requests Generic D3D11 FRC.
        const QString frcAutostart = qEnvironmentVariable("LAMBDA_FRC_AUTOSTART");
        if (frcAutostart == QLatin1String("generic") || frcAutostart == QLatin1String("generic-fast")) {
            QTimer::singleShot(5000, this, [this, frcAutostart] {
                if (mediaLoaded_ && video_ && video_->isRenderReady()) {
                    interpolationModeChanged(frcAutostart == QLatin1String("generic-fast")
                                                 ? kFast240FrcModeIndex : kGenericFrcModeIndex);
                }
            });
        }
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        auto *endFile = static_cast<mpv_event_end_file *>(event->data);
        if (endFile && endFile->reason == MPV_END_FILE_REASON_ERROR) {
            // The file could not be opened/decoded: tell the user and return
            // the player to its ready state instead of a silent black screen.
            const QString failedName = currentMediaTitle();
            mediaLoaded_ = false;
            if (!addonPlayback_) {
                removeRecent(currentPath_);
            }
            if (playerChrome_) {
                playerChrome_->setLoading(false);
                playerChrome_->setMediaLoaded(false);
                playerChrome_->setMediaTitle("Open or drop a video", "READY");
            }
            if (homePage_) homePage_->setCurrentMedia({}, {}, false);
            QString reason = QString::fromUtf8(mpv_error_string(endFile->error));
            if (addonPlayback_ && addonPlayback_->source.viaStreamingServer) {
                reason = QStringLiteral("the streaming engine could not deliver the file (no peers or data)");
            }
            showToast(QString("Could not play \"%1\": %2").arg(failedName, reason), true);
        }
        paused_ = true;
        updatePlaybackUi();
        updateCenterState();
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        auto *message = static_cast<mpv_event_log_message *>(event->data);
        if (message && interpolation_) {
            interpolation_->handleLogMessage(QString::fromUtf8(message->prefix),
                                             QString::fromUtf8(message->level),
                                             QString::fromUtf8(message->text));
        }
    }
}

void MainWindow::configureInterpolationShortcut()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Interpolation shortcut");
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel("Press your shortcut with Ctrl or Alt. Clear it to disable the shortcut.", &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *edit = new QKeySequenceEdit(interpolationShortcut_, &dialog);
    edit->setMaximumSequenceLength(1);
    edit->setClearButtonEnabled(true);
    layout->addWidget(edit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto sequence = edit->keySequence();
        if (!sequence.isEmpty()) {
            const auto combination = sequence[0];
            if (!(combination.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier))
                || (combination.key() == Qt::Key_O && combination.keyboardModifiers().testFlag(Qt::ControlModifier))) {
                label->setText("Choose a shortcut with Ctrl or Alt. Ctrl+O is reserved for Open video.");
                return;
            }
        }
        interpolationShortcut_ = sequence;
        appSettings().setValue("player/interpolationShortcut", sequence.toString(QKeySequence::PortableText));
        playerChrome_->setInterpolationShortcut(sequence.toString(QKeySequence::NativeText));
        dialog.accept();
    });
    edit->setFocus();
    dialog.exec();
}

void MainWindow::toggleInterpolation()
{
    if (!mediaLoaded_) return;
    if (interpolationMode_->currentIndex() > 0) {
        interpolationMode_->setCurrentIndex(0);
        showToast("Interpolation off");
        return;
    }
    auto *model = qobject_cast<QStandardItemModel *>(interpolationMode_->model());
    auto available = [&](int index) {
        return index > 0 && index < interpolationMode_->count() && model && model->item(index) && model->item(index)->isEnabled();
    };
    int index = lastInterpolationIndex_;
    if (!available(index)) {
        index = 0;
        for (int candidate : {3, 1, 2, 7, 11}) if (available(candidate)) { index = candidate; break; }
    }
    if (index) interpolationMode_->setCurrentIndex(index);
    else showToast("No interpolation mode is available. Open Smoothness for details.", true);
}

void MainWindow::interpolationModeChanged(int index)
{
    interpolationWarmup_.restart();
    interpolationLastDrops_ = -1;
    interpolationSlowSamples_ = 0;
    if (!mediaLoaded_ || !video_ || !video_->isRenderReady()) {
        const QSignalBlocker blocker(interpolationMode_);
        interpolationMode_->setCurrentIndex(frc_ && frc_->isEnabled() ? frcMenuIndex_
                                            : interpolation_ ? int(interpolation_->mode()) : 0);
        syncChromeSettings();
        showToast("Open a video before changing frame interpolation.");
        return;
    }
    if (!interpolation_ || index < 0) {
        return;
    }

    if (index > 0) {
        lastInterpolationIndex_ = index;
        appSettings().setValue("player/lastInterpolationIndex", index);
    }
    const bool acceleratedRife = (index == 1 || index == 2) && frc_
        && frc_->isAvailable(frc::ScreenInterpolationController::Backend::GenericD3D11);
    if (isFrcMode(index) || acceleratedRife) {
        // Post-render GPU FRC replaces the VapourSynth path entirely.
        if (!frc_) return;
        frc_->setBackend(frcBackendFor(index));
        if (frcBackendFor(index) == frc::ScreenInterpolationController::Backend::GenericD3D11) {
            frc_->setGenericMotion(frc::GenericD3D11Fruc::Motion::RifeReusable);
        }
        frc_->setTargetFps(acceleratedRife && index == 2 ? 60.0 : frcTargetFor(index));
        frcMenuIndex_ = index;
        if (frc_->isEnabled()) {
            updateQualityBadge();
            syncChromeSettings();
            showToast(QString("Smoothness: %1").arg(interpolationMode_->itemText(index)));
            return;
        }
        QString error;
        if (interpolation_->mode() != InterpolationController::Mode::Off) {
            interpolation_->setMode(InterpolationController::Mode::Off, &error);
        }
        double fps = 0.0, speed = 1.0;
        mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE, &fps);
        mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
        frc_->setSourceFps(fps, speed);
        if (!frc_->setEnabled(true, &error)) {
            const QSignalBlocker blocker(interpolationMode_);
            interpolationMode_->setCurrentIndex(0);
            updateQualityBadge();
            syncChromeSettings();
            showInterpolationError(error);
            return;
        }
        updateQualityBadge();
        syncChromeSettings();
        showToast(QString("Frame interpolation on: %1").arg(interpolationMode_->itemText(index)));
        return;
    }
    if (frc_ && frc_->isEnabled()) {
        frc_->setEnabled(false);
    }

    auto mode = InterpolationController::Mode::Off;
    if (index == 1) {
        mode = InterpolationController::Mode::RifeDouble;
    } else if (index == 2) {
        mode = InterpolationController::Mode::Rife60;
    }

    QString error;
    if (!interpolation_->setMode(mode, &error)) {
        const QSignalBlocker blocker(interpolationMode_);
        interpolationMode_->setCurrentIndex(0);
        updateQualityBadge();
        syncChromeSettings();
        showInterpolationError(error);
        return;
    }

    updateQualityBadge();
    syncChromeSettings();
    if (mode != InterpolationController::Mode::Off) {
        showToast(QString("Frame interpolation on: %1").arg(interpolationMode_->itemText(index)));
    }
}

QString MainWindow::formatFps(double fps)
{
    if (std::abs(fps - std::round(fps)) < 0.005) {
        return QString::number(qRound(fps));
    }
    QString text = QString::number(fps, 'f', 3);
    while (text.endsWith('0')) {
        text.chop(1);
    }
    return text;
}

void MainWindow::updateInterpolationLabels()
{
    if (!mpv_ || !interpolationMode_) {
        return;
    }

    double containerFps = 0.0;
    mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE, &containerFps);
    const double fps = InterpolationController::normalizedFps(containerFps);
    if (frc_) {
        double speed = 1.0;
        mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
        frc_->setSourceFps(fps > 0.0 ? fps : containerFps, speed);
    }

    if (fps > 0.0) {
        interpolationMode_->setItemText(0, QString("Original (%1 fps)").arg(formatFps(fps)));
        interpolationMode_->setItemText(1, QString("%1 fps (RIFE)").arg(formatFps(fps * 2.0)));
        interpolationMode_->setItemText(kGenericFrcModeIndex, QString("%1 fps (Universal D3D11)").arg(formatFps(fps * 2.0)));
        interpolationMode_->setItemText(kGeneric60FrcModeIndex, "60 fps (Universal D3D11)");
        interpolationMode_->setItemText(kGeneric120FrcModeIndex, "120 fps (Universal D3D11)");
        interpolationMode_->setItemText(kGeneric200FrcModeIndex, "200 fps (Universal D3D11)");
        interpolationMode_->setItemText(kFast240FrcModeIndex, "240 fps (Fast RIFE, experimental)");
        interpolationMode_->setItemText(kAmfFrcModeIndex, QString("%1 fps (AMD FRC)").arg(formatFps(fps * 2.0)));
        // Fixed-rate FRC modes only make sense above the source rate.
        if (auto *model = qobject_cast<QStandardItemModel *>(interpolationMode_->model())) {
            for (int i = kGenericFrcModeIndex; i < kGenericFrcModeIndex + kFrcModeCount; ++i) {
                QStandardItem *item = model->item(i);
                if (!item) continue;
                const bool available = frc_ && frc_->isAvailable(frcBackendFor(i));
                const bool fixedRate = frcTargetFor(i) > 0.0;
                const bool usable = available && (!fixedRate || frcTargetFor(i) > fps + 0.5);
                item->setEnabled(usable);
                if (available && fixedRate && !usable) {
                    item->setToolTip(QString("This video is already %1 fps").arg(formatFps(fps)));
                }
                if (!usable && interpolationMode_->currentIndex() == i) {
                    interpolationMode_->setCurrentIndex(0); // turns FRC off via interpolationModeChanged
                }
            }
        }
    } else {
        interpolationMode_->setItemText(kGenericFrcModeIndex, "Double frame rate (Universal D3D11)");
        interpolationMode_->setItemText(kGeneric60FrcModeIndex, "60 fps (Universal D3D11)");
        interpolationMode_->setItemText(kGeneric120FrcModeIndex, "120 fps (Universal D3D11)");
        interpolationMode_->setItemText(kGeneric200FrcModeIndex, "200 fps (Universal D3D11)");
        interpolationMode_->setItemText(kFast240FrcModeIndex, "240 fps (Fast RIFE, experimental)");
        interpolationMode_->setItemText(kAmfFrcModeIndex, "Double frame rate (AMD FRC)");
        interpolationMode_->setItemText(0, "Original");
        interpolationMode_->setItemText(1, "Double frame rate (RIFE)");
    }

    // 60 fps mode only makes sense below 60 fps.
    const bool can60 = InterpolationController::supports60(fps);
    if (auto *model = qobject_cast<QStandardItemModel *>(interpolationMode_->model())) {
        if (QStandardItem *item = model->item(2)) {
            item->setEnabled(can60);
            item->setToolTip(can60 ? QString()
                                   : QString("Only available for videos below 60 fps"));
        }
    }

    if (!can60 && interpolationMode_->currentIndex() == 2) {
        interpolationMode_->setCurrentIndex(0); // turns RIFE off via interpolationModeChanged
        showInterpolationError(fps > 0.0
                                   ? QString("This video is already %1 fps, so 60 fps mode was turned off.")
                                         .arg(formatFps(fps))
                                   : QString("This video does not report a frame rate, so 60 fps mode was turned off."));
    }
    syncChromeSettings();
}

void MainWindow::interpolationDeactivated(const QString &reason)
{
    const QSignalBlocker blocker(interpolationMode_);
    interpolationMode_->setCurrentIndex(0);
    updateQualityBadge();
    syncChromeSettings();
    showInterpolationError(reason);
}

void MainWindow::showInterpolationError(const QString &message)
{
    showToast(QString("Frame interpolation is off. Playback continues normally.\n%1").arg(message), true);
}

void MainWindow::updateSubtitleMargin()
{
    if (!mpv_ || !video_ || video_->height() <= 0) {
        return;
    }
    // Lift subtitles above the visible control deck. sub-margin-y is measured
    // in scaled pixels of a 720-line virtual screen; 22 is mpv's default.
    const int margin = subtitleInset_ > 0
                           ? qBound(22, qRound((subtitleInset_ + 14) * 720.0 / video_->height()), 520)
                           : 22;
    if (margin == lastSubtitleMargin_) {
        return;
    }
    lastSubtitleMargin_ = margin;
    setMpvPropertyInt64("sub-margin-y", margin);
}

void MainWindow::showToast(const QString &message, bool warning)
{
    // Deferred so it never runs inside mpv event processing. Themed toasts
    // are shown on whichever page is visible.
    QTimer::singleShot(0, this, [this, message, warning] {
        if (isPlayerVisible() && playerChrome_) {
            playerChrome_->toast(message, warning);
        } else if (homePage_) {
            homePage_->toast(message, warning);
        }
    });
}

QString MainWindow::mpvStringProperty(const QByteArray &name) const
{
    if (!mpv_) {
        return {};
    }

    char *value = mpv_get_property_string(mpv_, name.constData());
    if (!value) {
        return {};
    }

    const QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

bool MainWindow::mpvInt64Property(const QByteArray &name, qint64 &value) const
{
    if (!mpv_) {
        return false;
    }

    int64_t raw = 0;
    if (mpv_get_property(mpv_, name.constData(), MPV_FORMAT_INT64, &raw) < 0) {
        return false;
    }

    value = static_cast<qint64>(raw);
    return true;
}

bool MainWindow::mpvFlagProperty(const QByteArray &name, bool &value) const
{
    if (!mpv_) {
        return false;
    }

    int raw = 0;
    if (mpv_get_property(mpv_, name.constData(), MPV_FORMAT_FLAG, &raw) < 0) {
        return false;
    }

    value = raw != 0;
    return true;
}

void MainWindow::refreshTracks()
{
    if (!mpv_) {
        return;
    }

    const QSignalBlocker audioBlocker(audioTrack_);
    const QSignalBlocker subtitleBlocker(subtitleTrack_);

    audioTrack_->clear();
    subtitleTrack_->clear();
    subtitleTrack_->addItem("Off", QVariant::fromValue<qlonglong>(-1));

    qint64 trackCount = 0;
    mpvInt64Property("track-list/count", trackCount);

    int selectedAudioIndex = -1;
    int selectedSubtitleIndex = 0;
    int audioNumber = 0;
    int subtitleNumber = 0;

    for (qint64 i = 0; i < trackCount; ++i) {
        const QByteArray prefix = "track-list/" + QByteArray::number(i) + "/";
        const QString type = mpvStringProperty(prefix + "type");

        if (type != "audio" && type != "sub") {
            continue;
        }

        qint64 id = -1;
        if (!mpvInt64Property(prefix + "id", id)) {
            continue;
        }

        const QString title = mpvStringProperty(prefix + "title").trimmed();
        const QString language = mpvStringProperty(prefix + "lang").trimmed();
        bool selected = false;
        mpvFlagProperty(prefix + "selected", selected);

        QStringList details;
        if (!language.isEmpty()) {
            details << language;
        }
        if (!title.isEmpty() && title.compare(language, Qt::CaseInsensitive) != 0) {
            details << title;
        }

        if (type == "audio") {
            ++audioNumber;
            QString label = QString("Audio %1").arg(audioNumber);
            if (!details.isEmpty()) {
                label += QString(" — %1").arg(details.join(" · "));
            }

            audioTrack_->addItem(label, QVariant::fromValue<qlonglong>(id));
            if (selected) {
                selectedAudioIndex = audioTrack_->count() - 1;
            }
        } else {
            ++subtitleNumber;
            QString label = QString("Subtitle %1").arg(subtitleNumber);
            if (!details.isEmpty()) {
                label += QString(" — %1").arg(details.join(" · "));
            }
            bool external = false;
            mpvFlagProperty(prefix + "external", external);
            QString group = external ? QStringLiteral("External") : QStringLiteral("Embedded");
            const QString externalFile = mpvStringProperty(prefix + "external-filename");
            if (external && addonSubtitleLabels_.contains(externalFile)) {
                label = addonSubtitleLabels_.value(externalFile);
                group = QStringLiteral("Add-ons");
            }

            subtitleTrack_->addItem(label, QVariant::fromValue<qlonglong>(id));
            subtitleTrack_->setItemData(subtitleTrack_->count() - 1, group, Qt::UserRole + 1);
            if (selected) {
                selectedSubtitleIndex = subtitleTrack_->count() - 1;
            }
        }
    }

    // Addon subtitles not loaded yet: fetched by mpv only when chosen.
    for (int i = 0; i < addonSubtitles_.size(); ++i) {
        const AddonSubtitle &item = addonSubtitles_[i];
        if (addonSubtitleLabels_.contains(item.subtitle.url)) {
            continue;
        }
        QString label = QStringLiteral("%1 · %2").arg(stremio::languageName(item.subtitle.lang), item.addonName);
        if (item.subtitle.label && !item.subtitle.label->isEmpty()) {
            label += QStringLiteral(" · ") + *item.subtitle.label;
        }
        subtitleTrack_->addItem(label, QStringLiteral("addon:%1").arg(i));
        subtitleTrack_->setItemData(subtitleTrack_->count() - 1, QStringLiteral("Add-ons"), Qt::UserRole + 1);
    }

    if (audioTrack_->count() == 0) {
        audioTrack_->addItem("No audio tracks");
        audioTrack_->setEnabled(false);
    } else {
        audioTrack_->setEnabled(true);
        audioTrack_->setCurrentIndex(selectedAudioIndex >= 0 ? selectedAudioIndex : 0);
    }

    subtitleTrack_->setCurrentIndex(selectedSubtitleIndex);
    syncChromeSettings();
}

void MainWindow::audioTrackChanged(int index)
{
    if (index < 0 || !audioTrack_->isEnabled()) {
        return;
    }

    bool ok = false;
    const qlonglong id = audioTrack_->itemData(index).toLongLong(&ok);
    if (ok) {
        setMpvPropertyInt64("aid", id);
    }
    syncChromeSettings();
}

void MainWindow::subtitleTrackChanged(int index)
{
    if (index < 0) {
        return;
    }

    const QVariant data = subtitleTrack_->itemData(index);
    if (data.typeId() == QMetaType::QString && data.toString().startsWith(QLatin1String("addon:"))) {
        selectAddonSubtitle(data.toString().mid(6).toInt());
        return;
    }

    bool ok = false;
    const qlonglong id = data.toLongLong(&ok);
    if (!ok) {
        return;
    }

    if (id < 0) {
        mpv_set_property_string(mpv_, "sid", "no");
    } else {
        setMpvPropertyFlag("sub-visibility", true);
        setMpvPropertyInt64("sid", id);
    }

    syncChromeSettings();
    QTimer::singleShot(0, this, &MainWindow::refreshTracks);
}

void MainWindow::loadSubtitle()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Load subtitle",
        {},
        "Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.idx *.sup);;All files (*.*)");

    if (path.isEmpty()) {
        return;
    }

    setMpvPropertyFlag("sub-visibility", true);
    command({"sub-add", path, "select"});

    QTimer::singleShot(250, this, &MainWindow::refreshTracks);
}

bool MainWindow::keyEventOwnerIsThisWindow(QObject *watched) const
{
    // The application-wide filter sees key events several times (window,
    // widget, focus proxy). Handle each key once, at the QWindow level, and
    // never while a modal dialog (e.g. the file picker) is open.
    if (QApplication::activeModalWidget()) {
        return false;
    }
    auto *window = qobject_cast<QWindow *>(watched);
    return window && window == windowHandle();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == seek_ && event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton && seek_->width() > 0) {
            const int value = QStyle::sliderValueFromPosition(
                seek_->minimum(),
                seek_->maximum(),
                qRound(mouseEvent->position().x()),
                seek_->width());

            seek_->setValue(value);
            seeking_ = true;
            seekReleased();
            return true;
        }
    }

    if (event->type() == QEvent::KeyPress && isActiveWindow()
        && keyEventOwnerIsThisWindow(watched)) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_O && keyEvent->modifiers().testFlag(Qt::ControlModifier)) {
            if (!keyEvent->isAutoRepeat()) {
                QTimer::singleShot(0, this, &MainWindow::openFile);
            }
            return true;
        }
    }

    if (event->type() == QEvent::KeyPress && isActiveWindow() && isPlayerVisible()
        && keyEventOwnerIsThisWindow(watched)) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);

        if (!interpolationShortcut_.isEmpty()
            && QKeySequence(keyEvent->keyCombination()) == interpolationShortcut_) {
            if (!keyEvent->isAutoRepeat()) toggleInterpolation();
            return true;
        }
        switch (keyEvent->key()) {
        case Qt::Key_Space:
            if (!keyEvent->isAutoRepeat()) togglePause();
            return true;
        case Qt::Key_F:
            if (!keyEvent->isAutoRepeat()) toggleFullscreen();
            return true;
        case Qt::Key_Escape:
            if (playerChrome_) playerChrome_->closeSettings();
            if (fullscreenMode_) {
                toggleFullscreen();
                return true;
            }
            if (miniMode_) {
                toggleMiniPlayer();
                return true;
            }
            break;
        case Qt::Key_M:
            toggleMute();
            return true;
        case Qt::Key_Right:
            command({"seek", "5", "relative"});
            return true;
        case Qt::Key_Left:
            command({"seek", "-5", "relative"});
            return true;
        default:
            break;
        }
    }

    if (isPlayerVisible() && fullscreenMode_ && event->type() == QEvent::MouseMove) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && (widget == this || isAncestorOf(widget))) {
            showFullscreenControls();
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::toggleSettingsPanel()
{
    settingsVisible_ = !settingsVisible_;
    settingsPanel_->setVisible(settingsVisible_);
    settingsButton_->setProperty("active", settingsVisible_);
    settingsButton_->style()->unpolish(settingsButton_);
    settingsButton_->style()->polish(settingsButton_);

    controlsHeight_ = controls_->sizeHint().height();

    if (fullscreenMode_) {
        showFullscreenControls();
    }
    layoutOverlayWidgets();
    raiseOverlayWidgets();
}

void MainWindow::showTrackPicker(QComboBox *combo)
{
    if (!combo) {
        return;
    }

    if (!settingsVisible_) {
        settingsVisible_ = true;
        settingsPanel_->setVisible(true);
        settingsButton_->setProperty("active", true);
        settingsButton_->style()->unpolish(settingsButton_);
        settingsButton_->style()->polish(settingsButton_);
        controlsHeight_ = controls_->sizeHint().height();
        layoutOverlayWidgets();
    }

    if (fullscreenMode_) {
        showFullscreenControls();
    }

    combo->setFocus(Qt::MouseFocusReason);
    QTimer::singleShot(0, combo, &QComboBox::showPopup);
}

void MainWindow::updatePlaybackUi()
{
    const QString playGlyph = paused_ ? QStringLiteral("▶") : QStringLiteral("Ⅱ");
    playButton_->setText(playGlyph);
    railPlayerButton_->setText(playGlyph);
    centerPlayButton_->setText("▶");

    muteButton_->setText(muted_ ? "MUTE" : "VOL");
    muteButton_->setProperty("active", muted_);

    if (playerChrome_) {
        playerChrome_->setPlaybackState(paused_, muted_);
    }
}


void MainWindow::updateCenterState()
{
    // Native controls are retained only as backend state containers.
    // The visible center state is the real Vui CSS element.
    if (centerState_) centerState_->hide();
    if (playerChrome_) {
        playerChrome_->setMediaLoaded(mediaLoaded_);
        playerChrome_->setPlaybackState(paused_, muted_);
    }
    layoutOverlayWidgets();
}


void MainWindow::updateChapterInfo()
{
    qint64 chapter = -1;
    qint64 chapterCount = 0;

    mpvInt64Property("chapter", chapter);
    mpvInt64Property("chapter-list/count", chapterCount);

    if (chapter < 0 || chapterCount <= 0) {
        chapterIndexLabel_->setText("--");
        chapterTitleLabel_->setText("No chapters");
        if (playerChrome_) playerChrome_->setChapter("--", "No chapters");
        return;
    }

    const QString index = QString("%1").arg(chapter + 1, 2, 10, QChar('0'));
    QString title = mpvStringProperty("chapter-list/" + QByteArray::number(chapter) + "/title").trimmed();
    if (title.isEmpty()) {
        title = QString("Chapter %1").arg(chapter + 1);
    }
    chapterIndexLabel_->setText(index);
    chapterTitleLabel_->setText(title);
    if (playerChrome_) playerChrome_->setChapter(index, title);
}


void MainWindow::updateQualityBadge()
{
    if (!qualityPrimary_ || !qualitySecondary_) {
        return;
    }

    qint64 height = 0;
    QString quality = "VIDEO";

    if (mpvInt64Property("video-params/h", height) && height > 0) {
        if (height >= 2160) {
            quality = "4K";
        } else if (height >= 1440) {
            quality = "1440P";
        } else if (height >= 1080) {
            quality = "1080P";
        } else if (height >= 720) {
            quality = "720P";
        } else {
            quality = QString("%1P").arg(height);
        }
    }

    qualityPrimary_->setText(quality);

    const int mode = interpolationMode_ ? interpolationMode_->currentIndex() : 0;
    if (isFrcMode(mode)) {
        const double target = frcTargetFor(mode);
        const QString backend = mode == kFast240FrcModeIndex ? QStringLiteral("FAST RIFE")
            : frcBackendFor(mode) == frc::ScreenInterpolationController::Backend::GenericD3D11
            ? QStringLiteral("D3D11") : QStringLiteral("AMD FRC");
        qualitySecondary_->setText(target > 0 ? QString("%1 %2").arg(backend).arg(int(target)) : backend);
    } else if (mode == 1) {
        qualitySecondary_->setText("RIFE DOUBLE");
    } else if (mode == 2) {
        qualitySecondary_->setText("RIFE 60");
    } else {
        qualitySecondary_->setText("ORIGINAL");
    }

    if (playerChrome_) {
        playerChrome_->setQuality(qualityPrimary_->text(), qualitySecondary_->text());
    }
}

void MainWindow::layoutOverlayWidgets()
{
    if (!root_ || !playerChrome_) return;

    playerChrome_->setGeometry(root_->rect());
    if (fullscreenMode_) video_->clearMask();
    if (fullscreenMode_ || video_->geometry().isEmpty() || video_->width() < 320 || video_->height() < 180) {
        video_->setGeometry(root_->rect());
    }

    // Keep all legacy native chrome hidden. The visible controls are CSS.
    for (QWidget *legacyOverlay : {topBar_, sideRail_, qualityBadge_, centerState_, controls_}) {
        if (legacyOverlay) legacyOverlay->hide();
    }

    playerChrome_->raise();
}


void MainWindow::raiseOverlayWidgets()
{
    if (playerChrome_ && playerChrome_->isVisible()) {
        playerChrome_->raise();
    }
}


void MainWindow::setFullscreenChromeVisible(bool visible)
{
    if (!fullscreenMode_) visible = true;
    fullscreenControlsVisible_ = visible;
    if (playerChrome_) playerChrome_->setChromeVisible(visible);
    for (QWidget *legacyOverlay : {topBar_, sideRail_, qualityBadge_, centerState_, controls_}) {
        if (legacyOverlay) legacyOverlay->hide();
    }
}


QRect MainWindow::fullscreenControlsShownRect() const
{
    const int margin = qBound(14, root_->width() / 45, 28);
    const int height = controlsHeight_ > 0 ? controlsHeight_ : controls_->sizeHint().height();
    return QRect(margin,
                 qMax(margin, root_->height() - margin - height),
                 qMax(320, root_->width() - margin * 2),
                 height);
}

QRect MainWindow::fullscreenControlsHiddenRect() const
{
    const QRect shown = fullscreenControlsShownRect();
    return QRect(shown.x(), root_->height() + 4, shown.width(), shown.height());
}

void MainWindow::enterFullscreenControlsMode()
{
    fullscreenControlsTimer_->stop();
    fullscreenControlsVisible_ = false;
    setFullscreenChromeVisible(false);
    setCursor(Qt::BlankCursor);
}


void MainWindow::leaveFullscreenControlsMode()
{
    fullscreenControlsTimer_->stop();
    fullscreenControlsVisible_ = true;
    unsetCursor();
    setFullscreenChromeVisible(true);
    layoutOverlayWidgets();
}


void MainWindow::showFullscreenControls()
{
    if (!fullscreenMode_) return;
    fullscreenControlsVisible_ = true;
    unsetCursor();
    setFullscreenChromeVisible(true);
    fullscreenControlsTimer_->start();
}


void MainWindow::hideFullscreenControls()
{
    if (!fullscreenMode_) return;
    fullscreenControlsVisible_ = false;
    setFullscreenChromeVisible(false);
    setCursor(Qt::BlankCursor);
}


void MainWindow::showHome()
{
    if (!homePage_) {
        return;
    }

    if (miniMode_) {
        toggleMiniPlayer();
    }
    if (fullscreenMode_) {
        toggleFullscreen();
    }

    if (mediaLoaded_) {
        setMpvPropertyFlag("pause", true);
        rememberCurrentProgress();
    }

    homePage_->setCurrentMedia(mediaLoaded_ ? currentMediaTitle() : QString(),
                               mediaLoaded_ ? progressKey_ : QString(), mediaLoaded_);
    publishRecents();

    transitionTo(homePage_);
    setWindowTitle("LAMBDA Player");
}

void MainWindow::resumeFromHome()
{
    if (mediaLoaded_ && !currentPath_.isEmpty()) {
        showPlayer(true);
        return;
    }

    // No session: continue with the most recent video, else ask for one.
    for (const QVariant &entry : recents_) {
        const QString path = entry.toMap().value("path").toString();
        if (entry.toMap().value("online").toBool()) {
            homePage_->openOnline(QJsonObject::fromVariantMap(entry.toMap()));
            return;
        }
        if (QFileInfo::exists(path)) {
            openPath(path);
            return;
        }
    }
    openFile();
}

void MainWindow::showPlayer(bool resumePlayback)
{
    transitionTo(root_);

    if (resumePlayback && mediaLoaded_) {
        if (eofReached_) {
            command({"seek", "0", "absolute"});
        }
        setMpvPropertyFlag("pause", false);
    }
}

void MainWindow::transitionTo(QWidget *target)
{
    if (!stack_ || !target) {
        return;
    }

    if (transitionTimer_ && transitionTimer_->isActive()) {
        // A transition is already running: retarget it.
        transitionTarget_ = target;
        return;
    }

    if (stack_->currentWidget() == target) {
        if (target == root_) {
            layoutOverlayWidgets();
        }
        return;
    }

    // Fade the current page out (CSS, 170 ms), swap, then fade the new page in.
    if (stack_->currentWidget() == homePage_) {
        homePage_->playLeaveAnimation();
    } else if (playerChrome_) {
        playerChrome_->playLeaveAnimation();
    }

    transitionTarget_ = target;
    if (!transitionTimer_) {
        transitionTimer_ = new QTimer(this);
        transitionTimer_->setSingleShot(true);
        transitionTimer_->setInterval(170);
        connect(transitionTimer_, &QTimer::timeout, this, [this] {
            QWidget *next = transitionTarget_;
            transitionTarget_ = nullptr;
            if (!next || stack_->currentWidget() == next) {
                return;
            }
            stack_->setCurrentWidget(next);
            if (next == root_) {
                layoutOverlayWidgets();
                if (playerChrome_) playerChrome_->playEnterAnimation();
            } else if (next == homePage_) {
                homePage_->playEnterAnimation();
            }
            updateWindowState();
        });
    }
    transitionTimer_->start();
}

bool MainWindow::isPlayerVisible() const
{
    return stack_ && stack_->currentWidget() == root_;
}

void MainWindow::command(const QStringList &args)
{
    if (!mpv_) {
        return;
    }

    std::vector<QByteArray>utf8;
    std::vector<const char *>values;

    for (const auto &arg : args) {
        utf8.push_back(arg.toUtf8());
    }
    for (auto &arg : utf8) {
        values.push_back(arg.constData());
    }
    values.push_back(nullptr);

    mpv_command_async(mpv_, 0, values.data());
}

// ---- Files ------------------------------------------------------------------

namespace {


const QStringList &videoNameFilters()
{
    static const QStringList filters = {
        "*.mkv", "*.mp4", "*.m4v", "*.avi", "*.mov", "*.webm", "*.wmv", "*.flv",
        "*.ts", "*.mts", "*.m2ts", "*.mpg", "*.mpeg", "*.ogv", "*.3gp", "*.vob",
    };
    return filters;
}

QSettings appSettings()
{
    const QString dataDir = qEnvironmentVariable("LAMBDA_DATA_DIR");
    if (!dataDir.isEmpty()) {
        QDir().mkpath(dataDir);
        return QSettings(QDir(dataDir).filePath("player.ini"), QSettings::IniFormat);
    }
    return QSettings(QSettings::IniFormat, QSettings::UserScope, "LAMBDA", "LAMBDA Player");
}

} // namespace

QString MainWindow::dialogStartDirectory() const
{
    if (!currentPath_.isEmpty()) {
        return QFileInfo(currentPath_).absolutePath();
    }
    const QString last = appSettings().value("files/lastDirectory").toString();
    if (!last.isEmpty() && QFileInfo(last).isDir()) {
        return last;
    }
    return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
}

QStringList MainWindow::videosInFolder(const QString &directory)
{
    QDir dir(directory);
    QStringList files = dir.entryList(videoNameFilters(), QDir::Files | QDir::Readable);
    // Natural order: "Episode 2" before "Episode 10".
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&collator](const QString &a, const QString &b) {
        return collator.compare(a, b) < 0;
    });
    QStringList paths;
    for (const QString &file : files) {
        paths << dir.absoluteFilePath(file);
    }
    return paths;
}

QString MainWindow::findNextInFolder(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QStringList videos = videosInFolder(info.absolutePath());
    for (int i = 0; i < videos.size(); ++i) {
        if (QFileInfo(videos[i]).fileName().compare(info.fileName(), Qt::CaseInsensitive) == 0) {
            return i + 1 < videos.size() ? videos[i + 1] : QString();
        }
    }
    return {};
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Open video",
        dialogStartDirectory(),
        QString("Video files (%1);;All files (*.*)").arg(videoNameFilters().join(' ')));

    if (!path.isEmpty()) {
        openPath(path);
    }
}

void MainWindow::openFolder()
{
    const QString directory = QFileDialog::getExistingDirectory(this, "Play a folder", dialogStartDirectory());
    if (directory.isEmpty()) {
        return;
    }
    const QStringList videos = videosInFolder(directory);
    if (videos.isEmpty()) {
        showToast("No video files were found in that folder.", true);
        return;
    }
    openPath(videos.first());
}

void MainWindow::playNextInFolder()
{
    if (nextPath_.isEmpty()) {
        showToast("This is the last video in the folder.");
        return;
    }
    openPath(nextPath_);
}

void MainWindow::openLicenses()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString licenses = appDir.filePath("licenses");
    const QString target = QFileInfo(licenses).isDir() ? licenses : appDir.filePath("THIRD_PARTY_NOTICES.md");
    if (!QFileInfo::exists(target) || !QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        showToast("The third-party license files were not found next to LambdaPlayer.exe.", true);
    }
}

void MainWindow::openPath(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    if (!info.exists()) {
        removeRecent(path);
        showToast(QString("File not found:\n%1").arg(QDir::toNativeSeparators(path)), true);
        return;
    }

    // Keep the resume position of the video being replaced.
    if (mediaLoaded_) {
        rememberCurrentProgress();
        if (!eofReached_) {
            command({"write-watch-later-config"});
        }
    }

    currentPath_ = info.absoluteFilePath();
    progressKey_ = currentPath_;
    const auto localHistory = appSettings().value("playback/history").toMap().value(progressKey_).toMap();
    pendingResume_ = localHistory.value("watched").toBool() ? 0.0 : localHistory.value("position").toDouble();
    clearAddonSession();
    mediaLoaded_ = false;
    eofReached_ = false;
    nextPath_.clear();
    position_ = 0.0;
    duration_ = 0.0;
    appSettings().setValue("files/lastDirectory", info.absolutePath());

    const QString fileName = info.fileName();
    showPlayer(false);

    mediaEyebrow_->setText("LOADING");
    mediaTitle_->setText(fileName);
    if (playerChrome_) {
        playerChrome_->setMediaLoaded(false);
        playerChrome_->setFinished(false);
        playerChrome_->setHasNext(false);
        playerChrome_->setLoading(true);
        playerChrome_->setMediaTitle(fileName, "LOADING");
        playerChrome_->setTimeline(0.0, 0.0);
    }
    centerKicker_->setText("LOADING");
    centerText_->setText(fileName);
    centerState_->hide();

    // The OpenGL renderer is created when the player page is first shown;
    // mpv's libmpv VO needs it before video starts, so defer the first load.
    // showHome() pauses mpv and the pause flag survives loadfile: a newly
    // opened video starts playing (as Stremio's ShellVideo does).
    setMpvPropertyFlag("pause", false);
    if (video_ && video_->isRenderReady()) {
        command({"loadfile", currentPath_, "replace"});
    } else {
        pendingPath_ = currentPath_;
    }
    setWindowTitle(QString("%1 — LAMBDA Player").arg(fileName));
}

void MainWindow::openStream(const AddonPlayback &playback)
{
    if (playback.source.url.isEmpty()) {
        return;
    }

    // Keep the resume position of what is being replaced.
    if (mediaLoaded_) {
        rememberCurrentProgress();
        if (!eofReached_) {
            command({"write-watch-later-config"});
        }
    }

    clearAddonSession();
    addonPlayback_ = playback;
    currentPath_ = playback.source.url;
    progressKey_ = "online:" + playback.type + ":" + playback.videoId;
    const auto onlineHistory = appSettings().value("playback/history").toMap().value(progressKey_).toMap();
    pendingResume_ = onlineHistory.value("watched").toBool() ? 0.0 : onlineHistory.value("position").toDouble();
    mediaLoaded_ = false;
    eofReached_ = false;
    nextPath_.clear();
    position_ = 0.0;
    duration_ = 0.0;

    // behaviorHints.proxyHeaders.request: sent by mpv for this file only
    // (cleared again by clearAddonSession for the next file).
    setMpvStringList("http-header-fields", stremio::StreamResolver::mpvHeaderFields(playback.source.httpHeaders));

    // Subtitles embedded in the stream object (SDK stream.subtitles) are
    // offered right away; subtitle addons are asked once the file is loaded.
    addAddonSubtitles(playback.stream.subtitles,
                      playback.addonName.isEmpty() ? QStringLiteral("Stream") : playback.addonName + QStringLiteral(" · stream"));

    const QString title = currentMediaTitle();
    showPlayer(false);
    mediaEyebrow_->setText("LOADING");
    mediaTitle_->setText(title);
    if (playerChrome_) {
        playerChrome_->setMediaLoaded(false);
        playerChrome_->setFinished(false);
        playerChrome_->setHasNext(false);
        playerChrome_->setLoading(true);
        playerChrome_->setMediaTitle(title, playback.source.viaStreamingServer ? "CONNECTING" : "LOADING");
        playerChrome_->setTimeline(0.0, 0.0);
    }
    centerKicker_->setText("LOADING");
    centerText_->setText(title);
    centerState_->hide();

    // showHome() pauses mpv and the pause flag survives loadfile: a newly
    // opened video starts playing (as Stremio's ShellVideo does).
    setMpvPropertyFlag("pause", false);
    if (video_ && video_->isRenderReady()) {
        command({"loadfile", currentPath_, "replace"});
    } else {
        pendingPath_ = currentPath_;
    }
    setWindowTitle(QString("%1 — LAMBDA Player").arg(title));
}

QString MainWindow::currentMediaTitle() const
{
    if (addonPlayback_) {
        const QString title = addonPlayback_->title.isEmpty()
            ? stremio::filenameFromUrl(addonPlayback_->source.url)
            : addonPlayback_->title;
        return addonPlayback_->episodeLabel.isEmpty() ? title : title + QStringLiteral(" — ") + addonPlayback_->episodeLabel;
    }
    return QFileInfo(currentPath_).fileName();
}

void MainWindow::setMpvStringList(const char *name, const QStringList &values)
{
    if (!mpv_) {
        return;
    }
    std::vector<QByteArray> utf8;
    utf8.reserve(size_t(values.size()));
    for (const QString &value : values) {
        utf8.push_back(value.toUtf8());
    }
    std::vector<mpv_node> items(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        items[i].format = MPV_FORMAT_STRING;
        items[i].u.string = utf8[i].data();
    }
    mpv_node_list list{};
    list.num = int(items.size());
    list.values = items.data();
    mpv_node node{};
    node.format = MPV_FORMAT_NODE_ARRAY;
    node.u.list = &list;
    mpv_set_property(mpv_, name, MPV_FORMAT_NODE, &node);
}

void MainWindow::clearAddonSession()
{
    addonPlayback_.reset();
    addonSubtitles_.clear();
    addonSubtitleLabels_.clear();
    ++subtitleGeneration_;
    setMpvStringList("http-header-fields", {});
}

void MainWindow::addAddonSubtitles(const QList<stremio::Subtitles> &subtitles, const QString &addonName)
{
    bool added = false;
    for (const stremio::Subtitles &subtitle : subtitles) {
        const bool duplicate = std::any_of(addonSubtitles_.cbegin(), addonSubtitles_.cend(), [&](const AddonSubtitle &item) {
            return item.subtitle.url == subtitle.url;
        });
        if (!duplicate) {
            addonSubtitles_.append({subtitle, addonName});
            added = true;
        }
    }
    if (added && mediaLoaded_) {
        refreshTracks();
    }
}

void MainWindow::fetchAddonSubtitles()
{
    if (!addonPlayback_ || !stremio_) {
        return;
    }
    const int generation = ++subtitleGeneration_;
    const AddonPlayback playback = *addonPlayback_;
    // stremio-core player.rs: subtitles/{meta type}/{video id} with the
    // video hash, size and filename of the selected stream.
    stremio_->videoParams()->fetch(playback.stream, playback.source, this,
                                   [this, generation, playback](const stremio::VideoParams &params) {
        if (generation != subtitleGeneration_) {
            return;
        }
        const QList<stremio::PlannedRequest> plan =
            stremio_->content()->subtitlePlan(playback.type, playback.videoId, params);
        for (const stremio::PlannedRequest &planned : plan) {
            const stremio::Descriptor *addon = stremio_->content()->addon(planned.request.base);
            const QString addonName = addon ? addon->manifest.name : stremio::displayHost(planned.request.base);
            stremio_->client()->fetchResource(planned.request, this,
                                              [this, generation, addonName](const stremio::ResourceResult &result) {
                if (generation != subtitleGeneration_ || !result.response) {
                    return;
                }
                addAddonSubtitles(result.response->subtitles, addonName);
            });
        }
    });
}

void MainWindow::selectAddonSubtitle(int index)
{
    if (index < 0 || index >= addonSubtitles_.size()) {
        return;
    }
    const AddonSubtitle &item = addonSubtitles_[index];
    const QString label = QStringLiteral("%1 · %2").arg(stremio::languageName(item.subtitle.lang), item.addonName);
    addonSubtitleLabels_.insert(item.subtitle.url, label);
    setMpvPropertyFlag("sub-visibility", true);
    // Loaded through mpv like any external subtitle; downloaded once
    // (stremio-bugs#2292: slow generated subtitles must not be re-requested).
    command({"sub-add", item.subtitle.url, "select", item.subtitle.label.value_or(label), item.subtitle.lang});
    QTimer::singleShot(400, this, &MainWindow::refreshTracks);
}

void MainWindow::togglePause()
{
    if (!mediaLoaded_) {
        if (pendingPath_.isEmpty() && !(playerChrome_ && mediaEyebrow_->text() == "LOADING")) {
            openFile();
        }
        return;
    }
    if (eofReached_) {
        // Finished (keep-open): play again from the start.
        command({"seek", "0", "absolute"});
        setMpvPropertyFlag("pause", false);
        return;
    }
    setMpvPropertyFlag("pause", !paused_);
}


void MainWindow::toggleMute()
{
    setMpvPropertyFlag("mute", !muted_);
}

void MainWindow::toggleFullscreen()
{
    if (!isPlayerVisible()) return;

    // fullscreenMode_ is owned by the app: Qt also reports "full screen" for a
    // frameless window maximized on a monitor without a taskbar.
    if (fullscreenMode_) {
        fullscreenMode_ = false;
        command({"set", "panscan", "0"});
        leaveFullscreenControlsMode();
        showNormal();
        if (maximizedBeforeFullscreen_) {
            maximizeWindow();
        }
        QTimer::singleShot(0, this, &MainWindow::layoutOverlayWidgets);
    } else {
        if (miniMode_) {
            toggleMiniPlayer();
        }
        maximizedBeforeFullscreen_ = isWindowMaximized();
        if (maximizedBeforeFullscreen_) {
            toggleMaximized();
        }
        fullscreenMode_ = true;
        command({"set", "panscan", fullscreenFill_ ? "1" : "0"});
        enterFullscreenControlsMode();
        showFullScreen();
        applyFullscreenFrame();
        QTimer::singleShot(0, this, [this] {
            applyFullscreenFrame(); // Qt may reset window attributes while switching state
            layoutOverlayWidgets();
            showFullscreenControls();
        });
    }
    updateWindowState();
}

// ---- Mini player (always on top) -------------------------------------------

void MainWindow::setAlwaysOnTop(bool onTop)
{
#ifdef Q_OS_WIN
    // SetWindowPos instead of Qt::WindowStaysOnTopHint: changing window
    // flags would recreate the native window.
    ::SetWindowPos(reinterpret_cast<HWND>(winId()), onTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                   0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    Q_UNUSED(onTop);
#endif
}

void MainWindow::toggleMiniPlayer()
{
    if (!miniMode_) {
        if (!isPlayerVisible()) {
            return;
        }
        if (fullscreenMode_) {
            toggleFullscreen();
        }
        miniRestoreMaximized_ = isWindowMaximized();
        if (miniRestoreMaximized_) {
            toggleMaximized(); // restore first so geometry() is the normal geometry
        }
        miniRestoreGeometry_ = geometry();

        // Size the mini player to the video's aspect ratio.
        double aspect = 16.0 / 9.0;
        if (mpv_) {
            double videoAspect = 0.0;
            if (mpv_get_property(mpv_, "video-params/aspect", MPV_FORMAT_DOUBLE, &videoAspect) >= 0
                && videoAspect > 0.2 && videoAspect < 5.0) {
                aspect = videoAspect;
            }
        }
        const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
        int width = qBound(320, available.width() / 4, 560);
        int height = qBound(180, qRound(width / aspect), 420);
        width = qRound(height * aspect);
        const int margin = 24;

        miniMode_ = true;
        setMinimumSize(kMiniMinimumSize);
        setGeometry(available.right() - width - margin + 1, available.bottom() - height - margin + 1, width, height);
        setAlwaysOnTop(true);
        if (playerChrome_) playerChrome_->closeSettings();
    } else {
        miniMode_ = false;
        setAlwaysOnTop(false);
        setMinimumSize(kMinimumWindowSize);
        if (miniRestoreGeometry_.isValid()) {
            setGeometry(miniRestoreGeometry_);
        }
        if (miniRestoreMaximized_) {
            maximizeWindow();
        }
    }
    updateWindowState();
    QTimer::singleShot(0, this, &MainWindow::layoutOverlayWidgets);
}

// ---- Recently played ("Continue watching") ----------------------------------

void MainWindow::loadRecents()
{
    recents_ = appSettings().value("recent/items").toList();
    publishRecents();
}

void MainWindow::saveRecents()
{
    appSettings().setValue("recent/items", recents_);
}

int MainWindow::recentIndex(const QString &path) const
{
    for (int i = 0; i < recents_.size(); ++i) {
        if (recents_[i].toMap().value("path").toString().compare(path, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

void MainWindow::addRecent(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    QVariantMap entry;
    const int index = recentIndex(path);
    if (index >= 0) {
        entry = recents_.takeAt(index).toMap();
    }
    entry.insert("path", path);
    if (addonPlayback_) {
        entry.insert("online", true);
        entry.insert("name", currentMediaTitle());
        entry.insert("type", addonPlayback_->type);
        entry.insert("metaId", addonPlayback_->metaId);
        entry.insert("videoId", addonPlayback_->videoId);
        entry.insert("poster", addonPlayback_->poster);
        entry.insert("title", addonPlayback_->title);
        entry.insert("episodeLabel", addonPlayback_->episodeLabel);
    }
    entry.insert("opened", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    recents_.prepend(entry);
    while (recents_.size() > kMaxRecents) {
        recents_.removeLast();
    }
    saveRecents();
    publishRecents();
}

void MainWindow::removeRecent(const QString &path)
{
    const int index = recentIndex(path);
    if (index >= 0) {
        recents_.removeAt(index);
        saveRecents();
        publishRecents();
    }
}

void MainWindow::rememberCurrentProgress()
{
    if (!mediaLoaded_ || currentPath_.isEmpty()) {
        return;
    }
    if (progressKey_.isEmpty()) return;
    const int index = recentIndex(progressKey_);
    QVariantMap entry = index >= 0 ? recents_[index].toMap() : QVariantMap();
    entry.insert("position", position_);
    entry.insert("duration", duration_);
    entry.insert("watched", eofReached_ || (duration_ > 0.0 && position_ / duration_ > 0.97));
    if (index >= 0) recents_[index] = entry;
    auto history = appSettings().value("playback/history").toMap();
    history.insert(progressKey_, entry);
    appSettings().setValue("playback/history", history);
    saveRecents();
    if (homePage_->isVisible()) publishRecents();
}

void MainWindow::publishRecents()
{
    if (!homePage_) {
        return;
    }
    QJsonArray list;
    for (const QVariant &value : recents_) {
        const QVariantMap entry = value.toMap();
        const QString path = entry.value("path").toString();
        const QFileInfo info(path);
        if (!entry.value("online").toBool() && !info.exists()) {
            continue;
        }
        const double position = entry.value("position").toDouble();
        const double duration = entry.value("duration").toDouble();
        QJsonObject item;
        item.insert("path", path);
        item.insert("name", entry.value("online").toBool() ? entry.value("name").toString() : info.completeBaseName());
        item.insert("ext", entry.value("online").toBool() ? "ONLINE" : info.suffix().toUpper());
        for (const QString &key : {QString("online"), QString("type"), QString("metaId"), QString("videoId"), QString("poster"), QString("title"), QString("episodeLabel")}) item.insert(key, QJsonValue::fromVariant(entry.value(key)));
        item.insert("progress", duration > 0.0 ? qBound(0.0, position / duration, 1.0) : 0.0);
        item.insert("remaining", duration > 0.0 ? qMax(0.0, duration - position) : 0.0);
        item.insert("watched", entry.value("watched").toBool());
        list.append(item);
        if (list.size() >= 8) {
            break;
        }
    }
    homePage_->setRecents(list);
}

// ---- Frameless window -------------------------------------------------------

WindowBridge *MainWindow::activeWindowBridge() const
{
    if (isPlayerVisible()) {
        return playerChrome_ ? playerChrome_->windowBridge() : nullptr;
    }
    return homePage_ ? homePage_->windowBridge() : nullptr;
}

void MainWindow::connectWindowBridge(WindowBridge *bridge)
{
    if (!bridge) {
        return;
    }
    connect(bridge, &WindowBridge::updateCheckRequested, this, [this] { updates_->check(true); });
    connect(bridge, &WindowBridge::updateDownloadRequested, updates_, &UpdateChecker::download);
    connect(bridge, &WindowBridge::updateCancelRequested, updates_, &UpdateChecker::cancelDownload);
    connect(bridge, &WindowBridge::updateDismissRequested, updates_, &UpdateChecker::dismiss);
    connect(bridge, &WindowBridge::updateNotesRequested, updates_, &UpdateChecker::releaseNotes);
    connect(bridge, &WindowBridge::testUpdatesRequested, updates_, &UpdateChecker::setIncludeTests);
    connect(bridge, &WindowBridge::minimizeRequested, this, &QWidget::showMinimized);
    connect(bridge, &WindowBridge::toggleMaximizeRequested, this, [this] {
        if (miniMode_) {
            toggleMiniPlayer();
        } else if (fullscreenMode_) {
            toggleFullscreen();
        } else {
            toggleMaximized();
        }
    });
    connect(bridge, &WindowBridge::closeRequested, this, &QWidget::close);
}

bool MainWindow::isWindowMaximized() const
{
#ifdef Q_OS_WIN
    return ::IsZoomed(reinterpret_cast<HWND>(winId())) != FALSE;
#else
    return isMaximized();
#endif
}

void MainWindow::maximizeWindow()
{
    if (!isWindowMaximized()) {
        toggleMaximized();
    }
}

void MainWindow::toggleMaximized()
{
#ifdef Q_OS_WIN
    // Let Windows own the maximized state (Qt's own frameless maximize just
    // resizes the window, which Qt then reports as full screen when the work
    // area equals the monitor).
    ::ShowWindow(reinterpret_cast<HWND>(winId()), isWindowMaximized() ? SW_RESTORE : SW_MAXIMIZE);
#else
    isMaximized() ? showNormal() : showMaximized();
#endif
    updateWindowState();
}

void MainWindow::updateWindowState()
{
    const bool maximized = isWindowMaximized() && !fullscreenMode_;
    for (WindowBridge *bridge : {homePage_ ? homePage_->windowBridge() : nullptr,
                                 playerChrome_ ? playerChrome_->windowBridge() : nullptr}) {
        if (bridge) {
            bridge->setWindowState(maximized, fullscreenMode_, miniMode_);
        }
    }
}

void MainWindow::applyFullscreenFrame()
{
#ifdef Q_OS_WIN
    // Windows 11 keeps rounding corners and drawing its 1 px window border
    // (grey/white, theme-dependent) on a borderless fullscreen window unless
    // told otherwise: square corners, no border, no frame extension.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const DWORD noRounding = 1;              // DWMWCP_DONOTROUND
    ::DwmSetWindowAttribute(hwnd, 33, &noRounding, sizeof(noRounding)); // DWMWA_WINDOW_CORNER_PREFERENCE
    const COLORREF noBorder = 0xFFFFFFFE;    // DWMWA_COLOR_NONE
    ::DwmSetWindowAttribute(hwnd, 34, &noBorder, sizeof(noBorder));     // DWMWA_BORDER_COLOR
    const MARGINS none{0, 0, 0, 0};
    ::DwmExtendFrameIntoClientArea(hwnd, &none);
#endif
}

void MainWindow::applyNativeFrame()
{
#ifdef Q_OS_WIN
    if (fullscreenMode_) {
        applyFullscreenFrame();
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    // Restore the standard frame styles Qt drops for FramelessWindowHint:
    // they give the window its resize border, Aero Snap, min/max animations,
    // the DWM shadow and (Windows 11) rounded corners. WM_NCCALCSIZE below
    // removes the visible title bar/border they would normally draw.
    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    if ((style & wanted) != wanted) {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, style | wanted);
    }

    const DWORD cornerPreference = 2;       // DWMWCP_ROUND
    ::DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference)); // DWMWA_WINDOW_CORNER_PREFERENCE
    const COLORREF defaultBorder = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT (undo fullscreen)
    ::DwmSetWindowAttribute(hwnd, 34, &defaultBorder, sizeof(defaultBorder));      // DWMWA_BORDER_COLOR
    const BOOL darkMode = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));                 // DWMWA_USE_IMMERSIVE_DARK_MODE
    const MARGINS shadowMargins{0, 0, 1, 0};
    ::DwmExtendFrameIntoClientArea(hwnd, &shadowMargins);

    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                       | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
#endif
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    MSG *msg = static_cast<MSG *>(message);
    switch (msg->message) {
    case WM_NCCALCSIZE:
        if (msg->wParam == TRUE) {
            // The whole window is client area (no system title bar/border).
            // A maximized window with a resize frame extends past the monitor
            // by the frame thickness; pull the client area back inside it.
            if (::IsZoomed(msg->hwnd) && !fullscreenMode_) {
                auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
                const UINT dpi = ::GetDpiForWindow(msg->hwnd);
                const int frameX = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                                   + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int frameY = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                                   + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                RECT &client = params->rgrc[0];
                HMONITOR monitor = ::MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                if (::GetMonitorInfoW(monitor, &info)) {
                    // Only inset what actually lies outside the work area.
                    client.left = qMax<LONG>(client.left, qMin<LONG>(client.left + frameX, info.rcWork.left));
                    client.top = qMax<LONG>(client.top, qMin<LONG>(client.top + frameY, info.rcWork.top));
                    client.right = qMin<LONG>(client.right, qMax<LONG>(client.right - frameX, info.rcWork.right));
                    client.bottom = qMin<LONG>(client.bottom, qMax<LONG>(client.bottom - frameY, info.rcWork.bottom));
                }
            }
            *result = 0;
            return true;
        }
        break;
    case WM_NCACTIVATE:
        // Prevent Windows from painting an inactive caption over the client area.
        *result = ::DefWindowProcW(msg->hwnd, msg->message, msg->wParam, -1);
        return true;
    case WM_NCHITTEST: {
        if (fullscreenMode_) {
            *result = HTCLIENT;
            return true;
        }
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ::ScreenToClient(msg->hwnd, &point);
        RECT client;
        ::GetClientRect(msg->hwnd, &client);
        const qreal dpr = devicePixelRatioF();

        if (!::IsZoomed(msg->hwnd)) {
            const int border = qMax(4, qRound(kResizeBorder * dpr));
            const bool left = point.x < border;
            const bool right = point.x >= client.right - border;
            const bool top = point.y < border;
            const bool bottom = point.y >= client.bottom - border;
            if (top && left) { *result = HTTOPLEFT; return true; }
            if (top && right) { *result = HTTOPRIGHT; return true; }
            if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
            if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
            if (left) { *result = HTLEFT; return true; }
            if (right) { *result = HTRIGHT; return true; }
            if (top) { *result = HTTOP; return true; }
            if (bottom) { *result = HTBOTTOM; return true; }
        }

        const QPointF logical(point.x / dpr, point.y / dpr);
        if (WindowBridge *bridge = activeWindowBridge(); bridge && bridge->isDragPoint(logical)) {
            *result = HTCAPTION;
            return true;
        }
        *result = HTCLIENT;
        return true;
    }
    default:
        break;
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (!fullscreenMode_) {
            // Qt restores its own (frameless) styles after fullscreen.
            QTimer::singleShot(0, this, &MainWindow::applyNativeFrame);
        }
        if (isMinimized() && mediaLoaded_) {
            rememberCurrentProgress();
        }
        updateWindowState();
    }
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    applyNativeFrame();
    updateWindowState();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (root_) layoutOverlayWidgets();
}

void MainWindow::seekReleased()
{
    seeking_ = false;
    if (duration_ <= 0) {
        return;
    }

    const double target = duration_ * double(seek_->value()) / 1000.0;
    command({"seek", QString::number(target, 'f', 3), "absolute+exact"});
}

void MainWindow::volumeChanged(int value)
{
    setMpvPropertyDouble("volume", value);
    if (playerChrome_) playerChrome_->setVolume(value);
}


void MainWindow::speedChanged(int index)
{
    static double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    if (index >= 0 && index < 6) {
        setMpvPropertyDouble("speed", speeds[index]);
        updateInterpolationLabels();
        if (playerChrome_) playerChrome_->setSpeed(speeds[index]);
    }
}


void MainWindow::setMpvPropertyFlag(const char *name, bool value)
{
    if (!mpv_) {
        return;
    }

    int flag = value;
    mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_FLAG, &flag);
}

void MainWindow::setMpvPropertyDouble(const char *name, double value)
{
    if (mpv_) {
        mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_DOUBLE, &value);
    }
}

void MainWindow::setMpvPropertyInt64(const char *name, qint64 value)
{
    if (!mpv_) {
        return;
    }

    int64_t raw = static_cast<int64_t>(value);
    mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_INT64, &raw);
}

void MainWindow::updateTimeLabel()
{
    const QString position = formatTime(position_);
    const QString duration = formatTime(duration_);

    timeLabel_->setText(QString("%1  /  %2").arg(position, duration));
    timelinePositionLabel_->setText(position);
    timelineDurationLabel_->setText(duration);
    if (playerChrome_) playerChrome_->setTimeline(position_, duration_);
}



void MainWindow::syncChromeSettings()
{
    if (!playerChrome_) return;

    QStringList audio;
    for (int i = 0; i < audioTrack_->count(); ++i) audio << audioTrack_->itemText(i);

    QStringList subtitles;
    QStringList subtitleGroups;
    for (int i = 0; i < subtitleTrack_->count(); ++i) {
        subtitles << subtitleTrack_->itemText(i);
        subtitleGroups << subtitleTrack_->itemData(i, Qt::UserRole + 1).toString();
    }

    QStringList interpolation;
    QStringList interpolationReasons;
    QList<bool> interpolationEnabled;
    auto *model = qobject_cast<QStandardItemModel *>(interpolationMode_->model());
    for (int i = 0; i < interpolationMode_->count(); ++i) {
        interpolation << interpolationMode_->itemText(i);
        bool enabled = true;
        if (model && model->item(i)) enabled = model->item(i)->isEnabled();
        interpolationEnabled << enabled;
        interpolationReasons << interpolationMode_->itemData(i, Qt::ToolTipRole).toString();
    }

    playerChrome_->setSettings(audio, qMax(0, audioTrack_->currentIndex()),
                               subtitles, qMax(0, subtitleTrack_->currentIndex()),
                               interpolation, interpolationEnabled,
                               qMax(0, interpolationMode_->currentIndex()), subtitleGroups, fastQuality_, interpolationReasons);
}

QString MainWindow::formatTime(double seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }

    const int total = int(seconds);
    const int hours = total / 3600;
    const int minutes = total % 3600 / 60;
    const int secs = total % 60;

    if (hours) {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) {
        openPath(urls.first().toLocalFile());
    }
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        togglePause();
        break;
    case Qt::Key_F:
        toggleFullscreen();
        break;
    case Qt::Key_Escape:
        if (fullscreenMode_) {
            toggleFullscreen();
        } else {
            QMainWindow::keyPressEvent(event);
        }
        break;
    case Qt::Key_M:
        toggleMute();
        break;
    case Qt::Key_Right:
        command({"seek", "5", "relative"});
        break;
    case Qt::Key_Left:
        command({"seek", "-5", "relative"});
        break;
    default:
        QMainWindow::keyPressEvent(event);
    }
}
