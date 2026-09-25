#pragma once

// Minimal wrapper around AMD AMF's Frame Rate Conversion component.
//
// Context/component setup and the SubmitInput/QueryOutput sequence are taken
// from AMD's SimpleFRC sample (GPUOpen-LibrariesAndSDKs/AMF,
// amf/public/samples/CPPSamples/SimpleFRC/SimpleFRC.cpp, MIT): in
// FRC_x2_PRESENT mode every submitted source frame yields an interpolated
// frame (QueryOutput returns AMF_REPEAT) followed by the source frame
// (AMF_OK). A surface refused with AMF_INPUT_FULL / AMF_NEED_MORE_INPUT is
// submitted again, exactly as SimpleFRC does.
//
// Rates above 2x: FRC only doubles, so up to four components are cascaded
// (2x, 4x, 8x, 16x): every output of one stage is submitted to the next. Each
// output carries its content time in source frames, derived from the order
// measured with tools/frc-proof: after submitting input k a stage returns the
// midpoint of inputs k-2 and k-1, then input k-1 (one input further back with
// FRC_USE_FUTURE_FRAME).
//
// Not thread-safe: after init() every call must come from one thread
// (ScreenInterpolationController's worker).

#include <QString>

#include <deque>
#include <functional>
#include <vector>

#include "public/common/AMFFactory.h"
#include "public/include/components/FRC.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace frc {

struct FrcSettings
{
    amf_int64 profile = FRC_PROFILE_SUPER;
    amf_int64 searchMode = FRC_MV_SEARCH_NATIVE;
    bool useFutureFrame = false;
    bool fallbackBlend = false;
};

class AmfFrcInterpolator
{
public:
    static constexpr int kMaxStages = 4; // 2x .. 16x

    struct Output
    {
        amf::AMFSurfacePtr surface;
        double content = 0.0;       // position in source frames (fraction = generated)
    };

    AmfFrcInterpolator() = default;
    ~AmfFrcInterpolator();
    AmfFrcInterpolator(const AmfFrcInterpolator &) = delete;
    AmfFrcInterpolator &operator=(const AmfFrcInterpolator &) = delete;

    // Loads the AMF runtime and creates a DX11 context on `device` (which
    // must be multithread protected). Returns false with a reason when AMF
    // or its FRC component is unavailable (e.g. non-AMD GPUs).
    bool open(ID3D11Device *device, QString *error);
    // `stages` cascaded x2 components (1..kMaxStages -> 2x .. 16x frames).
    bool init(int width, int height, const FrcSettings &settings, int stages, QString *error);
    int stages() const { return int(frc_.size()); }
    void terminate();
    void close();

    bool isOpen() const { return context_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    FrcSettings settings() const { return settings_; }
    QString runtimeVersion() const { return runtimeVersion_; }

    // A BGRA DX11 surface from AMF's pool, usable as a render target.
    amf::AMFSurfacePtr allocInput(QString *error);

    // SimpleFRC's loop for one source frame (content = its source index):
    // submit (resubmitting while the component is full) through every stage
    // and hand each final output to `onOutput`.
    bool process(const amf::AMFSurfacePtr &input, double content,
                 const std::function<void(const Output &)> &onOutput, QString *error);

    // Drops the component's frame history (seek, resume, file change).
    void flush();

    int lastSubmitStatus() const { return lastSubmit_; }
    int lastQueryStatus() const { return lastQuery_; }

private:
    bool submitStage(size_t stage, const amf::AMFSurfacePtr &input, double content,
                     const std::function<void(const Output &)> &onOutput, QString *error);
    void drainStage(size_t stage, const std::function<void(const Output &)> &onOutput, QString *error, bool *ok);
    amf::AMFComponentPtr createComponent(QString *error);

    amf::AMFContextPtr context_;
    amf::AMFComponentPtr probeComponent_;
    std::vector<amf::AMFComponentPtr> frc_;
    std::vector<std::deque<double>> history_; // recent input content per stage
    FrcSettings settings_;
    QString runtimeVersion_;
    int width_ = 0;
    int height_ = 0;
    int lastSubmit_ = AMF_OK;
    int lastQuery_ = AMF_OK;
    bool factoryInitialized_ = false;
};

} // namespace frc
