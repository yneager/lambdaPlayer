#include "locallibrary.h"
#include <QCollator>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>
#include <algorithm>

namespace {
QSettings settings() {
    const QString dataDir = qEnvironmentVariable("LAMBDA_DATA_DIR");
    if (!dataDir.isEmpty()) {
        QDir().mkpath(dataDir);
        return QSettings(QDir(dataDir).filePath("player.ini"), QSettings::IniFormat);
    }
    return QSettings(QSettings::IniFormat, QSettings::UserScope, "LAMBDA", "LAMBDA Player");
}
QString cleanTitle(QString name) {
    name.replace(QRegularExpression("[._]"), " ");
    name.remove(QRegularExpression("\\b(?:S\\d{1,2}E\\d{1,3}|Season\\s*\\d+|(?:19|20)\\d{2}|2160p|1080p|720p|480p|BluRay|WEB[- ]?DL|WEBRip|HDTV|x26[45]|HEVC|REMUX)\\b.*", QRegularExpression::CaseInsensitiveOption));
    name.remove(QRegularExpression("[\\[\\]()]+"));
    return name.simplified();
}
}
LocalLibrary::LocalLibrary(QWidget *parent) : QObject(parent), dialogParent_(parent) {
    items_ = QJsonDocument::fromJson(settings().value("library/items").toByteArray()).array();
}
QJsonObject LocalLibrary::scan(const QString &folder) {
    const QStringList extensions{"mkv","mp4","m4v","avi","mov","webm","wmv","flv","ts","mts","m2ts","mpg","mpeg","ogv","3gp","vob"};
    QStringList paths;
    QDirIterator it(folder, QDir::Files | QDir::Readable | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (extensions.contains(QFileInfo(path).suffix().toLower())) paths.append(path);
    }
    QCollator collator; collator.setNumericMode(true); collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(paths.begin(), paths.end(), [&collator](const QString &a, const QString &b) { return collator.compare(a,b)<0; });
    QJsonArray episodes;
    const QRegularExpression episode("(?:s(\\d{1,2})[ ._-]*e(\\d{1,3})|(\\d{1,2})x(\\d{1,3}))", QRegularExpression::CaseInsensitiveOption);
    bool series = false;
    for (const QString &path : paths) {
        const QString name = QFileInfo(path).completeBaseName();
        const auto m = episode.match(name);
        QJsonObject entry{{"path",path},{"name",name},{"relative",QDir(folder).relativeFilePath(path)}};
        if (m.hasMatch()) {
            series = true;
            entry.insert("season", m.captured(m.captured(1).isEmpty()?3:1).toInt());
            entry.insert("episode",m.captured(m.captured(2).isEmpty()?4:2).toInt());
        }
        episodes.append(entry);
    }
    QString name = cleanTitle(QFileInfo(folder).fileName());
    if (name.isEmpty()) name = QFileInfo(folder).fileName();
    if (paths.size()==1 && !series) {
        const QString fileTitle = cleanTitle(QFileInfo(paths.first()).completeBaseName());
        if (!fileTitle.isEmpty()) name = fileTitle;
    }
    return {{"folder",folder},{"title",name},{"type",series || paths.size()>1 ? "series":"movie"},{"episodes",episodes}};
}
int LocalLibrary::indexOf(const QString &folder) const {
    for (int i=0;i<items_.size();++i) if(items_[i].toObject()["folder"].toString().compare(folder,Qt::CaseInsensitive)==0) return i;
    return -1;
}
void LocalLibrary::save() { settings().setValue("library/items",QJsonDocument(items_).toJson(QJsonDocument::Compact)); emit changed(state()); }
QJsonArray LocalLibrary::state() const {
    QJsonArray result;
    const QVariantMap history = settings().value("playback/history").toMap();
    for (const auto &value : items_) {
        auto item=value.toObject();
        QFile poster(item["posterFile"].toString());
        if(!poster.fileName().isEmpty() && poster.open(QIODevice::ReadOnly)) item.insert("poster",QString("data:image/jpeg;base64,")+QString::fromLatin1(poster.readAll().toBase64()));
        QJsonArray episodes;
        for(const auto &episode:item["episodes"].toArray()) {
            auto e=episode.toObject();
            auto progress=history.value(e["path"].toString()).toMap();
            e.insert("position",progress.value("position").toDouble());
            e.insert("duration",progress.value("duration").toDouble());
            e.insert("watched",progress.value("watched").toBool());
            episodes.append(e);
        }
        item.insert("episodes",episodes); result.append(item);
    }
    return result;
}
void LocalLibrary::chooseFolder() {
    const QString folder=QFileDialog::getExistingDirectory(dialogParent_,"Add a movie or series folder");
    if(!folder.isEmpty()) importFolder(QDir(folder).absolutePath());
}
void LocalLibrary::importFolder(const QString &folder) {
    emit notify("Scanning video files…",false);
    auto *watcher=new QFutureWatcher<QJsonObject>(this);
    connect(watcher,&QFutureWatcher<QJsonObject>::finished,this,[this,watcher,folder] {
        auto item=watcher->result(); watcher->deleteLater();
        if(item["episodes"].toArray().isEmpty()) { emit notify("No video files found in that folder.",true); return; }
        const int i=indexOf(folder);
        if(i>=0) {
            const auto old=items_[i].toObject();
            for(const QString &key:{QString("posterFile"),QString("poster"),QString("title"),QString("metaId")}) if(old.contains(key)) item.insert(key,old[key]);
            items_[i]=item;
        } else items_.append(item);
        save(); emit notify("Folder added to your library.",false);
        if(item["posterFile"].toString().isEmpty()) lookup(folder,item["title"].toString(),false);
    });
    watcher->setFuture(QtConcurrent::run([folder] { return scan(folder); }));
}
void LocalLibrary::refresh(const QString &folder) { if(indexOf(folder)>=0) importFolder(folder); }
void LocalLibrary::remove(const QString &folder) { const int i=indexOf(folder); if(i>=0) { items_.removeAt(i); save(); } }
void LocalLibrary::matchArtwork(const QString &folder) {
    const int i=indexOf(folder); if(i<0) return;
    bool ok=false;
    const QString title=QInputDialog::getText(dialogParent_,"Find artwork","Movie or series name",QLineEdit::Normal,items_[i].toObject()["title"].toString(),&ok);
    if(ok && !title.trimmed().isEmpty()) lookup(folder,title.trimmed(),true);
}
void LocalLibrary::lookup(const QString &folder,const QString &title,bool interactive) {
    const int i=indexOf(folder); if(i<0) return;
    const QString type=items_[i].toObject()["type"].toString();
    QNetworkRequest request(QUrl("https://v3-cinemeta.strem.io/catalog/"+type+"/top/search="+QString::fromUtf8(QUrl::toPercentEncoding(title))+".json"));
    request.setTransferTimeout(15000);
    auto *reply=network_.get(request);
    connect(reply,&QNetworkReply::finished,this,[this,reply,folder,title,interactive] {
        const auto metas=QJsonDocument::fromJson(reply->readAll()).object()["metas"].toArray(); reply->deleteLater();
        if(metas.isEmpty()) { emit notify("No artwork match found. Use Find artwork to try another name.",false); return; }
        int chosen=-1;
        for(int j=0;j<metas.size();++j) if(metas[j].toObject()["name"].toString().compare(title,Qt::CaseInsensitive)==0) { chosen=j; break; }
        if(interactive || chosen<0) {
            QStringList names;
            for(const auto &m:metas) { auto o=m.toObject(); names.append(o["name"].toString()+" · "+o["releaseInfo"].toString()+" · "+o["id"].toString()); }
            bool ok=false;
            const QString choice=QInputDialog::getItem(dialogParent_,"Choose matching title","Artwork matches for "+title,names,chosen<0?0:chosen,false,&ok);
            if(!ok) return;
            chosen=names.indexOf(choice);
        }
        const auto meta=metas[chosen].toObject();
        const int index=indexOf(folder); if(index<0) return;
        auto item=items_[index].toObject(); item.insert("title",meta["name"]); item.insert("metaId",meta["id"]); items_[index]=item; save();
        const QUrl url(meta["poster"].toString()); if(url.scheme()!="https") return;
        QNetworkRequest imageRequest(url); imageRequest.setTransferTimeout(15000);
        auto *image=network_.get(imageRequest);
        connect(image,&QNetworkReply::finished,this,[this,image,folder] {
            const auto data=image->readAll(); const bool ok=image->error()==QNetworkReply::NoError; image->deleteLater();
            const int index=indexOf(folder); if(!ok || index<0 || data.isEmpty() || data.size()>8*1024*1024) return;
            const QString profile=qEnvironmentVariable("LAMBDA_DATA_DIR");
            const QString dir=(profile.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation):profile)+"/posters"; QDir().mkpath(dir);
            const QString path=dir+"/"+QString::fromLatin1(QCryptographicHash::hash(folder.toUtf8(),QCryptographicHash::Sha256).toHex())+".jpg";
            QSaveFile file(path); if(!file.open(QIODevice::WriteOnly)) return; file.write(data); if(!file.commit()) return;
            auto item=items_[index].toObject(); item.insert("posterFile",path); items_[index]=item; save();
        });
    });
}
void LocalLibrary::play(const QString &folder,const QString &path) {
    const int i=indexOf(folder); if(i<0) return;
    for(const auto &e:items_[i].toObject()["episodes"].toArray()) if(e.toObject()["path"].toString()==path) { emit playRequested(path); return; }
}
QString LocalLibrary::nextFile(const QString &path) const {
    for(const auto &item:items_) {
        const auto episodes=item.toObject()["episodes"].toArray();
        for(int i=0;i+1<episodes.size();++i) if(episodes[i].toObject()["path"].toString()==path) return episodes[i+1].toObject()["path"].toString();
    }
    return {};
}
