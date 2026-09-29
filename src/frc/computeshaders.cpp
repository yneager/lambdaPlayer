#include "frc/computeshaders.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <mutex>

namespace frc {

namespace {

std::mutex cacheMutex;
QHash<QString, QByteArray> &runtimeCache()
{
    static QHash<QString, QByteArray> cache;
    return cache;
}

} // namespace

bool loadComputeShader(ID3D11Device *device, const char *hlslResource, const char *entry,
                       ID3D11ComputeShader **shader, QString *error)
{
    const QString resource = QString::fromLatin1(hlslResource);
    const QString key = QFileInfo(resource).completeBaseName() + QLatin1Char('_') + QString::fromLatin1(entry);
    QByteArray bytecode;
    QFile precompiled(QStringLiteral(":/frc/cso/") + key + QStringLiteral(".cso"));
    if (precompiled.open(QIODevice::ReadOnly)) {
        bytecode = precompiled.readAll();
    } else {
        std::lock_guard lock(cacheMutex);
        bytecode = runtimeCache().value(key);
        if (bytecode.isEmpty()) {
            QFile file(resource);
            if (!file.open(QIODevice::ReadOnly)) {
                if (error) *error = QStringLiteral("Could not read compute shader %1").arg(resource);
                return false;
            }
            const QByteArray source = file.readAll();
            Microsoft::WRL::ComPtr<ID3DBlob> blob;
            Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
            const HRESULT hr = D3DCompile(source.constData(), size_t(source.size()), hlslResource, nullptr, nullptr,
                                          entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &diagnostics);
            if (FAILED(hr)) {
                if (error) {
                    *error = QStringLiteral("Compute shader %1 (%2): %3").arg(resource, QString::fromLatin1(entry),
                        diagnostics ? QString::fromUtf8(static_cast<const char *>(diagnostics->GetBufferPointer()),
                                                        int(diagnostics->GetBufferSize())).trimmed()
                                    : QStringLiteral("D3DCompile failed (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0')));
                }
                return false;
            }
            bytecode = QByteArray(static_cast<const char *>(blob->GetBufferPointer()), int(blob->GetBufferSize()));
            runtimeCache().insert(key, bytecode);
        }
    }
    const HRESULT hr = device->CreateComputeShader(bytecode.constData(), size_t(bytecode.size()), nullptr, shader);
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("CreateComputeShader(%1) failed (0x%2)").arg(key).arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

} // namespace frc
