#include "homepage.h"
#include "locallibrary.h"
#include "windowbridge.h"

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHideEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <functional>

namespace {

// Home page actions exposed to home.js as "homeBridge".
class HomeBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    using QObject::QObject;
    QString version() const { return QCoreApplication::applicationVersion(); }

signals:
    void readyRequested();
    void openVideoRequested();
    void openFolderRequested();
    void resumeRequested();
    void openRecentRequested(const QString &path);
    void openLicensesRequested();

public slots:
    void ready() { emit readyRequested(); }
    void openVideo() { emit openVideoRequested(); }
    void openFolder() { emit openFolderRequested(); }
    void resume() { emit resumeRequested(); }
    void openRecent(const QString &path) { emit openRecentRequested(path); }
    void openLicenses() { emit openLicensesRequested(); }
};

class VuiPage final : public QWebEnginePage
{
public:
    explicit VuiPage(QObject *parent = nullptr)
        : QWebEnginePage(parent)
    {
    }

    std::function<bool(const QUrl &)> navigationHandler;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message,
                                  int line, const QString &source) override
    {
        if (level != InfoMessageLevel) {
            qWarning().noquote() << "[home]" << source.section('/', -1) << line << message;
        }
    }

    bool acceptNavigationRequest(const QUrl &url,
                                 NavigationType type,
                                 bool isMainFrame) override
    {
        if (type == QWebEnginePage::NavigationTypeLinkClicked
            && navigationHandler && navigationHandler(url)) {
            return false;
        }

        // The Home page is local; never navigate the app window to the web.
        if (isMainFrame && (url.scheme() == "http" || url.scheme() == "https")) {
            return false;
        }

        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }
};

class VuiView final : public QWebEngineView
{
public:
    explicit VuiView(QWidget *parent = nullptr)
        : QWebEngineView(parent)
    {
        setAcceptDrops(true);
    }

    std::function<void(const QString &)> localFileDropped;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const auto urls = event->mimeData()->urls();
            if (!urls.isEmpty() && urls.first().isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
        QWebEngineView::dragEnterEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        const auto urls = event->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile() && localFileDropped) {
            localFileDropped(urls.first().toLocalFile());
            event->acceptProposedAction();
            return;
        }
        QWebEngineView::dropEvent(event);
    }
};

} // namespace

HomePage::HomePage(QObject *addonsBridge, QWidget *parent)
    : QWidget(parent)
{
    setObjectName("homePage");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *view = new VuiView(this);
    view_ = view;
    view_->setContextMenuPolicy(Qt::NoContextMenu);
    view_->setStyleSheet("background:#050609;border:0;");
    view_->settings()->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, true);
    view_->settings()->setAttribute(QWebEngineSettings::WebGLEnabled, true);
    view_->settings()->setAttribute(QWebEngineSettings::Accelerated2dCanvasEnabled, true);
    view_->settings()->setAttribute(QWebEngineSettings::ShowScrollBars, true);
    // Addon posters, backgrounds and logos are remote images.
    view_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

    auto *page = new VuiPage(view_);
    // Paint the app background while loading instead of Chromium's white.
    page->setBackgroundColor(QColor("#050609"));
    page->navigationHandler = [this](const QUrl &url) {
        if (url.scheme() == "lambda") {
            if (url.host() == "resume") {
                emit resumeRequested();
            } else {
                emit openVideoRequested();
            }
            return true;
        }

        if (url.path().endsWith("/player.html") || url.fileName() == "player.html") {
            emit openVideoRequested();
            return true;
        }

        return false;
    };
    view_->setPage(page);

    auto *bridge = new HomeBridge(this);
    windowBridge_ = new WindowBridge(this);
    auto *channel = new QWebChannel(page);
    library_ = new LocalLibrary(this);
    channel->registerObject("library", library_);
    channel->registerObject("homeBridge", bridge);
    channel->registerObject("windowBridge", windowBridge_);
    if (addonsBridge) {
        // Registered before the page loads so qwebchannel.js sees it.
        channel->registerObject("stremio", addonsBridge);
    }
    page->setWebChannel(channel);

    connect(bridge, &HomeBridge::readyRequested, this, [this] {
        ready_ = true;
        pushState();
    });
    connect(bridge, &HomeBridge::openVideoRequested, this, &HomePage::openVideoRequested);
    connect(bridge, &HomeBridge::openFolderRequested, this, &HomePage::openFolderRequested);
    connect(bridge, &HomeBridge::resumeRequested, this, &HomePage::resumeRequested);
    connect(bridge, &HomeBridge::openRecentRequested, this, &HomePage::openPathRequested);
    connect(bridge, &HomeBridge::openLicensesRequested, this, &HomePage::openLicensesRequested);

    view->localFileDropped = [this](const QString &path) {
        emit openPathRequested(path);
    };

    layout->addWidget(view_);
    view_->load(QUrl("qrc:/vui/index.html"));
}

void HomePage::runScript(const QString &script)
{
    if (view_ && view_->page() && ready_) {
        view_->page()->runJavaScript(script);
    }
}

void HomePage::setRecents(const QJsonArray &recents)
{
    recents_ = recents;
    pushState();
}

void HomePage::setCurrentMedia(const QString &displayName, const QString &path, bool available)
{
    currentMediaName_ = displayName;
    currentMediaPath_ = path;
    currentMediaAvailable_ = available;
    pushState();
}

void HomePage::pushState()
{
    if (!ready_) {
        return;
    }
    QJsonObject session;
    session.insert("available", currentMediaAvailable_);
    session.insert("name", currentMediaName_);
    session.insert("path", currentMediaPath_);
    const QString sessionJson = QString::fromUtf8(QJsonDocument(session).toJson(QJsonDocument::Compact));
    const QString recentsJson = QString::fromUtf8(QJsonDocument(recents_).toJson(QJsonDocument::Compact));
    runScript(QString("window.lambdaHome&&(lambdaHome.setSession(%1),lambdaHome.setRecents(%2));")
                  .arg(sessionJson, recentsJson));
}

void HomePage::openOnline(const QJsonObject &item)
{
    runScript(QString("window.lambdaHome&&lambdaHome.openOnline(%1);").arg(QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact))));
}

void HomePage::toast(const QString &message, bool warning)
{
    QJsonArray args;
    args.append(message);
    const QString json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    runScript(QString("window.LambdaWindow&&LambdaWindow.toast(%1[0],'%2');")
                  .arg(json, warning ? "warn" : "info"));
}

void HomePage::playLeaveAnimation()
{
    runScript("window.LambdaWindow&&LambdaWindow.leave();");
}

void HomePage::playEnterAnimation()
{
    runScript("window.LambdaWindow&&LambdaWindow.enter();");
}

void HomePage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (view_ && view_->page()) {
        view_->page()->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    }
}

void HomePage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // Freezing must happen after the view is hidden, otherwise Chromium
    // refuses ("page is visible"). Deferred to the next event loop turn.
    QMetaObject::invokeMethod(this, [this] {
        if (!isVisible() && view_ && view_->page()) {
            view_->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
        }
    }, Qt::QueuedConnection);
}

#include "homepage.moc"
