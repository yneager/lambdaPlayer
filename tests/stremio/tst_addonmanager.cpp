// Addon management against a mock addon server (stremio-core
// update_profile.rs semantics + LAMBDA enable/reorder/persistence).

#include "mockaddonserver.h"
#include "stremio/addonmanager.h"
#include "stremio/addonurl.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace stremio;

class AddonManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void installAndDuplicate();
    void missingDefaultsPreserveConfigurationsAndRetry();
    void configuredUrlWithoutManifestSuffix();
    void sameIdDifferentConfigurations();
    void updateInPlace();
    void configurationRequired();
    void removeEnableReorderPersist();
    void stremioAddonHeader();
    void collectionImport();
    void collectionUrl();
    void failures();
    void refreshKeepsOldManifestOnError();

private:
    InstallOutcome install(AddonManager &manager, const QString &input);

    QTemporaryDir dir_;
    QString storage_;
};

namespace {

QByteArray manifest(const char *id, const char *version = "1.0.0", const char *extra = "")
{
    return QByteArray(R"({"id":")") + id + R"(","version":")" + version
        + R"(","name":"Addon )" + id + R"(","types":["movie","series"],"resources":["stream"])" + extra + "}";
}

} // namespace

void AddonManagerTest::init()
{
    QVERIFY(dir_.isValid());
    storage_ = dir_.filePath(QStringLiteral("addons-%1.json").arg(QRandomGenerator::global()->generate()));
}

InstallOutcome AddonManagerTest::install(AddonManager &manager, const QString &input)
{
    std::optional<InstallOutcome> outcome;
    manager.install(input, [&outcome](const InstallOutcome &o) { outcome = o; });
    QTest::qWaitFor([&outcome] { return outcome.has_value(); }, 20000);
    return outcome.value_or(InstallOutcome{});
}

void AddonManagerTest::installAndDuplicate()
{
    MockAddonServer server;
    server.route("/a/manifest.json", manifest("a"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    QSignalSpy changed(&manager, &AddonManager::changed);

    const InstallOutcome first = install(manager, server.base() + "/a/manifest.json");
    QCOMPARE(first.status, InstallOutcome::Status::Installed);
    QCOMPARE(manager.addons().size(), 1);
    QCOMPARE(changed.count(), 1);

    const InstallOutcome again = install(manager, server.base() + "/a/manifest.json");
    QCOMPARE(again.status, InstallOutcome::Status::AlreadyInstalled);
    QCOMPARE(manager.addons().size(), 1);
}

void AddonManagerTest::missingDefaultsPreserveConfigurationsAndRetry()
{
    MockAddonServer server;
    server.route("/configured/manifest.json", manifest("torrentio"));
    server.route("/cinemeta/manifest.json", manifest("cinemeta"));
    server.route("/subtitles/manifest.json", "unavailable", 503);
    AddonClient client;
    AddonManager manager(&client, storage_);
    const QString configured = server.base() + "/configured/manifest.json";
    QCOMPARE(install(manager, configured).status, InstallOutcome::Status::Installed);
    manager.setEnabled(configured, false);
    const QList<QPair<QString, QString>> defaults{
        {"cinemeta", server.base() + "/cinemeta/manifest.json"},
        {"subtitles", server.base() + "/subtitles/manifest.json"},
        {"torrentio", server.base() + "/unconfigured/manifest.json"},
    };
    std::optional<InstallOutcome> result;
    manager.installMissing(defaults, [&](const InstallOutcome &outcome) { result = outcome; });
    QVERIFY(QTest::qWaitFor([&] { return result.has_value(); }, 20000));
    QCOMPARE(result->status, InstallOutcome::Status::Failed);
    QCOMPARE(result->imported, 1);
    QCOMPARE(result->skipped, 1);
    QCOMPARE(result->failed, 1);
    QCOMPARE(server.count("/unconfigured/manifest.json"), 0);
    QCOMPARE(manager.addons().first().descriptor.transportUrl, configured);
    QVERIFY(!manager.addons().first().enabled);

    server.route("/subtitles/manifest.json", manifest("subtitles"));
    server.clearRequests();
    result.reset();
    manager.installMissing(defaults, [&](const InstallOutcome &outcome) { result = outcome; });
    QVERIFY(QTest::qWaitFor([&] { return result.has_value(); }, 20000));
    QCOMPARE(result->failed, 0);
    QCOMPARE(result->imported, 1);
    QCOMPARE(result->skipped, 2);
    QCOMPARE(server.count("/cinemeta/manifest.json"), 0);
    QCOMPARE(server.count("/unconfigured/manifest.json"), 0);
    QCOMPARE(manager.addons().size(), 3);

    server.clearRequests();
    result.reset();
    manager.installMissing(defaults, [&](const InstallOutcome &outcome) { result = outcome; });
    QVERIFY(result.has_value());
    QCOMPARE(result->skipped, 3);
    QVERIFY(result->message.startsWith("All installed"));
    QVERIFY(server.requests().isEmpty());

    QVERIFY(manager.remove(server.base() + "/subtitles/manifest.json"));
    result.reset();
    manager.installMissing(defaults, [&](const InstallOutcome &outcome) { result = outcome; });
    QVERIFY(QTest::qWaitFor([&] { return result.has_value(); }, 20000));
    QCOMPARE(result->imported, 1);
    QCOMPARE(result->skipped, 2);
}

void AddonManagerTest::configuredUrlWithoutManifestSuffix()
{
    MockAddonServer server;
    server.route("/providers=yts%7Ceztv/manifest.json?token=1", manifest("torrentio"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    const InstallOutcome outcome = install(manager, server.base() + "/providers=yts%7Ceztv?token=1");
    QCOMPARE(outcome.status, InstallOutcome::Status::Installed);
    QCOMPARE(manager.addons().first().descriptor.transportUrl,
             server.base() + "/providers=yts%7Ceztv/manifest.json?token=1");
    // stremio:// links work too.
    server.route("/cfg2/manifest.json", manifest("other"));
    const QString stremioLink = QString(server.base()).replace("http://", "stremio://") + "/cfg2/manifest.json";
    // stremio:// maps to https, which the local test server cannot speak: only check normalisation.
    InstallUrl url;
    QVERIFY(normalizeInstallUrl(stremioLink, url));
    QVERIFY(url.manifestUrl.startsWith("https://127.0.0.1:"));
}

void AddonManagerTest::sameIdDifferentConfigurations()
{
    MockAddonServer server;
    server.route("/rd/manifest.json", manifest("torrentio"));
    server.route("/ad/manifest.json", manifest("torrentio"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    QCOMPARE(install(manager, server.base() + "/rd/manifest.json").status, InstallOutcome::Status::Installed);
    QCOMPARE(install(manager, server.base() + "/ad/manifest.json").status, InstallOutcome::Status::Installed);
    QCOMPARE(manager.addons().size(), 2);
}

void AddonManagerTest::updateInPlace()
{
    MockAddonServer server;
    server.route("/a/manifest.json", manifest("a"));
    server.route("/b/manifest.json", manifest("b"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    install(manager, server.base() + "/a/manifest.json");
    install(manager, server.base() + "/b/manifest.json");
    server.route("/a/manifest.json", manifest("a", "1.1.0"));
    const InstallOutcome updated = install(manager, server.base() + "/a/manifest.json");
    QCOMPARE(updated.status, InstallOutcome::Status::Updated);
    QCOMPARE(manager.addons().size(), 2);
    QCOMPARE(manager.addons()[0].descriptor.manifest.version, QString("1.1.0")); // position kept
}

void AddonManagerTest::configurationRequired()
{
    MockAddonServer server;
    server.route("/manifest.json", manifest("needs-config", "1.0.0", R"(,"behaviorHints":{"configurable":true,"configurationRequired":true})"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    const InstallOutcome outcome = install(manager, server.base() + "/manifest.json");
    QCOMPARE(outcome.status, InstallOutcome::Status::ConfigurationRequired);
    QCOMPARE(outcome.configureUrl, server.base() + "/configure");
    QVERIFY(manager.addons().isEmpty());
}

void AddonManagerTest::removeEnableReorderPersist()
{
    MockAddonServer server;
    for (const char *id : {"a", "b", "c"}) {
        server.route(QByteArray("/") + id + "/manifest.json", manifest(id));
    }
    AddonClient client;
    {
        AddonManager manager(&client, storage_);
        for (const char *id : {"a", "b", "c"}) {
            install(manager, server.base() + "/" + id + "/manifest.json");
        }
        const QString a = server.base() + "/a/manifest.json";
        const QString b = server.base() + "/b/manifest.json";
        const QString c = server.base() + "/c/manifest.json";
        QVERIFY(manager.move(c, 0));
        QVERIFY(manager.setEnabled(a, false));
        QCOMPARE(manager.activeDescriptors().size(), 2);
        QCOMPARE(manager.activeDescriptors()[0].transportUrl, c);
        QVERIFY(manager.remove(b));
        QVERIFY(!manager.remove(b));
    }
    AddonManager reloaded(&client, storage_);
    reloaded.load();
    QCOMPARE(reloaded.addons().size(), 2);
    QCOMPARE(reloaded.addons()[0].descriptor.manifest.id, QString("c"));
    QCOMPARE(reloaded.addons()[1].descriptor.manifest.id, QString("a"));
    QVERIFY(!reloaded.addons()[1].enabled);
    QCOMPARE(reloaded.exportCollection().size(), 2);
}

void AddonManagerTest::stremioAddonHeader()
{
    MockAddonServer server;
    MockAddonServer::Route landing;
    landing.body = "<html>landing page</html>";
    landing.headers.emplaceBack("X-Stremio-Addon", "/real/manifest.json");
    server.route("/landing", landing);
    server.route("/landing/manifest.json", "<html>nope</html>", 404);
    server.route("/real/manifest.json", manifest("real"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    const InstallOutcome outcome = install(manager, server.base() + "/landing");
    QCOMPARE(outcome.status, InstallOutcome::Status::Installed);
    QCOMPARE(manager.addons().first().descriptor.transportUrl, server.base() + "/real/manifest.json");
}

void AddonManagerTest::collectionImport()
{
    MockAddonServer server;
    server.route("/fetched/manifest.json", manifest("fetched"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    const QByteArray json = QByteArray(R"({"result":{"addons":[
        {"transportUrl":"https://embedded.example/manifest.json","manifest":)") + manifest("embedded") + R"(,"flags":{"official":false}},
        {"transportUrl":")" + server.base().toUtf8() + R"(/fetched/manifest.json"},
        {"transportUrl":"https://embedded.example/manifest.json","manifest":)" + manifest("embedded") + R"(},
        {"manifest":{"id":"no-url"}},
        "not an object or url"
    ]}})";
    std::optional<InstallOutcome> outcome;
    manager.importCollection(QJsonDocument::fromJson(json).object(), [&outcome](const InstallOutcome &o) { outcome = o; });
    QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 10000);
    QCOMPARE(outcome->status, InstallOutcome::Status::CollectionImported);
    QCOMPARE(outcome->imported, 2);
    QCOMPARE(outcome->skipped, 1);
    QCOMPARE(outcome->failed, 2);
    QCOMPARE(manager.addons().size(), 2);
    QCOMPARE(manager.addons()[0].descriptor.manifest.id, QString("embedded"));
}

void AddonManagerTest::collectionUrl()
{
    MockAddonServer server;
    const QByteArray collection = QByteArray("[{\"transportUrl\":\"https://x.example/manifest.json\",\"manifest\":")
        + manifest("x") + "}]";
    server.route("/my-addons.json", collection);
    AddonClient client;
    AddonManager manager(&client, storage_);
    const InstallOutcome outcome = install(manager, server.base() + "/my-addons.json");
    QCOMPARE(outcome.status, InstallOutcome::Status::CollectionImported);
    QCOMPARE(outcome.imported, 1);
}

void AddonManagerTest::failures()
{
    MockAddonServer server;
    server.route("/html/manifest.json", "<html>configure page</html>");
    server.route("/html", "<html>configure page</html>");
    AddonClient client;
    AddonManager manager(&client, storage_);
    QCOMPARE(install(manager, server.base() + "/html/manifest.json").status, InstallOutcome::Status::Failed);
    QCOMPARE(install(manager, server.base() + "/html").status, InstallOutcome::Status::Failed);
    QCOMPARE(install(manager, "not a url").status, InstallOutcome::Status::Failed);
    QVERIFY(manager.addons().isEmpty());
}

void AddonManagerTest::refreshKeepsOldManifestOnError()
{
    MockAddonServer server;
    server.route("/a/manifest.json", manifest("a"));
    AddonClient client;
    AddonManager manager(&client, storage_);
    install(manager, server.base() + "/a/manifest.json");
    server.route("/a/manifest.json", "offline", 503);
    std::optional<bool> ok;
    manager.refresh(server.base() + "/a/manifest.json", [&ok](bool success, const QString &) { ok = success; });
    QTRY_VERIFY_WITH_TIMEOUT(ok.has_value(), 5000);
    QVERIFY(!*ok);
    QCOMPARE(manager.addons().first().descriptor.manifest.id, QString("a"));
    QVERIFY(manager.addons().first().lastError.contains("503"));

    server.route("/a/manifest.json", manifest("a", "2.0.0"));
    ok.reset();
    manager.refresh(server.base() + "/a/manifest.json", [&ok](bool success, const QString &) { ok = success; });
    QTRY_VERIFY_WITH_TIMEOUT(ok.has_value(), 5000);
    QVERIFY(*ok);
    QCOMPARE(manager.addons().first().descriptor.manifest.version, QString("2.0.0"));
    QVERIFY(manager.addons().first().lastError.isEmpty());
}

QTEST_GUILESS_MAIN(AddonManagerTest)
#include "tst_addonmanager.moc"
