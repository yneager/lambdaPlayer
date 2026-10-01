#pragma once

// Post-render frame generation with AMD AMF FRC.
//
// mpv decodes, times and renders the video exactly as without interpolation.
// Instead of drawing each newly rendered frame into Qt's framebuffer,
// MpvVideoWidget lets mpv render it into a D3D11 texture (shared with OpenGL
// through WGL_NV_DX_interop2) at the widget's physical pixel size. A worker
// thread feeds these textures to AMF FRC (FRC_x2_PRESENT; cascaded up to 16x
// for 60/120/200 fps) and copies every output into a ring of display
// textures. Each output has a content time in source frames; the widget draws,
// on a timer at the target rate, the frame whose content time is closest to
// "now minus the measured pipeline latency". The Qt/Chromium player chrome keeps being composited above
// the video, so no window is captured and nothing can feed back into the
// input. Audio is delayed by the measured presentation lag while active.
//
// Everything here runs on the GUI thread except the worker (AMF submission /
// polling and the output copies).

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <mpv/client.h>

#include "frc/amffrcinterpolator.h"
#include "frc/d3d11videoprocessorprobe.h"
#include "frc/genericd3d11fruc.h"

class QOpenGLContext;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace frc {

class WglDxInterop;

class ScreenInterpolationController final : public QObject
{
    Q_OBJECT

public:
    enum class Backend { GenericD3D11, AmdAmf };
    explicit ScreenInterpolationController(mpv_handle *mpv, QObject *parent = nullptr);
    ~ScreenInterpolationController() override;

    // Checks the whole chain once (D3D11, AMF runtime, FRC component, GL
    // interop) with the video widget's GL context current. The result is
    // cached; `reason` explains an unavailable backend.
    bool probe(QOpenGLContext *context, QString *reason);
    bool isAvailable() const { return available_; }
    bool isAvailable(Backend backend) const;
    QString unavailableReason() const { return unavailableReason_; }
    QString unavailableReason(Backend backend) const;
    QString adapterName() const { return adapterName_; }

    bool setEnabled(bool enabled, QString *error = nullptr);
    bool isEnabled() const { return enabled_; }
    void setBackend(Backend backend);
    void setGenericMotion(GenericD3D11Fruc::Motion motion);
    void setFastQuality(int quality);
    int fastQuality() const { return fastQuality_; }
    Backend backend() const { return backend_; }

    // Playback state from MainWindow.
    void setPaused(bool paused);
    void resetTimeline();        // seek, file change: drop all frame history
    void setSourceFps(double fps, double speed);
    // Output rate: 0 = twice the source rate, otherwise frames per second
    // (60/120/200). Takes effect with the next frame (FRC is reconfigured).
    void setTargetFps(double fps);
    double targetFps() const { return targetFps_; }
    // FRC stages (1..3) and presented rate for the current source/target.
    int stagesNeeded() const;
    double outputRate() const;
    // Decoded video size (video-params/w,h), for the "source" input mode.
    void setSourceSize(const QSize &size) { sourceSize_ = size; }

    // --- MpvVideoWidget hooks (GUI thread, GL context current) -------------
    // Called for every new mpv frame. Renders it through `render` into an
    // FRC input texture and queues it. Returns false when the widget should
    // draw the frame itself (disabled, paused, reconfiguring, failure).
    bool captureFrame(QOpenGLContext *context, const QSize &physicalSize, qreal devicePixelRatio,
                      const std::function<void(unsigned int fbo, int width, int height)> &render);
    // Draws the frame due now into `targetFbo`. False = draw normally.
    bool paint(unsigned int targetFbo, const QSize &physicalSize);
    // Called after Qt has swapped the composited video frame, rather than
    // when an output was merely selected by the presentation timer.
    void frameSwapped();
    // Must run with the GL context current before it is destroyed.
    void releaseGl();

    QJsonObject stats() const;

signals:
    void repaintRequested();
    // The backend stopped itself; playback continues without interpolation.
    void failed(const QString &reason);
    void diagnosticsUpdated(const QJsonObject &stats);

private:
    struct Slot
    {
        ID3D11Texture2D *texture = nullptr;
        enum class State { Free, Queued, Shown } state = State::Free;
    };
    struct Job
    {
        enum class Kind { Configure, Frame, Flush, Quit } kind = Kind::Frame;
        GpuFrame surface;
        qint64 seq = 0;
        qint64 captureNs = 0;
        int generation = 0;
        int width = 0;
        int height = 0;
        int stages = 1;
        double sourceFps = 0.0;
        double outputFps = 0.0;
    };
    struct Presentation
    {
        int slot = -1;
        double content = 0.0;   // source frames
        qint64 wallNs = 0;      // when this content is shown
        qint64 tick = 0;        // presentation tick it was assigned to
    };

    bool ensureDevice(QString *error);
    bool ensureGl(QOpenGLContext *context, QString *error);
    bool createSlots(int width, int height, QString *error);
    void destroySlots();
    void startWorker();
    void stopWorker();
    void workerLoop();
    void post(Job job);
    // GUI-thread handlers of worker results.
    void onOutput(int slot, double content, qint64 readyNs, int generation);
    void onConfigured(bool ok, const QString &error, int width, int height, int stages, int generation);
    void fail(const QString &reason);
    void schedule();
    void presentDue();
    void applyAudioDelay(double lagSeconds);
    void restoreAudioDelay();
    void stopPipeline();
    qint64 nowNs() const { return clock_.nsecsElapsed(); }
    double sourceIntervalNs() const;
    // mpv intended display time of content c (interpolated between the
    // capture times of the surrounding source frames); 0 = unknown.
    qint64 intendedNs(double content) const;
    qint64 tickTime(qint64 tick) const { return epochNs_ + qint64(double(tick) * tickPeriodNs_); }
    void freeSlot(int slot);
    void logStats();
    FrameInterpolator *activeInterpolator() const;

    mpv_handle *mpv_ = nullptr;
    FrcSettings settings_;

    // Devices
    ID3D11Device *device_ = nullptr;
    ID3D11DeviceContext *deviceContext_ = nullptr;
    QString adapterName_;
    std::unique_ptr<AmfFrcInterpolator> amf_;
    std::unique_ptr<GenericD3D11Fruc> generic_;
    GenericD3D11Fruc::Motion genericMotion_ = GenericD3D11Fruc::Motion::Rife;
    bool genericMotionExplicit_ = false;
    int fastQuality_ = 1; // 0 stable, 1 balanced, 2 maximum smoothness
    int effectiveFastQuality_ = 1;
    int fastOverloadSeconds_ = 0;
    int fastRecoverySeconds_ = 0;
    double fastRateScale_ = 1.0;
    int fastRateLowSeconds_ = 0;
    int fastRateRecoverySeconds_ = 0;
    std::unique_ptr<WglDxInterop> interop_;
    QOpenGLContext *glContext_ = nullptr;

    bool probed_ = false;
    bool available_ = false;
    QString unavailableReason_;
    bool amfAvailable_ = false;
    bool genericAvailable_ = false;
    QString amfUnavailableReason_;
    QString genericUnavailableReason_;
    D3D11VideoProcessorProbeResult videoProcessorProbe_;
    Backend backend_ = Backend::AmdAmf;
    bool enabled_ = false;
    bool paused_ = false;

    // Configuration
    QSize configuredSize_;    // FRC initialized for this size
    int configuredStages_ = 0;
    double targetFps_ = 0.0;
    int slotCount_ = 5;
    QSize requestedSize_;     // size being configured / waited for
    bool configuring_ = false;
    QTimer resizeDebounce_;
    QSize pendingSize_;
    int generation_ = 0;

    // Display ring (shared with the worker under slotMutex_)
    std::mutex slotMutex_;
    std::vector<Slot> slots_;
    int shownSlot_ = -1;
    std::deque<Presentation> queue_;
    QTimer presentTimer_;
    GpuFrame lastInput_; // shown until the first FRC output

    // Worker
    std::thread worker_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    std::atomic_int workerGeneration_{0};

    // Timing
    QElapsedTimer clock_;
    qint64 seq_ = 0;
    std::vector<qint64> captureTimes_;   // ring indexed by seq
    double fps_ = 0.0;
    double speed_ = 1.0;
    qint64 lastCaptureNs_ = 0;
    qint64 gridNs_ = 0;                  // phase-locked intended time of the last capture
    double measuredIntervalNs_ = 0.0;
    // Presentation clock: ticks at the output rate, phase-locked to content.
    bool epochValid_ = false;
    qint64 epochNs_ = 0;
    double tickOriginContent_ = 0.0;     // content of tick 0
    qint64 latencyChanges_ = 0;
    qint64 latencyLowSinceNs_ = 0;
    qint64 outputsSinceEpoch_ = 0;
    qint64 audioDelayChanges_ = 0;
    double contentPerTick_ = 0.0;        // source frames per output tick (configured)
    double pendingContentPerTick_ = 0.0;
    double tickPeriodNs_ = 1e9 / 48.0;
    qint64 lastPresentedTick_ = -1;
    qint64 lastPaintedTick_ = -1;
    qint64 lastSwappedTick_ = -1;
    qint64 swappedFrames_ = 0;
    qint64 repeatedSwaps_ = 0;
    qint64 lastSwapNs_ = 0;
    std::vector<double> swapIntervalsMs_;
    std::vector<double> sourceToSwapMs_;
    double shownContent_ = -1.0;
    // L: content -> presentation latency (measured arrival p95 + margin).
    double latencyNs_ = 0.0;
    std::vector<double> processingSamplesMs_;  // rolling, intended time -> available on GUI thread
    qint64 outputsSeen_ = 0;
    std::vector<double> dispatchSamplesMs_;    // worker ready -> GUI thread handled
    std::vector<double> timerLateSamplesMs_;   // presentation timer lateness
    std::vector<double> captureLockMs_, captureRenderMs_, captureUnlockMs_, paintMs_, captureJitterMs_;
    // Diagnostics only (LAMBDA_FRC_LOG): GUI event-loop latency, also while
    // FRC is off, to separate pre-existing stalls from this backend.
    QTimer probeTimer_;
    qint64 probeExpectedNs_ = 0;
    std::vector<double> guiLatencyMs_;
    static double percentile(std::vector<double> samples, double p);
    static void pushSample(std::vector<double> &samples, double value);

    // Audio compensation
    bool audioDelayApplied_ = false;
    double originalAudioDelay_ = 0.0;
    double appliedLagSeconds_ = 0.0;

    // Stats (GUI thread; worker counters are atomics)
    struct Counters
    {
        qint64 captures = 0;
        qint64 presented = 0;
        qint64 presentedGenerated = 0;
        qint64 late = 0;
        qint64 lateStale = 0;      // arrived after its presentation time
        qint64 lateTick = 0;       // mapped to an already presented tick
        qint64 lateContent = 0;    // older content than what is shown
        qint64 lateQueueFull = 0;  // capture skipped: worker queue full
        qint64 droppedPresentations = 0;
        qint64 skipped = 0;     // grid frames not needed for the target rate
    };
    Counters counters_;
    std::atomic<qint64> submitted_{0};
    std::atomic<qint64> outputs_{0};
    std::atomic<qint64> generated_{0};
    std::atomic<qint64> droppedOutputs_{0};
    std::atomic<int> lastSubmitStatus_{0};
    std::atomic<int> lastQueryStatus_{0};
    std::atomic<double> lastProcessMs_{0.0};
    // Worker wall time per source frame (processFrame incl. output copies);
    // the maximum is reset by logStats().
    std::atomic<double> workerMs_{0.0};
    std::atomic<double> workerMaxMs_{0.0};
    double lastWorkerPeakMs_ = 0.0;
    double lastLagMs_ = 0.0;
    qint64 lagFrames_ = -1;
    struct Rates
    {
        qint64 captures = 0, submitted = 0, outputs = 0, generated = 0, presented = 0, swapped = 0, repeated = 0;
        qint64 droppedOutputs = 0, droppedPresentations = 0;
        qint64 atNs = 0;
        double capturesPerSec = 0, submittedPerSec = 0, outputsPerSec = 0, generatedPerSec = 0, presentedPerSec = 0, swappedPerSec = 0;
        double droppedPerSec = 0, repeatedPerSec = 0;
    };
    Rates rates_;
    QTimer statsTimer_;
    qreal devicePixelRatio_ = 1.0;
    QString logPath_;
    // Input size: the widget's physical size (default: exactly the pixels on
    // screen) or, with LAMBDA_FRC_INPUT=source, the widget scaled up so the
    // video area has the decoded resolution (e.g. 3840x2160 on a 1440p
    // monitor; the result is scaled down for display).
    bool sourceSizedInput_ = false;
    QSize sourceSize_;
    QSize widgetSize_;
    QSize inputSizeFor(const QSize &widgetSize) const;
    // Overload watchdog
    qint64 windowLate_ = 0;
    qint64 windowPresented_ = 0;
    int overloadedSeconds_ = 0;
};

} // namespace frc
