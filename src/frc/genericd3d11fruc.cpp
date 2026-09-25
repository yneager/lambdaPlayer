#include "frc/genericd3d11fruc.h"

#include <QFile>
#include <QElapsedTimer>

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace frc {

namespace {

constexpr UINT kBlockSize = 32;
constexpr UINT kSearchRadius = 24;
constexpr size_t kInitialInputPool = 5;
constexpr size_t kMaximumInputPool = 8;

struct MotionParameters
{
    UINT width;
    UINT height;
    UINT blockSize;
    UINT searchRadius;
};

struct WarpParameters
{
    UINT width;
    UINT height;
    UINT blockSize;
    float interpolationTime;
};

static_assert(sizeof(MotionParameters) == 16);
static_assert(sizeof(WarpParameters) == 16);

QString hresultText(const QString &what, HRESULT hr)
{
    return QStringLiteral("%1 failed (0x%2)").arg(what).arg(quint32(hr), 8, 16, QLatin1Char('0'));
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
    device_ = device;
    if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        if (error) *error = QStringLiteral("Generic FRUC requires D3D11 feature level 11.0");
        close();
        return false;
    }
    device_->GetImmediateContext(&context_);
    if (!context_) {
        if (error) *error = QStringLiteral("Could not get the D3D11 immediate context");
        close();
        return false;
    }
    if (!compileShader(":/frc/generic_motion.hlsl", "main", motionShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_warp.hlsl", "main", warpShader_.GetAddressOf(), error)
        || !compileShader(":/frc/generic_copy.hlsl", "main", copyShader_.GetAddressOf(), error)) {
        close();
        return false;
    }
    return true;
}

bool GenericD3D11Fruc::compileShader(const char *resource, const char *entry,
                                    ID3D11ComputeShader **shader, QString *error)
{
    QFile file(QString::fromLatin1(resource));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Could not read compute shader %1").arg(QString::fromLatin1(resource));
        return false;
    }
    const QByteArray source = file.readAll();
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const HRESULT compileResult = D3DCompile(source.constData(), size_t(source.size()), resource,
                                              nullptr, nullptr, entry, "cs_5_0",
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                              0, &bytecode, &diagnostics);
    if (FAILED(compileResult)) {
        if (error) {
            const QString details = diagnostics
                ? QString::fromUtf8(static_cast<const char *>(diagnostics->GetBufferPointer()),
                                    int(diagnostics->GetBufferSize())).trimmed()
                : hresultText(QStringLiteral("D3DCompile"), compileResult);
            *error = QStringLiteral("Compute shader %1: %2").arg(QString::fromLatin1(resource), details);
        }
        return false;
    }
    const HRESULT createResult = device_->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                                               nullptr, shader);
    if (FAILED(createResult) && error) *error = hresultText(QStringLiteral("CreateComputeShader"), createResult);
    return SUCCEEDED(createResult);
}

bool GenericD3D11Fruc::initialize(int width, int height, const FrcSettings &settings, int stages, QString *error)
{
    const double sourceFps = 24.0;
    return initialize(width, height, settings, stages, sourceFps, sourceFps * 2.0, error);
}

bool GenericD3D11Fruc::initialize(int width, int height, const FrcSettings &, int stages,
                                  double sourceFps, double outputFps, QString *error)
{
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
    blocksX_ = (width_ + kBlockSize - 1) / kBlockSize;
    blocksY_ = (height_ + kBlockSize - 1) / kBlockSize;

    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(MotionParameters);
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

    if (!createFlowBuffer(blocksX_, blocksY_, &forwardFlow_, &forwardFlowSrv_, &forwardFlowUav_, error)
        || !createFlowBuffer(blocksX_, blocksY_, &backwardFlow_, &backwardFlowSrv_, &backwardFlowUav_, error)) {
        terminate();
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDesc{};
    outputDesc.Width = width_;
    outputDesc.Height = height_;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = device_->CreateTexture2D(&outputDesc, nullptr, &outputTexture_);
    if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(outputTexture_.Get(), nullptr, &outputUav_);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateTexture2D(output)"), hr);
        terminate();
        return false;
    }
    outputOwner_ = std::make_shared<InputTexture>();
    outputOwner_->texture = outputTexture_;

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

bool GenericD3D11Fruc::createFlowBuffer(UINT blocksX, UINT blocksY,
                                       Microsoft::WRL::ComPtr<ID3D11Buffer> *buffer,
                                       Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> *srv,
                                       Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> *uav,
                                       QString *error)
{
    const uint64_t count = uint64_t(blocksX) * uint64_t(blocksY);
    if (count == 0 || count > std::numeric_limits<UINT>::max() / sizeof(int32_t) / 2) {
        if (error) *error = QStringLiteral("Motion-vector field dimensions overflow D3D11 buffers");
        return false;
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(count * sizeof(int32_t) * 2);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(int32_t) * 2;
    HRESULT hr = device_->CreateBuffer(&desc, nullptr, buffer->GetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateBuffer(motion field)"), hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Buffer.NumElements = UINT(count);
    hr = device_->CreateShaderResourceView(buffer->Get(), &srvDesc, srv->GetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateShaderResourceView(motion field)"), hr);
        return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.NumElements = UINT(count);
    hr = device_->CreateUnorderedAccessView(buffer->Get(), &uavDesc, uav->GetAddressOf());
    if (FAILED(hr) && error) *error = hresultText(QStringLiteral("CreateUnorderedAccessView(motion field)"), hr);
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
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &input->texture);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(input->texture.Get(), nullptr, &input->srv);
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

bool GenericD3D11Fruc::runMotion(ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                                 ID3D11UnorderedAccessView *flow, QString *error)
{
    const MotionParameters values{width_, height_, kBlockSize, kSearchRadius};
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
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

bool GenericD3D11Fruc::runWarp(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current,
                               float interpolationTime, QString *error)
{
    const WarpParameters values{width_, height_, kBlockSize, interpolationTime};
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
    ID3D11ShaderResourceView *srvs[] = {previous, current, forwardFlowSrv_.Get(), backwardFlowSrv_.Get()};
    ID3D11UnorderedAccessView *uavs[] = {outputUav_.Get()};
    ID3D11Buffer *constant = constants_.Get();
    ID3D11SamplerState *sampler = linearSampler_.Get();
    context_->CSSetShader(warpShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constant);
    context_->CSSetSamplers(0, 1, &sampler);
    context_->CSSetShaderResources(0, 4, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[] = {nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView *nullUavs[] = {nullptr};
    context_->CSSetShaderResources(0, 4, nullSrvs);
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    Q_UNUSED(error);
    return true;
}

bool GenericD3D11Fruc::runCopy(ID3D11ShaderResourceView *source, QString *error)
{
    const WarpParameters values{width_, height_, kBlockSize, 0.0f};
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
    ID3D11ShaderResourceView *srvs[] = {source};
    ID3D11UnorderedAccessView *uavs[] = {outputUav_.Get()};
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
    if (needsMotion
        && (!runMotion(previous->srv.Get(), current->srv.Get(), forwardFlowUav_.Get(), error)
            || !runMotion(current->srv.Get(), previous->srv.Get(), backwardFlowUav_.Get(), error))) {
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
        const bool rendered = atCurrent
            ? runCopy(current->srv.Get(), error)
            : runWarp(previous->srv.Get(), current->srv.Get(), float(localTime), error);
        if (!rendered) {
            if (gpuTimer) {
                context_->End(gpuTimer->end.Get());
                context_->End(gpuTimer->disjoint.Get());
                gpuTimer->pending = true;
            }
            return false;
        }
        onOutput(InterpolatedFrame{GpuFrame{outputTexture_.Get(), outputOwner_}, atCurrent ? content : outputContent});
        nextOutputContent_ += outputStep_;
    }
    lastCpuSubmitMs_.store(double(timer.nsecsElapsed()) / 1000000.0);
    if (gpuTimer) {
        context_->End(gpuTimer->end.Get());
        context_->End(gpuTimer->disjoint.Get());
        gpuTimer->pending = true;
    }
    previous_ = input;
    previousContent_ = content;
    return true;
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
    outputOwner_.reset();
    outputTexture_.Reset();
    outputUav_.Reset();
    forwardFlowUav_.Reset(); forwardFlowSrv_.Reset(); forwardFlow_.Reset();
    backwardFlowUav_.Reset(); backwardFlowSrv_.Reset(); backwardFlow_.Reset();
    linearSampler_.Reset();
    constants_.Reset();
    width_ = height_ = blocksX_ = blocksY_ = 0;
    outputStep_ = 0.5;
    previousContent_ = nextOutputContent_ = 0.0;
    lastGpuExecutionMs_.store(0.0);
    lastCpuSubmitMs_.store(0.0);
}

void GenericD3D11Fruc::close()
{
    terminate();
    copyShader_.Reset();
    warpShader_.Reset();
    motionShader_.Reset();
    context_.Reset();
    device_.Reset();
}

} // namespace frc
