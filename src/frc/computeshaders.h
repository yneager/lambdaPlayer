#pragma once

// Compute-shader loading for the FRC backends. Build-time fxc output is
// embedded as :/frc/cso/<hlsl base name>_<entry>.cso; if it is missing the
// HLSL resource is compiled at runtime once per process (the RIFE kernels
// take many seconds to compile, so this is a slow fallback only).

#include <QString>

struct ID3D11Device;
struct ID3D11ComputeShader;

namespace frc {

bool loadComputeShader(ID3D11Device *device, const char *hlslResource, const char *entry,
                       ID3D11ComputeShader **shader, QString *error);

} // namespace frc
