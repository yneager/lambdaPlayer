#include "frc/screeninterpolationcontroller.h"

#include "frc/wgldxinterop.h"

#include "public/common/Thread.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QScreen>
#include <QScopeGuard>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>

namespace frc {

namespace {

constexpr int kCaptureRing = 256;
constexpr int kMaxPendingFrames = 4;     // frames queued for the worker
constexpr int kResizeSettleMs = 300;
constexpr qint64 kMs = 1000000;
constexpr double kGridPhaseGain = 0.05; // capture-time correction per source frame

amf_int64 envInt(const char *name, amf_int64 fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? value : fallback;
}

} // namespace

ScreenInterpolationController::ScreenInterpolationController(mpv_handle *mpv, QObject *parent)
    : QObject(parent)
    , mpv_(mpv)
{
    clock_.start();
    captureTimes_.assign(kCaptureRing, 0);
    // Production quality settings; environment overrides exist only for the
    // benchmark matrix (profile 1/2, search 0/1, future 0/1, fallback 0/1).
    settings_.profile = envInt("LAMBDA_FRC_PROFILE", FRC_PROFILE_SUPER);
    settings_.searchMode = envInt("LAMBDA_FRC_SEARCH", FRC_MV_SEARCH_NATIVE);
    settings_.useFutureFrame = envInt("LAMBDA_FRC_FUTURE", 0) != 0;
    settings_.fallbackBlend = envInt("LAMBDA_FRC_FALLBACK", 0) != 0;
    logPath_ = qEnvironmentVariable("LAMBDA_FRC_LOG");
    if (qEnvironmentVariableIntValue("LAMBDA_FRC_TIMER1MS")) {
        ::amf_increase_timer_precision(); // SimpleFRC: timeBeginPeriod(1)
    }
    sourceSizedInput_ = qEnvironmentVariable("LAMBDA_FRC_INPUT") == QLatin1String("source");

    resizeDebounce_.setSingleShot(true);
    connect(&resizeDebounce_, &QTimer::timeout, this, [this] {
        // Geometry is stable: the next captured frame reconfigures.
        requestedSize_ = pendingSize_;
        emit repaintRequested();
    });
    presentTimer_.setSingleShot(true);
    presentTimer_.setTimerType(Qt::PreciseTimer);
    connect(&presentTimer_, &QTimer::timeout, this, &ScreenInterpolationController::presentDue);
    statsTimer_.setInterval(1000);
    connect(&statsTimer_, &QTimer::timeout, this, &ScreenInterpolationController::logStats);
    if (!logPath_.isEmpty()) {
        probeTimer_.setTimerType(Qt::PreciseTimer);
        probeTimer_.setInterval(4);
        connect(&probeTimer_, &QTimer::timeout, this, [this] {
            const qint64 now = nowNs();
            if (probeExpectedNs_ > 0) pushSample(guiLatencyMs_, std::max(0.0, double(now - probeExpectedNs_) / kMs));
            probeExpectedNs_ = now + 4 * kMs;
        });
        probeTimer_.start();
        statsTimer_.start(); // log also while disabled
    }
}

ScreenInterpolationController::~ScreenInterpolationController()
{
    stopWorker();
    restoreAudioDelay();
    // GL objects must already be released through releaseGl(); only D3D
    // objects remain.
    {
        std::lock_guard lock(slotMutex_);
        for (Slot &slot : slots_) {
            if (slot.texture) slot.texture->Release();
        }
        slots_.clear();
    }
    amf_.reset();
    generic_.reset();
    if (deviceContext_) deviceContext_->Release();
    if (device_) device_->Release();
}

// ---- devices ------------------------------------------------------------------------

bool ScreenInterpolationController::ensureDevice(QString *error)
{
    if (device_) return true;

    // Use the adapter that runs the OpenGL context (interop requires one GPU).
    const QString renderer = QString::fromLatin1(
        reinterpret_cast<const char *>(QOpenGLContext::currentContext()->functions()->glGetString(GL_RENDERER)));
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *chosen = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            const QString name = QString::fromWCharArray(desc.Description);
            if (!chosen && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                && (renderer.contains(name, Qt::CaseInsensitive) || name.contains(renderer, Qt::CaseInsensitive))) {
                chosen = adapter;
                adapterName_ = name;
                continue;
            }
            adapter->Release();
        }
        factory->Release();
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device_, nullptr,
                                   &deviceContext_);
    if (chosen) chosen->Release();
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("Direct3D 11 is not available (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    if (adapterName_.isEmpty()) {
        IDXGIDevice *dxgi = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi)))) {
            IDXGIAdapter *adapter = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                adapter->GetDesc(&desc);
                adapterName_ = QString::fromWCharArray(desc.Description);
                adapter->Release();
            }
            dxgi->Release();
        }
    }
    // AMF, the worker copies and the GL interop use the device from
    // different threads.
    ID3D11Multithread *multithread = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    return true;
}

bool ScreenInterpolationController::ensureGl(QOpenGLContext *context, QString *error)
{
    if (interop_ && interop_->isOpen() && glContext_ == context) return true;
    if (!interop_) interop_ = std::make_unique<WglDxInterop>();
    glContext_ = context;
    return interop_->open(context, device_, error);
}

bool ScreenInterpolationController::probe(QOpenGLContext *context, QString *reason)
{
    if (probed_) {
        if (reason) *reason = unavailableReason_;
        return available_;
    }
    probed_ = true;
    QString error;
    const bool deviceReady = ensureDevice(&error);
    if (deviceReady) videoProcessorProbe_ = probeD3D11VideoProcessorFrc(device_, deviceContext_);
    const bool commonReady = deviceReady && ensureGl(context, &error);
    auto probeBackend = [this](FrameInterpolator *backend, QString *why) {
        if (!backend->open(device_, why)) return false;
        if (!backend->initialize(1280, 720, settings_, 1, why)) return false;
        GpuFrame frame = backend->allocateInput(why);
        const WglDxInterop::Texture *view = frame.texture ? interop_->textureFor(frame.texture, why) : nullptr;
        const bool works = view && interop_->lock(view);
        if (works) interop_->unlock(view);
        if (frame.texture) interop_->release(frame.texture);
        backend->terminate();
        if (!works && why && why->isEmpty()) *why = QStringLiteral("OpenGL/D3D11 texture interop probe failed");
        return works;
    };
    if (commonReady) {
        amf_ = std::make_unique<AmfFrcInterpolator>();
        generic_ = std::make_unique<GenericD3D11Fruc>();
        if (genericMotionExplicit_) generic_->setMotion(genericMotion_);
        generic_->setFastQuality(effectiveFastQuality_);
        if (genericMotion_ == GenericD3D11Fruc::Motion::RifeReusable)
            generic_->setAnalysisHeight(effectiveFastQuality_ == 0 ? 360 : effectiveFastQuality_ == 2 ? 720 : 540);
        amfAvailable_ = probeBackend(amf_.get(), &amfUnavailableReason_);
        genericAvailable_ = probeBackend(generic_.get(), &genericUnavailableReason_);
        if (!amfAvailable_ && !genericAvailable_ && interop_) interop_->close();
    } else {
        amfUnavailableReason_ = error;
        genericUnavailableReason_ = error;
    }
    available_ = amfAvailable_ || genericAvailable_;
    if (available_) {
        if (adapterName_.isEmpty()) adapterName_ = QStringLiteral("D3D11 adapter");
        unavailableReason_.clear();
    } else {
        unavailableReason_ = genericUnavailableReason_.isEmpty() ? amfUnavailableReason_ : genericUnavailableReason_;
    }
    if (reason) *reason = unavailableReason_;
    return available_;
}

bool ScreenInterpolationController::isAvailable(Backend backend) const
{
    return backend == Backend::GenericD3D11 ? genericAvailable_ : amfAvailable_;
}

QString ScreenInterpolationController::unavailableReason(Backend backend) const
{
    return backend == Backend::GenericD3D11 ? genericUnavailableReason_ : amfUnavailableReason_;
}

FrameInterpolator *ScreenInterpolationController::activeInterpolator() const
{
    return backend_ == Backend::GenericD3D11 ? static_cast<FrameInterpolator *>(generic_.get())
                                             : static_cast<FrameInterpolator *>(amf_.get());
}

void ScreenInterpolationController::setBackend(Backend backend)
{
    if (backend_ == backend) return;
    if (enabled_) setEnabled(false);
    backend_ = backend;
    configuredSize_ = QSize();
    emit repaintRequested();
}

void ScreenInterpolationController::setGenericMotion(GenericD3D11Fruc::Motion motion)
{
    if (genericMotionExplicit_ && genericMotion_ == motion) return;
    if (enabled_ && backend_ == Backend::GenericD3D11) setEnabled(false);
    if (motion == GenericD3D11Fruc::Motion::RifeReusable
        && genericMotion_ != GenericD3D11Fruc::Motion::RifeReusable) {
        effectiveFastQuality_ = fastQuality_;
        fastOverloadSeconds_ = fastRecoverySeconds_ = 0;
        fastRateScale_ = 1.0;
        fastRateLowSeconds_ = fastRateRecoverySeconds_ = 0;
    }
    genericMotion_ = motion;
    genericMotionExplicit_ = true;
    if (generic_) {
        generic_->setMotion(motion);
        generic_->setFastQuality(effectiveFastQuality_);
        generic_->setAnalysisHeight(motion == GenericD3D11Fruc::Motion::RifeReusable
                                        ? (effectiveFastQuality_ == 0 ? 360 : effectiveFastQuality_ == 2 ? 720 : 540)
                                        : 0);
    }
    configuredSize_ = QSize();
}

void ScreenInterpolationController::setFastQuality(int quality)
{
    quality = std::clamp(quality, 0, 2);
    if (fastQuality_ == quality) return;
    const bool restart = enabled_ && backend_ == Backend::GenericD3D11 && generic_
        && generic_->motion() == GenericD3D11Fruc::Motion::RifeReusable;
    if (restart) setEnabled(false);
    fastQuality_ = effectiveFastQuality_ = quality;
    fastOverloadSeconds_ = fastRecoverySeconds_ = 0;
    if (generic_ && generic_->motion() == GenericD3D11Fruc::Motion::RifeReusable) {
        generic_->setFastQuality(quality);
        generic_->setAnalysisHeight(quality == 0 ? 360 : quality == 2 ? 720 : 540);
        configuredSize_ = QSize();
    }
    if (restart) setEnabled(true);
}

// ---- enable / lifecycle -------------------------------------------------------------

bool ScreenInterpolationController::setEnabled(bool enabled, QString *error)
{
    if (enabled == enabled_) return true;
    if (enabled) {
        if (!isAvailable(backend_)) {
            const QString why = unavailableReason(backend_);
            if (error) *error = why.isEmpty() ? QStringLiteral("Selected FRC backend is not available") : why;
            return false;
        }
        enabled_ = true;
        configuredSize_ = QSize();
        pendingSize_ = QSize();
        requestedSize_ = QSize();
        counters_ = Counters();
        rates_ = Rates();
        rates_.atNs = nowNs();
        submitted_.store(0);
        outputs_.store(0);
        generated_.store(0);
        droppedOutputs_.store(0);
        windowLate_ = 0;
        overloadedSeconds_ = 0;
        processingSamplesMs_.clear();
        dispatchSamplesMs_.clear();
        timerLateSamplesMs_.clear();
        outputsSeen_ = 0;
        latencyNs_ = 0;
        lagFrames_ = -1;
        swappedFrames_ = 0;
        repeatedSwaps_ = 0;
        lastPaintedTick_ = lastSwappedTick_ = -1;
        lastSwapNs_ = 0;
        swapIntervalsMs_.clear();
        sourceToSwapMs_.clear();
        startWorker();
        statsTimer_.start();
        emit repaintRequested();
        return true;
    }
    stopPipeline();
    restoreAudioDelay();
    emit repaintRequested(); // paint() releases the GL side
    return true;
}

void ScreenInterpolationController::stopPipeline()
{
    enabled_ = false;
    if (logPath_.isEmpty()) statsTimer_.stop();
    presentTimer_.stop();
    resizeDebounce_.stop();
    stopWorker();
    queue_.clear();
    shownSlot_ = -1;
    configuredSize_ = QSize();
    configuring_ = false;
    if (FrameInterpolator *backend = activeInterpolator()) backend->terminate();
}

void ScreenInterpolationController::fail(const QString &reason)
{
    if (!enabled_) return;
    stopPipeline();
    restoreAudioDelay();
    emit repaintRequested();
    emit failed(reason);
}

void ScreenInterpolationController::setPaused(bool paused)
{
    if (paused_ == paused) return;
    paused_ = paused;
    // Paused: mpv draws its current frame itself. Resumed: start from fresh
    // frames, never interpolating across the pause.
    resetTimeline();
}

void ScreenInterpolationController::resetTimeline()
{
    ++generation_;
    workerGeneration_.store(generation_);
    {
        std::lock_guard lock(slotMutex_);
        for (Slot &slot : slots_) slot.state = Slot::State::Free;
    }
    queue_.clear();
    presentTimer_.stop();
    shownSlot_ = -1;
    lastInput_ = {};
    lastCaptureNs_ = 0;
    gridNs_ = 0;
    epochValid_ = false;
    shownContent_ = -1.0;
    lastPaintedTick_ = lastSwappedTick_ = -1;
    lastSwapNs_ = 0;
    swapIntervalsMs_.clear();
    sourceToSwapMs_.clear();
    // Latency is re-measured from fresh samples (seek/pause outliers).
    processingSamplesMs_.clear();
    outputsSeen_ = 0;
    if (enabled_ && worker_.joinable()) {
        Job job;
        job.kind = Job::Kind::Flush;
        job.generation = generation_;
        post(std::move(job));
    }
    emit repaintRequested();
}

void ScreenInterpolationController::setSourceFps(double fps, double speed)
{
    const double previousRate = 1e9 / sourceIntervalNs();
    fps_ = fps;
    speed_ = speed > 0 ? speed : 1.0;
    if (enabled_ && std::abs((1e9 / sourceIntervalNs()) - previousRate) > 0.01) {
        configuredSize_ = QSize();
        pendingSize_ = QSize();
        requestedSize_ = QSize();
    }
}

void ScreenInterpolationController::setTargetFps(double fps)
{
    const double next = fps > 0 ? fps : 0.0;
    if (std::abs(targetFps_ - next) > 0.01 && enabled_) {
        configuredSize_ = QSize();
        pendingSize_ = QSize();
        requestedSize_ = QSize();
    }
    targetFps_ = next;
    fastRateScale_ = 1.0;
    fastRateLowSeconds_ = fastRateRecoverySeconds_ = 0;
    emit repaintRequested(); // the next captured frame reconfigures if needed
}

double ScreenInterpolationController::outputRate() const
{
    const double source = 1e9 / sourceIntervalNs();
    const double requested = targetFps_ > 0 ? targetFps_ : source * 2.0;
    if (backend_ == Backend::GenericD3D11 && generic_
        && generic_->motion() == GenericD3D11Fruc::Motion::RifeReusable) {
        const QScreen *screen = QGuiApplication::primaryScreen();
        const double refresh = screen && screen->refreshRate() >= 30.0 ? screen->refreshRate() : requested;
        return std::max(source, std::min(requested, refresh) * fastRateScale_);
    }
    return requested;
}

int ScreenInterpolationController::stagesNeeded() const
{
    if (backend_ == Backend::GenericD3D11) return 1;
    if (targetFps_ <= 0) return 1;
    const double ratio = targetFps_ / (1e9 / sourceIntervalNs());
    const double exact = std::log2(ratio);
    // A power-of-two ratio maps 1:1 onto the FRC grid. Other ratios pick
    // frames from a grid at least 3x the target so consecutive frames step
    // almost evenly in time (e.g. 24 -> 60 fps uses the 192 fps grid).
    const bool powerOfTwo = std::abs(exact - std::round(exact)) < 0.01;
    const double wanted = powerOfTwo ? std::round(exact) : std::log2(ratio * 3.0);
    return std::clamp(int(std::ceil(wanted - 1e-6)), 1, AmfFrcInterpolator::kMaxStages);
}

qint64 ScreenInterpolationController::intendedNs(double content) const
{
    const qint64 n = qint64(std::floor(content));
    if (n <= 0 || n > seq_ || seq_ - n >= kCaptureRing - 1) return 0;
    const qint64 t0 = captureTimes_[size_t(n % kCaptureRing)];
    const double f = content - double(n);
    if (f < 1e-9) return t0;
    const qint64 t1 = n + 1 <= seq_ ? captureTimes_[size_t((n + 1) % kCaptureRing)] : t0 + qint64(sourceIntervalNs());
    return t0 + qint64(f * double(t1 - t0));
}

void ScreenInterpolationController::freeSlot(int slot)
{
    std::lock_guard lock(slotMutex_);
    if (slot >= 0 && slot < int(slots_.size()) && slot != shownSlot_) slots_[size_t(slot)].state = Slot::State::Free;
}

double ScreenInterpolationController::sourceIntervalNs() const
{
    if (fps_ > 1.0) return 1e9 / (fps_ * speed_);
    if (measuredIntervalNs_ > 0) return measuredIntervalNs_;
    return 1e9 / 24.0;
}

// ---- GL side ------------------------------------------------------------------------

bool ScreenInterpolationController::createSlots(int width, int height, QString *error)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = backend_ == Backend::GenericD3D11 ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                   : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    std::lock_guard lock(slotMutex_);
    for (int i = 0; i < slotCount_; ++i) {
        Slot slot;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr) || !interop_->textureFor(slot.texture, error)) {
            if (SUCCEEDED(hr) && error && error->isEmpty()) *error = QStringLiteral("display texture registration failed");
            if (FAILED(hr) && error) *error = QStringLiteral("CreateTexture2D failed (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
            if (slot.texture) slot.texture->Release();
            return false;
        }
        slots_.push_back(slot);
    }
    return true;
}

void ScreenInterpolationController::destroySlots()
{
    std::lock_guard lock(slotMutex_);
    for (Slot &slot : slots_) {
        if (interop_) interop_->release(slot.texture);
        slot.texture->Release();
    }
    slots_.clear();
    shownSlot_ = -1;
}

void ScreenInterpolationController::releaseGl()
{
    lastInput_ = {};
    destroySlots();
    if (interop_) interop_->close();
    glContext_ = nullptr;
}

QSize ScreenInterpolationController::inputSizeFor(const QSize &widgetSize) const
{
    if (!sourceSizedInput_ || sourceSize_.isEmpty() || widgetSize.isEmpty()) return widgetSize;
    // Scale the whole frame so the aspect-fitted video inside it gets its
    // decoded pixel size (fit scale s = min(W/vw, H/vh), so k = 1/s); never
    // below the on-screen size, never above D3D11's 8192 limit.
    double k = std::max(1.0, std::max(double(sourceSize_.width()) / widgetSize.width(),
                                      double(sourceSize_.height()) / widgetSize.height()));
    k = std::min(k, 8192.0 / std::max(widgetSize.width(), widgetSize.height()));
    return QSize(int(std::lround(widgetSize.width() * k / 2) * 2), int(std::lround(widgetSize.height() * k / 2) * 2));
}

bool ScreenInterpolationController::captureFrame(QOpenGLContext *context, const QSize &widgetPhysicalSize, qreal dpr,
                                                 const std::function<void(unsigned int, int, int)> &render)
{
    if (!enabled_ || paused_ || widgetPhysicalSize.isEmpty()) return false;
    widgetSize_ = widgetPhysicalSize;
    // The web chrome can report its final rectangle after the first decoded
    // frame. A temporary Qt minimum (100x30) must not scale a 4K source into
    // a pathological 7200x2160 FRC surface before the layout arrives.
    if (widgetPhysicalSize.width() < 320 || widgetPhysicalSize.height() < 180) return false;
    const QSize physicalSize = inputSizeFor(widgetPhysicalSize);
    devicePixelRatio_ = dpr;
    QString error;
    if (!ensureGl(context, &error)) {
        fail(error);
        return false;
    }

    // A new output rate (target or source fps) needs another cascade depth:
    // reconfigure right away at the current size.
    const int stages = stagesNeeded();
    if (!configuring_ && configuredSize_.isValid() && physicalSize == configuredSize_ && stages != configuredStages_) {
        configuredSize_ = QSize();
        pendingSize_ = physicalSize;
        requestedSize_ = physicalSize;
    }
    // Size changes: draw normally until the geometry settles, then
    // reconfigure FRC for the new native size.
    if (physicalSize != configuredSize_ && !configuring_) {
        if (physicalSize != pendingSize_) {
            pendingSize_ = physicalSize;
            requestedSize_ = QSize();
            resizeDebounce_.start(configuredSize_.isEmpty() ? 0 : kResizeSettleMs);
            return false;
        }
        if (requestedSize_ != physicalSize) return false; // still settling
        // Reconfigure: registrations of the old AMF pool textures are
        // dropped with the interop device; the worker re-inits FRC.
        resetTimeline();
        destroySlots();
        interop_->close();
        // Display ring: frames buffered over the pipeline latency at the
        // output rate, plus one source frame of outputs and headroom.
        slotCount_ = std::clamp(int(std::ceil(0.12 * outputRate())) + (1 << stages) + 4, 6, 32);
        if (!ensureGl(context, &error) || !createSlots(physicalSize.width(), physicalSize.height(), &error)) {
            fail(error);
            return false;
        }
        configuring_ = true;
        Job job;
        job.kind = Job::Kind::Configure;
        job.width = physicalSize.width();
        job.height = physicalSize.height();
        job.stages = stages;
        job.sourceFps = 1e9 / sourceIntervalNs();
        job.outputFps = outputRate();
        // Output ticks in content units, fixed with the backend's grid (the
        // measured source interval keeps moving when container fps is unknown).
        pendingContentPerTick_ = job.sourceFps / job.outputFps;
        job.generation = generation_;
        post(std::move(job));
        return false;
    }
    if (configuring_) return false;

    {
        std::lock_guard lock(jobMutex_);
        if (jobs_.size() >= size_t(kMaxPendingFrames)) {
            // The worker cannot keep up: the watchdog decides, keep playing.
            ++counters_.late;
            ++counters_.lateQueueFull;
            return false;
        }
    }

    FrameInterpolator *backend = activeInterpolator();
    GpuFrame surface = backend ? backend->allocateInput(&error) : GpuFrame{};
    if (!surface.texture || !surface.owner) {
        fail(error);
        return false;
    }
    auto *texture = surface.texture;
    const WglDxInterop::Texture *view = interop_->textureFor(texture, &error);
    const qint64 t0 = nowNs();
    if (!view || !interop_->lock(view)) {
        fail(error.isEmpty() ? QStringLiteral("wglDXLockObjectsNV failed") : error);
        return false;
    }
    const qint64 t1 = nowNs();
    render(view->glFramebuffer, physicalSize.width(), physicalSize.height());
    const qint64 t2 = nowNs();
    interop_->unlock(view);
    const qint64 t3 = nowNs();
    pushSample(captureLockMs_, double(t1 - t0) / kMs);
    pushSample(captureRenderMs_, double(t2 - t1) / kMs);
    pushSample(captureUnlockMs_, double(t3 - t2) / kMs);

    const qint64 now = nowNs();
    if (lastCaptureNs_ > 0) {
        const double interval = double(now - lastCaptureNs_);
        if (interval > 0 && interval < 2e8) {
            measuredIntervalNs_ = measuredIntervalNs_ > 0 ? measuredIntervalNs_ * 0.9 + interval * 0.1 : interval;
        }
    }
    lastCaptureNs_ = now;
    ++seq_;
    // mpv shows source frames on a steady grid, but the moment LAMBDA captures
    // each one jitters with decode and GUI load (bursty on high-bitrate 4K).
    // Presenting from raw capture instants turned that jitter into tick
    // collisions and "late" drops, so intended times follow a phase-locked
    // grid; skipped source frames advance it by whole intervals and a larger
    // disturbance (seek, stall) re-anchors it.
    qint64 intended = now;
    if (gridNs_ > 0) {
        const double interval = sourceIntervalNs();
        const double steps = std::max(1.0, std::round(double(now - gridNs_) / interval));
        const qint64 predicted = gridNs_ + qint64(steps * interval);
        const qint64 error = now - predicted;
        if (steps <= 4.0 && std::llabs(error) < qint64(interval * 0.75)) {
            intended = predicted + qint64(double(error) * kGridPhaseGain);
        }
    }
    pushSample(captureJitterMs_, double(std::llabs(now - intended)) / kMs);
    gridNs_ = intended;
    captureTimes_[size_t(seq_ % kCaptureRing)] = intended;
    ++counters_.captures;
    if (shownSlot_ < 0 && queue_.empty()) {
        lastInput_ = surface; // until FRC output arrives (start, seek, resume)
    }

    Job job;
    job.kind = Job::Kind::Frame;
    job.surface = surface;
    job.seq = seq_;
    job.captureNs = now;
    job.generation = generation_;
    post(std::move(job));
    return true;
}

bool ScreenInterpolationController::paint(unsigned int targetFbo, const QSize &physicalSize)
{
    if (!enabled_) {
        if (!slots_.empty() || (interop_ && interop_->isOpen())) releaseGl();
        return false;
    }
    if (paused_) return false;

    ID3D11Texture2D *texture = nullptr;
    {
        std::lock_guard lock(slotMutex_);
        if (shownSlot_ >= 0 && shownSlot_ < int(slots_.size())) texture = slots_[size_t(shownSlot_)].texture;
    }
    if (!texture) texture = lastInput_.texture;
    if (!texture || !interop_ || !interop_->isOpen()) return false;

    QString error;
    const qint64 paintStart = nowNs();
    const WglDxInterop::Texture *view = interop_->textureFor(texture, &error);
    if (!view || !interop_->lock(view)) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    auto *gl = glContext_->extraFunctions();
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, view->glFramebuffer);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFbo);
    const bool sameSize = int(desc.Width) == physicalSize.width() && int(desc.Height) == physicalSize.height();
    gl->glBlitFramebuffer(0, 0, GLint(desc.Width), GLint(desc.Height), 0, 0, physicalSize.width(), physicalSize.height(),
                          GL_COLOR_BUFFER_BIT, sameSize ? GL_NEAREST : GL_LINEAR);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    interop_->unlock(view);
    pushSample(paintMs_, double(nowNs() - paintStart) / kMs);
    lastPaintedTick_ = lastPresentedTick_;
    return true;
}

void ScreenInterpolationController::frameSwapped()
{
    if (!enabled_ || paused_ || lastPaintedTick_ < 0) return;
    if (lastPaintedTick_ <= lastSwappedTick_) {
        ++repeatedSwaps_;
        return;
    }
    const qint64 now = nowNs();
    if (lastSwapNs_ > 0) pushSample(swapIntervalsMs_, double(now - lastSwapNs_) / kMs);
    const qint64 intended = intendedNs(shownContent_);
    if (intended > 0) pushSample(sourceToSwapMs_, double(now - intended) / kMs);
    lastSwapNs_ = now;
    lastSwappedTick_ = lastPaintedTick_;
    ++swappedFrames_;
}

// ---- worker -------------------------------------------------------------------------

void ScreenInterpolationController::startWorker()
{
    if (worker_.joinable()) return;
    {
        std::lock_guard lock(jobMutex_);
        jobs_.clear();
    }
    workerGeneration_.store(generation_);
    worker_ = std::thread([this] { workerLoop(); });
}

void ScreenInterpolationController::stopWorker()
{
    if (!worker_.joinable()) return;
    {
        std::lock_guard lock(jobMutex_);
        jobs_.clear();
        Job quit;
        quit.kind = Job::Kind::Quit;
        jobs_.push_back(std::move(quit));
    }
    jobCv_.notify_one();
    worker_.join();
}

void ScreenInterpolationController::post(Job job)
{
    {
        std::lock_guard lock(jobMutex_);
        jobs_.push_back(std::move(job));
    }
    jobCv_.notify_one();
}

void ScreenInterpolationController::workerLoop()
{
    SetThreadDescription(GetCurrentThread(), L"LAMBDA AMF FRC");
    while (true) {
        Job job;
        {
            std::unique_lock lock(jobMutex_);
            jobCv_.wait(lock, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (job.kind == Job::Kind::Quit) break;

        if (job.kind == Job::Kind::Configure) {
            QString error;
            FrameInterpolator *backend = activeInterpolator();
            bool ok = false;
            if (backend == generic_.get()) {
                ok = generic_->initialize(job.width, job.height, settings_, job.stages,
                                          job.sourceFps, job.outputFps, &error);
            } else if (backend) {
                ok = backend->initialize(job.width, job.height, settings_, job.stages, &error);
            }
            QMetaObject::invokeMethod(this, [this, ok, error, w = job.width, h = job.height, st = job.stages, g = job.generation] {
                onConfigured(ok, error, w, h, st, g);
            }, Qt::QueuedConnection);
            continue;
        }
        if (job.kind == Job::Kind::Flush) {
            if (FrameInterpolator *backend = activeInterpolator()) backend->flush();
            continue;
        }
        if (job.generation != workerGeneration_.load()) continue; // before a seek/flush

        QString error;
        FrameInterpolator *backend = activeInterpolator();
        const qint64 workStart = clock_.nsecsElapsed();
        struct ReadyOutput { int slot; double content; qint64 readyNs; int generation; };
        std::vector<ReadyOutput> readyOutputs;
        const bool ok = backend && backend->processFrame(job.surface, double(job.seq), [&](const InterpolatedFrame &output) {
            int slotIndex = -1;
            {
                std::lock_guard lock(slotMutex_);
                if (job.generation != workerGeneration_.load()) return;
                for (size_t i = 0; i < slots_.size(); ++i) {
                    if (slots_[i].state == Slot::State::Free) {
                        slotIndex = int(i);
                        slots_[i].state = Slot::State::Queued;
                        break;
                    }
                }
                if (slotIndex < 0) {
                    ++droppedOutputs_;
                    return;
                }
                // One GPU copy per output: FRC's pooled surface -> display slot.
                deviceContext_->CopyResource(slots_[size_t(slotIndex)].texture, output.frame.texture);
            }
            const qint64 readyNs = clock_.nsecsElapsed();
            ++outputs_;
            if (output.content != std::floor(output.content)) ++generated_;
            readyOutputs.push_back({slotIndex, output.content, readyNs, job.generation});
        }, &error);
        // Submit all output copies together, then publish them to the GUI.
        // This keeps a queued callback from presenting a texture before its
        // copy has been submitted and avoids one D3D flush per 240 Hz frame.
        if (!readyOutputs.empty()) deviceContext_->Flush();
        for (const ReadyOutput &ready : readyOutputs) {
            QMetaObject::invokeMethod(this, [this, ready] {
                onOutput(ready.slot, ready.content, ready.readyNs, ready.generation);
            }, Qt::QueuedConnection);
        }
        const double workMs = double(clock_.nsecsElapsed() - workStart) / kMs;
        workerMs_.store(workMs);
        if (workMs > workerMaxMs_.load()) workerMaxMs_.store(workMs);
        ++submitted_;
        lastSubmitStatus_ = backend ? backend->lastSubmitStatus() : 0;
        lastQueryStatus_ = backend ? backend->lastQueryStatus() : 0;
        if (!ok) {
            const QString name = backend ? backend->name() : QStringLiteral("Frame interpolation");
            QMetaObject::invokeMethod(this, [this, name, error] { fail(name + QStringLiteral(" stopped: ") + error); },
                                      Qt::QueuedConnection);
        }
    }
}

// ---- results / presentation ---------------------------------------------------------

void ScreenInterpolationController::onConfigured(bool ok, const QString &error, int width, int height, int stages,
                                                 int generation)
{
    if (!enabled_) return;
    configuring_ = false;
    if (!ok) {
        fail(error);
        return;
    }
    Q_UNUSED(generation);
    configuredSize_ = QSize(width, height);
    configuredStages_ = stages;
    contentPerTick_ = pendingContentPerTick_;
    latencyNs_ = 0; // re-measured for this cascade depth
    processingSamplesMs_.clear();
    pendingSize_ = configuredSize_;
    requestedSize_ = configuredSize_;
    emit repaintRequested();
}

double ScreenInterpolationController::percentile(std::vector<double> samples, double p)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    return samples[size_t(double(samples.size() - 1) * p)];
}

void ScreenInterpolationController::pushSample(std::vector<double> &samples, double value)
{
    samples.push_back(value);
    if (samples.size() > 240) samples.erase(samples.begin(), samples.begin() + 48);
}

void ScreenInterpolationController::onOutput(int slot, double content, qint64 readyNs, int generation)
{
    if (!enabled_ || generation != generation_ || paused_) {
        freeSlot(slot);
        return;
    }
    const qint64 now = nowNs();
    pushSample(dispatchSamplesMs_, double(now - readyNs) / kMs);
    const qint64 intended = intendedNs(content);
    if (intended <= 0) {
        freeSlot(slot);
        return;
    }
    const double interval = sourceIntervalNs();
    const int stages = std::max(1, configuredStages_);
    ++outputsSeen_;
    lastProcessMs_ = double(readyNs - intended) / kMs;

    // L follows the 99th percentile of (available on the GUI thread -
    // intended time) + 3 ms, measured after a warm-up; until then the FRC hold
    // plus one source frame and a processing margin. Every change of L shifts
    // all later presentation times and re-syncs mpv's audio (a visible hitch),
    // so L is sticky: it rises at once with headroom when frames would be
    // late, and falls only after staying well above need for 15 s.
    pushSample(processingSamplesMs_, double(now - intended) / kMs);
    if (outputsSeen_ > (qint64(8) << stages) && processingSamplesMs_.size() >= 24) {
        const double measured = (percentile(processingSamplesMs_, 0.99) + 3.0) * kMs;
        constexpr double kHeadroomNs = 6.0 * kMs;
        const double previous = latencyNs_;
        if (latencyNs_ <= 0 || measured > latencyNs_) {
            latencyNs_ = measured + kHeadroomNs;
            latencyLowSinceNs_ = 0;
        } else if (measured < latencyNs_ - 20.0 * kMs) {
            if (latencyLowSinceNs_ == 0) latencyLowSinceNs_ = now;
            else if (now - latencyLowSinceNs_ > 15000 * kMs) {
                latencyNs_ = measured + kHeadroomNs;
                latencyLowSinceNs_ = 0;
            }
        } else {
            latencyLowSinceNs_ = 0;
        }
        if (latencyNs_ != previous) ++latencyChanges_;
    }
    const double hold = settings_.useFutureFrame ? 2.0 : 1.0;
    const double latency = latencyNs_ > 0 ? latencyNs_ : (hold + 1.0) * interval + 10.0 * kMs;

    Presentation p;
    p.slot = slot;
    p.content = content;
    p.wallNs = intended + qint64(latency);
    lagFrames_ = qint64(std::llround(hold));
    lastLagMs_ = latency / kMs;
    if (latencyNs_ > 0 && counters_.presented > qint64(outputRate() * 2)) applyAudioDelay(latency / 1e9); // after ~2 s

    // Presentation clock at the output rate. Output frames are evenly spaced in
    // content (source-frame) time, so each frame's tick follows from its
    // content alone and two frames never compete for one tick through wall
    // clock rounding. The tick clock follows the phase-locked source grid and
    // the measured latency - never frame arrival times, which on bursty
    // high-bitrate 4K would drag the clock and cascade into more late frames.
    tickPeriodNs_ = 1e9 / outputRate();
    const double contentPerTick = contentPerTick_ > 0.0 ? contentPerTick_ : tickPeriodNs_ / interval;
    if (!epochValid_) {
        // Output contents are origin + k * step; ticks count from the origin
        // so rounding never maps two outputs onto one tick.
        epochValid_ = true;
        outputsSinceEpoch_ = 0;
        tickOriginContent_ = content;
        lastPresentedTick_ = -1;
    }
    p.tick = qint64(std::llround((content - tickOriginContent_) / contentPerTick));
    // The source grid is already phase-locked: the tick clock snaps to latency
    // changes and absorbs the grid's small per-frame corrections gradually.
    const qint64 epochTarget = p.wallNs - qint64(double(p.tick) * tickPeriodNs_);
    const qint64 drift = epochTarget - epochNs_;
    epochNs_ = (outputsSinceEpoch_++ == 0 || std::llabs(drift) > 2 * kMs) ? epochTarget
                                                                           : epochNs_ + qint64(double(drift) * 0.1);
    if (p.tick <= lastPresentedTick_ || content <= shownContent_) {
        ++counters_.late;
        if (p.tick <= lastPresentedTick_) ++counters_.lateTick;
        else ++counters_.lateContent;
        freeSlot(slot);
        return;
    }
    // Grids finer than the target (AMF cascades): keep the content nearest
    // to the tick.
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->tick != p.tick) continue;
        ++counters_.skipped;
        const double target = double(p.tick) * contentPerTick;
        if (std::abs(it->content - target) <= std::abs(content - target)) {
            freeSlot(slot);
            return;
        }
        freeSlot(it->slot);
        queue_.erase(it);
        break;
    }
    auto pos = std::upper_bound(queue_.begin(), queue_.end(), p.tick,
                                [](qint64 tick, const Presentation &item) { return tick < item.tick; });
    queue_.insert(pos, p);
    schedule();
}

void ScreenInterpolationController::schedule()
{
    if (!epochValid_ || queue_.empty()) {
        presentTimer_.stop();
        return;
    }
    const qint64 waitNs = tickTime(queue_.front().tick) - nowNs();
    // Do not post a chain of zero-delay timers during the fractional part of
    // a 240 Hz tick. One precise millisecond timer is cheaper and lets Qt
    // compose the frame already selected for this refresh.
    presentTimer_.start(waitNs <= 0 ? 0 : int(std::max<qint64>(1, (waitNs + kMs - 1) / kMs)));
}

void ScreenInterpolationController::presentDue()
{
    if (queue_.empty()) return;
    const qint64 now = nowNs();
    const qint64 currentTick = qint64(std::floor(double(now + kMs - epochNs_) / tickPeriodNs_));
    if (queue_.front().tick > currentTick) {
        schedule(); // woke up early
        return;
    }
    pushSample(timerLateSamplesMs_, double(now - tickTime(queue_.front().tick)) / kMs);

    // Show the newest frame that is due; older due frames were overtaken
    // (GUI stall) and are dropped.
    int chosen = -1;
    Presentation shown;
    while (!queue_.empty() && queue_.front().tick <= currentTick) {
        if (chosen >= 0) {
            ++counters_.droppedPresentations;
            freeSlot(chosen);
        }
        shown = queue_.front();
        chosen = shown.slot;
        queue_.pop_front();
    }
    if (chosen >= 0) {
        {
            std::lock_guard lock(slotMutex_);
            if (shownSlot_ >= 0 && shownSlot_ < int(slots_.size())) slots_[size_t(shownSlot_)].state = Slot::State::Free;
            shownSlot_ = chosen;
            slots_[size_t(chosen)].state = Slot::State::Shown;
        }
        lastInput_ = {};
        lastPresentedTick_ = shown.tick;
        shownContent_ = shown.content;
        ++counters_.presented;
        ++windowPresented_;
        if (shown.content != std::floor(shown.content)) ++counters_.presentedGenerated;
        emit repaintRequested();
    }
    schedule();
}

// ---- audio --------------------------------------------------------------------------

void ScreenInterpolationController::applyAudioDelay(double lagSeconds)
{
    if (!mpv_) return;
    if (!audioDelayApplied_) {
        double current = 0.0;
        mpv_get_property(mpv_, "audio-delay", MPV_FORMAT_DOUBLE, &current);
        originalAudioDelay_ = current;
        audioDelayApplied_ = true;
        appliedLagSeconds_ = -1.0;
    }
    // Re-apply only on a meaningful change (each change resyncs audio).
    if (std::abs(lagSeconds - appliedLagSeconds_) < 0.003) return;
    appliedLagSeconds_ = lagSeconds;
    ++audioDelayChanges_;
    double value = originalAudioDelay_ + lagSeconds;
    mpv_set_property(mpv_, "audio-delay", MPV_FORMAT_DOUBLE, &value);
}

void ScreenInterpolationController::restoreAudioDelay()
{
    if (!audioDelayApplied_ || !mpv_) return;
    double value = originalAudioDelay_;
    mpv_set_property(mpv_, "audio-delay", MPV_FORMAT_DOUBLE, &value);
    audioDelayApplied_ = false;
    appliedLagSeconds_ = 0.0;
}

// ---- stats / watchdog ---------------------------------------------------------------

void ScreenInterpolationController::logStats()
{
    const qint64 now = nowNs();
    const double seconds = std::max(0.001, double(now - rates_.atNs) / 1e9);
    const qint64 submitted = submitted_.load(), outputs = outputs_.load(), generated = generated_.load();
    rates_.capturesPerSec = (counters_.captures - rates_.captures) / seconds;
    rates_.submittedPerSec = (submitted - rates_.submitted) / seconds;
    rates_.outputsPerSec = (outputs - rates_.outputs) / seconds;
    rates_.generatedPerSec = (generated - rates_.generated) / seconds;
    rates_.presentedPerSec = (counters_.presented - rates_.presented) / seconds;
    rates_.swappedPerSec = (swappedFrames_ - rates_.swapped) / seconds;
    rates_.repeatedPerSec = (repeatedSwaps_ - rates_.repeated) / seconds;
    const qint64 droppedOutputCount = droppedOutputs_.load();
    rates_.droppedPerSec = ((droppedOutputCount - rates_.droppedOutputs)
        + (counters_.droppedPresentations - rates_.droppedPresentations)) / seconds;
    rates_.captures = counters_.captures;
    rates_.submitted = submitted;
    rates_.outputs = outputs;
    rates_.generated = generated;
    rates_.presented = counters_.presented;
    rates_.swapped = swappedFrames_;
    rates_.repeated = repeatedSwaps_;
    rates_.droppedOutputs = droppedOutputCount;
    rates_.droppedPresentations = counters_.droppedPresentations;
    rates_.atNs = now;
    const double peakWorkMs = workerMaxMs_.exchange(0.0);
    lastWorkerPeakMs_ = peakWorkMs;

    // Watchdog: sustained failure to present about twice the source rate
    // returns to native playback (the resolution never changes).
    const bool running = enabled_ && !paused_ && !configuring_ && configuredSize_.isValid() && rates_.capturesPerSec > 5;
    const double expected = outputRate();
    const double displayCeiling = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->refreshRate() : expected;
    const double visibleTarget = std::min(expected, displayCeiling);
    const bool validVideoArea = widgetSize_.width() > 320 && widgetSize_.height() > 180;
    const bool swappedSlow = validVideoArea && swappedFrames_ > 0 && rates_.swappedPerSec < visibleTarget * 0.65;
    const bool overloaded = running && (rates_.presentedPerSec < expected * 0.75 || swappedSlow
        || counters_.late > windowLate_ + expected * 0.25);
    windowLate_ = counters_.late;
    windowPresented_ = 0;
    overloadedSeconds_ = overloaded ? overloadedSeconds_ + 1 : 0;

    // The quality slider is a ceiling: sustained GPU pressure lowers only
    // the motion-analysis resolution. Recover conservatively to avoid a
    // reinitialization oscillation during a difficult scene.
    const bool fast = running && backend_ == Backend::GenericD3D11 && generic_
        && generic_->motion() == GenericD3D11Fruc::Motion::RifeReusable
        && widgetSize_.width() > 320 && widgetSize_.height() > 180;
    if (fast) {
        const double budgetMs = sourceIntervalNs() / kMs;
        const bool pressure = peakWorkMs > budgetMs * 0.88
                           || (swappedFrames_ > 0 && rates_.swappedPerSec < visibleTarget * 0.8);
        fastOverloadSeconds_ = pressure ? fastOverloadSeconds_ + 1 : 0;
        fastRecoverySeconds_ = pressure ? 0 : fastRecoverySeconds_ + 1;
        int nextQuality = effectiveFastQuality_;
        if (fastOverloadSeconds_ >= 3 && nextQuality > 0) --nextQuality;
        else if (fastRecoverySeconds_ >= 15 && nextQuality < fastQuality_) ++nextQuality;
        if (nextQuality != effectiveFastQuality_) {
            setEnabled(false); // join worker before changing its model geometry
            effectiveFastQuality_ = nextQuality;
            fastOverloadSeconds_ = fastRecoverySeconds_ = 0;
            overloadedSeconds_ = 0;
            generic_->setAnalysisHeight(nextQuality == 0 ? 360 : nextQuality == 2 ? 720 : 540);
            generic_->setFastQuality(nextQuality);
            configuredSize_ = QSize();
            setEnabled(true);
        }
        // Once the cheaper analysis setting is exhausted, reduce the number
        // of synthesized frames instead of repeatedly overflowing the display
        // ring. Recover only after an extended period of clean, cheap swaps.
        const bool presentationPressure = swappedSlow || rates_.droppedPerSec > visibleTarget * 0.1;
        fastRateLowSeconds_ = effectiveFastQuality_ == 0 && presentationPressure
            ? fastRateLowSeconds_ + 1 : 0;
        fastRateRecoverySeconds_ = fastRateScale_ < 1.0
            && !presentationPressure && rates_.swappedPerSec >= visibleTarget * 0.95
            && peakWorkMs < budgetMs * 0.65 ? fastRateRecoverySeconds_ + 1 : 0;
        double nextScale = fastRateScale_;
        if (fastRateLowSeconds_ >= 3 && nextScale > 0.5) nextScale = 0.5;
        else if (fastRateRecoverySeconds_ >= 20 && nextScale < 1.0) nextScale = 1.0;
        if (nextScale != fastRateScale_) {
            fastRateScale_ = nextScale;
            fastRateLowSeconds_ = fastRateRecoverySeconds_ = overloadedSeconds_ = 0;
            configuredSize_ = pendingSize_ = requestedSize_ = QSize();
            resetTimeline();
        }
    } else {
        fastOverloadSeconds_ = fastRecoverySeconds_ = 0;
    }

    if (!logPath_.isEmpty()) {
        QFile file(logPath_);
        if (file.open(QIODevice::Append)) {
            file.write(QJsonDocument(stats()).toJson(QJsonDocument::Compact) + '\n');
        }
    }
    emit diagnosticsUpdated(stats());
    if (overloadedSeconds_ >= (fast ? 8 : 4)) {
        overloadedSeconds_ = 0;
        fail(QStringLiteral("%1 could not keep up (%2 of %3 frames/s presented)")
                 .arg(activeInterpolator() ? activeInterpolator()->name() : QStringLiteral("Frame interpolation"))
                 .arg(rates_.presentedPerSec, 0, 'f', 1).arg(expected, 0, 'f', 1));
    }
}

QJsonObject ScreenInterpolationController::stats() const
{
    QJsonObject o;
    o.insert("available", isAvailable(backend_));
    o.insert("amdAmfAvailable", amfAvailable_);
    o.insert("genericD3d11Available", genericAvailable_);
    o.insert("backend", activeInterpolator() ? activeInterpolator()->name() : QString());
    o.insert("fastQuality", fastQuality_);
    o.insert("effectiveFastQuality", effectiveFastQuality_);
    o.insert("fastRateScale", fastRateScale_);
    o.insert("unavailableReason", unavailableReason(backend_));
    o.insert("enabled", enabled_);
    o.insert("paused", paused_);
    o.insert("adapter", adapterName_);
    o.insert("d3d11VideoProcessorFrcAdvertised", videoProcessorProbe_.frameRateConversion);
    o.insert("d3d11VideoProcessorBGRAInput", videoProcessorProbe_.bgraInput);
    o.insert("d3d11VideoProcessorBGRAOutput", videoProcessorProbe_.bgraOutput);
    o.insert("d3d11VideoProcessorCreated", videoProcessorProbe_.processorCreated);
    o.insert("d3d11VideoProcessorStreamRateConfigured", videoProcessorProbe_.streamRateConfigured);
    o.insert("d3d11VideoProcessorRateModeCount", int(videoProcessorProbe_.rateConversionModeCount));
    o.insert("d3d11VideoProcessorRateModeIndex", int(videoProcessorProbe_.selectedRateConversionIndex));
    o.insert("d3d11VideoProcessorPastFrames", int(videoProcessorProbe_.pastFrames));
    o.insert("d3d11VideoProcessorFutureFrames", int(videoProcessorProbe_.futureFrames));
    o.insert("d3d11VideoProcessorTestWidth", int(videoProcessorProbe_.testedWidth));
    o.insert("d3d11VideoProcessorTestHeight", int(videoProcessorProbe_.testedHeight));
    o.insert("d3d11VideoProcessorProbeReason", videoProcessorProbe_.reason);
    o.insert("amfRuntime", amf_ ? amf_->runtimeVersion() : QString());
    o.insert("genericGpuExecutionMs", generic_ ? generic_->lastGpuExecutionMs() : 0.0);
    o.insert("genericCpuSubmitMs", generic_ ? generic_->lastCpuSubmitMs() : 0.0);
    if (generic_) {
        const auto motion = generic_->motion();
        const bool rife = motion != GenericD3D11Fruc::Motion::Block;
        o.insert("genericMotion", motion == GenericD3D11Fruc::Motion::RifeReusable
                                      ? "RIFE reusable midpoint flow" : rife ? "RIFE flownet" : "block matching");
        o.insert("rifeModel", rife ? generic_->modelName() : QString());
        o.insert("fastStaticPairs", generic_->skippedStaticPairs());
        o.insert("fastCutPairs", generic_->skippedCutPairs());
        o.insert("fastRefinedPairs", generic_->refinedPairs());
        o.insert("rifeAnalysisWidth", generic_->analysisSize().width());
        o.insert("rifeAnalysisHeight", generic_->analysisSize().height());
        o.insert("genericSeparateComputeDevice", generic_->separateComputeDevice());
    }
    o.insert("capture", "mpv render output -> D3D11 texture (WGL_NV_DX_interop2)");
    o.insert("format", backend_ == Backend::GenericD3D11 ? "RGBA8 (same 8-bit output; BGRA inputs swizzled on GPU)"
                                                          : "BGRA8 (same 8-bit output as the normal path)");
    o.insert("inputWidth", configuredSize_.width());
    o.insert("inputHeight", configuredSize_.height());
    o.insert("devicePixelRatio", devicePixelRatio_);
    o.insert("videoAreaWidth", widgetSize_.width());
    o.insert("videoAreaHeight", widgetSize_.height());
    o.insert("sourceWidth", sourceSize_.width());
    o.insert("sourceHeight", sourceSize_.height());
    o.insert("inputMode", sourceSizedInput_ ? "source (decoded resolution, scaled for display)" : "on-screen pixels");
    o.insert("nativeSourceResolution", configuredSize_.isValid() && sourceSize_.isValid()
                                           && configuredSize_.width() >= sourceSize_.width()
                                           && configuredSize_.height() >= sourceSize_.height());
    if (QScreen *screen = QGuiApplication::primaryScreen()) o.insert("primaryRefreshHz", screen->refreshRate());
    o.insert("frcProfile", settings_.profile == FRC_PROFILE_SUPER ? "SUPER" : settings_.profile == FRC_PROFILE_HIGH ? "HIGH" : "LOW");
    o.insert("frcSearch", settings_.searchMode == FRC_MV_SEARCH_NATIVE ? "NATIVE" : "PERFORMANCE");
    o.insert("frcFutureFrame", settings_.useFutureFrame);
    o.insert("frcFallbackBlend", settings_.fallbackBlend);
    o.insert("sourceFps", fps_ * speed_);
    o.insert("measuredSourceFps", measuredIntervalNs_ > 0 ? 1e9 / measuredIntervalNs_ : 0.0);
    o.insert("capturesPerSec", rates_.capturesPerSec);
    o.insert("submittedPerSec", rates_.submittedPerSec);
    o.insert("outputsPerSec", rates_.outputsPerSec);
    o.insert("generatedPerSec", rates_.generatedPerSec);
    o.insert("presentedPerSec", rates_.presentedPerSec);
    o.insert("swappedPerSec", rates_.swappedPerSec);
    o.insert("repeatedSwapsPerSec", rates_.repeatedPerSec);
    o.insert("droppedPerSec", rates_.droppedPerSec);
    o.insert("swapIntervalP95Ms", percentile(swapIntervalsMs_, 0.95));
    o.insert("sourceToSwapP95Ms", percentile(sourceToSwapMs_, 0.95));
    o.insert("swappedFrames", swappedFrames_);
    o.insert("captures", counters_.captures);
    o.insert("submitted", submitted_.load());
    o.insert("outputs", outputs_.load());
    o.insert("generated", generated_.load());
    o.insert("presented", counters_.presented);
    o.insert("presentedGenerated", counters_.presentedGenerated);
    o.insert("late", counters_.late);
    o.insert("lateStale", counters_.lateStale);
    o.insert("lateTick", counters_.lateTick);
    o.insert("lateContent", counters_.lateContent);
    o.insert("lateQueueFull", counters_.lateQueueFull);
    o.insert("droppedPresentations", counters_.droppedPresentations);
    o.insert("droppedOutputs", droppedOutputs_.load());
    o.insert("presentQueue", int(queue_.size()));
    o.insert("lastSubmitStatus", lastSubmitStatus_.load());
    o.insert("lastQueryStatus", lastQueryStatus_.load());
    o.insert("processingMs", lastProcessMs_.load());
    o.insert("workerMs", workerMs_.load());
    o.insert("workerMaxMs", lastWorkerPeakMs_);
    o.insert("targetFps", targetFps_ > 0 ? targetFps_ : outputRate());
    o.insert("outputRate", outputRate());
    o.insert("frcStages", configuredStages_);
    o.insert("gridFps", backend_ == Backend::GenericD3D11
                            ? outputRate()
                            : (1e9 / sourceIntervalNs()) * double(1 << std::max(0, configuredStages_)));
    o.insert("displaySlots", slotCount_);
    o.insert("skipped", counters_.skipped);
    o.insert("latencyMs", latencyNs_ / kMs);
    o.insert("latencyChanges", latencyChanges_);
    o.insert("audioDelayChanges", audioDelayChanges_);
    o.insert("intendedToAvailableP95Ms", percentile(processingSamplesMs_, 0.95));
    o.insert("guiDispatchP50Ms", percentile(dispatchSamplesMs_, 0.5));
    o.insert("guiDispatchP95Ms", percentile(dispatchSamplesMs_, 0.95));
    o.insert("timerLateP50Ms", percentile(timerLateSamplesMs_, 0.5));
    o.insert("timerLateP95Ms", percentile(timerLateSamplesMs_, 0.95));
    o.insert("captureLockP95Ms", percentile(captureLockMs_, 0.95));
    o.insert("captureJitterP95Ms", percentile(captureJitterMs_, 0.95));
    o.insert("captureRenderP50Ms", percentile(captureRenderMs_, 0.5));
    o.insert("captureRenderP95Ms", percentile(captureRenderMs_, 0.95));
    o.insert("captureUnlockP95Ms", percentile(captureUnlockMs_, 0.95));
    o.insert("paintP50Ms", percentile(paintMs_, 0.5));
    o.insert("paintP95Ms", percentile(paintMs_, 0.95));
    o.insert("guiLatencyP50Ms", percentile(guiLatencyMs_, 0.5));
    o.insert("guiLatencyP95Ms", percentile(guiLatencyMs_, 0.95));
    o.insert("guiLatencyMaxMs", percentile(guiLatencyMs_, 1.0));
    o.insert("frcLagSourceFrames", lagFrames_);
    o.insert("visualLagMs", lastLagMs_);
    o.insert("audioDelayCompensationMs", audioDelayApplied_ ? appliedLagSeconds_ * 1000.0 : 0.0);
    o.insert("originalAudioDelayMs", originalAudioDelay_ * 1000.0);
    return o;
}

} // namespace frc
