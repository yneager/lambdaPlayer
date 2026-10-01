#pragma once

#include "frc/frameinterpolator.h"
#include "frc/rifed3d11.h"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <QSize>

#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace frc {

// Vendor-neutral D3D11 compute backend.
//
// Default (Rife): RIFE v4.x flownet inference on reduced "analysis" copies of
// the two source frames, once per requested output timestep (RIFE's flow is
// timestep-dependent). Its flow and fusion mask are upsampled to output size
// and the ORIGINAL full-resolution frames are warped and blended, which is
// RIFE v4's own final synthesis step. Output resolution is never reduced.
//
// Block (LAMBDA_GENERIC_FRC_MOTION=block): the earlier experimental
// block-matching estimator, kept only for A/B comparison.
class GenericD3D11Fruc final : public FrameInterpolator
{
public:
    enum class Motion { Rife, Block, RifeReusable };

    // Overrides for tests/benchmarks; call before open()/initialize().
    void setMotion(Motion motion) { motion_ = motion; motionOverridden_ = true; }
    void setModelDirectory(const QString &directory) { modelDirectory_ = directory; }
    void setAnalysisHeight(int height) { analysisHeightOverride_ = height; }
    void setFastQuality(int quality) { fastQuality_ = quality; }
    Motion motion() const { return motion_; }
    QString modelName() const { return rife_.modelName(); }
    // Benchmark only: synchronous per-stage GPU timestamps.
    void setStageTiming(bool enabled) { stageTiming_ = enabled; }
    std::vector<std::pair<QString, double>> takeStageTimes();
    bool separateComputeDevice() const { return separate_; }
    QSize analysisSize() const { return QSize(int(analysisWidth_), int(analysisHeight_)); }

    GenericD3D11Fruc() = default;
    ~GenericD3D11Fruc() override;
    GenericD3D11Fruc(const GenericD3D11Fruc &) = delete;
    GenericD3D11Fruc &operator=(const GenericD3D11Fruc &) = delete;

    bool open(ID3D11Device *device, QString *error) override;
    bool initialize(int width, int height, const FrcSettings &settings, int stages, QString *error) override;
    bool initialize(int width, int height, const FrcSettings &settings, int stages,
                    double sourceFps, double outputFps, QString *error);
    GpuFrame allocateInput(QString *error) override;
    bool processFrame(const GpuFrame &input, double content,
                      const OutputCallback &onOutput, QString *error) override;
    void flush() override;
    void terminate() override;
    void close() override;

    QString name() const override;
    int stages() const override { return initialized_ ? 1 : 0; }
    double lastGpuExecutionMs() const { return lastGpuExecutionMs_.load(); }
    double lastCpuSubmitMs() const { return lastCpuSubmitMs_.load(); }
    qint64 skippedStaticPairs() const { return skippedStaticPairs_.load(); }
    qint64 skippedCutPairs() const { return skippedCutPairs_.load(); }
    qint64 refinedPairs() const { return refinedPairs_.load(); }

private:
    // `texture` belongs to the device passed to open() (OpenGL renders into
    // it through the interop); `srv` views the same memory on the compute
    // device, which is a separate device when separate_ is set.
    struct InputTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> computeTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    // Output ring entry: written on the compute device, copied by the
    // controller on the interop device (`interop` is the same memory).
    struct OutputSlot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> compute;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> interop;
        std::shared_ptr<InputTexture> owner;
        UINT64 ready = 0;   // output fence value when rendered
        UINT64 copied = 0;  // copy fence value after the controller's copy
    };

    struct MotionTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    struct FlowTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    struct GpuTimer
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };

    bool compileShader(const char *resource, const char *entry, ID3D11ComputeShader **shader, QString *error);
    bool createFlowTexture(UINT blockWidth, UINT blockHeight, FlowTexture *flow, QString *error);
    bool createMotionTexture(MotionTexture *texture, QString *error);
    bool createInputTexture(std::shared_ptr<InputTexture> *texture, QString *error);
    bool runDownscale(ID3D11ShaderResourceView *source, ID3D11UnorderedAccessView *destination, QString *error);
    bool runMotion(ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                   ID3D11UnorderedAccessView *flow, QString *error);
    bool runFlowCleanup(ID3D11ShaderResourceView *source, ID3D11UnorderedAccessView *destination,
                        QString *error);
    bool runCopy(ID3D11ShaderResourceView *source, QString *error);
    bool runWarp(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current,
                 float interpolationTime, QString *error);
    void pollGpuTimers();
    void stamp(const char *label);
    bool loadRife(QString *error);
    bool initializeRife(double outputFps, QString *error);
    void runPrepare(ID3D11ShaderResourceView *frame, const RifeD3D11Network::TensorView &tensor);
    void runSceneDiff();
    void packRifeMotion();
    void runSynthesize(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current, float t,
                       bool reusable, bool noMotion);
    void bindIo(ID3D11ComputeShader *shader, const void *constants,
                std::initializer_list<ID3D11ShaderResourceView *> srvs, ID3D11UnorderedAccessView *uav,
                UINT gx, UINT gy, UINT gz);

    bool createComputeDevice(QString *error);
    bool createSharedTexture(ID3D11Device *owner, ID3D11Device *other, const D3D11_TEXTURE2D_DESC &desc,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D> *ownerTexture,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D> *otherTexture, QString *error);
    bool createOutputRing(size_t count, QString *error);
    bool waitForOutput(const OutputSlot &slot, QString *error);

    // device_/context_ run the compute work. With separate_ they are a second
    // device on the same adapter, so the OpenGL present on the GUI thread
    // (which waits for the interop device's queue) never waits behind a whole
    // inference; frames cross devices through shared textures and fences.
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Device> interopDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> interopContext_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> computeContext4_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> interopContext4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> inputFenceInterop_;   // interop signals: capture done
    Microsoft::WRL::ComPtr<ID3D11Fence> inputFenceCompute_;
    Microsoft::WRL::ComPtr<ID3D11Fence> outputFence_;         // compute signals: output ready
    Microsoft::WRL::ComPtr<ID3D11Fence> copyFenceInterop_;    // interop signals: slot copied
    Microsoft::WRL::ComPtr<ID3D11Fence> copyFenceCompute_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 inputValue_ = 0;
    UINT64 outputValue_ = 0;
    UINT64 copyValue_ = 0;
    bool separate_ = false;
    std::vector<OutputSlot> outputs_;
    size_t nextOutput_ = 0;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> motionShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> downscaleShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cleanupShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> warpShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> copyShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> prepareShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> sceneShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> synthesizeShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> packShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sceneUav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ioConstants_;
    RifeD3D11Network rife_;
    Motion motion_ = Motion::Rife;
    bool motionOverridden_ = false;
    bool rifeLoaded_ = false;
    QString modelDirectory_;
    int analysisHeightOverride_ = 0;
    UINT analysisWidth_ = 0;
    UINT analysisHeight_ = 0;
    UINT paddedWidth_ = 0;
    UINT paddedHeight_ = 0;
    float sceneThreshold_ = 0.0f;
    bool stageTiming_ = false;
    Microsoft::WRL::ComPtr<ID3D11Query> stageDisjoint_;
    std::vector<std::pair<QString, Microsoft::WRL::ComPtr<ID3D11Query>>> stageQueries_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    FlowTexture rawForwardFlow_;
    FlowTexture forwardFlow_;
    FlowTexture rawBackwardFlow_;
    FlowTexture backwardFlow_;
    MotionTexture motionFrameA_;
    MotionTexture motionFrameB_;
    MotionTexture rifeMotion_;
    MotionTexture rifeMask_;
    MotionTexture rifeMotionBackup_;
    MotionTexture rifeMaskBackup_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneReadback_;
    int fastQuality_ = 1;
    double sourceBudgetMs_ = 1000.0 / 24.0;
    ID3D11UnorderedAccessView *outputUav_ = nullptr;  // current ring slot
    std::vector<std::shared_ptr<InputTexture>> inputPool_;
    std::array<GpuTimer, 4> gpuTimers_;
    size_t nextGpuTimer_ = 0;
    GpuFrame previous_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT motionWidth_ = 0;
    UINT motionHeight_ = 0;
    UINT blocksX_ = 0;
    UINT blocksY_ = 0;
    double outputStep_ = 0.5;
    double previousContent_ = 0.0;
    double nextOutputContent_ = 0.0;
    bool initialized_ = false;
    std::atomic<double> lastGpuExecutionMs_{0.0};
    std::atomic<double> lastCpuSubmitMs_{0.0};
    std::atomic<qint64> skippedStaticPairs_{0};
    std::atomic<qint64> skippedCutPairs_{0};
    std::atomic<qint64> refinedPairs_{0};
};

} // namespace frc
