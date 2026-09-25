#include "frc/d3d11videoprocessorprobe.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace frc {

namespace {

using Microsoft::WRL::ComPtr;

QString hresultText(const QString &what, HRESULT hr)
{
    return QStringLiteral("%1 failed (0x%2)")
        .arg(what).arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

} // namespace

D3D11VideoProcessorProbeResult probeD3D11VideoProcessorFrc(
    ID3D11Device *device, ID3D11DeviceContext *context,
    unsigned int width, unsigned int height)
{
    D3D11VideoProcessorProbeResult result;
    result.testedWidth = width;
    result.testedHeight = height;
    if (!device || !context || !width || !height) {
        result.reason = QStringLiteral("D3D11 device/context or probe dimensions are invalid");
        return result;
    }

    ComPtr<ID3D11VideoDevice> videoDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(videoDevice.GetAddressOf()));
    if (FAILED(hr)) {
        result.reason = hresultText(QStringLiteral("QueryInterface(ID3D11VideoDevice)"), hr);
        return result;
    }
    ComPtr<ID3D11VideoContext> videoContext;
    hr = context->QueryInterface(IID_PPV_ARGS(videoContext.GetAddressOf()));
    if (FAILED(hr)) {
        result.reason = hresultText(QStringLiteral("QueryInterface(ID3D11VideoContext)"), hr);
        return result;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = DXGI_RATIONAL{24, 1};
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputFrameRate = DXGI_RATIONAL{48, 1};
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    hr = videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator);
    if (FAILED(hr)) {
        result.reason = hresultText(QStringLiteral("CreateVideoProcessorEnumerator(24->48)"), hr);
        return result;
    }
    result.enumeratorCreated = true;

    UINT formatSupport = 0;
    hr = enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &formatSupport);
    if (FAILED(hr)) {
        result.reason = hresultText(QStringLiteral("CheckVideoProcessorFormat(BGRA8)"), hr);
        return result;
    }
    result.bgraInput = (formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;
    result.bgraOutput = (formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) != 0;

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    hr = enumerator->GetVideoProcessorCaps(&caps);
    if (FAILED(hr)) {
        result.reason = hresultText(QStringLiteral("GetVideoProcessorCaps"), hr);
        return result;
    }

    for (UINT index = 0; index < caps.RateConversionCapsCount; ++index) {
        D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rateCaps{};
        hr = enumerator->GetVideoProcessorRateConversionCaps(index, &rateCaps);
        if (FAILED(hr)) continue;
        if ((rateCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION) == 0)
            continue;

        result.frameRateConversion = true;
        ++result.rateConversionModeCount;

        // Try each advertised FRC mode. Creating a processor validates that
        // the mode is accepted for this device/format/size. Configure the
        // stream for a generated-rate path (RepeatFrame == FALSE); a full
        // VideoProcessorBlt texture test is still required before promotion.
        ComPtr<ID3D11VideoProcessor> processor;
        hr = videoDevice->CreateVideoProcessor(enumerator.Get(), index, &processor);
        if (FAILED(hr)) continue;
        if (!result.processorCreated) {
            result.processorCreated = true;
            result.selectedRateConversionIndex = index;
            result.pastFrames = rateCaps.PastFrames;
            result.futureFrames = rateCaps.FutureFrames;
        }
        videoContext->VideoProcessorSetStreamOutputRate(
            processor.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
        result.streamRateConfigured = true;
        result.selectedRateConversionIndex = index;
        result.pastFrames = rateCaps.PastFrames;
        result.futureFrames = rateCaps.FutureFrames;
        break;
    }

    if (!result.frameRateConversion) {
        result.reason = QStringLiteral("Driver advertises no frame-rate-conversion mode for this 4K 24->48 profile");
    } else if (!result.bgraInput || !result.bgraOutput) {
        result.reason = QStringLiteral("Driver FRC mode lacks BGRA8 input or output support");
    } else if (!result.processorCreated) {
        result.reason = QStringLiteral("FRC is advertised, but no rate-conversion processor could be created");
    } else if (!result.streamRateConfigured) {
        result.reason = QStringLiteral("Processor was created, but its generated-rate stream setup failed");
    } else {
        result.reason = QStringLiteral("Advertised/configured only; frame blit and output quality are not validated");
    }
    return result;
}

} // namespace frc
