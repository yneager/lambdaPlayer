#include "mpvvideowidget.h"
#include "mpvglstate.h"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <mpv/render_gl.h>

#include "frc/screeninterpolationcontroller.h"

MpvVideoWidget::MpvVideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setObjectName("videoSurface");
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Keep receiving frames while partially covered by the web chrome.
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    connect(this, &QOpenGLWidget::frameSwapped, this, [this] {
        const bool changed = lastPaintWasFrc_
            ? (frc_ && frc_->frameSwapped()) : nativePicture_ > 0 && nativePresentation_.swapped(double(nativePicture_));
        if (changed) emit videoFramePresented();
        if (renderContext_) {
            mpv_render_context_report_swap(renderContext_);
        }
    });
}

MpvVideoWidget::~MpvVideoWidget()
{
    shutdown();
}

void MpvVideoWidget::setMpv(mpv_handle *mpv)
{
    mpv_ = mpv;
}

void MpvVideoWidget::setFrameInterpolation(frc::ScreenInterpolationController *controller)
{
    frc_ = controller;
    connect(controller, &frc::ScreenInterpolationController::repaintRequested, this, qOverload<>(&QWidget::update));
}

QSize MpvVideoWidget::physicalSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(qRound(width() * dpr), qRound(height() * dpr));
}

void *MpvVideoWidget::getProcAddress(void *, const char *name)
{
    QOpenGLContext *glContext = QOpenGLContext::currentContext();
    if (!glContext) {
        return nullptr;
    }
    return reinterpret_cast<void *>(glContext->getProcAddress(QByteArray(name)));
}

void MpvVideoWidget::onMpvUpdate(void *ctx)
{
    // Called from an mpv thread: hop to the GUI thread before touching Qt.
    auto *widget = static_cast<MpvVideoWidget *>(ctx);
    if (!widget->frameRequestPending_.exchange(true)) {
        QMetaObject::invokeMethod(widget, "requestFrame", Qt::QueuedConnection);
    }
}

void MpvVideoWidget::requestFrame()
{
    frameRequestPending_.store(false);
    if (!renderContext_ || !(mpv_render_context_update(renderContext_) & MPV_RENDER_UPDATE_FRAME)) {
        return;
    }
    if (frc_ && frc_->isEnabled() && context()) {
        // mpv renders the new frame at the widget's native pixel size into
        // an FRC input texture; the controller schedules what is shown.
        makeCurrent();
        const bool captured = frc_->captureFrame(context(), physicalSize(), devicePixelRatioF(),
                                                 [this](unsigned int fbo, int w, int h) { renderMpv(int(fbo), w, h); });
        doneCurrent();
        if (captured) {
            return;
        }
    }
    update();
}

void MpvVideoWidget::initializeGL()
{
    if (renderContext_ || !mpv_) {
        if (!mpv_) {
            emit renderFailed("libmpv is not initialised.");
        }
        return;
    }

    prepareMpvOpenGL(context()->extraFunctions());
    mpv_opengl_init_params glInit{};
    glInit.get_proc_address = &MpvVideoWidget::getProcAddress;
    glInit.get_proc_address_ctx = nullptr;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    const int rc = mpv_render_context_create(&renderContext_, mpv_, params);
    if (rc < 0) {
        renderContext_ = nullptr;
        emit renderFailed(QString("mpv could not create its OpenGL renderer: %1")
                              .arg(QString::fromUtf8(mpv_error_string(rc))));
        return;
    }

    mpv_render_context_set_update_callback(renderContext_, &MpvVideoWidget::onMpvUpdate, this);

    // The GL context can be recreated (e.g. when the widget changes top-level);
    // the mpv render context must never outlive the context it was made for.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        if (frc_) frc_->releaseGl();
        releaseRenderContext();
    }, Qt::DirectConnection);

    emit renderReady();
}

void MpvVideoWidget::paintGL()
{
    if (!renderContext_) {
        return;
    }

    lastPaintWasFrc_ = frc_ && frc_->paint(defaultFramebufferObject(), physicalSize());
    if (lastPaintWasFrc_) return;
    mpv_render_frame_info info{};
    const mpv_render_param query{MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info};
    if (mpv_render_context_get_info(renderContext_, query) >= 0
        && (info.flags & MPV_RENDER_FRAME_INFO_PRESENT)
        && !(info.flags & (MPV_RENDER_FRAME_INFO_REDRAW | MPV_RENDER_FRAME_INFO_REPEAT))) {
        ++nativePicture_;
    }
    const QSize size = physicalSize();
    renderMpv(static_cast<int>(defaultFramebufferObject()), size.width(), size.height());
}

void MpvVideoWidget::renderMpv(int fboId, int width, int height)
{
    prepareMpvOpenGL(context()->extraFunctions());
    mpv_opengl_fbo fbo{};
    fbo.fbo = fboId;
    fbo.w = width;
    fbo.h = height;
    fbo.internal_format = 0;

    int flipY = 1;
    int blockForTargetTime = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(renderContext_, params);
}

void MpvVideoWidget::releaseRenderContext()
{
    if (!renderContext_) {
        return;
    }
    mpv_render_context_set_update_callback(renderContext_, nullptr, nullptr);
    mpv_render_context_free(renderContext_);
    renderContext_ = nullptr;
}

void MpvVideoWidget::shutdown()
{
    if (!renderContext_) {
        return;
    }
    // mpv_render_context_free() may call GL functions: the context must be current.
    const bool hasContext = context() != nullptr;
    if (hasContext) {
        makeCurrent();
        if (frc_) frc_->releaseGl();
    }
    releaseRenderContext();
    if (hasContext) {
        doneCurrent();
    }
}
