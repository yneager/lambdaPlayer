#pragma once

// Backend-neutral contract for post-mpv-render interpolation. Pixels and
// ownership remain GPU-side; payload retains the backend resource behind each
// D3D11 texture (AMF surface, or a plain D3D11 allocation).

#include <QString>

#include <functional>
#include <memory>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace frc {

struct FrcSettings
{
    int profile = 2;       // AMF profile value: SUPER
    int searchMode = 0;    // AMF motion-search value: NATIVE
    bool useFutureFrame = false;
    bool fallbackBlend = false;
};

struct GpuFrame
{
    ID3D11Texture2D *texture = nullptr;
    std::shared_ptr<void> owner;
};

struct InterpolatedFrame
{
    GpuFrame frame;
    double content = 0.0;  // source-frame position; fractional means generated
    double pictureContent = -1.0; // actual picture identity; held frames retain their source position
};

class FrameInterpolator
{
public:
    using OutputCallback = std::function<void(const InterpolatedFrame &)>;

    virtual ~FrameInterpolator() = default;
    virtual bool open(ID3D11Device *device, QString *error) = 0;
    virtual bool initialize(int width, int height, const FrcSettings &settings,
                            int stages, QString *error) = 0;
    virtual GpuFrame allocateInput(QString *error) = 0;
    virtual bool processFrame(const GpuFrame &input, double content,
                              const OutputCallback &onOutput, QString *error) = 0;
    virtual void flush() = 0;
    virtual void terminate() = 0;
    virtual void close() = 0;

    virtual QString name() const = 0;
    virtual QString runtimeVersion() const { return {}; }
    virtual int stages() const = 0;
    virtual int lastSubmitStatus() const { return 0; }
    virtual int lastQueryStatus() const { return 0; }
};

} // namespace frc
