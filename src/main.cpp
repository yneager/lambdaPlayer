#include <QApplication>
#include <QIcon>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStyleFactory>
#include <QSurfaceFormat>

#include <clocale>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    // The video (QOpenGLWidget + libmpv render API) and the Qt WebEngine UI
    // share one top-level window, so both must composite through OpenGL and
    // share contexts. Both calls must happen before QApplication exists.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    // Don't block the GUI thread in SwapBuffers: with the default interval 1
    // every repaint of the window waited ~31 ms during video playback
    // (measured GUI event latency 32 ms p50; 0.06 ms with interval 0). The
    // window is composited by DWM, which still paces presentation. This also
    // lets the AMD FRC presenter show frames on time.
    // LAMBDA_SWAP_INTERVAL=1 restores the previous behaviour.
    {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(qEnvironmentVariableIsSet("LAMBDA_SWAP_INTERVAL")
                                   ? qEnvironmentVariableIntValue("LAMBDA_SWAP_INTERVAL")
                                   : 0);
        QSurfaceFormat::setDefaultFormat(format);
    }

    // Smooth scrolling: on the desktop-OpenGL path above, Chromium leaves GPU
    // rasterization off, so every newly revealed tile of the Vui pages (large
    // blurs, gradients, glows) is painted on the CPU while scrolling. Measured
    // on the Home page: 35-43% late frames and up to ~480 ms stalls without
    // it, 0-1.4% with it. Must be set before Qt WebEngine initialises. Users
    // can still override it with their own QTWEBENGINE_CHROMIUM_FLAGS
    // (e.g. --disable-gpu-rasterization).
    QByteArray chromiumFlags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!chromiumFlags.contains("gpu-rasterization")) {
        chromiumFlags = (chromiumFlags + " --enable-gpu-rasterization").trimmed();
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", chromiumFlags);
    }

    QApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");

    app.setApplicationName("LAMBDA Player");
    app.setOrganizationName("LAMBDA");
    app.setApplicationVersion(LAMBDA_VERSION);
    app.setWindowIcon(QIcon(":/branding/lambda.ico"));
    app.setStyle(QStyleFactory::create("Fusion"));

    app.setStyleSheet(R"QSS(
        QMainWindow {
            background: #050609;
        }

        QWidget {
            background: transparent;
            color: #f7f8fb;
            font-family: "Segoe UI Variable";
            font-size: 10pt;
        }

        QWidget#playerPage {
            background: #050609;
        }

        QWidget#appRoot,
        QWidget#homePage,
        QWidget#homeCanvas,
        QScrollArea#homeScroll,
        QScrollArea#homeScroll > QWidget > QWidget {
            background: #050609;
        }

        QWidget#transitionCurtain {
            background: #050609;
        }

        QScrollArea#homeScroll,
        QScrollArea#homeRail {
            border: none;
        }

        QScrollArea#homeRail,
        QScrollArea#homeRail > QWidget > QWidget,
        QWidget#homeRailContent {
            background: transparent;
        }

        QFrame#homeNav {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
                                        stop:0 rgba(24, 29, 41, 228),
                                        stop:1 rgba(9, 12, 18, 218));
            border: 1px solid rgba(255, 255, 255, 30);
            border-radius: 21px;
        }

        QLabel#homeBrand,
        QLabel#footerBrand {
            color: #b6fff6;
            font-size: 9pt;
            font-weight: 800;
            letter-spacing: 2px;
        }

        QPushButton[homeNavLink="true"] {
            color: rgba(255, 255, 255, 125);
            background: transparent;
            border: none;
            padding: 8px 7px;
            font-size: 9pt;
            font-weight: 600;
        }

        QPushButton[homeNavLink="true"]:hover,
        QPushButton[homeNavLink="true"][active="true"] {
            color: #ffffff;
        }

        QPushButton#homeOpen {
            color: #07100f;
            background: #8ff8e9;
            border: 1px solid #d1fff9;
            border-radius: 13px;
            min-height: 38px;
            padding: 0 14px;
            font-size: 8pt;
            font-weight: 800;
            letter-spacing: 1px;
        }

        QPushButton#homeOpen:hover {
            background: #b6fff6;
        }

        QPushButton[homeRound="true"],
        QPushButton#homeProfile {
            color: rgba(255,255,255,180);
            background: rgba(255,255,255,10);
            border: 1px solid rgba(255,255,255,18);
            border-radius: 13px;
            min-width: 38px;
            max-width: 38px;
            min-height: 38px;
            max-height: 38px;
            font-weight: 700;
        }

        QPushButton[homeRound="true"]:hover,
        QPushButton#homeProfile:hover {
            color: #ffffff;
            background: rgba(255,255,255,20);
            border-color: rgba(120,245,231,65);
        }

        QPushButton#homeProfile {
            color: #f5f7fb;
            border-color: rgba(120,245,231,52);
        }

        QLabel#heroKicker {
            color: #78f5e7;
            font-size: 8pt;
            font-weight: 800;
            letter-spacing: 3px;
        }

        QLabel#heroTitle {
            color: #f5f7fb;
            font-size: 48px;
            font-weight: 700;
        }

        QLabel#heroSummary {
            color: rgba(241,244,252,155);
            font-size: 11pt;
            line-height: 1.55;
        }

        QLabel[heroMeta="true"] {
            color: rgba(255,255,255,125);
            font-size: 8pt;
            font-weight: 600;
        }

        QLabel[heroMeta="true"][accent="true"] {
            color: #78f5e7;
        }

        QLabel[heroMeta="true"][boxed="true"] {
            color: rgba(255,255,255,180);
            border: 1px solid rgba(255,255,255,28);
            border-radius: 5px;
            padding: 2px 5px;
        }

        QPushButton#heroPlay,
        QPushButton#heroMore {
            min-height: 46px;
            border-radius: 15px;
            padding: 0 20px;
            font-size: 9pt;
            font-weight: 700;
        }

        QPushButton#heroPlay {
            color: #07100f;
            background: #8ff8e9;
            border: 1px solid #d1fff9;
        }

        QPushButton#heroPlay:hover {
            background: #b6fff6;
        }

        QPushButton#heroMore {
            color: rgba(255,255,255,215);
            background: rgba(20,24,34,190);
            border: 1px solid rgba(255,255,255,28);
        }

        QPushButton#heroMore:hover {
            color: #ffffff;
            background: rgba(255,255,255,20);
            border-color: rgba(255,255,255,48);
        }

        QFrame#contentStage {
            background: #050609;
            border: none;
        }

        QLabel#sectionEyebrow {
            color: rgba(120,245,231,165);
            font-size: 7pt;
            font-weight: 800;
            letter-spacing: 2px;
        }

        QLabel#sectionTitle {
            color: #f5f7fb;
            font-size: 22pt;
            font-weight: 650;
        }

        QLabel#sectionAction {
            color: rgba(255,255,255,120);
            font-size: 9pt;
            font-weight: 600;
        }

        QWidget#homeFooter {
            background: #07090d;
            border-top: 1px solid rgba(255,255,255,14);
        }

        QLabel#footerCopy {
            color: rgba(235,239,248,95);
            font-size: 8pt;
        }

        QWidget#videoSurface {
            background: #000000;
        }

        QWidget#topBar,
        QWidget#sideRail,
        QWidget#controlDeck,
        QWidget#qualityBadge {
            background-color: rgba(15, 18, 26, 235);
            border: 1px solid rgba(255, 255, 255, 30);
            border-radius: 18px;
        }

        QWidget#controlDeck {
            border-radius: 22px;
        }

        QWidget#settingsPanel,
        QWidget#chapterPill {
            background-color: rgba(255, 255, 255, 10);
            border: 1px solid rgba(255, 255, 255, 18);
            border-radius: 13px;
        }

        QLabel#brandMark {
            color: #78f5e7;
            font-weight: 800;
            letter-spacing: 2px;
        }

        QLabel#eyebrow,
        QLabel#centerKicker,
        QLabel#chapterKicker {
            color: rgba(120, 245, 231, 180);
            font-size: 8pt;
            font-weight: 700;
        }

        QLabel#mediaTitle {
            color: #f7f8fb;
            font-size: 11pt;
            font-weight: 600;
        }

        QLabel#mutedLabel,
        QLabel#timelineLabel {
            color: rgba(235, 239, 249, 120);
            font-size: 8pt;
        }

        QLabel#timecode {
            color: rgba(255, 255, 255, 205);
            font-family: "Consolas";
            font-size: 9pt;
        }

        QLabel#qualityPrimary {
            color: #f7f8fb;
            font-size: 8pt;
            font-weight: 800;
        }

        QLabel#qualitySecondary {
            color: rgba(255, 255, 255, 155);
            font-size: 8pt;
            font-weight: 700;
        }

        QLabel#statusDot {
            color: #78f5e7;
            font-size: 11pt;
        }

        QLabel#chapterIndex {
            color: #a7fff6;
            background-color: rgba(120, 245, 231, 16);
            border: 1px solid rgba(120, 245, 231, 32);
            border-radius: 9px;
            padding: 5px 7px;
            font-size: 8pt;
            font-weight: 800;
        }

        QLabel#chapterTitle {
            color: rgba(255, 255, 255, 220);
            font-size: 9pt;
            font-weight: 600;
        }

        QLabel#wave {
            color: rgba(120, 245, 231, 135);
            font-family: "Consolas";
            font-size: 9pt;
        }

        QPushButton {
            outline: none;
        }

        QPushButton[uiRole="icon"],
        QPushButton[uiRole="rail"],
        QPushButton[uiRole="text"],
        QComboBox {
            color: rgba(255, 255, 255, 205);
            background-color: rgba(255, 255, 255, 9);
            border: 1px solid rgba(255, 255, 255, 19);
            border-radius: 11px;
            padding: 6px 9px;
        }

        QPushButton[uiRole="icon"] {
            min-width: 30px;
            min-height: 30px;
            font-weight: 700;
        }

        QPushButton[uiRole="rail"] {
            min-width: 38px;
            min-height: 38px;
            font-weight: 800;
        }

        QPushButton[uiRole="text"] {
            min-height: 30px;
            font-weight: 600;
        }

        QPushButton[uiRole="strong"] {
            color: #04110e;
            background-color: #8ff8e9;
            border: 1px solid #d4fff9;
            border-radius: 13px;
            min-width: 36px;
            min-height: 36px;
            font-size: 13pt;
            font-weight: 900;
        }

        QPushButton[uiRole="playCore"] {
            color: #04110e;
            background-color: #8ff8e9;
            border: 2px solid #d4fff9;
            border-radius: 34px;
            min-width: 64px;
            max-width: 64px;
            min-height: 64px;
            max-height: 64px;
            font-size: 20pt;
            font-weight: 900;
        }

        QPushButton[uiRole="icon"]:hover,
        QPushButton[uiRole="rail"]:hover,
        QPushButton[uiRole="text"]:hover,
        QComboBox:hover {
            color: #ffffff;
            background-color: rgba(255, 255, 255, 20);
            border-color: rgba(120, 245, 231, 75);
        }

        QPushButton[active="true"] {
            color: #a7fff6;
            background-color: rgba(120, 245, 231, 18);
            border-color: rgba(120, 245, 231, 48);
        }

        QPushButton:disabled {
            color: rgba(255, 255, 255, 55);
            background-color: rgba(255, 255, 255, 4);
            border-color: rgba(255, 255, 255, 10);
        }

        QComboBox {
            min-height: 28px;
            padding-right: 22px;
        }

        QComboBox::drop-down {
            border: none;
            width: 20px;
        }

        QComboBox QAbstractItemView {
            background: #11141b;
            color: #f7f8fb;
            border: 1px solid #303642;
            selection-background-color: #263e3c;
            selection-color: #a7fff6;
            padding: 4px;
        }

        QSlider#timelineSlider::groove:horizontal {
            height: 5px;
            background: rgba(255, 255, 255, 24);
            border-radius: 2px;
        }

        QSlider#timelineSlider::sub-page:horizontal {
            background: #78f5e7;
            border-radius: 2px;
        }

        QSlider#timelineSlider::handle:horizontal {
            width: 13px;
            margin: -5px 0;
            border-radius: 6px;
            background: #ddfff9;
            border: 2px solid #63cabe;
        }

        QSlider#volumeSlider::groove:horizontal {
            height: 3px;
            background: rgba(255, 255, 255, 25);
            border-radius: 2px;
        }

        QSlider#volumeSlider::sub-page:horizontal {
            background: #78f5e7;
            border-radius: 2px;
        }

        QSlider#volumeSlider::handle:horizontal {
            width: 10px;
            margin: -4px 0;
            border-radius: 5px;
            background: #d9fff9;
        }

        QToolTip {
            color: #f7f8fb;
            background: #11141b;
            border: 1px solid #353b46;
            padding: 5px;
        }
    )QSS");

    MainWindow window;
    window.resize(1360, 840);
    window.show();

    if (app.arguments().size() > 1) {
        window.openPath(app.arguments().at(1));
    }

    return app.exec();
}
