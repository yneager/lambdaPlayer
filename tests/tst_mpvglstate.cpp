#include "mpvglstate.h"
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QtTest>
#include <array>

class MpvGlStateTest : public QObject
{
    Q_OBJECT
private slots:
    void dirtyTransfersDoNotCorruptVideo() {
        QOpenGLContext context;
        if (!context.create()) QSKIP("OpenGL context unavailable on this platform plugin");
        if (context.format().majorVersion() < 3) QSKIP("OpenGL 3 transfer support unavailable");
        QOffscreenSurface surface;
        surface.setFormat(context.format());
        surface.create();
        QVERIFY(context.makeCurrent(&surface));
        auto *gl = context.extraFunctions();
        gl->initializeOpenGLFunctions();
        GLuint buffer = 0, texture = 0, fbo = 0;
        gl->glGenBuffers(1, &buffer);
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, 64, nullptr, GL_STREAM_DRAW);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 1024);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
        gl->glPixelStorei(GL_PACK_ROW_LENGTH, 1024);
        gl->glEnable(GL_SCISSOR_TEST);
        gl->glScissor(0, 0, 1, 1);
        gl->glEnable(GL_BLEND);
        gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        prepareMpvOpenGL(gl);
        QVERIFY(!gl->glIsEnabled(GL_SCISSOR_TEST));
        QVERIFY(!gl->glIsEnabled(GL_BLEND));
        std::array<unsigned char, 64> source{}, actual{};
        for (size_t i = 0; i < source.size(); i += 4) {
            source[i] = 102; source[i+1] = 153; source[i+2] = 204; source[i+3] = 255;
        }
        gl->glGenTextures(1, &texture);
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, source.data());
        QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        QCOMPARE(gl->glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        gl->glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(actual == source);
        gl->glDeleteFramebuffers(1, &fbo);
        gl->glDeleteTextures(1, &texture);
        gl->glDeleteBuffers(1, &buffer);
        context.doneCurrent();
    }
};
QTEST_MAIN(MpvGlStateTest)
#include "tst_mpvglstate.moc"
