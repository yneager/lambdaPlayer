#pragma once
#include <QOpenGLExtraFunctions>

// libmpv's OpenGL API requires standard state. Qt composition and glyph
// uploads can leave unpack strides or a pixel buffer bound in this context.
// A stale unpack buffer/stride corrupts subsequent video and subtitle uploads.
inline void prepareMpvOpenGL(QOpenGLExtraFunctions *gl)
{
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl->glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    gl->glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_FRAMEBUFFER_SRGB);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glDepthMask(GL_TRUE);
    gl->glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl->glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glUseProgram(0);
}
