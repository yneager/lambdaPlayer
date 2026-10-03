#include "windowbridge.h"
#include <QGuiApplication>
#include <QApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <algorithm>
#include <cmath>

namespace {
QString preferencePath()
{
    const auto root = qEnvironmentVariable("LAMBDA_DATA_DIR");
    return QDir(root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : root).filePath("ui-settings.ini");
}
}

QJsonObject WindowBridge::preferences() const
{
    return preferences_;
}

static QJsonObject loadPreferences()
{
    QSettings settings(preferencePath(), QSettings::IniFormat);
    return {{"palette", settings.value("palette", "mint").toString()},
            {"fps", settings.value("fps", 120).toInt()},
            {"smoothScroll", settings.value("smoothScroll", true).toBool()},
            {"reduceMotion", settings.value("reduceMotion", false).toBool()},
            {"torrentEncryption", settings.value("torrentEncryption", true).toBool()}};
}

void WindowBridge::setPreference(const QString &key, const QVariant &value)
{
    if (key == "palette") {
        if (!QStringList{"mint", "ocean", "violet", "amber"}.contains(value.toString())) return;
    } else if (key == "fps") {
        if (!QList<int>{60, 120, 240}.contains(value.toInt())) return;
    } else if (key != "smoothScroll" && key != "reduceMotion" && key != "torrentEncryption") return;
    QSettings settings(preferencePath(), QSettings::IniFormat);
    settings.setValue(key, value);
    settings.sync();
    // Home and player have separate channels but share the same preferences.
    for (auto *widget : QApplication::topLevelWidgets()) {
        for (auto *bridge : widget->findChildren<WindowBridge *>()) {
            bridge->preferences_ = loadPreferences();
            emit bridge->preferencesChanged();
            emit bridge->stateChanged();
        }
    }
}

WindowBridge::WindowBridge(QObject *parent)
    : QObject(parent)
{
    preferences_ = loadPreferences();
    uiFrameClock_.start();
    uiFrameTimer_.setSingleShot(true);
    uiFrameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&uiFrameTimer_, &QChronoTimer::timeout, this, &WindowBridge::uiFrame);
}

bool WindowBridge::uiFramePacing() const
{
    return QCoreApplication::instance()->property("lambdaUiFramePacing").toBool();
}

void WindowBridge::requestUiFrame()
{
    if (uiFrameTimer_.isActive()) return;
    const double rate = uiRefreshRate();
    const double safeRate = std::isfinite(rate) && rate >= 24.0 ? std::min(rate, 360.0) : 60.0;
    const qint64 period = qRound64(1e9 / safeRate);
    const qint64 now = uiFrameClock_.nsecsElapsed();
    // Absolute deadlines preserve fractional rates (120 Hz is 8.333 ms).
    // Skip missed ticks instead of queuing catch-up frames after a stall.
    uiFrameDeadline_ = std::max((now / period + 1) * period, uiFrameDeadline_ + period);
    uiFrameTimer_.setInterval(std::chrono::nanoseconds(uiFrameDeadline_ - now));
    uiFrameTimer_.start();
}

double WindowBridge::uiRefreshRate() const
{
    auto *widget = qobject_cast<QWidget *>(parent());
    QScreen *screen = widget && widget->window()->windowHandle()
                          ? widget->window()->windowHandle()->screen()
                          : QGuiApplication::primaryScreen();
    return std::min(screen ? screen->refreshRate() : 60.0, double(preferences().value("fps").toInt(120)));
}

void WindowBridge::setWindowState(bool maximized, bool fullscreen, bool mini)
{
    if (maximized == maximized_ && fullscreen == fullscreen_ && mini == mini_) {
        return;
    }
    maximized_ = maximized;
    fullscreen_ = fullscreen;
    mini_ = mini;
    emit stateChanged();
}

QList<QRectF> WindowBridge::toRects(const QVariantList &list)
{
    QList<QRectF> rects;
    rects.reserve(list.size());
    for (const QVariant &entry : list) {
        const QVariantList values = entry.toList();
        if (values.size() != 4) {
            continue;
        }
        const QRectF rect(values[0].toDouble(), values[1].toDouble(),
                          values[2].toDouble(), values[3].toDouble());
        if (rect.isValid()) {
            rects.append(rect);
        }
    }
    return rects;
}

void WindowBridge::setDragRegions(const QVariantList &drag, const QVariantList &holes)
{
    drag_ = toRects(drag);
    holes_ = toRects(holes);
}

bool WindowBridge::isDragPoint(const QPointF &point) const
{
    for (const QRectF &hole : holes_) {
        if (hole.contains(point)) {
            return false;
        }
    }
    for (const QRectF &area : drag_) {
        if (area.contains(point)) {
            return true;
        }
    }
    return false;
}
