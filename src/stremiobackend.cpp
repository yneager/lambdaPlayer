#include "stremiobackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr qint64 kDefaultCacheSize = 10LL * 1024 * 1024 * 1024; // the server's default
const QString kCacheFolderName = QStringLiteral("LAMBDA stream cache");

QSettings appSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope, "LAMBDA", "LAMBDA Player");
}

QString bundledFile(const QString &name, const char *overrideVariable)
{
    const QString overridePath = qEnvironmentVariable(overrideVariable);
    if (!overridePath.isEmpty()) {
        return overridePath;
    }
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    return QFileInfo(path).isFile() ? path : QString();
}

} // namespace

StremioBackend::StremioBackend(QObject *parent)
    : QObject(parent)
{
    // Developer aid: LAMBDA_DATA_DIR keeps test runs out of the real profile.
    const QString overrideDir = qEnvironmentVariable("LAMBDA_DATA_DIR");
    dataDir_ = overrideDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        : overrideDir;
    QDir().mkpath(dataDir_);

    client_ = new stremio::AddonClient(this);
    client_->setDiskCacheDirectory(QDir(dataDir_).filePath(QStringLiteral("addon-cache")));

    addons_ = new stremio::AddonManager(client_, QDir(dataDir_).filePath(QStringLiteral("addons.json")), this);
    addons_->load();

    content_ = new stremio::ContentService(addons_, client_, this);

    // Built-in streaming engine, started on first use.
    process_ = new stremio::StreamingServerProcess(this);
    applyServerOptions();
    connect(process_, &stremio::StreamingServerProcess::portChanged, this, [](quint16 port) {
        // A stable port keeps stream URLs (and mpv's resume positions) stable.
        appSettings().setValue(QStringLiteral("streaming/port"), port);
    });
    connect(process_, &stremio::StreamingServerProcess::started, this, &StremioBackend::pushCacheSize);
    connect(qApp, &QCoreApplication::aboutToQuit, process_, &stremio::StreamingServerProcess::stop);

    server_ = new stremio::StreamingServer(client_->networkAccessManager(), this);
    server_->setProcess(process_);
    server_->setExternalUrl(appSettings().value(QStringLiteral("streaming/externalServerUrl")).toString());

    resolver_ = new stremio::StreamResolver(client_->networkAccessManager(), server_, this);
    videoParams_ = new stremio::VideoParamsFetcher(client_->networkAccessManager(), server_, this);

    // Keep installed manifests current (new catalogs, changed configuration),
    // without blocking startup. A failed refresh keeps the stored manifest.
    QTimer::singleShot(4000, addons_, &stremio::AddonManager::refreshAll);
}

void StremioBackend::applyServerOptions()
{
    stremio::StreamingServerProcess::Options options;
    options.program = bundledFile(QStringLiteral("lambda-stream-server.exe"), "LAMBDA_STREAM_SERVER");
    options.configDir = QDir(dataDir_).filePath(QStringLiteral("stream-server"));
    options.cacheDir = cacheDirectory();
    options.port = quint16(appSettings().value(QStringLiteral("streaming/port"), 0).toUInt());
    process_->setOptions(options);
}

void StremioBackend::setStreamingServerUrl(const QString &url)
{
    server_->setExternalUrl(url);
    appSettings().setValue(QStringLiteral("streaming/externalServerUrl"), server_->externalUrl());
}

QString StremioBackend::cacheLocation() const
{
    const QString location = appSettings().value(QStringLiteral("streaming/cacheLocation")).toString();
    return location.isEmpty() ? dataDir_ : location;
}

bool StremioBackend::hasCustomCacheLocation() const
{
    return !appSettings().value(QStringLiteral("streaming/cacheLocation")).toString().isEmpty();
}

QString StremioBackend::cacheDirectory() const
{
    // Always a folder of our own, so clearing never touches the user's files.
    return QDir(cacheLocation()).filePath(kCacheFolderName);
}

void StremioBackend::setCacheLocation(const QString &location)
{
    const QString value = QDir::cleanPath(location.trimmed()) == QDir::cleanPath(dataDir_) ? QString() : location.trimmed();
    appSettings().setValue(QStringLiteral("streaming/cacheLocation"), value);
    applyServerOptions(); // restarts a running engine on the new folder
}

qint64 StremioBackend::cacheSizeLimit() const
{
    return appSettings().value(QStringLiteral("streaming/cacheSize"), kDefaultCacheSize).toLongLong();
}

void StremioBackend::setCacheSizeLimit(qint64 bytes)
{
    appSettings().setValue(QStringLiteral("streaming/cacheSize"), qMax<qint64>(bytes, 512LL * 1024 * 1024));
    pushCacheSize();
}

void StremioBackend::pushCacheSize()
{
    if (!process_->isRunning()) {
        return; // sent when it starts
    }
    // Partial update of the server's settings (routes/system.rs update_settings).
    QNetworkRequest request(QUrl(process_->url() + QStringLiteral("settings")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(5000);
    const QJsonObject body{{"cacheSize", double(cacheSizeLimit())}};
    QNetworkReply *reply = client_->networkAccessManager()->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

qint64 StremioBackend::cacheUsage() const
{
    qint64 total = 0;
    QDirIterator it(cacheDirectory(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
#ifdef Q_OS_WIN
        // Torrent files are sparse: count what is really on disk.
        DWORD high = 0;
        const DWORD low = GetCompressedFileSizeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(it.filePath()).utf16()), &high);
        if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
            total += (qint64(high) << 32) | low;
            continue;
        }
#endif
        total += it.fileInfo().size();
    }
    return total;
}

bool StremioBackend::clearCache(QString *error)
{
    // The engine keeps files open; it restarts on the next torrent.
    process_->stop();
    QDir dir(cacheDirectory());
    if (!dir.exists()) {
        return true;
    }
    bool ok = true;
    for (const QFileInfo &entry : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const bool removed = entry.isDir() ? QDir(entry.absoluteFilePath()).removeRecursively()
                                           : QFile::remove(entry.absoluteFilePath());
        ok = ok && removed;
    }
    if (!ok && error) {
        *error = tr("Some cached files are still in use and were not removed.");
    }
    return ok;
}

QStringList StremioBackend::defaultAddonUrls()
{
    // Stremio/stremio-official-addons index.json: Cinemeta (catalogs and
    // metadata for IMDb ids), OpenSubtitles v3 (subtitles), and Torrentio (streams).
    return {
        QStringLiteral("https://v3-cinemeta.strem.io/manifest.json"),
        QStringLiteral("https://opensubtitles-v3.strem.io/manifest.json"),
        QStringLiteral("https://torrentio.strem.fun/manifest.json"),
    };
}
