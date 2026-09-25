#include "frc/wgldxinterop.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <cstring>

#include <windows.h>
#include <d3d11.h>

namespace frc {

namespace {

// WGL_NV_DX_interop entry points and tokens (Khronos registry).
using PfnOpenDevice = HANDLE(WINAPI *)(void *dxDevice);
using PfnCloseDevice = BOOL(WINAPI *)(HANDLE device);
using PfnRegisterObject = HANDLE(WINAPI *)(HANDLE device, void *dxObject, GLuint name, GLenum type, GLenum access);
using PfnUnregisterObject = BOOL(WINAPI *)(HANDLE device, HANDLE object);
using PfnLockObjects = BOOL(WINAPI *)(HANDLE device, GLint count, HANDLE *objects);
using PfnUnlockObjects = BOOL(WINAPI *)(HANDLE device, GLint count, HANDLE *objects);
using PfnGetExtensionsString = const char *(WINAPI *)(HDC);
constexpr GLenum kAccessReadWrite = 0x0001; // WGL_ACCESS_READ_WRITE_NV

struct Api
{
    PfnOpenDevice openDevice = nullptr;
    PfnCloseDevice closeDevice = nullptr;
    PfnRegisterObject registerObject = nullptr;
    PfnUnregisterObject unregisterObject = nullptr;
    PfnLockObjects lockObjects = nullptr;
    PfnUnlockObjects unlockObjects = nullptr;

    bool resolve(QOpenGLContext *context)
    {
        auto get = [context](const char *name) { return reinterpret_cast<void *>(context->getProcAddress(name)); };
        openDevice = reinterpret_cast<PfnOpenDevice>(get("wglDXOpenDeviceNV"));
        closeDevice = reinterpret_cast<PfnCloseDevice>(get("wglDXCloseDeviceNV"));
        registerObject = reinterpret_cast<PfnRegisterObject>(get("wglDXRegisterObjectNV"));
        unregisterObject = reinterpret_cast<PfnUnregisterObject>(get("wglDXUnregisterObjectNV"));
        lockObjects = reinterpret_cast<PfnLockObjects>(get("wglDXLockObjectsNV"));
        unlockObjects = reinterpret_cast<PfnUnlockObjects>(get("wglDXUnlockObjectsNV"));
        return openDevice && closeDevice && registerObject && unregisterObject && lockObjects && unlockObjects;
    }
};

Api &api()
{
    static Api instance;
    return instance;
}

} // namespace

WglDxInterop::~WglDxInterop()
{
    close();
}

bool WglDxInterop::isSupported(QOpenGLContext *context)
{
    if (!context) return false;
    auto getExtensions = reinterpret_cast<PfnGetExtensionsString>(context->getProcAddress("wglGetExtensionsStringARB"));
    if (!getExtensions) return false;
    const char *extensions = getExtensions(wglGetCurrentDC());
    return extensions && std::strstr(extensions, "WGL_NV_DX_interop2") && api().resolve(context);
}

bool WglDxInterop::open(QOpenGLContext *context, ID3D11Device *device, QString *error)
{
    close();
    if (!isSupported(context)) {
        if (error) *error = QStringLiteral("The OpenGL driver does not support WGL_NV_DX_interop2");
        return false;
    }
    context_ = context;
    device_ = api().openDevice(device);
    if (!device_) {
        if (error) *error = QStringLiteral("wglDXOpenDeviceNV failed (error %1); the video and Direct3D devices may be on different GPUs")
                                .arg(GetLastError());
        return false;
    }
    return true;
}

void WglDxInterop::close()
{
    if (!device_) {
        textures_.clear();
        return;
    }
    auto *gl = context_->extraFunctions();
    for (auto it = textures_.begin(); it != textures_.end(); ++it) {
        api().unregisterObject(device_, it->handle);
        gl->glDeleteFramebuffers(1, &it->glFramebuffer);
        gl->glDeleteTextures(1, &it->glTexture);
    }
    textures_.clear();
    api().closeDevice(device_);
    device_ = nullptr;
}

const WglDxInterop::Texture *WglDxInterop::textureFor(ID3D11Texture2D *texture, QString *error)
{
    auto it = textures_.find(texture);
    if (it != textures_.end()) return &it.value();

    auto *gl = context_->extraFunctions();
    Texture entry;
    gl->glGenTextures(1, &entry.glTexture);
    entry.handle = api().registerObject(device_, texture, entry.glTexture, GL_TEXTURE_2D, kAccessReadWrite);
    if (!entry.handle) {
        if (error) *error = QStringLiteral("wglDXRegisterObjectNV failed (error %1)").arg(GetLastError());
        gl->glDeleteTextures(1, &entry.glTexture);
        return nullptr;
    }
    // The framebuffer attachment is only valid while the object is locked.
    if (!api().lockObjects(device_, 1, &entry.handle)) {
        if (error) *error = QStringLiteral("wglDXLockObjectsNV failed (error %1)").arg(GetLastError());
        api().unregisterObject(device_, entry.handle);
        gl->glDeleteTextures(1, &entry.glTexture);
        return nullptr;
    }
    GLint previous = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glGenFramebuffers(1, &entry.glFramebuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, entry.glFramebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry.glTexture, 0);
    const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous));
    api().unlockObjects(device_, 1, &entry.handle);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) *error = QStringLiteral("Interop framebuffer incomplete (0x%1)").arg(status, 0, 16);
        api().unregisterObject(device_, entry.handle);
        gl->glDeleteFramebuffers(1, &entry.glFramebuffer);
        gl->glDeleteTextures(1, &entry.glTexture);
        return nullptr;
    }
    return &textures_.insert(texture, entry).value();
}

void WglDxInterop::release(ID3D11Texture2D *texture)
{
    auto it = textures_.find(texture);
    if (it == textures_.end()) return;
    auto *gl = context_->extraFunctions();
    api().unregisterObject(device_, it->handle);
    gl->glDeleteFramebuffers(1, &it->glFramebuffer);
    gl->glDeleteTextures(1, &it->glTexture);
    textures_.erase(it);
}

bool WglDxInterop::lock(const Texture *texture)
{
    HANDLE handle = texture->handle;
    return api().lockObjects(device_, 1, &handle) != FALSE;
}

void WglDxInterop::unlock(const Texture *texture)
{
    HANDLE handle = texture->handle;
    api().unlockObjects(device_, 1, &handle);
}

} // namespace frc
