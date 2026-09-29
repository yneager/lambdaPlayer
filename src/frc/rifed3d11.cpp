#include "frc/rifed3d11.h"

#include "frc/computeshaders.h"

#include <QDir>
#include <QFile>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>

namespace frc {

namespace {

// Matches `cbuffer Op` in rife_ops.hlsl.
struct OpConstants
{
    uint32_t p[16];
    float f[8];
};
static_assert(sizeof(OpConstants) == 96);

constexpr uint32_t kNone = 0xffffffffu;
constexpr uint32_t kFp16Tag = 0x01306B47u;
constexpr int kKeep = std::numeric_limits<int>::max();

QString hresultText(const QString &what, HRESULT hr)
{
    return QStringLiteral("%1 failed (0x%2)").arg(what).arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1fu;
    uint32_t mantissa = h & 0x3ffu;
    uint32_t bits = sign;
    if (exponent == 31) {
        bits |= 0x7f800000u | (mantissa << 13);
    } else if (exponent != 0) {
        bits |= ((exponent + 112) << 23) | (mantissa << 13);
    } else if (mantissa != 0) {
        int e = -14;
        while (!(mantissa & 0x400u)) {
            mantissa <<= 1;
            --e;
        }
        bits |= uint32_t(e + 127) << 23 | ((mantissa & 0x3ffu) << 13);
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

UINT ceilDiv(UINT a, UINT b)
{
    return (a + b - 1) / b;
}

} // namespace

int RifeD3D11Network::Layer::integer(int id, int fallback) const
{
    const auto it = params.find(id);
    return it == params.end() ? fallback : std::stoi(it->second);
}

float RifeD3D11Network::Layer::real(int id, float fallback) const
{
    const auto it = params.find(id);
    return it == params.end() ? fallback : std::stof(it->second);
}

RifeD3D11Network::~RifeD3D11Network()
{
    release();
}

int RifeD3D11Network::blobIndex(const std::string &name)
{
    const auto it = blobByName_.find(name);
    if (it != blobByName_.end()) return it->second;
    const int index = int(blobNames_.size());
    blobNames_.push_back(name);
    blobByName_.emplace(name, index);
    return index;
}

bool RifeD3D11Network::load(const QString &modelDirectory, QString *error)
{
    release();
    layers_.clear();
    blobNames_.clear();
    blobByName_.clear();
    weights_.clear();
    const QDir dir(modelDirectory);
    QFile paramFile(dir.filePath(QStringLiteral("flownet.param")));
    QFile binFile(dir.filePath(QStringLiteral("flownet.bin")));
    if (!paramFile.open(QIODevice::ReadOnly) || !binFile.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("RIFE model not found in %1").arg(QDir::toNativeSeparators(modelDirectory));
        return false;
    }
    modelName_ = dir.dirName();
    std::istringstream param(paramFile.readAll().toStdString());
    const QByteArray bin = binFile.readAll();
    size_t position = 0;

    auto fail = [&](const QString &why) {
        if (error) *error = QStringLiteral("RIFE model %1: %2").arg(modelName_, why);
        layers_.clear();
        return false;
    };
    auto readRaw = [&](size_t count, size_t *offset) {
        if (position + count * 4 > size_t(bin.size())) return false;
        *offset = weights_.size();
        weights_.resize(weights_.size() + count);
        std::memcpy(weights_.data() + *offset, bin.constData() + position, count * 4);
        position += count * 4;
        return true;
    };
    auto readTagged = [&](size_t count, size_t *offset) {
        if (position + 4 > size_t(bin.size())) return false;
        uint32_t tag = 0;
        std::memcpy(&tag, bin.constData() + position, 4);
        position += 4;
        if (tag == 0) return readRaw(count, offset);
        if (tag != kFp16Tag || position + count * 2 > size_t(bin.size())) return false;
        *offset = weights_.size();
        weights_.resize(weights_.size() + count);
        const auto *halves = reinterpret_cast<const uint16_t *>(bin.constData() + position);
        for (size_t i = 0; i < count; ++i) weights_[*offset + i] = halfToFloat(halves[i]);
        position += (count * 2 + 3) / 4 * 4;
        return true;
    };

    int magic = 0, layerCount = 0, blobCount = 0;
    param >> magic >> layerCount >> blobCount;
    if (magic != 7767517 || layerCount <= 0) return fail(QStringLiteral("unsupported param header"));
    layers_.reserve(size_t(layerCount));
    std::string line;
    std::getline(param, line);
    while (std::getline(param, line)) {
        std::istringstream tokens(line);
        Layer layer;
        int bottomCount = 0, topCount = 0;
        if (!(tokens >> layer.type >> layer.name >> bottomCount >> topCount)) continue;
        for (int i = 0; i < bottomCount; ++i) {
            std::string name;
            tokens >> name;
            layer.bottoms.push_back(blobIndex(name));
        }
        for (int i = 0; i < topCount; ++i) {
            std::string name;
            tokens >> name;
            layer.tops.push_back(blobIndex(name));
        }
        std::string item;
        while (tokens >> item) {
            const size_t equals = item.find('=');
            if (equals == std::string::npos) return fail(QStringLiteral("bad parameter in %1").arg(QString::fromStdString(layer.name)));
            const int key = std::stoi(item.substr(0, equals));
            const std::string value = item.substr(equals + 1);
            if (key <= -23300) {
                std::vector<float> values;
                std::stringstream list(value);
                std::string number;
                bool first = true;
                while (std::getline(list, number, ',')) {
                    if (!first) values.push_back(std::stof(number));
                    first = false;
                }
                layer.arrays[-key - 23300] = std::move(values);
            } else {
                layer.params[key] = value;
            }
        }
        if (layer.type == "Convolution" || layer.type == "Deconvolution") {
            if (layer.integer(8, 0) != 0) return fail(QStringLiteral("int8 layers are not supported"));
            if (!readTagged(size_t(layer.integer(6, 0)), &layer.weights)
                || (layer.integer(5, 0) && !readRaw(size_t(layer.integer(0, 0)), &layer.bias))) {
                return fail(QStringLiteral("flownet.bin is truncated or uses an unsupported weight encoding"));
            }
        } else if (layer.type == "MemoryData") {
            const size_t count = size_t(std::max(1, layer.integer(0, 1))) * size_t(std::max(1, layer.integer(1, 1)))
                                 * size_t(std::max(1, layer.integer(2, 1)));
            if (!readRaw(count, &layer.data)) return fail(QStringLiteral("flownet.bin is truncated"));
        }
        layer.originalTops = layer.tops;
        layers_.push_back(std::move(layer));
    }
    if (int(layers_.size()) != layerCount) return fail(QStringLiteral("layer count mismatch"));
    if (position != size_t(bin.size())) return fail(QStringLiteral("flownet.bin does not match flownet.param"));
    return true;
}

int RifeD3D11Network::root(int blob, UINT *channelOffset) const
{
    UINT offset = 0;
    while (blobs_[size_t(blob)].viewOf >= 0) {
        offset += blobs_[size_t(blob)].channelOffset;
        blob = blobs_[size_t(blob)].viewOf;
    }
    if (channelOffset) *channelOffset = offset;
    return blob;
}

bool RifeD3D11Network::inferShapes(UINT width, UINT height, QString *error)
{
    blobs_.assign(blobNames_.size(), Blob());
    auto fail = [&](const Layer &layer, const QString &why) {
        if (error) {
            *error = QStringLiteral("RIFE model %1: %2 (%3 %4)")
                         .arg(modelName_, why, QString::fromStdString(layer.type), QString::fromStdString(layer.name));
        }
        return false;
    };
    for (size_t i = 0; i < layers_.size(); ++i) {
        const Layer &layer = layers_[i];
        auto in = [&](size_t k) -> Blob & { return blobs_[size_t(layer.bottoms[k])]; };
        auto out = [&](size_t k) -> Blob & { return blobs_[size_t(layer.tops[k])]; };
        if (layer.tops.empty()) return fail(layer, QStringLiteral("layer without output"));
        const std::string &type = layer.type;
        if (type == "Input") {
            const std::string &name = blobNames_[size_t(layer.tops[0])];
            Blob &b = out(0);
            if (name == "in0" || name == "in1") {
                b.c = 3;
                b.h = height;
                b.w = width;
                (name == "in0" ? in0_ : in1_) = layer.tops[0];
            } else if (name == "in2") {
                b.c = b.h = b.w = 1;
                in2_ = layer.tops[0];
            } else {
                return fail(layer, QStringLiteral("unexpected network input"));
            }
        } else if (type == "MemoryData") {
            Blob &b = out(0);
            b.w = UINT(std::max(1, layer.integer(0, 1)));
            b.h = UINT(std::max(1, layer.integer(1, 1)));
            b.c = UINT(std::max(1, layer.integer(2, 1)));
            b.constant = true;
            b.constantOffset = layer.data;
        } else if (type == "Split") {
            for (size_t k = 0; k < layer.tops.size(); ++k) {
                Blob &b = out(k);
                b = in(0);
                b.viewOf = layer.bottoms[0];
                b.channelOffset = 0;
                b.constant = false;
                if (in(0).constant) {
                    b.viewOf = -1;
                    b.constant = true;
                }
            }
        } else if (type == "Concat") {
            if (layer.integer(0, 0) != 0) return fail(layer, QStringLiteral("only channel concatenation is supported"));
            Blob &b = out(0);
            b.w = in(0).w;
            b.h = in(0).h;
            for (size_t k = 0; k < layer.bottoms.size(); ++k) {
                if (in(k).w != b.w || in(k).h != b.h) return fail(layer, QStringLiteral("concat size mismatch"));
                b.c += in(k).c;
            }
        } else if (type == "Crop") {
            const auto starts = layer.arrays.find(9);
            const auto ends = layer.arrays.find(10);
            const auto axes = layer.arrays.find(11);
            if (starts == layer.arrays.end() || ends == layer.arrays.end() || axes == layer.arrays.end()
                || starts->second.size() != 1 || ends->second.size() != 1 || axes->second.size() != 1
                || axes->second[0] != 0.0f) {
                return fail(layer, QStringLiteral("only channel slicing is supported"));
            }
            const Blob &source = in(0);
            const long long c = source.c;
            long long start = (long long)starts->second[0];
            long long end = ends->second[0] >= 2147483647.0f ? c : (long long)ends->second[0];
            if (start < 0) start += c;
            if (end < 0) end += c;
            end = std::min(end, c);
            if (start < 0 || start >= end) return fail(layer, QStringLiteral("empty channel slice"));
            Blob &b = out(0);
            b.w = source.w;
            b.h = source.h;
            b.c = UINT(end - start);
            b.viewOf = layer.bottoms[0];
            b.channelOffset = UINT(start);
        } else if (type == "BinaryOp") {
            const Blob &a = in(0);
            Blob &b = out(0);
            if (layer.integer(1, 0) != 0 || layer.bottoms.size() == 1) {
                b.w = a.w;
                b.h = a.h;
                b.c = a.c;
            } else {
                const Blob &other = in(1);
                auto broadcast = [](UINT x, UINT y, bool *ok) {
                    if (x == y || y == 1) return x;
                    if (x == 1) return y;
                    *ok = false;
                    return x;
                };
                bool ok = true;
                b.w = broadcast(a.w, other.w, &ok);
                b.h = broadcast(a.h, other.h, &ok);
                b.c = broadcast(a.c, other.c, &ok);
                if (!ok) return fail(layer, QStringLiteral("operands cannot be broadcast"));
            }
        } else if (type == "Eltwise") {
            if (layer.integer(0, 1) != 1 || layer.bottoms.size() != 2)
                return fail(layer, QStringLiteral("only two-input weighted sums are supported"));
            if (in(0).w != in(1).w || in(0).h != in(1).h || in(0).c != in(1).c)
                return fail(layer, QStringLiteral("eltwise size mismatch"));
            Blob &b = out(0);
            b.w = in(0).w;
            b.h = in(0).h;
            b.c = in(0).c;
        } else if (type == "Convolution") {
            const int kernel = layer.integer(1, 0);
            const int stride = layer.integer(3, 1);
            const int pad = layer.integer(4, 0);
            const int outC = layer.integer(0, 0);
            const int activation = layer.integer(9, 0);
            if (kernel != 3 || layer.integer(11, kernel) != 3 || layer.integer(2, 1) != 1 || pad != 1
                || layer.integer(14, pad) != 1 || layer.integer(15, pad) != 1 || layer.integer(16, pad) != 1
                || (stride != 1 && stride != 2) || layer.integer(13, stride) != stride) {
                return fail(layer, QStringLiteral("only 3x3 convolutions with stride 1/2 and padding 1 are supported"));
            }
            if (activation != 0 && activation != 1 && activation != 2)
                return fail(layer, QStringLiteral("unsupported activation"));
            if (size_t(layer.integer(6, 0)) != size_t(outC) * in(0).c * 9)
                return fail(layer, QStringLiteral("weight count does not match the input channels"));
            Blob &b = out(0);
            b.c = UINT(outC);
            b.w = (in(0).w + 2 - 3) / UINT(stride) + 1;
            b.h = (in(0).h + 2 - 3) / UINT(stride) + 1;
        } else if (type == "Deconvolution") {
            const int kernel = layer.integer(1, 0);
            const int stride = layer.integer(3, 1);
            const int pad = layer.integer(4, 0);
            const int outC = layer.integer(0, 0);
            if (kernel <= 0 || layer.integer(11, kernel) != kernel || layer.integer(2, 1) != 1
                || layer.integer(13, stride) != stride || layer.integer(14, pad) != pad
                || layer.integer(15, pad) != pad || layer.integer(16, pad) != pad || layer.integer(9, 0) != 0
                || layer.integer(18, 0) != 0 || layer.integer(20, 0) != 0) {
                return fail(layer, QStringLiteral("unsupported deconvolution parameters"));
            }
            if (size_t(layer.integer(6, 0)) != size_t(outC) * in(0).c * size_t(kernel) * size_t(kernel))
                return fail(layer, QStringLiteral("weight count does not match the input channels"));
            Blob &b = out(0);
            b.c = UINT(outC);
            b.w = (in(0).w - 1) * UINT(stride) + UINT(kernel) - 2 * UINT(pad);
            b.h = (in(0).h - 1) * UINT(stride) + UINT(kernel) - 2 * UINT(pad);
        } else if (type == "Interp") {
            if (layer.integer(0, 0) != 2 || layer.integer(6, 0) != 0 || layer.integer(7, 0) != 0)
                return fail(layer, QStringLiteral("only static bilinear resize without align_corners is supported"));
            const Blob &source = in(0);
            Blob &b = out(0);
            b.c = source.c;
            b.h = layer.integer(3, 0) > 0 ? UINT(layer.integer(3, 0)) : UINT(float(source.h) * layer.real(1, 1.0f));
            b.w = layer.integer(4, 0) > 0 ? UINT(layer.integer(4, 0)) : UINT(float(source.w) * layer.real(2, 1.0f));
            if (b.w == 0 || b.h == 0) return fail(layer, QStringLiteral("resize to an empty tensor"));
            if (b.w == source.w && b.h == source.h) {
                b.viewOf = layer.bottoms[0];
                b.channelOffset = 0;
            }
        } else if (type == "PixelShuffle") {
            const UINT r = UINT(layer.integer(0, 1));
            if (layer.integer(1, 0) != 0 || r == 0 || in(0).c % (r * r) != 0)
                return fail(layer, QStringLiteral("unsupported pixel shuffle"));
            Blob &b = out(0);
            b.c = in(0).c / (r * r);
            b.w = in(0).w * r;
            b.h = in(0).h * r;
        } else if (type == "ReLU" || type == "Sigmoid") {
            Blob &b = out(0);
            b.w = in(0).w;
            b.h = in(0).h;
            b.c = in(0).c;
        } else if (type == "rife.Warp") {
            if (layer.bottoms.size() != 2 || in(1).c < 2 || in(1).w != in(0).w || in(1).h != in(0).h)
                return fail(layer, QStringLiteral("unexpected warp operands"));
            Blob &b = out(0);
            b.w = in(0).w;
            b.h = in(0).h;
            b.c = in(0).c;
        } else {
            return fail(layer, QStringLiteral("unsupported layer type"));
        }
        for (int top : layer.tops) {
            if (blobs_[size_t(top)].producer < 0) blobs_[size_t(top)].producer = int(i);
        }
    }
    if (in0_ < 0 || in1_ < 0 || in2_ < 0) {
        if (error) *error = QStringLiteral("RIFE model %1 does not have the in0/in1/in2 inputs of a v4 flownet").arg(modelName_);
        return false;
    }
    return true;
}

void RifeD3D11Network::fuseResidualBlocks(const std::vector<bool> &needed)
{
    // RIFE v4 ResConv: leaky(conv(x) * beta + x). Folding the multiply, add
    // and activation into the convolution epilogue saves three full passes.
    std::vector<std::vector<int>> consumers(blobs_.size());
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (!needed[i]) continue;
        for (int b : layers_[i].bottoms) consumers[size_t(b)].push_back(int(i));
    }
    auto only = [&](int blob) { return consumers[size_t(blob)].size() == 1 ? consumers[size_t(blob)][0] : -1; };
    for (size_t i = 0; i < layers_.size(); ++i) {
        Layer &conv = layers_[i];
        if (!needed[i] || conv.removed || conv.type != "Convolution" || conv.integer(9, 0) != 0) continue;
        const int convOut = conv.tops[0];
        const int mulIndex = only(convOut);
        if (mulIndex < 0) continue;
        Layer &mul = layers_[size_t(mulIndex)];
        if (mul.type != "BinaryOp" || mul.integer(0, 0) != 2 || mul.integer(1, 0) != 0 || mul.bottoms.size() != 2) continue;
        const int betaBlob = mul.bottoms[0] == convOut ? mul.bottoms[1] : mul.bottoms[0];
        const Blob &beta = blobs_[size_t(betaBlob)];
        const Blob &shape = blobs_[size_t(convOut)];
        if (!beta.constant || beta.w != 1 || beta.h != 1 || beta.c != shape.c) continue;
        const int addIndex = only(mul.tops[0]);
        if (addIndex < 0) continue;
        Layer &add = layers_[size_t(addIndex)];
        if (add.type != "BinaryOp" || add.integer(0, 0) != 0 || add.integer(1, 0) != 0 || add.bottoms.size() != 2) continue;
        const int residual = add.bottoms[0] == mul.tops[0] ? add.bottoms[1] : add.bottoms[0];
        const Blob &residualBlob = blobs_[size_t(residual)];
        if (residualBlob.constant || residualBlob.c != shape.c || residualBlob.w != shape.w || residualBlob.h != shape.h) continue;
        const int residualRoot = root(residual, nullptr);
        if (blobs_[size_t(residualRoot)].producer >= int(i)) continue;
        const int reluIndex = only(add.tops[0]);
        if (reluIndex < 0 || layers_[size_t(reluIndex)].type != "ReLU") continue;
        Layer &relu = layers_[size_t(reluIndex)];
        conv.beta = beta.constantOffset;
        conv.residual = residual;
        conv.activation = true;
        conv.slope = relu.real(0, 0.0f);
        conv.tops = relu.tops;
        blobs_[size_t(relu.tops[0])].producer = int(i);
        mul.removed = add.removed = relu.removed = true;
    }
}

RifeD3D11Network::Location RifeD3D11Network::locate(int blob) const
{
    Location location;
    const Blob &b = blobs_[size_t(blob)];
    if (b.constant) {
        location.srv = weightSrv_.Get();
        location.offset = UINT(b.constantOffset);
        return location;
    }
    UINT channel = 0;
    const int r = root(blob, &channel);
    const Buffer &buffer = buffers_[size_t(blobs_[size_t(r)].storage)];
    location.srv = buffer.srv.Get();
    location.uav = buffer.uav.Get();
    location.offset = channel * b.h * b.w;
    return location;
}

RifeD3D11Network::TensorView RifeD3D11Network::view(int blob) const
{
    TensorView v;
    if (!built_ || blob < 0) return v;
    const Location location = locate(blob);
    const Blob &b = blobs_[size_t(blob)];
    UINT channel = 0;
    const int r = root(blob, &channel);
    v.buffer = buffers_[size_t(blobs_[size_t(r)].storage)].buffer.Get();
    v.srv = location.srv;
    v.uav = location.uav;
    v.offset = location.offset;
    v.channels = b.c;
    v.width = b.w;
    v.height = b.h;
    return v;
}

RifeD3D11Network::TensorView RifeD3D11Network::input(int index) const
{
    return view(index == 0 ? in0_ : in1_);
}

bool RifeD3D11Network::compileKernels(QString *error)
{
    static const char *const entries[KernelCount] = {"conv3x3", "deconv", "deconv4x4s2", "interp", "binary",
                                                     "activation", "copy", "pixelShuffle", "warp"};
    for (int i = 0; i < KernelCount; ++i) {
        if (!loadComputeShader(device_.Get(), ":/frc/rife_ops.hlsl", entries[i], kernels_[i].GetAddressOf(), error))
            return false;
    }
    return true;
}

bool RifeD3D11Network::addDispatch(Kernel kernel, const void *constants,
                                   std::initializer_list<ID3D11ShaderResourceView *> srvs,
                                   ID3D11UnorderedAccessView *uav, UINT gx, UINT gy, UINT gz, QString *error)
{
    Dispatch dispatch;
    dispatch.shader = kernels_[kernel].Get();
    dispatch.kernel = kernel;
    dispatch.layer = currentLayer_;
    size_t slot = 0;
    for (ID3D11ShaderResourceView *srv : srvs) dispatch.srvs[slot++] = srv;
    dispatch.uav = uav;
    dispatch.groups[0] = std::max(1u, gx);
    dispatch.groups[1] = std::max(1u, gy);
    dispatch.groups[2] = std::max(1u, gz);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(OpConstants);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{constants, 0, 0};
    const HRESULT hr = device_->CreateBuffer(&desc, &data, &dispatch.constants);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateBuffer(rife constants)"), hr);
        return false;
    }
    dispatches_.push_back(std::move(dispatch));
    return true;
}

bool RifeD3D11Network::plan(const std::vector<bool> &needed, QString *error)
{
    std::vector<int> executed;
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (needed[i] && !layers_[i].removed) executed.push_back(int(i));
    }

    // Lifetime of every root tensor, measured in executed-layer steps.
    std::vector<int> lastUse(blobs_.size(), -1);
    auto use = [&](int blob, int step) {
        if (blobs_[size_t(blob)].constant) return;
        const int r = root(blob, nullptr);
        lastUse[size_t(r)] = std::max(lastUse[size_t(r)], step);
    };
    for (size_t step = 0; step < executed.size(); ++step) {
        const Layer &layer = layers_[size_t(executed[step])];
        for (int b : layer.bottoms) use(b, int(step));
        if (layer.residual >= 0) use(layer.residual, int(step));
        for (int top : layer.tops) use(top, int(step));
    }
    for (int b : {in0_, in1_, in2_, flowA_, flowB_, mask_, image_}) {
        if (b >= 0) use(b, kKeep);
    }

    // Linear-scan buffer reuse: a freed buffer is recycled by later tensors
    // (never by the outputs of the layer that still reads it).
    struct Slot
    {
        UINT elements = 0;
        bool free = false;
    };
    std::vector<Slot> pool;
    auto allocate = [&](int blob) {
        const Blob &b = blobs_[size_t(blob)];
        const UINT elements = std::max(1u, b.c * b.h * b.w);
        int best = -1;
        int largest = -1;
        for (size_t s = 0; s < pool.size(); ++s) {
            if (!pool[s].free) continue;
            if (pool[s].elements >= elements && (best < 0 || pool[s].elements < pool[size_t(best)].elements)) best = int(s);
            if (largest < 0 || pool[s].elements > pool[size_t(largest)].elements) largest = int(s);
        }
        if (best < 0) best = largest;
        if (best < 0) {
            best = int(pool.size());
            pool.push_back(Slot());
        }
        pool[size_t(best)].free = false;
        pool[size_t(best)].elements = std::max(pool[size_t(best)].elements, elements);
        blobs_[size_t(blob)].storage = best;
    };
    for (int b : {in0_, in1_, in2_}) allocate(b);
    for (size_t step = 0; step < executed.size(); ++step) {
        const Layer &layer = layers_[size_t(executed[step])];
        if (layer.type != "Input") {
            for (int top : layer.tops) {
                const Blob &b = blobs_[size_t(top)];
                if (b.viewOf < 0 && !b.constant) allocate(top);
            }
        }
        for (size_t r = 0; r < blobs_.size(); ++r) {
            if (lastUse[r] == int(step) && blobs_[r].storage >= 0) pool[size_t(blobs_[r].storage)].free = true;
        }
    }

    buffers_.resize(pool.size());
    arenaBytes_ = 0;
    for (size_t s = 0; s < pool.size(); ++s) {
        Buffer &buffer = buffers_[s];
        buffer.elements = pool[s].elements;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = buffer.elements * 4;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = 4;
        HRESULT hr = device_->CreateBuffer(&desc, nullptr, &buffer.buffer);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(buffer.buffer.Get(), nullptr, &buffer.srv);
        if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(buffer.buffer.Get(), nullptr, &buffer.uav);
        if (FAILED(hr)) {
            if (error) *error = hresultText(QStringLiteral("CreateBuffer(rife tensor, %1 MB)").arg(desc.ByteWidth >> 20), hr);
            return false;
        }
        arenaBytes_ += desc.ByteWidth;
    }

    for (int index : executed) {
        const Layer &layer = layers_[size_t(index)];
        const std::string &type = layer.type;
        currentLayer_ = layer.name;
        if (type == "Input" || type == "MemoryData" || type == "Split" || type == "Crop") continue;
        const Blob &out = blobs_[size_t(layer.tops[0])];
        if (out.viewOf >= 0) continue; // identity Interp
        const Location o = locate(layer.tops[0]);
        OpConstants k{};
        auto setTensor = [&](int slot, int blob) {
            const Blob &b = blobs_[size_t(blob)];
            const Location l = locate(blob);
            k.p[slot * 4 + 0] = b.w;
            k.p[slot * 4 + 1] = b.h;
            k.p[slot * 4 + 2] = b.c;
            k.p[slot * 4 + 3] = l.offset;
        };
        bool ok = true;
        if (type == "Convolution") {
            setTensor(0, layer.bottoms[0]);
            setTensor(1, layer.tops[0]);
            k.p[8] = uint32_t(layer.weights);
            k.p[9] = layer.bias == size_t(-1) ? kNone : uint32_t(layer.bias);
            k.p[10] = layer.beta == size_t(-1) ? kNone : uint32_t(layer.beta);
            ID3D11ShaderResourceView *residual = nullptr;
            k.p[11] = kNone;
            if (layer.residual >= 0) {
                const Location r = locate(layer.residual);
                residual = r.srv;
                k.p[11] = r.offset;
            }
            k.p[12] = UINT(layer.integer(3, 1));
            // ncnn activation 1 = ReLU, 2 = leaky ReLU (slope in array 10).
            const int activation = layer.integer(9, 0);
            const auto slope = layer.arrays.find(10);
            k.p[13] = (layer.activation || activation != 0) ? 1u : 0u;
            if (layer.activation) {
                k.f[0] = layer.slope;
            } else if (activation == 2 && slope != layer.arrays.end() && !slope->second.empty()) {
                k.f[0] = slope->second[0];
            }
            ok = addDispatch(Conv, &k, {locate(layer.bottoms[0]).srv, weightSrv_.Get(), residual}, o.uav,
                             ceilDiv(out.w, 16), ceilDiv(out.h, 8), ceilDiv(out.c, 16), error);
        } else if (type == "Deconvolution") {
            setTensor(0, layer.bottoms[0]);
            setTensor(1, layer.tops[0]);
            k.p[8] = uint32_t(layer.weights);
            k.p[9] = layer.bias == size_t(-1) ? kNone : uint32_t(layer.bias);
            k.p[10] = UINT(layer.integer(1, 0));
            k.p[11] = UINT(layer.integer(3, 1));
            k.p[12] = UINT(layer.integer(4, 0));
            const Blob &in = blobs_[size_t(layer.bottoms[0])];
            if (k.p[10] == 4 && k.p[11] == 2 && k.p[12] == 1) {
                ok = addDispatch(Deconv4x4s2, &k, {locate(layer.bottoms[0]).srv, weightSrv_.Get()}, o.uav,
                                 ceilDiv(in.w, 8), ceilDiv(in.h, 8), ceilDiv(out.c, 8), error);
            } else {
                ok = addDispatch(Deconv, &k, {locate(layer.bottoms[0]).srv, weightSrv_.Get()}, o.uav,
                                 ceilDiv(out.w, 8), ceilDiv(out.h, 8), ceilDiv(out.c, 8), error);
            }
        } else if (type == "Interp") {
            setTensor(0, layer.bottoms[0]);
            setTensor(1, layer.tops[0]);
            ok = addDispatch(Interp, &k, {locate(layer.bottoms[0]).srv}, o.uav,
                             ceilDiv(out.w, 8), ceilDiv(out.h, 8), out.c, error);
        } else if (type == "BinaryOp" || type == "Eltwise") {
            setTensor(0, layer.bottoms[0]);
            ID3D11ShaderResourceView *b = nullptr;
            if (type == "Eltwise") {
                setTensor(1, layer.bottoms[1]);
                b = locate(layer.bottoms[1]).srv;
                k.p[12] = 100;
                const auto coefficients = layer.arrays.find(1);
                const bool weighted = coefficients != layer.arrays.end() && coefficients->second.size() >= 2;
                k.f[1] = weighted ? coefficients->second[0] : 1.0f;
                k.f[2] = weighted ? coefficients->second[1] : 1.0f;
            } else {
                k.p[12] = UINT(layer.integer(0, 0));
                if (k.p[12] == 6 || k.p[12] > 8) {
                    if (error) *error = QStringLiteral("RIFE model %1: unsupported binary operation").arg(modelName_);
                    return false;
                }
                if (layer.integer(1, 0) != 0 || layer.bottoms.size() == 1) {
                    k.p[13] = 1;
                    k.f[0] = layer.real(2, 0.0f);
                } else {
                    setTensor(1, layer.bottoms[1]);
                    b = locate(layer.bottoms[1]).srv;
                }
            }
            setTensor(2, layer.tops[0]);
            ok = addDispatch(Binary, &k, {locate(layer.bottoms[0]).srv, b}, o.uav,
                             ceilDiv(out.w, 8), ceilDiv(out.h, 8), out.c, error);
        } else if (type == "ReLU" || type == "Sigmoid") {
            setTensor(0, layer.bottoms[0]);
            setTensor(1, layer.tops[0]);
            k.p[12] = type == "Sigmoid" ? 1u : 0u;
            k.f[0] = type == "ReLU" ? layer.real(0, 0.0f) : 0.0f;
            ok = addDispatch(Activation, &k, {locate(layer.bottoms[0]).srv}, o.uav,
                             ceilDiv(out.w, 8), ceilDiv(out.h, 8), out.c, error);
        } else if (type == "Concat") {
            UINT channel = 0;
            for (int bottom : layer.bottoms) {
                const Blob &in = blobs_[size_t(bottom)];
                OpConstants c{};
                const Location l = locate(bottom);
                c.p[0] = in.w;
                c.p[1] = in.h;
                c.p[2] = in.c;
                c.p[3] = l.offset;
                c.p[4] = out.w;
                c.p[5] = out.h;
                c.p[6] = out.c;
                c.p[7] = o.offset + channel * out.h * out.w;
                channel += in.c;
                if (!addDispatch(Copy, &c, {l.srv}, o.uav, ceilDiv(in.w, 8), ceilDiv(in.h, 8), in.c, error)) return false;
            }
        } else if (type == "PixelShuffle") {
            setTensor(0, layer.bottoms[0]);
            setTensor(1, layer.tops[0]);
            k.p[12] = UINT(layer.integer(0, 1));
            ok = addDispatch(PixelShuffle, &k, {locate(layer.bottoms[0]).srv}, o.uav,
                             ceilDiv(out.w, 8), ceilDiv(out.h, 8), out.c, error);
        } else if (type == "rife.Warp") {
            setTensor(0, layer.bottoms[0]);
            setTensor(1, layer.bottoms[1]);
            setTensor(2, layer.tops[0]);
            ok = addDispatch(Warp, &k, {locate(layer.bottoms[0]).srv, locate(layer.bottoms[1]).srv}, o.uav,
                             ceilDiv(out.w, 8), ceilDiv(out.h, 8), out.c, error);
        }
        if (!ok) return false;
    }
    return true;
}

bool RifeD3D11Network::build(ID3D11Device *device, UINT width, UINT height, bool fullImage, QString *error)
{
    release();
    if (layers_.empty()) {
        if (error) *error = QStringLiteral("RIFE model is not loaded");
        return false;
    }
    if (!device || width == 0 || height == 0 || width % kAlignment || height % kAlignment) {
        if (error) *error = QStringLiteral("RIFE tensor size %1x%2 is not a multiple of %3").arg(width).arg(height).arg(kAlignment);
        return false;
    }
    device_ = device;
    for (Layer &layer : layers_) {
        layer.tops = layer.originalTops;
        layer.beta = size_t(-1);
        layer.residual = -1;
        layer.activation = false;
        layer.removed = false;
    }
    in0_ = in1_ = in2_ = flowA_ = flowB_ = mask_ = image_ = -1;
    if (!inferShapes(width, height, error)) {
        release();
        return false;
    }

    // The final flow feeds the last warps of in0/in1; the fusion mask is the
    // last Sigmoid. out0 is RIFE's own blend at analysis size.
    for (size_t i = 0; i < layers_.size(); ++i) {
        const Layer &layer = layers_[i];
        if (layer.type == "rife.Warp") {
            const int image = root(layer.bottoms[0], nullptr);
            if (image == in0_) flowA_ = layer.bottoms[1];
            if (image == in1_) flowB_ = layer.bottoms[1];
        } else if (layer.type == "Sigmoid") {
            mask_ = layer.tops[0];
        }
    }
    const auto out0 = blobByName_.find("out0");
    if (fullImage && out0 != blobByName_.end()) image_ = out0->second;
    if (flowA_ < 0 || flowB_ < 0 || mask_ < 0 || (fullImage && image_ < 0)) {
        if (error) *error = QStringLiteral("RIFE model %1 is not a recognised v4 flownet (flow/mask outputs not found)").arg(modelName_);
        release();
        return false;
    }

    std::vector<bool> neededBlob(blobs_.size(), false);
    std::vector<bool> needed(layers_.size(), false);
    for (int b : {flowA_, flowB_, mask_, image_}) {
        if (b >= 0) neededBlob[size_t(b)] = true;
    }
    for (size_t i = layers_.size(); i-- > 0;) {
        bool any = false;
        for (int top : layers_[i].tops) any = any || neededBlob[size_t(top)];
        if (!any) continue;
        needed[i] = true;
        for (int b : layers_[i].bottoms) neededBlob[size_t(b)] = true;
    }
    needed[size_t(blobs_[size_t(in2_)].producer)] = true;
    fuseResidualBlocks(needed);

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(std::max<size_t>(1, weights_.size()) * 4);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = 4;
    const float zero = 0.0f;
    D3D11_SUBRESOURCE_DATA data{weights_.empty() ? &zero : weights_.data(), 0, 0};
    HRESULT hr = device_->CreateBuffer(&desc, &data, &weightBuffer_);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(weightBuffer_.Get(), nullptr, &weightSrv_);
    if (FAILED(hr)) {
        if (error) *error = hresultText(QStringLiteral("CreateBuffer(rife weights)"), hr);
        release();
        return false;
    }
    if (!compileKernels(error) || !plan(needed, error)) {
        release();
        return false;
    }
    built_ = true;
    return true;
}

void RifeD3D11Network::run(ID3D11DeviceContext *context, float timestep)
{
    if (!built_) return;
    const Location t = locate(in2_);
    ID3D11Resource *timeBuffer = nullptr;
    t.srv->GetResource(&timeBuffer);
    const D3D11_BOX box{t.offset * 4, 0, 0, t.offset * 4 + 4, 1, 1};
    context->UpdateSubresource(timeBuffer, 0, &box, &timestep, 0, 0);
    timeBuffer->Release();

    ID3D11UnorderedAccessView *nullUav = nullptr;
    ID3D11ShaderResourceView *nullSrvs[3] = {nullptr, nullptr, nullptr};
    for (size_t i = 0; i < dispatches_.size(); ++i) {
        const Dispatch &d = dispatches_[i];
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShader(d.shader, nullptr, 0);
        ID3D11Buffer *constants = d.constants.Get();
        context->CSSetConstantBuffers(0, 1, &constants);
        context->CSSetShaderResources(0, 3, d.srvs);
        context->CSSetUnorderedAccessViews(0, 1, &d.uav, nullptr);
        context->Dispatch(d.groups[0], d.groups[1], d.groups[2]);
        // Submit in short batches: the GUI thread's OpenGL present (same GPU)
        // otherwise waits behind a whole inference in one DMA buffer.
        if (flushInterval_ > 0 && (i + 1) % size_t(flushInterval_) == 0) context->Flush();
    }
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context->CSSetShaderResources(0, 3, nullSrvs);
}

std::vector<RifeD3D11Network::ProfileRow> RifeD3D11Network::profile(ID3D11DeviceContext *context, float timestep)
{
    static const char *const names[KernelCount] = {"conv3x3", "deconv", "deconv4x4s2", "interp", "binary",
                                                   "activation", "copy", "pixelShuffle", "warp"};
    std::vector<ProfileRow> rows;
    if (!built_) return rows;
    const Location t = locate(in2_);
    ID3D11Resource *timeBuffer = nullptr;
    t.srv->GetResource(&timeBuffer);
    const D3D11_BOX box{t.offset * 4, 0, 0, t.offset * 4 + 4, 1, 1};
    context->UpdateSubresource(timeBuffer, 0, &box, &timestep, 0, 0);
    timeBuffer->Release();
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    device_->CreateQuery(&desc, &disjoint);
    desc.Query = D3D11_QUERY_TIMESTAMP;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> stamps(dispatches_.size() + 1);
    for (auto &q : stamps) device_->CreateQuery(&desc, &q);
    context->Begin(disjoint.Get());
    context->End(stamps[0].Get());
    ID3D11UnorderedAccessView *nullUav = nullptr;
    for (size_t i = 0; i < dispatches_.size(); ++i) {
        const Dispatch &d = dispatches_[i];
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShader(d.shader, nullptr, 0);
        ID3D11Buffer *constants = d.constants.Get();
        context->CSSetConstantBuffers(0, 1, &constants);
        context->CSSetShaderResources(0, 3, d.srvs);
        context->CSSetUnorderedAccessViews(0, 1, &d.uav, nullptr);
        context->Dispatch(d.groups[0], d.groups[1], d.groups[2]);
        context->End(stamps[i + 1].Get());
    }
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
    while (context->GetData(disjoint.Get(), &data, sizeof(data), 0) != S_OK) {}
    std::vector<UINT64> ticks(stamps.size());
    for (size_t i = 0; i < stamps.size(); ++i) {
        while (context->GetData(stamps[i].Get(), &ticks[i], sizeof(UINT64), 0) != S_OK) {}
    }
    for (size_t i = 0; i < dispatches_.size(); ++i) {
        rows.push_back({QString::fromStdString(dispatches_[i].layer), QString::fromLatin1(names[dispatches_[i].kernel]),
                         double(ticks[i + 1] - ticks[i]) * 1000.0 / double(data.Frequency)});
    }
    return rows;
}

void RifeD3D11Network::release()
{
    built_ = false;
    dispatches_.clear();
    buffers_.clear();
    weightSrv_.Reset();
    weightBuffer_.Reset();
    for (auto &kernel : kernels_) kernel.Reset();
    device_.Reset();
    arenaBytes_ = 0;
}

} // namespace frc
