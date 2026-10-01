#include "frc/genericd3d11fruc.h"

#include "frc/computeshaders.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace frc {

namespace {

// Motion is searched at a video-like 640x360 ceiling. The final warp still
// samples the untouched output-resolution frames, so this does not downscale
// the displayed image.
constexpr UINT kDefaultMotionWidth = 640;
constexpr UINT kBlockSize = 6;
constexpr UINT kSearchRadius = 6;
// RIFE analysis height (source frames keep their full resolution; only the
// flownet sees this size). On 4K test frames 720 scored within 0.1 dB of
// 1080 at about half the GPU time, and native-size analysis did not fix the
// large-motion failures (docs/frc/UNIVERSAL_FRC_EXPERIMENT.md).
// LAMBDA_RIFE_ANALYSIS_HEIGHT overrides it.
constexpr int kDefaultAnalysisHeight = 720;
constexpr int kHighRateAnalysisHeight = 540;
// Broad RGB difference threshold, supplemented by spatial correspondence
// and dark-shot checks. LAMBDA_RIFE_SCENE_THRESHOLD overrides it;
// a value <= 0 disables the check.
constexpr float kDefaultSceneThreshold = 0.1f;
// Preferred models, first found wins. v4.6 (the VapourSynth path's model) is
// deliberately not chosen automatically: in the static-subtitle test it
// doubled the text. LAMBDA_RIFE_MODEL names another directory under
// rife/models, or an absolute model path.
const char *const kPreferredModels[] = {"rife-v4.26_ensembleFalse", "rife-v4.25-lite_ensembleFalse"};
// RIFE dispatches per D3D11 Flush (LAMBDA_RIFE_FLUSH_DISPATCHES, 0 = off).
// Off by default: at 120 fps it did not shorten the GUI thread's interop
// lock wait, which blocks until all queued D3D11 work has completed.
constexpr int kDefaultFlushDispatches = 0;
constexpr size_t kInitialInputPool = 5;
constexpr size_t kMaximumInputPool = 8;

struct MotionParameters
{
    UINT width;
    UINT height;
    UINT motionWidth;
    UINT motionHeight;
    UINT blockSize;
    UINT searchRadius;
};

struct WarpParameters
{
    UINT width;
    UINT height;
    UINT motionWidth;
    UINT motionHeight;
    UINT blockSize;
    float interpolationTime;
    float padding0;
    float padding1;
};

struct DownscaleParameters
{
    UINT inputWidth;
    UINT inputHeight;
    UINT outputWidth;
    UINT outputHeight;
};

struct CleanupParameters
{
    UINT width;
    UINT height;
    float padding0;
    float padding1;
};

// Matches `cbuffer Op` in rife_io.hlsl.
struct IoParameters
{
    uint32_t p[16];
    float f[8];
};

static_assert(sizeof(IoParameters) == 96);
static_assert(sizeof(MotionParameters) == 24);
static_assert(sizeof(WarpParameters) == 32);
static_assert(sizeof(DownscaleParameters) == 16);
static_assert(sizeof(CleanupParameters) == 16);

QString hresultText(const QString &what, HRESULT hr)
{
    return QStringLiteral("%1 failed (0x%2)").arg(what).arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

template<typename T>
void updateConstants(ID3D11DeviceContext *context, ID3D11Buffer *buffer, const T &values)
{
    static_assert(sizeof(T) <= sizeof(WarpParameters));
    std::array<uint32_t, sizeof(WarpParameters) / sizeof(uint32_t)> padded{};
    std::memcpy(padded.data(), &values, sizeof(T));
    context->UpdateSubresource(buffer, 0, nullptr, padded.data(), 0, 0);
}

} // namespace

GenericD3D11Fruc::~GenericD3D11Fruc()
{
    close();
}

bool GenericD3D11Fruc::open(ID3D11Device *device, QString *error)
{
    if (!device) {
        if (error) *error = QStringLiteral("D3D11 device is null");
        return false;
    }
    interopDevice_ = device;
    if (interopDevice_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        if (error) *error = QStringLiteral("Generic FRUC requires D3D11 feature level 11.0");
        close();
        return false;
    }
    interopDevice_->GetImmediateContext(&interopContext_);
    if (!interopContext_) {
        if (error) *error = QStringLiteral("Could not get the D3D11 immediate context");
        close();
        return false;
    }
    device_ = interopDevice_;
    context_ = interopContext_;
    separate_ = false;
    if (preferSeparateDevice_ && qEnvironmentVariable("LAMBDA_GENERIC_FRC_SEPARATE_DEVICE") != QLatin1String("0")) {
        QString why;
        if (!createComputeDevice(&why)) {
            // Shared textures/fences unavailable: fall back to one device.
            qWarning("Generic FRC: separate compute device unavailable (%s)", qPrintable(why));
            device_ = interopDevice_;
            context_ = interopContext_;
            separate_ = false;
        }
    }
    if (!compileShader(":/frc/generic_downscale.hlsl", "main", downscaleShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_motion.hlsl", "main", motionShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_cleanup.hlsl", "main", cleanupShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_warp.hlsl", "main", warpShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_scene.hlsl", "main", sceneShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_copy.hlsl", "main", copyShader_.GetAddressOf(), error)) {
        close();
        return false;
    }
    if (!motionOverridden_) {
        const QString choice = qEnvironmentVariable("LAMBDA_GENERIC_FRC_MOTION");
        motion_ = choice == QLatin1String("block") ? Motion::Block
                : choice == QLatin1String("rife-reuse") ? Motion::RifeReusable : Motion::Rife;
    }
    if (motion_ != Motion::Block && !loadRife(error)) {
        close();
        return false;
    }
    return true;
}

bool GenericD3D11Fruc::createComputeDevice(QString *error)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = interopDevice_.As(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (SUCCEEDED(hr)) {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                               &device, nullptr, &context);
    }
    Microsoft::WRL::ComPtr<ID3D11Device5> compute5;
    Microsoft::WRL::ComPtr<ID3D11Device5> interop5;
    if (SUCCEEDED(hr)) hr = device.As(&compute5);
    if (SUCCEEDED(hr)) hr = interopDevice_.As(&interop5);
    if (SUCCEEDED(hr)) hr = context.As(&computeContext4_);
    if (SUCCEEDED(hr)) hr = interopContext_.As(&interopContext4_);
    auto sharedFence = [](ID3D11Device5 *owner, ID3D11Device5 *other, Microsoft::WRL::ComPtr<ID3D11Fence> *mine,
                          Microsoft::WRL::ComPtr<ID3D11Fence> *theirs) {
        HRESULT result = owner->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(mine->ReleaseAndGetAddressOf()));
        HANDLE handle = nullptr;
        if (SUCCEEDED(result)) result = (*mine)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(result)) result = other->OpenSharedFence(handle, IID_PPV_ARGS(theirs->ReleaseAndGetAddressOf()));
        if (handle) CloseHandle(handle);
        return result;
    };
    if (SUCCEEDED(hr)) hr = sharedFence(interop5.Get(), compute5.Get(), &inputFenceInterop_, &inputFenceCompute_);
    if (SUCCEEDED(hr)) hr = sharedFence(interop5.Get(), compute5.Get(), &copyFenceInterop_, &copyFenceCompute_);
    if (SUCCEEDED(hr)) hr = compute5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&outputFence_));
    if (SUCCEEDED(hr) && !fenceEvent_) {
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("separate compute device"), hr);
        computeContext4_.Reset();
        interopContext4_.Reset();
        inputFenceInterop_.Reset();
        inputFenceCompute_.Reset();
        copyFenceInterop_.Reset();
        copyFenceCompute_.Reset();
        outputFence_.Reset();
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    device_ = device;
    context_ = context;
    separate_ = true;
    return true;
}

bool GenericD3D11Fruc::createSharedTexture(ID3D11Device *owner, ID3D11Device *other, const D3D11_TEXTURE2D_DESC &desc,
                                           Microsoft::WRL::ComPtr<ID3D11Texture2D> *ownerTexture,
                                           Microsoft::WRL::ComPtr<ID3D11Texture2D> *otherTexture, QString *error)
{
    D3D11_TEXTURE2D_DESC shared = desc;
    shared.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = owner->CreateTexture2D(&shared, nullptr, ownerTexture->ReleaseAndGetAddressOf());
    Microsoft::WRL::ComPtr<IDXGIResource1> resource;
    Microsoft::WRL::ComPtr<ID3D11Device1> other1;
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) hr = ownerTexture->As(&resource);
    if (SUCCEEDED(hr)) {
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &handle);
    }
    if (SUCCEEDED(hr)) hr = other->QueryInterface(IID_PPV_ARGS(&other1));
    if (SUCCEEDED(hr)) hr = other1->OpenSharedResource1(handle, IID_PPV_ARGS(otherTexture->ReleaseAndGetAddressOf()));
    if (handle) CloseHandle(handle);
    if (FAILED(hr) && error) *error = hresultText(QStringLiteral("shared texture"), hr);
    return SUCCEEDED(hr);
}

bool GenericD3D11Fruc::createOutputRing(size_t count, QString *error)
{
    outputs_.clear();
    nextOutput_ = 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (size_t i = 0; i < count; ++i) {
        OutputSlot slot;
        HRESULT hr = S_OK;
        if (separate_) {
            if (!createSharedTexture(device_.Get(), interopDevice_.Get(), desc, &slot.compute, &slot.interop, error))
                return false;
        } else {
            hr = device_->CreateTexture2D(&desc, nullptr, &slot.compute);
            slot.interop = slot.compute;
        }
        if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(slot.compute.Get(), nullptr, &slot.uav);
        if (FAILED(hr)) {
            if (error) *error = hresultText(QStringLiteral("CreateTexture2D(output)"), hr);
            return false;
        }
        slot.owner = std::make_shared<InputTexture>();
        slot.owner->texture = slot.interop;
        outputs_.push_back(std::move(slot));
    }
    return true;
}

bool GenericD3D11Fruc::waitForOutput(const OutputSlot &slot, QString *error)
{
    if (!separate_ || outputFence_->GetCompletedValue() >= slot.ready) return true;
    if (SUCCEEDED(outputFence_->SetEventOnCompletion(slot.ready, fenceEvent_))
        && WaitForSingleObject(fenceEvent_, 2000) == WAIT_OBJECT_0) {
        return true;
    }
    if (error) *error = QStringLiteral("GPU did not finish an interpolated frame within 2 s");
    return false;
}

QString GenericD3D11Fruc::name() const
{
    return motion_ == Motion::Block ? QStringLiteral("Generic D3D11 block matching")
         : motion_ == Motion::RifeReusable ? QStringLiteral("Generic D3D11 RIFE reusable flow")
         : QStringLiteral("Generic D3D11 RIFE");
}

bool GenericD3D11Fruc::loadScene(QString *error)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(IoParameters);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT hr = device_->CreateBuffer(&desc, nullptr, &ioConstants_);
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC scene{};
        scene.ByteWidth = 8;
        scene.Usage = D3D11_USAGE_DEFAULT;
        scene.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        scene.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        scene.StructureByteStride = 4;
        const float zero[2] = {0.0f, 0.0f};
        D3D11_SUBRESOURCE_DATA data{&zero, 0, 0};
        hr = device_->CreateBuffer(&scene, &data, &sceneBuffer_);
    }
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(sceneBuffer_.Get(), nullptr, &sceneSrv_);
    if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(sceneBuffer_.Get(), nullptr, &sceneUav_);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateBuffer(RIFE I/O)"), hr);
        return false;
    }
    return true;
}

bool GenericD3D11Fruc::loadRife(QString *error)
{
    if (!compileShader(":/frc/rife_io.hlsl", "prepare", prepareShader_.GetAddressOf(), error)
        || !compileShader(":/frc/rife_io.hlsl", "packMotion", packShader_.GetAddressOf(), error)
        || !compileShader(":/frc/rife_io.hlsl", "synthesize", synthesizeShader_.GetAddressOf(), error)) {
        return false;
    }
    if (rifeLoaded_) return true;

    QString directory = modelDirectory_;
    const QDir models(QCoreApplication::applicationDirPath() + QStringLiteral("/rife/models"));
    if (directory.isEmpty()) {
        const QString requested = qEnvironmentVariable("LAMBDA_RIFE_MODEL");
        if (!requested.isEmpty()) {
            directory = QDir::isAbsolutePath(requested) ? requested : models.filePath(requested);
        } else {
            for (const char *name : kPreferredModels) {
                const QString candidate = models.filePath(QString::fromLatin1(name));
                if (QFile::exists(candidate + QStringLiteral("/flownet.param"))) {
                    directory = candidate;
                    break;
                }
            }
        }
    }
    if (directory.isEmpty()) {
        if (error) *error = QStringLiteral("No RIFE v4 model found in %1").arg(QDir::toNativeSeparators(models.path()));
        return false;
    }
    rifeLoaded_ = rife_.load(directory, error);
    return rifeLoaded_;
}

bool GenericD3D11Fruc::initializeRife(double outputFps, QString *error)
{
    // Universal can be probed without models; switching to neural flow loads
    // its model lazily rather than disabling the cheap backend at startup.
    if (!loadRife(error)) return false;
    bool ok = false;
    int requested = qEnvironmentVariableIntValue("LAMBDA_RIFE_ANALYSIS_HEIGHT", &ok);
    if (analysisHeightOverride_ > 0) requested = analysisHeightOverride_;
    else if (!ok || requested <= 0) {
        // A 4K 30->60 stream also needs 60 full-size syntheses per second.
        // Budget by absolute output rate as well as the number of inferences
        // per source pair; 540p analysis was ~40% cheaper than 720p in the
        // 4K measurements with only a small measured quality difference.
        const bool highRate4k = height_ >= 2000 && outputFps >= 60.0 - 0.01;
        requested = outputStep_ < 0.5 - 1e-6 || highRate4k
            ? kHighRateAnalysisHeight : kDefaultAnalysisHeight;
    }
    // Never analyse above the captured size; only the flownet input shrinks.
    analysisHeight_ = UINT(std::clamp<int>(requested, 64, int(height_)));
    analysisWidth_ = std::max(1u, UINT(std::lround(double(width_) * analysisHeight_ / double(height_))));
    constexpr UINT align = RifeD3D11Network::kAlignment;
    paddedWidth_ = (analysisWidth_ + align - 1) / align * align;
    paddedHeight_ = (analysisHeight_ + align - 1) / align * align;
    if (!rife_.build(device_.Get(), paddedWidth_, paddedHeight_, false, error)) return false;
    bool flushOk = false;
    const int flushInterval = qEnvironmentVariableIntValue("LAMBDA_RIFE_FLUSH_DISPATCHES", &flushOk);
    rife_.setFlushInterval(flushOk ? std::max(0, flushInterval) : kDefaultFlushDispatches);
    // Flow (A.xy, B.xy) and mask at analysis size, sampled bilinearly by the
    // full-resolution synthesis.
    for (int i = 0; i < (motion_ == Motion::RifeReusable && fastQuality_ == 2 ? 4 : 2); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = analysisWidth_;
        desc.Height = analysisHeight_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = i % 2 == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        MotionTexture &target = i == 0 ? rifeMotion_ : i == 1 ? rifeMask_
                               : i == 2 ? rifeMotionBackup_ : rifeMaskBackup_;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &target.texture);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv);
        if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(target.texture.Get(), nullptr, &target.uav);
        if (FAILED(hr)) {
            if (error) *error = hresultText(QStringLiteral("CreateTexture2D(RIFE motion)"), hr);
            return false;
        }
    }
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 2 * sizeof(float);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = device_->CreateBuffer(&desc, nullptr, &sceneReadback_);
        if (FAILED(hr)) {
            if (error) *error = hresultText(QStringLiteral("CreateBuffer(scene readback)"), hr);
            return false;
        }
    }
    return true;
}

void GenericD3D11Fruc::bindIo(ID3D11ComputeShader *shader, const void *constants,
                              std::initializer_list<ID3D11ShaderResourceView *> srvs,
                              ID3D11UnorderedAccessView *uav, UINT gx, UINT gy, UINT gz)
{
    context_->UpdateSubresource(ioConstants_.Get(), 0, nullptr, constants, 0, 0);
    ID3D11ShaderResourceView *views[6] = {};
    size_t count = 0;
    for (ID3D11ShaderResourceView *srv : srvs) views[count++] = srv;
    ID3D11Buffer *constant = ioConstants_.Get();
    ID3D11SamplerState *sampler = linearSampler_.Get();
    ID3D11UnorderedAccessView *nullUav = nullptr;
    ID3D11ShaderResourceView *nullSrvs[6] = {};
    context_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context_->CSSetShader(shader, nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetSamplers(0, 1, &sampler);
    context_->CSSetShaderResources(0, 6, views);
    context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context_->Dispatch(gx, gy, gz);
    context_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context_->CSSetShaderResources(0, 6, nullSrvs);
}

void GenericD3D11Fruc::runPrepare(ID3D11ShaderResourceView *frame, const RifeD3D11Network::TensorView &tensor)
{
    IoParameters k{};
    k.p[0] = width_;
    k.p[1] = height_;
    k.p[2] = analysisWidth_;
    k.p[3] = analysisHeight_;
    k.p[4] = paddedWidth_;
    k.p[5] = paddedHeight_;
    k.p[6] = std::max(1u, UINT(std::ceil(double(height_) / double(analysisHeight_) / 2.0)));
    k.p[7] = tensor.offset;
    bindIo(prepareShader_.Get(), &k, {frame}, tensor.uav, (paddedWidth_ + 7) / 8, (paddedHeight_ + 7) / 8, 1);
}

void GenericD3D11Fruc::runSceneDiff(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current)
{
    IoParameters k{};
    k.f[0] = sceneThreshold_;
    bindIo(sceneShader_.Get(), &k, {previous,current}, sceneUav_.Get(), 1, 1, 1);
}

void GenericD3D11Fruc::packRifeMotion()
{
    const RifeD3D11Network::TensorView flowA = rife_.flowA();
    const RifeD3D11Network::TensorView flowB = rife_.flowB();
    const RifeD3D11Network::TensorView mask = rife_.mask();
    IoParameters pack{};
    pack.p[0] = analysisWidth_;
    pack.p[1] = analysisHeight_;
    pack.p[2] = paddedWidth_;
    pack.p[3] = paddedHeight_;
    pack.p[4] = flowA.offset;
    pack.p[5] = flowB.offset;
    pack.p[6] = mask.offset;
    // Two UAVs: bind the mask texture at u1 around the shared helper.
    ID3D11UnorderedAccessView *maskUav = rifeMask_.uav.Get();
    context_->CSSetUnorderedAccessViews(1, 1, &maskUav, nullptr);
    bindIo(packShader_.Get(), &pack, {flowA.srv, flowB.srv, mask.srv}, rifeMotion_.uav.Get(),
           (analysisWidth_ + 7) / 8, (analysisHeight_ + 7) / 8, 1);
    ID3D11UnorderedAccessView *nullUav = nullptr;
    context_->CSSetUnorderedAccessViews(1, 1, &nullUav, nullptr);
}

void GenericD3D11Fruc::runSynthesize(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current, float t,
                                     bool reusable, bool noMotion)
{
    IoParameters k{};
    k.p[0] = width_;
    k.p[1] = height_;
    k.p[2] = analysisWidth_;
    k.p[3] = analysisHeight_;
    k.p[8] = reusable ? 1u : 0u;
    k.p[9] = sceneThreshold_ > 0.0f ? 1u : 0u;
    k.p[10] = noMotion ? 1u : 0u;
    k.f[0] = t;
    k.f[1] = sceneThreshold_;
    bindIo(synthesizeShader_.Get(), &k, {previous, current, rifeMotion_.srv.Get(), rifeMask_.srv.Get(), sceneSrv_.Get()},
           outputUav_, (width_ + 7) / 8, (height_ + 7) / 8, 1);
}

bool GenericD3D11Fruc::compileShader(const char *resource, const char *entry,
                                    ID3D11ComputeShader **shader, QString *error)
{
    return loadComputeShader(device_.Get(), resource, entry, shader, error);
}

bool GenericD3D11Fruc::initialize(int width, int height, const FrcSettings &settings, int stages, QString *error)
{
    const double sourceFps = 24.0;
    return initialize(width, height, settings, stages, sourceFps, sourceFps * 2.0, error);
}

bool GenericD3D11Fruc::initialize(int width, int height, const FrcSettings &, int stages,
                                  double sourceFps, double outputFps, QString *error)
{
    skippedStaticPairs_.store(0);
    skippedCutPairs_.store(0);
    refinedPairs_.store(0);
    sourceBudgetMs_ = 1000.0 / std::max(1.0, sourceFps);
    terminate();
    if (!device_ || !context_) {
        if (error) *error = QStringLiteral("Generic D3D11 FRUC is not open");
        return false;
    }
    if (stages != 1) {
        if (error) *error = QStringLiteral("Generic D3D11 FRUC uses one motion stage for all output rates");
        return false;
    }
    if (width <= 0 || height <= 0) {
        if (error) *error = QStringLiteral("Invalid FRUC input dimensions");
        return false;
    }
    width_ = UINT(width);
    height_ = UINT(height);
    const double validSourceFps = sourceFps > 0.0 ? sourceFps : 24.0;
    const double validOutputFps = outputFps > validSourceFps ? outputFps : validSourceFps * 2.0;
    outputStep_ = validSourceFps / validOutputFps;
    bool motionWidthOk = false;
    const int configuredMotionWidth = qEnvironmentVariableIntValue("LAMBDA_GENERIC_FRC_MOTION_WIDTH", &motionWidthOk);
    const UINT motionMaxWidth = motionWidthOk
        ? UINT(std::clamp(configuredMotionWidth, 128, int(kDefaultMotionWidth)))
        : kDefaultMotionWidth;
    const UINT motionMaxHeight = (motionMaxWidth * 9 + 8) / 16;
    const double motionScale = std::min({double(motionMaxWidth) / double(width_),
                                         double(motionMaxHeight) / double(height_), 1.0});
    motionWidth_ = std::max(1u, UINT(std::lround(double(width_) * motionScale)));
    motionHeight_ = std::max(1u, UINT(std::lround(double(height_) * motionScale)));
    blocksX_ = (motionWidth_ + kBlockSize - 1) / kBlockSize;
    blocksY_ = (motionHeight_ + kBlockSize - 1) / kBlockSize;

    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(WarpParameters);
    constantsDesc.Usage = D3D11_USAGE_DEFAULT;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT hr = device_->CreateBuffer(&constantsDesc, nullptr, &constants_);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateBuffer(constants)"), hr);
        terminate();
        return false;
    }

    for (GpuTimer &timer : gpuTimers_) {
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        hr = device_->CreateQuery(&queryDesc, &timer.disjoint);
        if (SUCCEEDED(hr)) {
            queryDesc.Query = D3D11_QUERY_TIMESTAMP;
            hr = device_->CreateQuery(&queryDesc, &timer.begin);
        }
        if (SUCCEEDED(hr)) hr = device_->CreateQuery(&queryDesc, &timer.end);
        if (FAILED(hr)) {
            // Timing is diagnostic; the interpolation backend still works on
            // drivers that reject timestamp queries.
            for (GpuTimer &created : gpuTimers_) {
                created.disjoint.Reset();
                created.begin.Reset();
                created.end.Reset();
            }
            break;
        }
    }
    nextGpuTimer_ = 0;

    if (!createFlowTexture(blocksX_, blocksY_, &rawForwardFlow_, error)
        || !createFlowTexture(blocksX_, blocksY_, &forwardFlow_, error)
        || !createFlowTexture(blocksX_, blocksY_, &rawBackwardFlow_, error)
        || !createFlowTexture(blocksX_, blocksY_, &backwardFlow_, error)
        || !createMotionTexture(&motionFrameA_, error)
        || !createMotionTexture(&motionFrameB_, error)) {
        terminate();
        return false;
    }

    // One entry per output a source pair can produce, plus one in flight.
    const size_t ringSize = separate_ ? size_t(std::ceil(1.0 / outputStep_ - 1e-9)) + 2 : 1;
    if (!createOutputRing(ringSize, error)) {
        terminate();
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&samplerDesc, &linearSampler_);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateSamplerState"), hr);
        terminate();
        return false;
    }

    bool thresholdOk = false;
    const double threshold = qEnvironmentVariable("LAMBDA_RIFE_SCENE_THRESHOLD").toDouble(&thresholdOk);
    sceneThreshold_ = thresholdOk ? float(threshold) : kDefaultSceneThreshold;
    if (!loadScene(error)) { terminate(); return false; }
    if (motion_ != Motion::Block) {
        if (!initializeRife(validOutputFps, error)) {
            terminate();
            return false;
        }
    }

    for (size_t i = 0; i < kInitialInputPool; ++i) {
        std::shared_ptr<InputTexture> input;
        if (!createInputTexture(&input, error)) {
            terminate();
            return false;
        }
        inputPool_.push_back(std::move(input));
    }
    initialized_ = true;
    return true;
}

bool GenericD3D11Fruc::createFlowTexture(UINT blocksX, UINT blocksY, FlowTexture *flow, QString *error)
{
    if (blocksX == 0 || blocksY == 0) {
        if (error) *error = QStringLiteral("Invalid motion-vector field dimensions");
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = blocksX;
    desc.Height = blocksY;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &flow->texture);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateTexture2D(motion field)"), hr);
        return false;
    }
    hr = device_->CreateShaderResourceView(flow->texture.Get(), nullptr, &flow->srv);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateShaderResourceView(motion field)"), hr);
        return false;
    }
    hr = device_->CreateUnorderedAccessView(flow->texture.Get(), nullptr, &flow->uav);
    if (FAILED(hr) && error) *error = hresultText(QStringLiteral("CreateUnorderedAccessView(motion field)"), hr);
    return SUCCEEDED(hr);
}

bool GenericD3D11Fruc::createMotionTexture(MotionTexture *texture, QString *error)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = motionWidth_;
    desc.Height = motionHeight_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture->texture);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texture->texture.Get(), nullptr, &texture->srv);
    if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(texture->texture.Get(), nullptr, &texture->uav);
    if (FAILED(hr) && error) *error = hresultText(QStringLiteral("CreateTexture2D(reduced motion frame)"), hr);
    return SUCCEEDED(hr);
}

bool GenericD3D11Fruc::createInputTexture(std::shared_ptr<InputTexture> *texture, QString *error)
{
    auto input = std::make_shared<InputTexture>();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = S_OK;
    if (separate_) {
        if (!createSharedTexture(interopDevice_.Get(), device_.Get(), desc, &input->texture, &input->computeTexture,
                                 error)) {
            return false;
        }
    } else {
        hr = device_->CreateTexture2D(&desc, nullptr, &input->texture);
        input->computeTexture = input->texture;
    }
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(input->computeTexture.Get(), nullptr, &input->srv);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateTexture2D(input)"), hr);
        return false;
    }
    *texture = std::move(input);
    return true;
}

GpuFrame GenericD3D11Fruc::allocateInput(QString *error)
{
    if (!initialized_) {
        if (error) *error = QStringLiteral("Generic D3D11 FRUC is not initialized");
        return {};
    }
    for (const auto &input : inputPool_) {
        if (input.use_count() == 1) return {input->texture.Get(), input};
    }
    if (inputPool_.size() < kMaximumInputPool) {
        std::shared_ptr<InputTexture> input;
        if (!createInputTexture(&input, error)) return {};
        inputPool_.push_back(input);
        return {input->texture.Get(), std::move(input)};
    }
    if (error) *error = QStringLiteral("Generic FRUC input queue is full");
    return {};
}

bool GenericD3D11Fruc::runDownscale(ID3D11ShaderResourceView *source,
                                    ID3D11UnorderedAccessView *destination, QString *error)
{
    const DownscaleParameters values{width_, height_, motionWidth_, motionHeight_};
    updateConstants(context_.Get(), constants_.Get(), values);
    ID3D11ShaderResourceView *srvs[] = {source};
    ID3D11UnorderedAccessView *uavs[] = {destination};
    ID3D11Buffer *constant = constants_.Get();
    ID3D11SamplerState *sampler = linearSampler_.Get();
    context_->CSSetShader(downscaleShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetSamplers(0, 1, &sampler);
    context_->CSSetShaderResources(0, 1, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((motionWidth_ + 7) / 8, (motionHeight_ + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[] = {nullptr};
    ID3D11UnorderedAccessView *nullUavs[] = {nullptr};
    context_->CSSetShaderResources(0, 1, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    Q_UNUSED(error);
    return true;
}

bool GenericD3D11Fruc::runMotion(ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                                 ID3D11UnorderedAccessView *flow, QString *error)
{
    const MotionParameters values{motionWidth_, motionHeight_, motionWidth_, motionHeight_,
                                  kBlockSize, kSearchRadius};
    updateConstants(context_.Get(), constants_.Get(), values);
    ID3D11ShaderResourceView *srvs[] = {a, b};
    ID3D11UnorderedAccessView *uavs[] = {flow};
    ID3D11Buffer *constant = constants_.Get();
    context_->CSSetShader(motionShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetShaderResources(0, 2, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((blocksX_ + 7) / 8, (blocksY_ + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[] = {nullptr, nullptr};
    ID3D11UnorderedAccessView *nullUavs[] = {nullptr};
    context_->CSSetShaderResources(0, 2, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    Q_UNUSED(error);
    return true;
}

bool GenericD3D11Fruc::runFlowCleanup(ID3D11ShaderResourceView *source,
                                      ID3D11UnorderedAccessView *destination, QString *error)
{
    const CleanupParameters values{blocksX_, blocksY_, 0.0f, 0.0f};
    updateConstants(context_.Get(), constants_.Get(), values);
    ID3D11ShaderResourceView *srvs[] = {source};
    ID3D11UnorderedAccessView *uavs[] = {destination};
    ID3D11Buffer *constant = constants_.Get();
    context_->CSSetShader(cleanupShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetShaderResources(0, 1, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((blocksX_ + 7) / 8, (blocksY_ + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[] = {nullptr};
    ID3D11UnorderedAccessView *nullUavs[] = {nullptr};
    context_->CSSetShaderResources(0, 1, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    Q_UNUSED(error);
    return true;
}

bool GenericD3D11Fruc::runWarp(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current,
                               float interpolationTime, QString *error)
{
    const WarpParameters values{width_, height_, motionWidth_, motionHeight_, kBlockSize,
                                interpolationTime, sceneThreshold_ > 0.0f ? 1.0f : 0.0f, 0.0f};
    updateConstants(context_.Get(), constants_.Get(), values);
    ID3D11ShaderResourceView *srvs[] = {previous, current, forwardFlow_.srv.Get(), backwardFlow_.srv.Get(), sceneSrv_.Get()};
    ID3D11UnorderedAccessView *uavs[] = {outputUav_};
    ID3D11Buffer *constant = constants_.Get();
    ID3D11SamplerState *sampler = linearSampler_.Get();
    context_->CSSetShader(warpShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetSamplers(0, 1, &sampler);
    context_->CSSetShaderResources(0, 5, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView *nullUavs[] = {nullptr};
    context_->CSSetShaderResources(0, 5, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    Q_UNUSED(error);
    return true;
}

bool GenericD3D11Fruc::runCopy(ID3D11ShaderResourceView *source, QString *error)
{
    const WarpParameters values{width_, height_, motionWidth_, motionHeight_, kBlockSize, 0.0f, 0.0f, 0.0f};
    updateConstants(context_.Get(), constants_.Get(), values);
    ID3D11ShaderResourceView *srvs[] = {source};
    ID3D11UnorderedAccessView *uavs[] = {outputUav_};
    ID3D11Buffer *constant = constants_.Get();
    context_->CSSetShader(copyShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetShaderResources(0, 1, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[] = {nullptr};
    ID3D11UnorderedAccessView *nullUavs[] = {nullptr};
    context_->CSSetShaderResources(0, 1, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    Q_UNUSED(error);
    return true;
}

bool GenericD3D11Fruc::processFrame(const GpuFrame &input, double content,
                                   const OutputCallback &onOutput, QString *error)
{
    if (!initialized_ || !input.texture || !input.owner) {
        if (error) *error = QStringLiteral("Generic FRUC received an invalid input frame");
        return false;
    }
    auto current = std::static_pointer_cast<InputTexture>(input.owner);
    if (!previous_.texture) {
        previous_ = input;
        previousContent_ = content;
        nextOutputContent_ = content + outputStep_;
        return true;
    }
    auto previous = std::static_pointer_cast<InputTexture>(previous_.owner);
    if (!previous || !current) {
        if (error) *error = QStringLiteral("Generic FRUC frame ownership expired");
        return false;
    }
    const double contentSpan = content - previousContent_;
    if (contentSpan <= 0.0) {
        previous_ = input;
        previousContent_ = content;
        return true;
    }
    constexpr double kContentEpsilon = 1e-7;
    bool needsMotion = false;
    for (double outputContent = nextOutputContent_; outputContent <= content + kContentEpsilon;
         outputContent += outputStep_) {
        if (outputContent < content - kContentEpsilon) {
            needsMotion = true;
            break;
        }
    }

    pollGpuTimers();
    GpuTimer *gpuTimer = nullptr;
    for (size_t i = 0; i < gpuTimers_.size(); ++i) {
        const size_t index = (nextGpuTimer_ + i) % gpuTimers_.size();
        GpuTimer &candidate = gpuTimers_[index];
        if (candidate.disjoint && candidate.begin && candidate.end && !candidate.pending) {
            gpuTimer = &candidate;
            nextGpuTimer_ = (index + 1) % gpuTimers_.size();
            break;
        }
    }
    if (gpuTimer && (needsMotion || nextOutputContent_ <= content + kContentEpsilon)) {
        context_->Begin(gpuTimer->disjoint.Get());
        context_->End(gpuTimer->begin.Get());
    } else {
        gpuTimer = nullptr;
    }
    QElapsedTimer timer;
    timer.start();
    if (separate_) {
        // The capture was rendered through the interop device: order the
        // compute device reads after it on the GPU.
        interopContext4_->Signal(inputFenceInterop_.Get(), ++inputValue_);
        interopContext_->Flush();
        computeContext4_->Wait(inputFenceCompute_.Get(), inputValue_);
    }
    // Outputs are handed to the controller only once the compute device has
    // finished them (CPU wait on this worker thread), so the controller copy
    // into a display slot never queues behind inference work on the device
    // that the OpenGL present waits for.
    struct Pending
    {
        OutputSlot *slot;
        double content;
    };
    std::vector<Pending> pending;
    auto emitPending = [&]() {
        if (pending.empty()) return true;
        context_->Flush();
        for (const Pending &item : pending) {
            if (!waitForOutput(*item.slot, error)) return false;
            onOutput(InterpolatedFrame{GpuFrame{item.slot->interop.Get(), item.slot->owner}, item.content});
            interopContext4_->Signal(copyFenceInterop_.Get(), ++copyValue_);
            item.slot->copied = copyValue_;
        }
        pending.clear();
        return true;
    };
    const bool rife = motion_ != Motion::Block;
    bool refinePair = false;
    bool skipMotion = false;
    if (needsMotion) runSceneDiff(previous->srv.Get(), current->srv.Get());
    if (needsMotion && rife) {
        stamp("start");
        runPrepare(previous->srv.Get(), rife_.input(0));
        runPrepare(current->srv.Get(), rife_.input(1));
        stamp("prepare+scene");
        if (sceneReadback_) {
            context_->CopyResource(sceneReadback_.Get(), sceneBuffer_.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(sceneReadback_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                const float difference = *static_cast<const float *>(mapped.pData);
                const bool staticPair = difference < 0.0025f;
                const bool cutPair = static_cast<const float *>(mapped.pData)[1] > 0.5f;
                skipMotion = staticPair || cutPair;
                if (staticPair) ++skippedStaticPairs_;
                else if (cutPair) ++skippedCutPairs_;
                refinePair = motion_ == Motion::RifeReusable && !skipMotion && fastQuality_ == 2 && difference > 0.025f
                    && lastGpuExecutionMs_.load() < sourceBudgetMs_ * 0.7;
                context_->Unmap(sceneReadback_.Get(), 0);
            }
        }
        if (motion_ == Motion::RifeReusable && !skipMotion) {
            rife_.run(context_.Get(), 0.5f);
            stamp("network");
            packRifeMotion();
            stamp("pack motion");
            if (refinePair) {
                context_->CopyResource(rifeMotionBackup_.texture.Get(), rifeMotion_.texture.Get());
                context_->CopyResource(rifeMaskBackup_.texture.Get(), rifeMask_.texture.Get());
            }
        }
    }
    if (needsMotion && !rife
        && (!runDownscale(previous->srv.Get(), motionFrameA_.uav.Get(), error)
            || !runDownscale(current->srv.Get(), motionFrameB_.uav.Get(), error)
            || !runMotion(motionFrameA_.srv.Get(), motionFrameB_.srv.Get(), rawForwardFlow_.uav.Get(), error)
            || !runMotion(motionFrameB_.srv.Get(), motionFrameA_.srv.Get(), rawBackwardFlow_.uav.Get(), error)
            || !runFlowCleanup(rawForwardFlow_.srv.Get(), forwardFlow_.uav.Get(), error)
            || !runFlowCleanup(rawBackwardFlow_.srv.Get(), backwardFlow_.uav.Get(), error))) {
        if (gpuTimer) {
            context_->End(gpuTimer->end.Get());
            context_->End(gpuTimer->disjoint.Get());
            gpuTimer->pending = true;
        }
        return false;
    }
    while (nextOutputContent_ <= content + kContentEpsilon) {
        const double outputContent = nextOutputContent_;
        const double localTime = (outputContent - previousContent_) / contentSpan;
        const bool atCurrent = localTime >= 1.0 - kContentEpsilon;
        OutputSlot &slot = outputs_[nextOutput_];
        nextOutput_ = (nextOutput_ + 1) % outputs_.size();
        if (slot.ready > slot.copied && !emitPending()) return false;
        if (separate_ && slot.copied > 0) computeContext4_->Wait(copyFenceCompute_.Get(), slot.copied);
        outputUav_ = slot.uav.Get();
        bool rendered = true;
        if (atCurrent) {
            rendered = runCopy(current->srv.Get(), error);
            stamp("copy source");
        } else if (rife) {
            // Exact RIFE runs per timestep; fast mode reuses the midpoint
            // flow for every requested output in this source-frame pair.
            if (motion_ == Motion::Rife && !skipMotion) {
                rife_.run(context_.Get(), float(localTime));
                stamp("network");
                packRifeMotion();
                stamp("pack motion");
            }
            const double refineTime = (qint64(std::llround(previousContent_)) & 1) ? 0.8 : 0.2;
            const bool exact = refinePair && std::abs(localTime - refineTime) < outputStep_ * 0.51;
            if (exact) {
                rife_.run(context_.Get(), float(localTime));
                packRifeMotion();
                ++refinedPairs_;
                stamp("selective exact");
            }
            runSynthesize(previous->srv.Get(), current->srv.Get(), float(localTime),
                          !exact && motion_ == Motion::RifeReusable, skipMotion);
            stamp("synthesize");
            if (exact) {
                context_->CopyResource(rifeMotion_.texture.Get(), rifeMotionBackup_.texture.Get());
                context_->CopyResource(rifeMask_.texture.Get(), rifeMaskBackup_.texture.Get());
            }
        } else {
            rendered = runWarp(previous->srv.Get(), current->srv.Get(), float(localTime), error);
        }
        if (!rendered) {
            if (gpuTimer) {
                context_->End(gpuTimer->end.Get());
                context_->End(gpuTimer->disjoint.Get());
                gpuTimer->pending = true;
            }
            return false;
        }
        const double frameContent = atCurrent ? content : outputContent;
        if (separate_) {
            computeContext4_->Signal(outputFence_.Get(), ++outputValue_);
            slot.ready = outputValue_;
            pending.push_back({&slot, frameContent});
        } else {
            onOutput(InterpolatedFrame{GpuFrame{slot.interop.Get(), slot.owner}, frameContent});
        }
        nextOutputContent_ += outputStep_;
    }
    lastCpuSubmitMs_.store(double(timer.nsecsElapsed()) / 1000000.0);
    if (gpuTimer) {
        context_->End(gpuTimer->end.Get());
        context_->End(gpuTimer->disjoint.Get());
        gpuTimer->pending = true;
    }
    if (!emitPending()) return false;
    previous_ = input;
    previousContent_ = content;
    return true;
}

void GenericD3D11Fruc::stamp(const char *label)
{
    if (!stageTiming_) return;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    if (stageQueries_.empty()) {
        D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        device_->CreateQuery(&disjoint, &stageDisjoint_);
        context_->Begin(stageDisjoint_.Get());
    }
    device_->CreateQuery(&desc, &query);
    context_->End(query.Get());
    stageQueries_.push_back({QString::fromLatin1(label), query});
}

std::vector<std::pair<QString, double>> GenericD3D11Fruc::takeStageTimes()
{
    std::vector<std::pair<QString, double>> times;
    if (stageQueries_.empty()) return times;
    context_->End(stageDisjoint_.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
    while (context_->GetData(stageDisjoint_.Get(), &data, sizeof(data), 0) != S_OK) {}
    UINT64 previous = 0;
    for (size_t i = 0; i < stageQueries_.size(); ++i) {
        UINT64 tick = 0;
        while (context_->GetData(stageQueries_[i].second.Get(), &tick, sizeof(tick), 0) != S_OK) {}
        if (i > 0) times.push_back({stageQueries_[i].first, double(tick - previous) * 1000.0 / double(data.Frequency)});
        previous = tick;
    }
    stageQueries_.clear();
    return times;
}

void GenericD3D11Fruc::pollGpuTimers()
{
    for (GpuTimer &timer : gpuTimers_) {
        if (!timer.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0;
        UINT64 end = 0;
        if (context_->GetData(timer.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH)
                != S_OK
            || context_->GetData(timer.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context_->GetData(timer.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        if (!disjoint.Disjoint && disjoint.Frequency > 0 && end >= begin) {
            const double milliseconds = (double(end - begin) * 1000.0) / double(disjoint.Frequency);
            lastGpuExecutionMs_.store(milliseconds);
        }
        timer.pending = false;
    }
}

void GenericD3D11Fruc::flush()
{
    previous_ = {};
    previousContent_ = 0.0;
    nextOutputContent_ = 0.0;
}

void GenericD3D11Fruc::terminate()
{
    initialized_ = false;
    previous_ = {};
    inputPool_.clear();
    for (GpuTimer &timer : gpuTimers_) {
        timer.pending = false;
        timer.disjoint.Reset();
        timer.begin.Reset();
        timer.end.Reset();
    }
    nextGpuTimer_ = 0;
    outputs_.clear();
    nextOutput_ = 0;
    outputUav_ = nullptr;
    rawForwardFlow_ = {};
    forwardFlow_ = {};
    rawBackwardFlow_ = {};
    backwardFlow_ = {};
    motionFrameA_ = {};
    motionFrameB_ = {};
    rife_.release();
    rifeMotion_ = {};
    rifeMask_ = {};
    rifeMotionBackup_ = {};
    rifeMaskBackup_ = {};
    sceneReadback_.Reset();
    analysisWidth_ = analysisHeight_ = paddedWidth_ = paddedHeight_ = 0;
    linearSampler_.Reset();
    constants_.Reset();
    width_ = height_ = motionWidth_ = motionHeight_ = blocksX_ = blocksY_ = 0;
    outputStep_ = 0.5;
    previousContent_ = nextOutputContent_ = 0.0;
    lastGpuExecutionMs_.store(0.0);
    lastCpuSubmitMs_.store(0.0);
}

void GenericD3D11Fruc::close()
{
    terminate();
    prepareShader_.Reset();
    sceneShader_.Reset();
    synthesizeShader_.Reset();
    packShader_.Reset();
    sceneSrv_.Reset();
    sceneUav_.Reset();
    sceneBuffer_.Reset();
    ioConstants_.Reset();
    copyShader_.Reset();
    warpShader_.Reset();
    cleanupShader_.Reset();
    motionShader_.Reset();
    downscaleShader_.Reset();
    inputFenceInterop_.Reset();
    inputFenceCompute_.Reset();
    copyFenceInterop_.Reset();
    copyFenceCompute_.Reset();
    outputFence_.Reset();
    computeContext4_.Reset();
    interopContext4_.Reset();
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    separate_ = false;
    context_.Reset();
    device_.Reset();
    interopContext_.Reset();
    interopDevice_.Reset();
}

} // namespace frc
