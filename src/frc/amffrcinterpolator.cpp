#include "frc/amffrcinterpolator.h"

#include <d3d11.h>

#include <algorithm>
#include <thread>

namespace frc {

namespace {

QString resultText(AMF_RESULT res)
{
    return QStringLiteral("AMF error %1").arg(int(res));
}

GpuFrame frameFor(amf::AMFSurfacePtr surface)
{
    if (!surface || !surface->GetPlaneAt(0)) return {};
    auto *texture = static_cast<ID3D11Texture2D *>(surface->GetPlaneAt(0)->GetNative());
    auto owner = std::make_shared<amf::AMFSurfacePtr>(std::move(surface));
    return {texture, std::move(owner)};
}

} // namespace

AmfFrcInterpolator::~AmfFrcInterpolator()
{
    close();
}

bool AmfFrcInterpolator::open(ID3D11Device *device, QString *error)
{
    if (context_) {
        return true;
    }
    AMF_RESULT res = g_AMFFactory.Init();
    if (res != AMF_OK) {
        if (error) *error = QStringLiteral("AMD AMF runtime (amfrt64.dll) is not available: %1").arg(resultText(res));
        return false;
    }
    factoryInitialized_ = true;
    const amf_uint64 version = g_AMFFactory.AMFQueryVersion();
    runtimeVersion_ = QStringLiteral("%1.%2.%3.%4")
                          .arg(AMF_GET_MAJOR_VERSION(version))
                          .arg(AMF_GET_MINOR_VERSION(version))
                          .arg(AMF_GET_SUBMINOR_VERSION(version))
                          .arg(AMF_GET_BUILD_VERSION(version));

    res = g_AMFFactory.GetFactory()->CreateContext(&context_);
    if (res == AMF_OK) {
        res = context_->InitDX11(device);
    }
    if (res != AMF_OK) {
        if (error) *error = QStringLiteral("AMF could not use the Direct3D 11 device: %1").arg(resultText(res));
        close();
        return false;
    }
    // Probe the component so "unavailable" is known before playback uses it.
    probeComponent_ = createComponent(error);
    if (!probeComponent_) {
        close();
        return false;
    }
    probeComponent_ = nullptr;
    return true;
}

amf::AMFComponentPtr AmfFrcInterpolator::createComponent(QString *error)
{
    amf::AMFComponentPtr component;
    const AMF_RESULT res = g_AMFFactory.GetFactory()->CreateComponent(context_, AMFFRC, &component);
    if (res != AMF_OK) {
        if (error) *error = QStringLiteral("AMF Frame Rate Conversion is not supported by this GPU/driver: %1").arg(resultText(res));
        return nullptr;
    }
    return component;
}

bool AmfFrcInterpolator::init(int width, int height, const FrcSettings &settings, int stages, QString *error)
{
    terminate();
    if (!context_) {
        if (error) *error = QStringLiteral("AMF is not open");
        return false;
    }
    stages = std::clamp(stages, 1, kMaxStages);
    for (int i = 0; i < stages; ++i) {
        amf::AMFComponentPtr component = createComponent(error);
        if (!component) {
            terminate();
            return false;
        }
        // SimpleFRC's configuration order.
        component->SetProperty(AMF_FRC_ENGINE_TYPE, amf_int64(FRC_ENGINE_DX11));
        component->SetProperty(AMF_FRC_MODE, amf_int64(FRC_x2_PRESENT));
        component->SetProperty(AMF_FRC_ENABLE_FALLBACK, settings.fallbackBlend);
        component->SetProperty(AMF_FRC_INDICATOR, false);
        component->SetProperty(AMF_FRC_PROFILE, settings.profile);
        component->SetProperty(AMF_FRC_MV_SEARCH_MODE, settings.searchMode);
        component->SetProperty(AMF_FRC_USE_FUTURE_FRAME, settings.useFutureFrame);
        const AMF_RESULT res = component->Init(amf::AMF_SURFACE_BGRA, width, height);
        if (res != AMF_OK) {
            if (error) *error = QStringLiteral("AMF FRC could not start at %1x%2: %3").arg(width).arg(height).arg(resultText(res));
            terminate();
            return false;
        }
        frc_.push_back(component);
    }
    history_.assign(frc_.size(), {});
    width_ = width;
    height_ = height;
    settings_ = settings;
    return true;
}

void AmfFrcInterpolator::terminate()
{
    for (amf::AMFComponentPtr &component : frc_) {
        component->Terminate();
    }
    frc_.clear();
    history_.clear();
    width_ = height_ = 0;
}

void AmfFrcInterpolator::close()
{
    terminate();
    probeComponent_ = nullptr;
    // SimpleFRC: components first, context last.
    if (context_) {
        context_->Terminate();
        context_ = nullptr;
    }
    if (factoryInitialized_) {
        g_AMFFactory.Terminate();
        factoryInitialized_ = false;
    }
}

amf::AMFSurfacePtr AmfFrcInterpolator::allocAmfInput(QString *error)
{
    amf::AMFSurfacePtr surface;
    const AMF_RESULT res = context_->AllocSurface(amf::AMF_MEMORY_DX11, amf::AMF_SURFACE_BGRA, width_, height_, &surface);
    if (res != AMF_OK && error) {
        *error = QStringLiteral("AllocSurface failed: %1").arg(resultText(res));
    }
    return surface;
}

GpuFrame AmfFrcInterpolator::allocateInput(QString *error)
{
    return frameFor(allocAmfInput(error));
}

void AmfFrcInterpolator::drainStage(size_t stage, const std::function<void(const InterpolatedFrame &)> &onOutput, QString *error,
                                    bool *ok)
{
    const std::deque<double> &history = history_[stage];
    const int lag = settings_.useFutureFrame ? 2 : 1;
    // Content of input "i back from the latest" (clamped at the first one).
    auto back = [&history](int i) {
        return history.empty() ? 0.0 : history[size_t(std::max(0, int(history.size()) - 1 - i))];
    };
    while (*ok) {
        amf::AMFSurfacePtr out;
        const AMF_RESULT res = frc_[stage]->QueryOutput(reinterpret_cast<amf::AMFData **>(&out));
        lastQuery_ = res;
        if (res == AMF_EOF) break;
        if (res != AMF_OK && res != AMF_REPEAT) break;
        if (!out) break;
        double content = back(lag);
        if (res == AMF_REPEAT && int(history.size()) > lag + 1) {
            content = (back(lag) + back(lag + 1)) / 2.0; // generated midpoint
        }
        if (stage + 1 < frc_.size()) {
            *ok = submitStage(stage + 1, out, content, onOutput, error);
        } else {
            onOutput(InterpolatedFrame{frameFor(out), content});
        }
    }
}

bool AmfFrcInterpolator::submitStage(size_t stage, const amf::AMFSurfacePtr &input, double content,
                                     const std::function<void(const InterpolatedFrame &)> &onOutput, QString *error)
{
    bool ok = true;
    for (int attempt = 0; attempt < 200 && ok; ++attempt) {
        const AMF_RESULT res = frc_[stage]->SubmitInput(input);
        if (stage == 0) lastSubmit_ = res;
        if (res == AMF_NEED_MORE_INPUT || res == AMF_INPUT_FULL) {
            // Keep the surface and submit it again after collecting output
            // (SimpleFRC). Yield instead of spinning.
            drainStage(stage, onOutput, error, &ok);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        if (res != AMF_OK) {
            if (error) *error = QStringLiteral("SubmitInput failed (stage %1): %2").arg(stage + 1).arg(resultText(res));
            return false;
        }
        std::deque<double> &history = history_[stage];
        history.push_back(content);
        if (history.size() > 8) history.pop_front();
        drainStage(stage, onOutput, error, &ok);
        return ok;
    }
    if (ok && error) *error = QStringLiteral("AMF FRC stayed full for 100 ms (stage %1)").arg(stage + 1);
    return false;
}

bool AmfFrcInterpolator::processAmf(const amf::AMFSurfacePtr &input, double content,
                                    const std::function<void(const InterpolatedFrame &)> &onOutput, QString *error)
{
    if (frc_.empty()) {
        if (error) *error = QStringLiteral("FRC is not initialized");
        return false;
    }
    return submitStage(0, input, content, onOutput, error);
}

bool AmfFrcInterpolator::processFrame(const GpuFrame &input, double content,
                                      const OutputCallback &onOutput, QString *error)
{
    if (!input.texture || !input.owner) {
        if (error) *error = QStringLiteral("AMF input frame has no owned texture");
        return false;
    }
    const auto surface = std::static_pointer_cast<amf::AMFSurfacePtr>(input.owner);
    if (!surface || !*surface) {
        if (error) *error = QStringLiteral("AMF input surface expired before processing");
        return false;
    }
    (*surface)->SetPts(amf_pts(content));
    return processAmf(*surface, content, onOutput, error);
}

void AmfFrcInterpolator::flush()
{
    for (amf::AMFComponentPtr &component : frc_) {
        component->Flush();
    }
    for (std::deque<double> &history : history_) {
        history.clear();
    }
}

} // namespace frc
