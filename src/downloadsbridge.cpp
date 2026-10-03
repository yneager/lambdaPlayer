#include "downloadsbridge.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QEventLoop>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUuid>
#include <QUrl>
#include <QWidget>

DownloadsBridge::DownloadsBridge(QObject *parent) : QObject(parent)
{
    const auto overrideDir = qEnvironmentVariable("LAMBDA_DATA_DIR");
    dataDirectory_ = QDir(overrideDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : overrideDir).filePath("downloads");
    QDir().mkpath(dataDirectory_);
    QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
    directory_ = settings.value("directory", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
    connect(&process_, &QProcess::finished, this, [this] { emit engineStopped(); });
}

DownloadsBridge::~DownloadsBridge()
{
    if (process_.state() == QProcess::NotRunning) return;
    // Ask the owned engine to flush its SQLite database and stop gracefully.
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/jsonrpc").arg(port_)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = network.post(request, QJsonDocument(QJsonObject{
        {"jsonrpc", "2.0"}, {"id", "shutdown"}, {"method", "aria2.shutdown"},
        {"params", QJsonArray{QString("token:") + secret_}}
    }).toJson(QJsonDocument::Compact));
    // Deliver the HTTP request before waiting for the child process.
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(1000);
    loop.exec();
    if (!process_.waitForFinished(4000)) { process_.kill(); process_.waitForFinished(1000); }
}

QJsonObject DownloadsBridge::start()
{
    if (process_.state() == QProcess::NotRunning) {
        const QString binary = QDir(QCoreApplication::applicationDirPath()).filePath("aria2c.exe");
        if (!QFileInfo::exists(binary)) return {{"error", "The bundled download engine is missing. Rebuild the player to install it."}};
        QTcpServer socket;
        if (!socket.listen(QHostAddress::LocalHost, 0)) return {{"error", socket.errorString()}};
        port_ = socket.serverPort();
        socket.close();
        secret_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString config = QDir(dataDirectory_).filePath("aria2.conf");
        if (!QFileInfo::exists(config) && !QFile::copy(":/downloads/aria2.conf", config))
            return {{"error", "Unable to create the download engine configuration."}};
        // Host bindings adapted from Motrix's Aria2ConfigBuilder.buildArgs.
        QSettings uiSettings(QDir(dataDirectory_).filePath("../ui-settings.ini"), QSettings::IniFormat);
        encrypted_ = uiSettings.value("torrentEncryption", true).toBool();
        QStringList args{
            "--conf-path=" + config, "--enable-rpc=true", "--rpc-listen-all=false",
            "--rpc-allow-origin-all=true", "--rpc-listen-port=" + QString::number(port_),
            "--rpc-secret=" + secret_, "--dir=" + directory_,
            "--enable-sqlite3-persistence=true", "--sqlite3-db-path=" + QDir(dataDirectory_).filePath("aria2.db"),
            "--sqlite3-history-limit=10000", "--save-session=" + QDir(dataDirectory_).filePath("aria2.session"),
            "--dht-file-path=" + QDir(dataDirectory_).filePath("dht.dat"),
            "--dht-file-path6=" + QDir(dataDirectory_).filePath("dht6.dat"),
            "--max-concurrent-downloads=5", "--max-connection-per-server=16", "--split=16",
            // Defaults adapted from DLmanager's engine-settings and auto profile.
            "--bt-max-peers=128", "--bt-enable-lpd=true", "--enable-dht=true",
            "--enable-dht6=true", "--enable-peer-exchange=true", "--disk-cache=32M",
            "--max-download-limit=0", "--max-overall-download-limit=0",
            "--bt-request-peer-speed-limit=10M", "--min-split-size=4M",
            "--socket-recv-buffer-size=1M",
            QString("--bt-force-encryption=%1").arg(encrypted_ ? "true" : "false"),
            // Use a range separate from FDM's customary 6881 listener.
            "--listen-port=49160-49200", "--dht-listen-port=49160-49200",
            "--file-allocation=none", "--bt-save-metadata=true", "--bt-metadata-only=false",
            "--auto-file-renaming=false", "--allow-overwrite=false", "--rpc-save-upload-metadata=true",
            "--force-save=true", "--continue=false", "--pause=false", "--pause-metadata=false", "--rpc-max-request-size=32M",
            "--bt-seed-unverified=false", "--seed-time=0", "--console-log-level=error"
        };
        for (const auto &arg : args) if (arg.contains('\n') || arg.contains('\r')) return {{"error", "Download paths must not contain line breaks."}};
        QDir().mkpath(directory_);
        process_.setWorkingDirectory(dataDirectory_);
        process_.setProgram(binary);
        process_.setArguments(args);
        process_.setStandardOutputFile(QProcess::nullDevice());
        process_.setStandardErrorFile(QDir(dataDirectory_).filePath("engine.log"));
        process_.start();
        if (!process_.waitForStarted(2000)) return {{"error", process_.errorString()}};
    }
    return {{"port", int(port_)}, {"secret", secret_}, {"directory", directory_},
            {"encrypted", encrypted_}};
}

QString DownloadsBridge::chooseFolder()
{
    const auto path = QFileDialog::getExistingDirectory(qobject_cast<QWidget *>(parent()), "Download folder", directory_);
    if (path.isEmpty()) return directory_;
    directory_ = path;
    QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("directory", directory_);
    return directory_;
}

QJsonObject DownloadsBridge::chooseTorrent()
{
    const auto path = QFileDialog::getOpenFileName(qobject_cast<QWidget *>(parent()), "Add torrent", {}, "Torrent files (*.torrent)");
    if (path.isEmpty()) return {};
    return readTorrent(path);
}

QJsonObject DownloadsBridge::readTorrent(const QString &path)
{
    QFile file(path);
    if (!path.endsWith(".torrent", Qt::CaseInsensitive) || !file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        return {{"error", "Unable to read torrent (maximum 16 MB)."}};
    return {{"name", QFileInfo(path).fileName()}, {"data", QString::fromLatin1(file.readAll().toBase64())}};
}

void DownloadsBridge::receiveDrop(const QString &text)
{
    if (QFileInfo(text).suffix().compare("torrent", Qt::CaseInsensitive) == 0 && QFileInfo(text).isFile()) {
        emit downloadDropped(readTorrent(text));
        return;
    }
    const QUrl url(text.trimmed());
    if (url.isValid() && QStringList{"http", "https", "ftp", "magnet"}.contains(url.scheme().toLower()))
        emit downloadDropped({{"uri", text.trimmed()}});
}

void DownloadsBridge::reveal(const QString &path)
{
    const auto target = path.isEmpty() ? directory_ : QFileInfo(path).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}
