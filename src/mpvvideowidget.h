#pragma once

#include <QOpenGLWidget>
#include "videopresentation.h"
#include <QString>
#include <mpv/client.h>
#include <atomic>

struct mpv_render_context;

namespace frc {
class ScreenInterpolationController;
}

// Video surface for libmpv's official render API (vo=libmpv, OpenGL).
//
// mpv still decodes, times and renders every frame (including subtitles/OSD
// and the RIFE VapourSynth filter); this widget only hands mpv an OpenGL
// framebuffer to draw into. Rendering inside Qt's own compositor, instead of
// into a separate native child window (`wid`), is what allows the translucent
// Vui web chrome to sit on top of the video and the window to have rounded
// corners.
class MpvVideoWidget final : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit MpvVideoWidget(QWidget *parent = nullptr);
    ~MpvVideoWidget() override;

    // Must be called once, after mpv_initialize() and before the widget is
    // first shown.
    void setMpv(mpv_handle *mpv);

    bool isRenderReady() const { return renderContext_ != nullptr; }

    // Post-render AMD FRC: while it is enabled, every new mpv frame is
    // rendered into its input texture and it decides what to draw.
    void setFrameInterpolation(frc::ScreenInterpolationController *controller);

    // Frees the mpv render context. Must run before mpv_terminate_destroy().
    void shutdown();

signals:
    void renderReady();
    void videoFramePresented();
    void renderFailed(const QString &reason);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    static void onMpvUpdate(void *ctx);
    static void *getProcAddress(void *ctx, const char *name);
    Q_INVOKABLE void requestFrame();
    void releaseRenderContext();
    void renderMpv(int fbo, int width, int height);
    QSize physicalSize() const;

    mpv_handle *mpv_ = nullptr;
    mpv_render_context *renderContext_ = nullptr;
    std::atomic_bool frameRequestPending_{false};
    frc::ScreenInterpolationController *frc_ = nullptr;
    bool lastPaintWasFrc_ = false;
    quint64 nativePicture_ = 0;
    VideoPresentationTracker nativePresentation_;
};
