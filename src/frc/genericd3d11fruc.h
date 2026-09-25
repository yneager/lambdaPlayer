#pragma once

#include "frc/frameinterpolator.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace frc {

// Vendor-neutral D3D11 compute backend. Uses sparse block SAD at 32x32
// full-resolution cells, then bidirectional texture warping at each requested
// output time. Motion is calculated once per source pair and reused.
class GenericD3D11Fruc final : public FrameInterpolator
{
public:
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

    QString name() const override { return QStringLiteral("Generic D3D11 compute"); }
    int stages() const override { return initialized_ ? 1 : 0; }
    double lastGpuExecutionMs() const { return lastGpuExecutionMs_.load(); }
    double lastCpuSubmitMs() const { return lastCpuSubmitMs_.load(); }

private:
    struct InputTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    struct GpuTimer
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };

    bool compileShader(const char *resource, const char *entry, ID3D11ComputeShader **shader, QString *error);
    bool createFlowBuffer(UINT blockWidth, UINT blockHeight,
                          Microsoft::WRL::ComPtr<ID3D11Buffer> *buffer,
                          Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> *srv,
                          Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> *uav, QString *error);
    bool createInputTexture(std::shared_ptr<InputTexture> *texture, QString *error);
    bool runMotion(ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                   ID3D11UnorderedAccessView *flow, QString *error);
    bool runCopy(ID3D11ShaderResourceView *source, QString *error);
    bool runWarp(ID3D11ShaderResourceView *previous, ID3D11ShaderResourceView *current,
                 float interpolationTime, QString *error);
    void pollGpuTimers();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> motionShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> warpShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> copyShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> forwardFlow_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> forwardFlowSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> forwardFlowUav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> backwardFlow_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> backwardFlowSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> backwardFlowUav_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUav_;
    std::shared_ptr<InputTexture> outputOwner_;
    std::vector<std::shared_ptr<InputTexture>> inputPool_;
    std::array<GpuTimer, 4> gpuTimers_;
    size_t nextGpuTimer_ = 0;
    GpuFrame previous_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT blocksX_ = 0;
    UINT blocksY_ = 0;
    double outputStep_ = 0.5;
    double previousContent_ = 0.0;
    double nextOutputContent_ = 0.0;
    bool initialized_ = false;
    std::atomic<double> lastGpuExecutionMs_{0.0};
    std::atomic<double> lastCpuSubmitMs_{0.0};
};

} // namespace frc
