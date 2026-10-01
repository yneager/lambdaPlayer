// frc_bench: offline tests of LAMBDA's post-render frame interpolators on
// saved frames, at native resolution, through the same FrameInterpolator
// interface the player uses. Not shipped with the app.
//
//   frc_bench validate <model dir> <w> <h> <a.f32> <b.f32> <out.f32>
//       Full RIFE graph (its own analysis-size blend) on raw planar float RGB
//       inputs; compare against tools/rife-reference.py (VapourSynth plugin).
//   frc_bench interp <backend> <A.png> <B.png> <out.png> [options]
//       backend: rife | block | amf. Writes the t=0.5 frame at A's size.
//       --truth G.png      report PSNR against the true middle frame
//       --model DIR        RIFE model directory
//       --analysis H       RIFE analysis height
//       --repeat N         time N interpolations (GPU timestamp, synchronized)
//   frc_bench synth <source.png> <out dir> <dx> <dy>
//       Known-motion triplet (pan + fast occluder + thin lines + text).
//   frc_bench grid <out.png> <x> <y> <w> <h> <scale> <label=image.png>...
//       Side-by-side crops for visual review.

#include "frc/amffrcinterpolator.h"
#include "frc/genericd3d11fruc.h"
#include "frc/rifed3d11.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int fail(const QString &message)
{
    std::fprintf(stderr, "error: %s\n", qPrintable(message));
    return 1;
}

bool createDevice(ComPtr<ID3D11Device> *device, ComPtr<ID3D11DeviceContext> *context)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, device->GetAddressOf(), nullptr, context->GetAddressOf()))) {
        return false;
    }
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED((*device)->QueryInterface(IID_PPV_ARGS(&multithread)))) multithread->SetMultithreadProtected(TRUE);
    return true;
}

// Waits for all submitted GPU work and returns the elapsed GPU time of the
// work recorded by `record` in milliseconds.
double gpuTime(ID3D11Device *device, ID3D11DeviceContext *context, const std::function<void()> &record)
{
    ComPtr<ID3D11Query> disjoint, begin, end;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    device->CreateQuery(&desc, &disjoint);
    desc.Query = D3D11_QUERY_TIMESTAMP;
    device->CreateQuery(&desc, &begin);
    device->CreateQuery(&desc, &end);
    context->Begin(disjoint.Get());
    context->End(begin.Get());
    record();
    context->End(end.Get());
    context->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    while (context->GetData(disjoint.Get(), &d, sizeof(d), 0) != S_OK) {}
    UINT64 t0 = 0, t1 = 0;
    while (context->GetData(begin.Get(), &t0, sizeof(t0), 0) != S_OK) {}
    while (context->GetData(end.Get(), &t1, sizeof(t1), 0) != S_OK) {}
    return d.Disjoint ? -1.0 : double(t1 - t0) * 1000.0 / double(d.Frequency);
}

QImage readTexture(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return {};
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    QImage image(int(desc.Width), int(desc.Height), rgba ? QImage::Format_RGBA8888 : QImage::Format_ARGB32);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(image.scanLine(int(y)), static_cast<const uchar *>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    }
    context->Unmap(staging.Get(), 0);
    return image.convertToFormat(QImage::Format_RGB32);
}

double psnr(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return -1.0;
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const int dr = qRed(pa[x]) - qRed(pb[x]);
            const int dg = qGreen(pa[x]) - qGreen(pb[x]);
            const int db = qBlue(pa[x]) - qBlue(pb[x]);
            sum += double(dr * dr + dg * dg + db * db);
        }
    }
    const double mse = sum / (3.0 * a.width() * a.height());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

QImage blend(const QImage &a, const QImage &b, double t = 0.5)
{
    QImage out(a.size(), QImage::Format_RGB32);
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        QRgb *po = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            po[x] = qRgb(qRound(qRed(pa[x]) * (1 - t) + qRed(pb[x]) * t),
                         qRound(qGreen(pa[x]) * (1 - t) + qGreen(pb[x]) * t),
                         qRound(qBlue(pa[x]) * (1 - t) + qBlue(pb[x]) * t));
        }
    }
    return out;
}

bool readRaw(const QString &path, std::vector<float> *data, size_t count)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != qint64(count * 4)) return false;
    data->resize(count);
    return file.read(reinterpret_cast<char *>(data->data()), qint64(count * 4)) == qint64(count * 4);
}

int validate(const QStringList &args)
{
    if (args.size() < 6) return fail(QStringLiteral("validate <model> <w> <h> <a.f32> <b.f32> <out.f32>"));
    const UINT w = args[1].toUInt(), h = args[2].toUInt();
    const UINT pw = (w + 63) / 64 * 64, ph = (h + 63) / 64 * 64;
    std::vector<float> a, b;
    if (!readRaw(args[3], &a, size_t(3) * w * h) || !readRaw(args[4], &b, size_t(3) * w * h))
        return fail(QStringLiteral("could not read the raw inputs"));
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createDevice(&device, &context)) return fail(QStringLiteral("no D3D11 device"));
    frc::RifeD3D11Network net;
    QString error;
    if (!net.load(args[0], &error) || !net.build(device.Get(), pw, ph, true, &error)) return fail(error);
    for (int i = 0; i < 2; ++i) {
        const std::vector<float> &src = i == 0 ? a : b;
        std::vector<float> padded(size_t(3) * pw * ph, 0.0f);
        for (UINT c = 0; c < 3; ++c)
            for (UINT y = 0; y < h; ++y)
                std::memcpy(&padded[(c * ph + y) * pw], &src[(c * h + y) * w], w * 4);
        const frc::RifeD3D11Network::TensorView in = net.input(i);
        const D3D11_BOX box{in.offset * 4, 0, 0, UINT((in.offset + padded.size()) * 4), 1, 1};
        context->UpdateSubresource(in.buffer, 0, &box, padded.data(), 0, 0);
    }
    const double ms = gpuTime(device.Get(), context.Get(), [&] { net.run(context.Get(), 0.5f); });
    const frc::RifeD3D11Network::TensorView out = net.image();
    D3D11_BUFFER_DESC desc{};
    out.buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    device->CreateBuffer(&desc, nullptr, &staging);
    context->CopyResource(staging.Get(), out.buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    const float *result = static_cast<const float *>(mapped.pData) + out.offset;
    std::vector<float> cropped(size_t(3) * w * h);
    for (UINT c = 0; c < 3; ++c)
        for (UINT y = 0; y < h; ++y)
            std::memcpy(&cropped[(c * h + y) * w], &result[(c * ph + y) * pw], w * 4);
    context->Unmap(staging.Get(), 0);
    QFile file(args[5]);
    if (!file.open(QIODevice::WriteOnly)) return fail(QStringLiteral("cannot write output"));
    file.write(reinterpret_cast<const char *>(cropped.data()), qint64(cropped.size() * 4));
    std::printf("model %s  tensor %ux%u  dispatches %d  arena %.1f MB  gpu %.2f ms (first run)\n",
                qPrintable(net.modelName()), pw, ph, net.dispatchCount(), net.arenaBytes() / 1048576.0, ms);
    return 0;
}

int interp(const QStringList &args)
{
    if (args.size() < 5) return fail(QStringLiteral("interp <rife|rife-reuse|block|amf> <A.png> <B.png> <out.png> [options]"));
    const QString backendName = args[1];
    QImage a = QImage(args[2]).convertToFormat(QImage::Format_ARGB32);
    QImage b = QImage(args[3]).convertToFormat(QImage::Format_ARGB32);
    if (a.isNull() || b.isNull() || a.size() != b.size()) return fail(QStringLiteral("cannot read matching A/B images"));
    QString truthPath, model;
    QStringList history;
    int analysis = 0, repeat = 0, quality = 1;
    double outputFps = 48.0, targetTime = 0.5;
    for (int i = 5; i + 1 < args.size(); i += 2) {
        if (args[i] == QLatin1String("--truth")) truthPath = args[i + 1];
        else if (args[i] == QLatin1String("--model")) model = args[i + 1];
        else if (args[i] == QLatin1String("--analysis")) analysis = args[i + 1].toInt();
        else if (args[i] == QLatin1String("--quality")) quality = args[i + 1].toInt();
        else if (args[i] == QLatin1String("--repeat")) repeat = args[i + 1].toInt();
        else if (args[i] == QLatin1String("--fps")) outputFps = args[i + 1].toDouble();
        else if (args[i] == QLatin1String("--time")) targetTime = args[i + 1].toDouble();
        else if (args[i] == QLatin1String("--pre")) history = args[i + 1].split(QLatin1Char(','));
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createDevice(&device, &context)) return fail(QStringLiteral("no D3D11 device"));

    std::unique_ptr<frc::FrameInterpolator> backend;
    frc::GenericD3D11Fruc *generic = nullptr;
    if (backendName == QLatin1String("amf")) {
        backend = std::make_unique<frc::AmfFrcInterpolator>();
    } else {
        auto g = std::make_unique<frc::GenericD3D11Fruc>();
        g->setMotion(backendName == QLatin1String("block") ? frc::GenericD3D11Fruc::Motion::Block
                   : backendName == QLatin1String("rife-reuse") ? frc::GenericD3D11Fruc::Motion::RifeReusable
                   : frc::GenericD3D11Fruc::Motion::Rife);
        if (!model.isEmpty()) g->setModelDirectory(model);
        if (analysis > 0) g->setAnalysisHeight(analysis);
        g->setFastQuality(std::clamp(quality, 0, 2));
        generic = g.get();
        backend = std::move(g);
    }
    QString error;
    frc::FrcSettings settings;
    if (!backend->open(device.Get(), &error)) return fail(error);
    const bool initialized = generic ? generic->initialize(a.width(), a.height(), settings, 1, 24.0, outputFps, &error)
                                     : backend->initialize(a.width(), a.height(), settings, 1, &error);
    if (!initialized) return fail(error);

    auto upload = [&](const QImage &image, QString *why) {
        frc::GpuFrame frame = backend->allocateInput(why);
        if (frame.texture) context->UpdateSubresource(frame.texture, 0, nullptr, image.constBits(), UINT(image.bytesPerLine()), 0);
        return frame;
    };
    // Earlier frames (--pre) give history-dependent backends (AMF) a warm
    // start; the A/B midpoint is then content index history + 0.5.
    std::vector<QImage> earlier;
    for (const QString &path : history) earlier.push_back(QImage(path).convertToFormat(QImage::Format_ARGB32));
    const double base = double(earlier.size());
    QImage result;
    bool wanted = false;
    auto onOutput = [&](const frc::InterpolatedFrame &output) {
        if (wanted && std::abs(output.content - (base + targetTime)) < 1e-5)
            result = readTexture(device.Get(), context.Get(), output.frame.texture);
    };
    // AMF returns the A/B midpoint after a third submission (one frame of lag).
    auto runPair = [&](bool capture) {
        backend->flush();
        wanted = false;
        for (size_t i = 0; i < earlier.size(); ++i) {
            frc::GpuFrame f = upload(earlier[i], &error);
            if (!f.texture || !backend->processFrame(f, double(i), onOutput, &error)) return false;
        }
        wanted = capture;
        frc::GpuFrame fa = upload(a, &error);
        frc::GpuFrame fb = upload(b, &error);
        if (!fa.texture || !fb.texture) return false;
        if (!backend->processFrame(fa, base, onOutput, &error)) return false;
        bool ok = backend->processFrame(fb, base + 1.0, onOutput, &error);
        if (ok && !generic) {
            frc::GpuFrame fc = upload(b, &error);
            ok = fc.texture && backend->processFrame(fc, base + 2.0, onOutput, &error);
        }
        return ok;
    };
    if (!runPair(true)) return fail(error);
    if (result.isNull()) return fail(QStringLiteral("backend produced no frame at the requested time"));
    result.save(args[4]);

    std::printf("backend %s", qPrintable(backend->name()));
    if (generic && generic->motion() != frc::GenericD3D11Fruc::Motion::Block) {
        std::printf("  model %s  analysis %dx%d", qPrintable(generic->modelName()), generic->analysisSize().width(),
                    generic->analysisSize().height());
    }
    std::printf("  output %dx%d\n", result.width(), result.height());
    if (!truthPath.isEmpty()) {
        const QImage truth = QImage(truthPath).convertToFormat(QImage::Format_RGB32);
        const QImage ra = a.convertToFormat(QImage::Format_RGB32);
        const QImage rb = b.convertToFormat(QImage::Format_RGB32);
        std::printf("PSNR vs truth: result %.2f dB | blend %.2f dB | repeat A %.2f dB\n", psnr(result, truth),
                    psnr(blend(ra, rb, targetTime), truth), psnr(ra, truth));
    }
    if (repeat > 0) {
        // Upload both frames first, then time only the interpolating call.
        std::vector<double> gpu;
        QElapsedTimer cpu;
        double cpuTotal = 0.0;
        for (int i = 0; i < repeat; ++i) {
            backend->flush();
            wanted = false;
            frc::GpuFrame fa = upload(a, &error), fb = upload(b, &error), fc;
            if (!generic) fc = upload(b, &error);
            if (!backend->processFrame(fa, 0.0, onOutput, &error)) return fail(error);
            if (!generic && !backend->processFrame(fb, 1.0, onOutput, &error)) return fail(error);
            context->Flush();
            gpuTime(device.Get(), context.Get(), [] {});
            bool ok = true;
            gpu.push_back(gpuTime(device.Get(), context.Get(), [&] {
                cpu.start();
                ok = generic ? backend->processFrame(fb, 1.0, onOutput, &error)
                             : backend->processFrame(fc, 2.0, onOutput, &error);
                cpuTotal += double(cpu.nsecsElapsed()) / 1e6;
            }));
            if (!ok) return fail(error);
        }
        if (generic) {
            generic->setStageTiming(true);
            backend->flush();
            frc::GpuFrame fa = upload(a, &error), fb = upload(b, &error);
            backend->processFrame(fa, 0.0, onOutput, &error);
            backend->processFrame(fb, 1.0, onOutput, &error);
            for (const auto &[stage, ms] : generic->takeStageTimes()) std::printf("  stage %-14s %6.2f ms\n", qPrintable(stage), ms);
            generic->setStageTiming(false);
        }
        std::sort(gpu.begin(), gpu.end());
        std::printf("interpolation GPU ms (incl. source copy): median %.2f  min %.2f  max %.2f  | CPU submit %.2f ms avg (n=%d)\n",
                    gpu[gpu.size() / 2], gpu.front(), gpu.back(), cpuTotal / repeat, repeat);
    }
    if (generic && generic->motion() == frc::GenericD3D11Fruc::Motion::RifeReusable)
        std::printf("fast pairs: static %lld  cuts %lld  refined %lld\n",
                    static_cast<long long>(generic->skippedStaticPairs()),
                    static_cast<long long>(generic->skippedCutPairs()),
                    static_cast<long long>(generic->refinedPairs()));
    backend->close();
    return 0;
}

// Known-motion triplet: the background pans by (dx, dy) per frame pair, an
// occluding card crosses fast, thin lines and text move with it. Frames are
// rendered at several times from the same scene description.
int synth(const QStringList &args)
{
    if (args.size() < 5) return fail(QStringLiteral("synth <source.png> <out dir> <dx> <dy>"));
    const QImage source = QImage(args[1]).convertToFormat(QImage::Format_RGB32);
    if (source.isNull()) return fail(QStringLiteral("cannot read source"));
    const double dx = args[3].toDouble(), dy = args[4].toDouble();
    const int marginX = 2 * int(std::ceil(std::abs(dx))) + 4, marginY = 2 * int(std::ceil(std::abs(dy))) + 4;
    const QSize size(source.width() - 2 * marginX, source.height() - 2 * marginY);
    // P2/P1 are history frames (t = -2, -1) for backends that need them.
    const char *names[] = {"P2.png", "P1.png", "A.png", "truth_010.png", "truth_020.png",
                           "truth_025.png", "truth.png", "truth_075.png", "truth_080.png", "truth_090.png", "B.png"};
    const double times[] = {-2.0, -1.0, 0.0, 0.1, 0.2, 0.25, 0.5, 0.75, 0.8, 0.9, 1.0};
    for (int i = 0; i < 11; ++i) {
        const double t = times[i];
        QImage frame(size, QImage::Format_RGB32);
        QPainter p(&frame);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-marginX + dx * t, -marginY + dy * t);
        p.drawImage(0, 0, source);
        p.resetTransform();
        // Fast occluder moving opposite to the pan: exposes/hides background.
        const double cardX = size.width() * 0.3 + t * size.width() * 0.08;
        const QRectF card(cardX, size.height() * 0.3, size.width() * 0.18, size.height() * 0.35);
        p.fillRect(card, QColor(30, 30, 40));
        p.setPen(QPen(QColor(240, 240, 240), 2.0));
        for (int k = 0; k < 12; ++k) {
            const double x = card.left() + 10 + k * card.width() / 12.0;
            p.drawLine(QPointF(x, card.top() + 10), QPointF(x + 30, card.bottom() - 10));
        }
        QFont font;
        font.setPixelSize(int(size.height() * 0.03));
        p.setFont(font);
        p.drawText(card.adjusted(12, 12, -12, -12), Qt::AlignBottom | Qt::AlignLeft, QStringLiteral("Subtitle-like TEXT 0123"));
        // Burned-in subtitle: static on screen while the picture pans under it
        // (mpv draws subtitles before LAMBDA captures the frame).
        QFont subtitle;
        subtitle.setPixelSize(int(size.height() * 0.045));
        subtitle.setBold(true);
        QPainterPath text;
        text.addText(QPointF(size.width() * 0.25, size.height() * 0.92), subtitle,
                     QStringLiteral("Static subtitle over a moving picture"));
        p.setPen(QPen(Qt::black, 6.0));
        p.setBrush(Qt::white);
        p.drawPath(text);
        // Thin static-to-scene line (moves with the pan) and one rotating spoke.
        p.setPen(QPen(QColor(255, 60, 60), 1.0));
        const double lx = size.width() * 0.7 + dx * t;
        p.drawLine(QPointF(lx, size.height() * 0.1 + dy * t), QPointF(lx + 200, size.height() * 0.9 + dy * t));
        p.end();
        frame.save(args[2] + QLatin1Char('/') + QLatin1String(names[i]));
    }
    std::printf("wrote %dx%d triplet\n", size.width(), size.height());
    return 0;
}

int grid(const QStringList &args)
{
    if (args.size() < 8) return fail(QStringLiteral("grid <out.png> <x> <y> <w> <h> <scale> <label=image>..."));
    const QRect crop(args[2].toInt(), args[3].toInt(), args[4].toInt(), args[5].toInt());
    const int scale = std::max(1, args[6].toInt());
    const int n = int(args.size()) - 7;
    const int cols = n <= 3 ? n : (n + 1) / 2;
    const int rows = (n + cols - 1) / cols;
    const int cellW = crop.width() * scale, cellH = crop.height() * scale + 28;
    QImage out(cols * cellW + (cols - 1) * 6, rows * cellH + (rows - 1) * 6, QImage::Format_RGB32);
    out.fill(QColor(20, 20, 20));
    QPainter p(&out);
    QFont font;
    font.setPixelSize(20);
    p.setFont(font);
    for (int i = 0; i < n; ++i) {
        const QString item = args[7 + i];
        const int eq = item.indexOf(QLatin1Char('='));
        const QImage image(item.mid(eq + 1));
        const int x = (i % cols) * (cellW + 6), y = (i / cols) * (cellH + 6);
        p.setPen(Qt::white);
        p.drawText(QRect(x + 4, y, cellW, 26), Qt::AlignVCenter, item.left(eq));
        p.drawImage(QRect(x, y + 28, cellW, cellH - 28), image.copy(crop).scaled(crop.size() * scale));
    }
    p.end();
    return out.save(args[1]) ? 0 : fail(QStringLiteral("cannot write grid"));
}

// Per-dispatch GPU profile of the RIFE network at a tensor size.
int profile(const QStringList &args)
{
    if (args.size() < 4) return fail(QStringLiteral("profile <model> <padded w> <padded h> [top N]"));
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createDevice(&device, &context)) return fail(QStringLiteral("no D3D11 device"));
    frc::RifeD3D11Network net;
    QString error;
    if (!net.load(args[1], &error) || !net.build(device.Get(), args[2].toUInt(), args[3].toUInt(), false, &error))
        return fail(error);
    for (int i = 0; i < 5; ++i) net.profile(context.Get(), 0.5f);
    std::vector<double> totals;
    std::vector<frc::RifeD3D11Network::ProfileRow> rows;
    for (int i = 0; i < 10; ++i) {
        auto r = net.profile(context.Get(), 0.5f);
        if (rows.empty()) rows = r;
        else for (size_t k = 0; k < r.size(); ++k) rows[k].ms += r[k].ms;
    }
    std::map<QString, std::pair<double, int>> byKernel;
    double total = 0.0;
    for (auto &row : rows) {
        row.ms /= 10.0;
        total += row.ms;
        byKernel[row.kernel].first += row.ms;
        byKernel[row.kernel].second += 1;
    }
    std::printf("total %.2f ms, %zu dispatches\n", total, rows.size());
    for (const auto &[kernel, v] : byKernel) std::printf("  %-12s %7.2f ms  (%d)\n", qPrintable(kernel), v.first, v.second);
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.ms > b.ms; });
    const int top = args.size() > 4 ? args[4].toInt() : 15;
    for (int i = 0; i < top && i < int(rows.size()); ++i)
        std::printf("  %7.3f ms  %-12s %s\n", rows[size_t(i)].ms, qPrintable(rows[size_t(i)].kernel), qPrintable(rows[size_t(i)].layer));
    return 0;
}

// PNG -> raw planar float RGB (C, H, W) at w x h, for `validate`.
int toF32(const QStringList &args)
{
    if (args.size() < 5) return fail(QStringLiteral("tof32 <in.png> <w> <h> <out.f32>"));
    const int w = args[2].toInt(), h = args[3].toInt();
    const QImage image = QImage(args[1]).scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB32);
    if (image.isNull()) return fail(QStringLiteral("cannot read image"));
    std::vector<float> data(size_t(3) * w * h);
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            data[(size_t(0) * h + y) * w + x] = qRed(row[x]) / 255.0f;
            data[(size_t(1) * h + y) * w + x] = qGreen(row[x]) / 255.0f;
            data[(size_t(2) * h + y) * w + x] = qBlue(row[x]) / 255.0f;
        }
    }
    QFile file(args[4]);
    if (!file.open(QIODevice::WriteOnly)) return fail(QStringLiteral("cannot write output"));
    file.write(reinterpret_cast<const char *>(data.data()), qint64(data.size() * 4));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) return fail(QStringLiteral("usage: frc_bench validate|interp|synth|grid ..."));
    const QString command = args.takeFirst();
    args.prepend(command);
    if (command == QLatin1String("validate")) return validate(args.mid(1));
    if (command == QLatin1String("interp")) return interp(args);
    if (command == QLatin1String("synth")) return synth(args);
    if (command == QLatin1String("grid")) return grid(args);
    if (command == QLatin1String("tof32")) return toF32(args);
    if (command == QLatin1String("profile")) return profile(args);
    return fail(QStringLiteral("unknown command ") + command);
}
