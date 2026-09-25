#pragma once

// OpenGL <-> Direct3D 11 texture sharing through the WGL_NV_DX_interop2
// extension (Khronos registry: WGL_NV_DX_interop / WGL_NV_DX_interop2; AMD
// and NVIDIA drivers implement it). Used so mpv (which only renders through
// OpenGL in libmpv's render API) can draw into textures AMF FRC reads, and
// so Qt's OpenGL widget can draw AMF's output. No pixel data leaves the GPU.
//
// All calls must be made with the owning OpenGL context current.

#include <QHash>
#include <QString>

struct ID3D11Device;
struct ID3D11Texture2D;
class QOpenGLContext;

namespace frc {

class WglDxInterop
{
public:
    struct Texture
    {
        unsigned int glTexture = 0;
        unsigned int glFramebuffer = 0; // colour attachment = glTexture
        void *handle = nullptr;         // wglDXRegisterObjectNV handle
    };

    WglDxInterop() = default;
    ~WglDxInterop();
    WglDxInterop(const WglDxInterop &) = delete;
    WglDxInterop &operator=(const WglDxInterop &) = delete;

    // Resolves the extension from `context` and opens `device` for interop.
    bool open(QOpenGLContext *context, ID3D11Device *device, QString *error);
    // Unregisters every texture and closes the device.
    void close();
    bool isOpen() const { return device_ != nullptr; }

    // Registered GL view (texture + framebuffer) of `texture`; registration
    // happens once per texture and is cached.
    const Texture *textureFor(ID3D11Texture2D *texture, QString *error);
    void release(ID3D11Texture2D *texture);

    bool lock(const Texture *texture);
    void unlock(const Texture *texture);

    // The GL context must be current and must belong to a GPU that can open
    // D3D11 devices through the extension.
    static bool isSupported(QOpenGLContext *context);

private:
    QOpenGLContext *context_ = nullptr;
    void *device_ = nullptr;
    QHash<ID3D11Texture2D *, Texture> textures_;
};

} // namespace frc
