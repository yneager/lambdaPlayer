#include "updatechecker.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVersionNumber>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>
#include <memory>

namespace {
QSettings settings() {
    const QString profile = qEnvironmentVariable("LAMBDA_DATA_DIR");
    if (!profile.isEmpty()) {
        QDir().mkpath(profile);
        return QSettings(QDir(profile).filePath("player.ini"), QSettings::IniFormat);
    }
    return QSettings(QSettings::IniFormat, QSettings::UserScope, "LAMBDA", "LAMBDA Player");
}
const QRegularExpression versionPattern("^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:-([0-9A-Za-z.-]+))?(?:\\+[0-9A-Za-z.-]+)?$");
bool trustedUrl(const QString &value, const QString &prefix) {
    const QUrl url(value);
    return url.scheme() == "https" && url.host() == "github.com" && url.port(-1) == -1
           && url.userInfo().isEmpty() && url.path().startsWith(prefix);
}
bool numeric(const QString &value) { return QRegularExpression("^[0-9]+$").match(value).hasMatch(); }
bool validDigest(const QString &value) { return QRegularExpression("^[a-fA-F0-9]{64}$").match(value).hasMatch(); }
bool httpsUrl(const QString &value) {
    const QUrl url(value);
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() && url.userInfo().isEmpty();
}
QString powershell() { return qEnvironmentVariable("SystemRoot", "C:/Windows") + "/System32/WindowsPowerShell/v1.0/powershell.exe"; }
}

bool UpdateChecker::newerThan(const QString &candidate, const QString &current) {
    const auto a = versionPattern.match(candidate), b = versionPattern.match(current);
    if (!a.hasMatch() || !b.hasMatch()) return false;
    const auto base = [](const QRegularExpressionMatch &m) {
        return QVersionNumber::fromString(m.captured(1)+'.'+m.captured(2)+'.'+m.captured(3));
    };
    const int core = QVersionNumber::compare(base(a), base(b));
    if (core) return core > 0;
    const QString ap = a.captured(4), bp = b.captured(4);
    if (ap.isEmpty() || bp.isEmpty()) return ap.isEmpty() && !bp.isEmpty();
    const auto ai = ap.split('.'), bi = bp.split('.');
    for (int i = 0; i < qMin(ai.size(), bi.size()); ++i) {
        if (ai[i] == bi[i]) continue;
        const bool an = numeric(ai[i]), bn = numeric(bi[i]);
        if (an != bn) return !an;
        if (an && ai[i].size() != bi[i].size()) return ai[i].size() > bi[i].size();
        return QString::compare(ai[i], bi[i], Qt::CaseSensitive) > 0;
    }
    return ai.size() > bi.size();
}
QJsonObject UpdateChecker::selectRelease(const QJsonArray &releases, const QString &current, bool includeTests) {
    QJsonObject selected;
    const QRegularExpression installer("^LAMBDA-Player-Windows-x64-v[0-9A-Za-z.+-]+\\.zip$");
    for (const auto &value : releases) {
        const auto release = value.toObject();
        const QString tag = release["tag_name"].toString();
        const bool test = release["prerelease"].toBool() || tag.contains('-');
        if (release["draft"].toBool() || (!includeTests && test) || !newerThan(tag, current)) continue;
        const QString notes = release["html_url"].toString();
        if (!trustedUrl(notes, "/yneager/lambdaPlayer/releases/tag/")) continue;
        for (const auto &assetValue : release["assets"].toArray()) {
            const auto asset = assetValue.toObject();
            const QString url = asset["browser_download_url"].toString();
            const QString digest = asset["digest"].toString().remove("sha256:");
            if (asset["state"].toString() != "uploaded" || !installer.match(asset["name"].toString()).hasMatch()
                || !validDigest(digest) || !trustedUrl(url, "/yneager/lambdaPlayer/releases/download/")) continue;
            if (selected.isEmpty() || newerThan(tag, selected["tag"].toString()))
                selected = {{"tag", tag}, {"downloadUrl", url}, {"notesUrl", notes}, {"notes", release["body"]},
                            {"sha256", digest.toLower()}, {"size", asset["size"]}, {"prerelease", test}};
            break;
        }
    }
    return selected;
}
QJsonObject UpdateChecker::selectFeed(const QJsonObject &feed, const QString &current, bool includeTests) {
    QJsonObject selected;
    if (feed["schemaVersion"].toInt() != 1) return selected;
    for (const auto &value : feed["releases"].toArray()) {
        const auto r = value.toObject();
        const auto tag = r["version"].toString();
        const bool test = r["prerelease"].toBool() || tag.contains('-');
        const auto url = r["url"].toString(), digest = r["sha256"].toString();
        if ((!includeTests && test) || !newerThan(tag, current) || !httpsUrl(url)
            || !QUrl(url).path().endsWith(".zip", Qt::CaseInsensitive) || !validDigest(digest)
            || r["size"].toDouble() <= 0 || r["size"].toDouble() > 2.0*1024*1024*1024) continue;
        if (selected.isEmpty() || newerThan(tag, selected["tag"].toString()))
            selected = {{"tag",tag},{"prerelease",test},{"downloadUrl",url},{"sha256",digest.toLower()},
                        {"size",r["size"]},{"notes",r["notes"]},{"notesUrl",r["notesUrl"]}};
    }
    return selected;
}

UpdateChecker::UpdateChecker(const QString &currentTag, QObject *parent, const QUrl &endpoint)
    : QObject(parent), currentTag_(currentTag), endpoint_(endpoint) {
    includeTests_ = settings().value("updates/includeTests", currentTag.contains('-')).toBool();
    QFile source(QCoreApplication::applicationDirPath()+"/update-source.json");
    if (source.open(QIODevice::ReadOnly)) {
        const QString url = QJsonDocument::fromJson(source.readAll()).object()["feedUrl"].toString();
        if (httpsUrl(url)) endpoint_ = QUrl(url);
    }
    const auto cached = QJsonDocument::fromJson(settings().value("updates/cached").toByteArray()).object();
    if (newerThan(cached["tag"].toString(), currentTag_) && (includeTests_ || !cached["prerelease"].toBool())
        && settings().value("updates/cachedSource").toString() == endpoint_.toString()
        && httpsUrl(cached["downloadUrl"].toString()) && validDigest(cached["sha256"].toString())) available_ = cached;
}
QJsonObject UpdateChecker::state() const {
    auto result = available_;
    result.insert("currentTag", currentTag_);
    result.insert("includeTests", includeTests_);
    result.insert("status", status_);
    result.insert("busy", busy_);
    result.insert("progress", progress_);
    result.insert("available", !available_.isEmpty());
    result.insert("showNotification", !available_.isEmpty() && settings().value("updates/dismissed").toString() != available_["tag"].toString());
    return result;
}
void UpdateChecker::publish() { emit changed(state()); }
void UpdateChecker::start() {
    const QString lastJob = settings().value("updates/job").toString();
    if (!lastJob.isEmpty()) {
        auto *resultTimer = new QTimer(this);
        connect(resultTimer, &QTimer::timeout, this, [this, resultTimer, lastJob] {
            QFile file(lastJob+"/result.json");
            if (!file.open(QIODevice::ReadOnly)) return;
            const auto result = QJsonDocument::fromJson(file.readAll()).object();
            if (!result.contains("ok")) return;
            file.close();
            settings().remove("updates/job");
            resultTimer->stop(); resultTimer->deleteLater();
            emit notify(result["message"].toString(), !result["ok"].toBool());
            const QString base = qEnvironmentVariable("LAMBDA_DATA_DIR", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
            const QFileInfo jobInfo(lastJob);
            const QString parent = QDir(base).filePath("updates");
            // Only clean files in a validated updater-created job directory.
            if (QRegularExpression("^[a-f0-9]{32}$").match(jobInfo.fileName()).hasMatch()
                && !jobInfo.isSymLink() && jobInfo.dir().canonicalPath()==QDir(parent).canonicalPath()) {
                for (const auto &name : {"update.zip","update.ps1","job.json","ready","result.json"})
                    QFile::remove(QDir(lastJob).filePath(QLatin1String(name)));
                QDir(parent).rmdir(jobInfo.fileName());
            }
        });
        resultTimer->start(3000);
    }
    QTimer::singleShot(3000, this, [this] { check(false); });
    auto *timer = new QTimer(this);
    timer->setInterval(6 * 60 * 60 * 1000);
    connect(timer, &QTimer::timeout, this, [this] { check(false); });
    timer->start();
}
void UpdateChecker::check(bool manual) {
    if (busy_) return;
    const auto last = QDateTime::fromString(settings().value("updates/lastCheck").toString(), Qt::ISODate);
    const auto now = QDateTime::currentDateTimeUtc();
    if (!manual && last.isValid() && last <= now && last.secsTo(now) < 6 * 60 * 60) return;
    busy_ = true; status_ = "checking"; publish();
    QNetworkRequest request(endpoint_);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "LAMBDA-Player/" + currentTag_.toUtf8());
    request.setTransferTimeout(15000);
    auto *reply = network_.get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > 2 * 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual] {
        busy_ = false;
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(reply->readAll(), &parse);
        const bool success = reply->error() == QNetworkReply::NoError && parse.error == QJsonParseError::NoError
            && (json.isArray() || (json.isObject() && json.object()["schemaVersion"].toInt() == 1
                                  && json.object()["releases"].isArray()));
        reply->deleteLater();
        if (!success) {
            status_ = "error"; publish();
            if (manual) emit notify("Updates could not be checked. Check your connection and try again later.", true);
            return;
        }
        available_ = json.isArray() ? selectRelease(json.array(), currentTag_, includeTests_)
                                   : selectFeed(json.object(), currentTag_, includeTests_);
        settings().setValue("updates/cached", QJsonDocument(available_).toJson(QJsonDocument::Compact));
        settings().setValue("updates/cachedSource", endpoint_.toString());
        settings().setValue("updates/lastCheck", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        if (manual && !available_.isEmpty()) settings().remove("updates/dismissed");
        status_ = "ready"; publish();
        if (manual && available_.isEmpty()) emit notify("You’re up to date for your selected update channel.", false);
    });
}
void UpdateChecker::setIncludeTests(bool enabled) {
    if (busy_) return;
    includeTests_ = enabled;
    settings().setValue("updates/includeTests", enabled);
    if (!enabled && available_["prerelease"].toBool()) available_ = {};
    publish();
    if (busy_) return;
    check(true);
}
void UpdateChecker::dismiss() {
    if (busy_) return;
    if (!available_.isEmpty()) settings().setValue("updates/dismissed", available_["tag"].toString());
    publish();
}
void UpdateChecker::download() {
    if (busy_ || available_.isEmpty()) return;
    const QString url = available_["downloadUrl"].toString();
    if (!httpsUrl(url) || !validDigest(available_["sha256"].toString())) return;
    const QString base = qEnvironmentVariable("LAMBDA_DATA_DIR", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    const QString job = QDir(base).filePath("updates/"+QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().mkpath(job)) { failUpdate("Cannot create the update download folder."); return; }
    auto file = std::make_shared<QFile>(job+"/update.zip");
    if (!file->open(QIODevice::WriteOnly)) { failUpdate("Cannot save the update download."); return; }
    busy_=true; progress_=0; status_="downloading"; publish();
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(60000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = network_.get(request); downloadReply_=reply;
    auto writeOk = std::make_shared<bool>(true);
    auto drain = [file,hash,reply,writeOk] {
        if (!reply->isOpen()) return;
        const auto data=reply->readAll();
        if (file->size()+data.size() > 2LL*1024*1024*1024 || file->write(data)!=data.size()) {
            *writeOk=false; reply->abort(); return;
        }
        hash->addData(data);
    };
    connect(reply,&QNetworkReply::readyRead,this,drain);
    connect(reply,&QNetworkReply::downloadProgress,this,[this](qint64 received,qint64 total) {
        const auto size = qint64(available_["size"].toDouble());
        if (size>0) total=size;
        const int percent = total>0 ? qMin(100,int(100.0*received/total)) : 0;
        if (percent != progress_) { progress_=percent; publish(); }
    });
    connect(reply,&QNetworkReply::finished,this,[this,reply,file,hash,writeOk,drain,job] {
        drain(); file->close(); downloadReply_=nullptr;
        const bool cancelled = reply->error()==QNetworkReply::OperationCanceledError && *writeOk;
        const bool ok=reply->error()==QNetworkReply::NoError && *writeOk;
        reply->deleteLater();
        if (!ok) {
            file->remove();
            if (cancelled) { busy_=false; status_="ready"; publish(); }
            else failUpdate("The update download failed. Your current version is unchanged; try again.");
            return;
        }
        status_="verifying"; publish();
        const auto size=qint64(available_["size"].toDouble());
        if (hash->result().toHex()!=available_["sha256"].toString().toLatin1() || (size>0 && file->size()!=size)) {
            file->remove(); failUpdate("Update verification failed. No files were installed; try again."); return;
        }
        prepareUpdate(job);
    });
}
void UpdateChecker::cancelDownload() { if (downloadReply_) downloadReply_->abort(); }
void UpdateChecker::failUpdate(const QString &message) { busy_=false; status_="error"; publish(); emit notify(message,true); }
void UpdateChecker::prepareUpdate(const QString &job) {
    QFile resource(":/updater/update.ps1"), script(job+"/update.ps1"), config(job+"/job.json");
    if (!resource.open(QIODevice::ReadOnly) || !script.open(QIODevice::WriteOnly) || !config.open(QIODevice::WriteOnly)) {
        failUpdate("Cannot prepare the update. Your current version is unchanged."); return;
    }
    const auto bytes=resource.readAll();
    const auto data=QJsonDocument(QJsonObject{{"target",QCoreApplication::applicationDirPath()},
        {"id",QFileInfo(job).fileName()},{"processId",double(QCoreApplication::applicationPid())},{"tag",available_["tag"]}}).toJson();
    if (script.write(bytes)!=bytes.size() || config.write(data)!=data.size()) {
        failUpdate("Cannot write the update job."); return;
    }
    script.close(); config.close();
    status_="preparing"; publish();
    auto *process=new QProcess(this);
    const QStringList args={"-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",job+"/update.ps1","-Job",job+"/job.json"};
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= 0x08000000; }); // CREATE_NO_WINDOW
#endif
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
        if(error==QProcess::FailedToStart) { process->deleteLater(); failUpdate("Windows could not start the updater. Your current version is unchanged."); }
    });
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,args,job](int code,QProcess::ExitStatus exit) {
        const QString error=QString::fromLocal8Bit(process->readAllStandardError()).left(600);
        process->deleteLater();
        if(code!=0 || exit!=QProcess::NormalExit) { failUpdate("Cannot prepare the update: "+error); return; }
        QProcess helper;
        helper.setProgram(powershell()); helper.setArguments(args+QStringList{"-Apply"}); helper.setWorkingDirectory(job);
#ifdef Q_OS_WIN
        helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= 0x08000000; });
#endif
        if(!helper.startDetached()) { failUpdate("Cannot start the update restart. Your current version is unchanged."); return; }
        settings().setValue("updates/job",job);
        settings().sync();
        status_="restarting"; publish();
        emit notify("Update ready. LAMBDA will restart and keep your library and saved playback positions.",false);
        QTimer::singleShot(800,this,[this] { emit restartRequested(); });
    });
    process->start(powershell(),args);
}
void UpdateChecker::releaseNotes() {
    const QString url = available_["notesUrl"].toString();
    if (trustedUrl(url, "/yneager/lambdaPlayer/releases/tag/")) QDesktopServices::openUrl(QUrl(url));
}
