#include "locallibrary.h"
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class LibraryTest : public QObject {
    Q_OBJECT
private slots:
    void nestedSeasonsAndNaturalOrder() {
        QTemporaryDir temp;
        const QString root=temp.path()+"/Example.Show.2024.1080p";
        QVERIFY(QDir().mkpath(root+"/Season 1"));
        QVERIFY(QDir().mkpath(root+"/Season 2"));
        for (const QString &name : {QString("Season 1/Example.S01E10.mkv"),QString("Season 1/Example.S01E02.MP4"),QString("Season 2/Example.S02E01.mkv"),QString("readme.txt")}) {
            QFile file(root+"/"+name); QVERIFY(file.open(QIODevice::WriteOnly));
        }
        const auto item=LocalLibrary::scan(root);
        QCOMPARE(item["title"].toString(),QString("Example Show"));
        QCOMPARE(item["type"].toString(),QString("series"));
        const auto episodes=item["episodes"].toArray();
        QCOMPARE(episodes.size(),3);
        QCOMPARE(episodes[0].toObject()["episode"].toInt(),2);
        QCOMPARE(episodes[1].toObject()["episode"].toInt(),10);
        QCOMPARE(episodes[2].toObject()["season"].toInt(),2);
    }
    void movieAndEmptyFolder() {
        QTemporaryDir temp;
        QCOMPARE(LocalLibrary::scan(temp.path())["episodes"].toArray().size(),0);
        QFile file(temp.path()+"/A.Movie.2023.2160p.WEB-DL.mkv"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        const auto item=LocalLibrary::scan(temp.path());
        QCOMPARE(item["type"].toString(),QString("movie"));
        QCOMPARE(item["title"].toString(),QString("A Movie"));
    }
    void persistentEpisodeProgressAndRemoval() {
        QTemporaryDir temp;
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
        const QJsonArray episodes{QJsonObject{{"path","C:/shows/one.mkv"}},QJsonObject{{"path","C:/shows/two.mkv"}}};
        QSettings settings(QSettings::IniFormat,QSettings::UserScope,"LAMBDA","LAMBDA Player");
        settings.setValue("library/items",QJsonDocument(QJsonArray{QJsonObject{{"folder","C:/shows"},{"episodes",episodes}}}).toJson());
        settings.setValue("playback/history",QVariantMap{{"C:/shows/two.mkv",QVariantMap{{"position",123.5},{"duration",600.0},{"watched",false}}}});
        settings.sync();
        LocalLibrary library(nullptr);
        QCOMPARE(library.nextFile("C:/shows/one.mkv"),QString("C:/shows/two.mkv"));
        QCOMPARE(library.state()[0].toObject()["episodes"].toArray()[1].toObject()["position"].toDouble(),123.5);
        QSignalSpy play(&library,&LocalLibrary::playRequested);
        library.play("C:/shows","C:/untrusted.mkv"); QCOMPARE(play.count(),0);
        library.play("C:/shows","C:/shows/two.mkv"); QCOMPARE(play.count(),1);
        library.remove("C:/shows");
        LocalLibrary reloaded(nullptr); QCOMPARE(reloaded.state().size(),0);
        QVERIFY(settings.value("playback/history").toMap().contains("C:/shows/two.mkv"));
    }
};
QTEST_MAIN(LibraryTest)
#include "tst_locallibrary.moc"
