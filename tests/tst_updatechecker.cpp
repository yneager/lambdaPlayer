#include "updatechecker.h"
#include <QJsonDocument>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <QSslServer>
#include <QSslKey>
#include <QFile>
#include <QCryptographicHash>

namespace {
QJsonObject release(const QString &tag, bool test = false) {
    const QString prefix = "https://github.com/yneager/lambdaPlayer/releases/";
    return {{"tag_name",tag},{"prerelease",test},{"draft",false},{"html_url",prefix+"tag/"+tag},
            {"assets",QJsonArray{QJsonObject{{"name","LAMBDA-Player-Windows-x64-"+tag+".zip"},{"state","uploaded"},
                {"browser_download_url",prefix+"download/"+tag+"/update.zip"},{"digest","sha256:"+QString(64,'a')},{"size",1234}}}}};
}
class Feed : public QTcpServer {
public:
    QByteArray body;
    int requests = 0;
    Feed() {
        listen(QHostAddress::LocalHost,0);
        connect(this,&QTcpServer::newConnection,this,[this] {
            auto *socket=nextPendingConnection();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket] {
                auto data=socket->property("request").toByteArray()+socket->readAll();
                socket->setProperty("request",data);
                if(!data.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
                socket->setProperty("sent",true); ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);
                socket->disconnectFromHost();
            });
        });
    }
    QUrl url() const { return QUrl("http://127.0.0.1:"+QString::number(serverPort())+"/releases"); }
};
// This key/certificate are public test fixtures, trusted only by this test process.
class SecureDownload : public QSslServer {
public:
    QByteArray body="verified update fixture";
    SecureDownload() {
        QFile certificate(QStringLiteral(LAMBDA_UPDATE_TEST_DATA "/cert.pem")); certificate.open(QIODevice::ReadOnly);
        const QSslCertificate cert(certificate.readAll());
        QFile key(QStringLiteral(LAMBDA_UPDATE_TEST_DATA "/key.pem")); key.open(QIODevice::ReadOnly);
        auto serverConfig=QSslConfiguration::defaultConfiguration();
        serverConfig.setLocalCertificate(cert); serverConfig.setPrivateKey(QSslKey(key.readAll(),QSsl::Rsa));
        serverConfig.setPeerVerifyMode(QSslSocket::VerifyNone); setSslConfiguration(serverConfig);
        auto clientConfig=QSslConfiguration::defaultConfiguration(); clientConfig.addCaCertificate(cert);
        QSslConfiguration::setDefaultConfiguration(clientConfig);
        listen(QHostAddress::LocalHost,0);
        connect(this,&QTcpServer::pendingConnectionAvailable,this,[this] {
            auto *socket=nextPendingConnection();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket] {
                socket->readAll(); if(socket->property("sent").toBool()) return;
                socket->setProperty("sent",true);
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);
                socket->disconnectFromHost();
            });
        });
    }
    QUrl url() const { return QUrl("https://localhost:"+QString::number(serverPort())+"/update.zip"); }
};
}
class UpdateTest : public QObject {
    Q_OBJECT
    QTemporaryDir profile_;
private slots:
    void init() {
        qputenv("LAMBDA_DATA_DIR",profile_.path().toUtf8());
        QSettings settings(profile_.path()+"/player.ini",QSettings::IniFormat); settings.clear(); settings.sync();
    }
    void cleanupTestCase() { qunsetenv("LAMBDA_DATA_DIR"); }
    void versionOrder_data() {
        QTest::addColumn<QString>("candidate"); QTest::addColumn<QString>("current"); QTest::addColumn<bool>("newer");
        QTest::newRow("numeric-version") << "v0.10.0" << "v0.9.0" << true;
        QTest::newRow("numeric-test") << "v0.2.7-test.10" << "v0.2.7-test.2" << true;
        QTest::newRow("stable-promotes-test") << "v0.2.7" << "v0.2.7-test.2" << true;
        QTest::newRow("no-stable-downgrade") << "v0.2.7-test.10" << "v0.2.7" << false;
        QTest::newRow("equal") << "v0.2.7-test.2" << "v0.2.7-test.2" << false;
        QTest::newRow("older") << "v0.2.6" << "v0.2.7-test.2" << false;
        QTest::newRow("build-metadata") << "v0.2.7+build.2" << "v0.2.7+build.1" << false;
        QTest::newRow("invalid") << "latest" << "v0.2.7" << false;
    }
    void versionOrder() {
        QFETCH(QString,candidate); QFETCH(QString,current); QFETCH(bool,newer);
        QCOMPARE(UpdateChecker::newerThan(candidate,current),newer);
    }
    void stableAndTestChannels() {
        const QJsonArray feed{release("v0.3.0-test.1",true),release("v0.2.8"),release("v0.2.7-test.10",true)};
        QCOMPARE(UpdateChecker::selectRelease(feed,"v0.2.7-test.2",false)["tag"].toString(),QString("v0.2.8"));
        QCOMPARE(UpdateChecker::selectRelease(feed,"v0.2.7-test.2",true)["tag"].toString(),QString("v0.3.0-test.1"));
        QVERIFY(UpdateChecker::selectRelease(feed,"v0.3.0",true).isEmpty());
    }
    void ignoreIncompleteDraftOrWrongAssets() {
        auto draft=release("v9.0.0"); draft.insert("draft",true);
        auto uploading=release("v8.0.0"); auto asset=uploading["assets"].toArray()[0].toObject(); asset.insert("state","uploading"); uploading.insert("assets",QJsonArray{asset});
        auto wrongHost=release("v7.0.0"); asset=wrongHost["assets"].toArray()[0].toObject(); asset.insert("browser_download_url","https://example.com/setup.exe"); wrongHost.insert("assets",QJsonArray{asset});
        auto zip=release("v6.0.0"); asset=zip["assets"].toArray()[0].toObject(); asset.insert("name","Source.zip"); zip.insert("assets",QJsonArray{asset});
        QCOMPARE(UpdateChecker::selectRelease(QJsonArray{draft,uploading,wrongHost,zip,release("v0.2.8")},"v0.2.7",false)["tag"].toString(),QString("v0.2.8"));
    }
    void independentHostingFeed() {
        QJsonObject r{{"version","v0.2.8"},{"url","https://updates.example.com/player.zip"},
                      {"sha256",QString(64,'a')},{"size",1234},{"notes","Faster updates"}};
        QJsonObject feed{{"schemaVersion",1},{"releases",QJsonArray{r}}};
        QCOMPARE(UpdateChecker::selectFeed(feed,"v0.2.7",false)["tag"].toString(),QString("v0.2.8"));
        r["url"]="http://updates.example.com/player.zip"; feed["releases"]=QJsonArray{r};
        QVERIFY(UpdateChecker::selectFeed(feed,"v0.2.7",false).isEmpty());
        r["url"]="https://updates.example.com/player.zip"; r["sha256"]="bad"; feed["releases"]=QJsonArray{r};
        QVERIFY(UpdateChecker::selectFeed(feed,"v0.2.7",false).isEmpty());
        r["sha256"]=QString(64,'a'); r["version"]="v0.2.8-test.1"; feed["releases"]=QJsonArray{r};
        QVERIFY(UpdateChecker::selectFeed(feed,"v0.2.7",false).isEmpty());
        QVERIFY(!UpdateChecker::selectFeed(feed,"v0.2.7",true).isEmpty());
    }
    void missingDigestRejected() {
        auto r=release("v0.2.8"); auto asset=r["assets"].toArray()[0].toObject();
        asset.remove("digest"); r["assets"]=QJsonArray{asset};
        QVERIFY(UpdateChecker::selectRelease(QJsonArray{r},"v0.2.7",true).isEmpty());
    }
    void downloadVerificationAndCancellation() {
        QVERIFY(QSslSocket::supportsSsl());
        SecureDownload payload; Feed feed;
        auto manifest=[&](const QString &hash,qint64 size) {
            feed.body=QJsonDocument(QJsonObject{{"schemaVersion",1},{"releases",QJsonArray{QJsonObject{
                {"version","v0.2.8"},{"url",payload.url().toString()},{"sha256",hash},{"size",double(size)}}}}}).toJson();
        };
        manifest(QString(64,'0'),payload.body.size());
        UpdateChecker checker("v0.2.7",nullptr,feed.url());
        checker.check(); QTRY_VERIFY(checker.state()["available"].toBool());
        QSignalSpy messages(&checker,&UpdateChecker::notify), restarts(&checker,&UpdateChecker::restartRequested);
        checker.download(); QTRY_COMPARE(checker.state()["status"].toString(),QString("error"));
        QVERIFY(messages.last()[0].toString().contains("verification failed"));
        QCOMPARE(restarts.count(),0);
        const auto digest=QString::fromLatin1(QCryptographicHash::hash(payload.body,QCryptographicHash::Sha256).toHex());
        manifest(digest,payload.body.size()+1);
        checker.check(); QTRY_COMPARE(checker.state()["status"].toString(),QString("ready"));
        checker.download(); QTRY_COMPARE(checker.state()["status"].toString(),QString("error"));
        QVERIFY(messages.last()[0].toString().contains("verification failed"));
        manifest(digest,payload.body.size());
        checker.check(); QTRY_COMPARE(checker.state()["status"].toString(),QString("ready"));
        checker.download(); checker.cancelDownload();
        QTRY_COMPARE(checker.state()["status"].toString(),QString("ready"));
        QCOMPARE(restarts.count(),0);
        checker.download(); QTRY_COMPARE(checker.state()["status"].toString(),QString("error"));
        // The test binary deliberately has no embedded helper: verification passes,
        // then preparation stops before accessing the real application directory.
        QVERIFY(messages.last()[0].toString().contains("Cannot prepare"));
        QCOMPARE(restarts.count(),0);
    }
    void backgroundFetchCachingAndDismissal() {
        Feed feed; feed.body=QJsonDocument(QJsonArray{release("v0.2.8")}).toJson();
        UpdateChecker checker("v0.2.7-test.2",nullptr,feed.url());
        checker.check(false); checker.check(false);
        QTRY_VERIFY(checker.state()["available"].toBool());
        QCOMPARE(feed.requests,1);
        QVERIFY(checker.state()["showNotification"].toBool());
        checker.dismiss(); QVERIFY(!checker.state()["showNotification"].toBool());
        UpdateChecker restarted("v0.2.7-test.2",nullptr,feed.url());
        QVERIFY(restarted.state()["available"].toBool());
        QVERIFY(!restarted.state()["showNotification"].toBool());
        restarted.check(false); QTest::qWait(100); QCOMPARE(feed.requests,1);
        restarted.check(true); QTRY_VERIFY(restarted.state()["showNotification"].toBool());
        QCOMPARE(feed.requests,2);
        UpdateChecker installed("v0.2.8",nullptr,feed.url()); QVERIFY(!installed.state()["available"].toBool());
    }
    void backgroundFailureQuietManualFailureVisible() {
        Feed feed; feed.body="{}";
        UpdateChecker checker("v0.2.7",nullptr,feed.url());
        QSignalSpy messages(&checker,&UpdateChecker::notify);
        checker.check(false); QTRY_COMPARE(checker.state()["status"].toString(),QString("error"));
        QCOMPARE(messages.count(),0);
        checker.check(true); QTRY_COMPARE(messages.count(),1);
    }
};
QTEST_GUILESS_MAIN(UpdateTest)
#include "tst_updatechecker.moc"
