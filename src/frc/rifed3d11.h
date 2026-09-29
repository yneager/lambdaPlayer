#pragma once

// RIFE v4.x flownet executed with D3D11 compute shaders.
//
// The network is read from the ncnn param/bin pair published with
// rife-ncnn-vulkan / VapourSynth-RIFE-ncnn-Vulkan (MIT; RIFE weights from
// hzwer/Practical-RIFE, MIT). The graph is interpreted directly: no ncnn,
// Vulkan, CUDA or CPU tensor copies are involved, so the tensors live on the
// same D3D11 device as LAMBDA's captured frames.
//
// build() fixes the tensor size (padded to kAlignment). LAMBDA feeds reduced
// "analysis" copies of the full-resolution frames and only reads the final
// flow and fusion mask; the full-resolution warp happens elsewhere
// (rife_io.hlsl synthesize).

#include <QString>

#include <d3d11.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <vector>

namespace frc {

class RifeD3D11Network
{
public:
    static constexpr unsigned kAlignment = 64;

    struct TensorView
    {
        ID3D11Buffer *buffer = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        ID3D11UnorderedAccessView *uav = nullptr;
        UINT offset = 0;   // element offset of channel 0
        UINT channels = 0;
        UINT width = 0;    // padded tensor width
        UINT height = 0;
    };

    RifeD3D11Network() = default;
    ~RifeD3D11Network();
    RifeD3D11Network(const RifeD3D11Network &) = delete;
    RifeD3D11Network &operator=(const RifeD3D11Network &) = delete;

    // Parses flownet.param/flownet.bin from a model directory (CPU only).
    bool load(const QString &modelDirectory, QString *error);
    // Plans and allocates the network for padded tensor dimensions (multiples
    // of kAlignment). fullImage also keeps RIFE's own analysis-size output.
    bool build(ID3D11Device *device, UINT width, UINT height, bool fullImage, QString *error);
    void release();
    bool isBuilt() const { return built_; }

    // 3-channel RGB inputs in [0, 1]; the caller writes them before run().
    TensorView input(int index) const;
    // Records every dispatch of one inference for timestep t in (0, 1).
    void run(ID3D11DeviceContext *context, float timestep);
    // Benchmark only: runs synchronously with a GPU timestamp per dispatch and
    // returns (layer, kernel, milliseconds) rows.
    struct ProfileRow
    {
        QString layer;
        QString kernel;
        double ms = 0.0;
    };
    std::vector<ProfileRow> profile(ID3D11DeviceContext *context, float timestep);

    TensorView flowA() const { return view(flowA_); }  // A -> t displacement (x, y)
    TensorView flowB() const { return view(flowB_); }  // B -> t displacement (x, y)
    TensorView mask() const { return view(mask_); }    // weight of warped A
    TensorView image() const { return view(image_); }  // only with fullImage

    // Dispatches per context Flush() in run(); 0 = never flush.
    void setFlushInterval(int dispatches) { flushInterval_ = dispatches; }

    QString modelName() const { return modelName_; }
    int dispatchCount() const { return int(dispatches_.size()); }
    quint64 arenaBytes() const { return arenaBytes_; }

private:
    struct Layer
    {
        std::string type;
        std::string name;
        std::vector<int> bottoms;
        std::vector<int> tops;
        std::vector<int> originalTops;  // tops before residual fusion
        std::map<int, std::string> params;
        std::map<int, std::vector<float>> arrays;
        size_t weights = size_t(-1);  // offsets into weights_ (floats)
        size_t bias = size_t(-1);
        size_t data = size_t(-1);     // MemoryData
        // Fused residual-block epilogue (Convolution only).
        size_t beta = size_t(-1);
        int residual = -1;
        float slope = 0.0f;
        bool activation = false;
        bool removed = false;

        int integer(int id, int fallback) const;
        float real(int id, float fallback) const;
    };

    struct Blob
    {
        UINT c = 0, h = 0, w = 0;
        int viewOf = -1;          // Split/Crop/identity Interp alias
        UINT channelOffset = 0;   // relative to viewOf
        bool constant = false;    // MemoryData in weights_
        size_t constantOffset = 0;
        int producer = -1;
        int storage = -1;         // physical arena buffer of a root blob
    };

    struct Buffer
    {
        UINT elements = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    enum Kernel { Conv, Deconv, Deconv4x4s2, Interp, Binary, Activation, Copy, PixelShuffle, Warp, KernelCount };

    struct Dispatch
    {
        ID3D11ComputeShader *shader = nullptr;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
        ID3D11ShaderResourceView *srvs[3] = {nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView *uav = nullptr;
        UINT groups[3] = {1, 1, 1};
        Kernel kernel = Conv;
        std::string layer;
    };

    struct Location
    {
        ID3D11ShaderResourceView *srv = nullptr;
        ID3D11UnorderedAccessView *uav = nullptr;
        UINT offset = 0;
    };

    int blobIndex(const std::string &name);
    bool inferShapes(UINT width, UINT height, QString *error);
    void fuseResidualBlocks(const std::vector<bool> &needed);
    int root(int blob, UINT *channelOffset) const;
    Location locate(int blob) const;
    TensorView view(int blob) const;
    bool compileKernels(QString *error);
    bool addDispatch(Kernel kernel, const void *constants, std::initializer_list<ID3D11ShaderResourceView *> srvs,
                     ID3D11UnorderedAccessView *uav, UINT gx, UINT gy, UINT gz, QString *error);
    bool plan(const std::vector<bool> &needed, QString *error);

    QString modelName_;
    std::vector<Layer> layers_;
    std::vector<Blob> blobs_;
    std::vector<std::string> blobNames_;
    std::map<std::string, int> blobByName_;
    std::vector<float> weights_;
    int in0_ = -1, in1_ = -1, in2_ = -1;
    int flowA_ = -1, flowB_ = -1, mask_ = -1, image_ = -1;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> kernels_[KernelCount];
    Microsoft::WRL::ComPtr<ID3D11Buffer> weightBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> weightSrv_;
    std::vector<Buffer> buffers_;
    std::vector<Dispatch> dispatches_;
    quint64 arenaBytes_ = 0;
    std::string currentLayer_;
    int flushInterval_ = 0;
    bool built_ = false;
};

} // namespace frc
