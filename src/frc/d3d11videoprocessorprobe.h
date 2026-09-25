#pragma once

#include <QString>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace frc {

struct D3D11VideoProcessorProbeResult
{
    bool enumeratorCreated = false;
    bool bgraInput = false;
    bool bgraOutput = false;
    bool frameRateConversion = false;
    bool processorCreated = false;
    bool streamRateConfigured = false;
    unsigned int rateConversionModeCount = 0;
    unsigned int selectedRateConversionIndex = 0;
    unsigned int pastFrames = 0;
    unsigned int futureFrames = 0;
    unsigned int testedWidth = 3840;
    unsigned int testedHeight = 2160;
    QString reason;
};

// Reads the driver's advertised 24 -> 48 fps D3D11 video-processor caps at
// the requested size, including format support and temporal reference needs.
// This is a capability probe only; it does not run video frames through the
// processor and must not by itself be presented as a working FRC backend.
D3D11VideoProcessorProbeResult probeD3D11VideoProcessorFrc(
    ID3D11Device *device, ID3D11DeviceContext *context,
    unsigned int width = 3840, unsigned int height = 2160);

} // namespace frc
