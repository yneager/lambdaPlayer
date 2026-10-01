#pragma once

// Installed addons: the equivalent of Stremio's profile.addons.
// Semantics from stremio-core src/models/ctx/update_profile.rs:
// - identity is the transport URL (the same manifest id may be installed
//   several times with different configurations);
// - installing an identical descriptor again is refused, installing the same
//   URL again replaces it in place, a new URL is appended;
// - a manifest with behaviorHints.configurationRequired cannot be installed;
// - order is the list order (stremio-addon-manager reorders the same list).
// LAMBDA additions: an enabled flag (disabled addons are skipped when
// planning requests) and the last refresh error, persisted with the list.

#include "stremio/addonclient.h"
#include "stremio/manifest.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace stremio {

struct InstalledAddon
{
    Descriptor descriptor;
    bool enabled = true;
    QString lastError;
    QDateTime updated;
};

struct InstallOutcome
{
    enum class Status {
        Installed,
        Updated,
        AlreadyInstalled,
        ConfigurationRequired,
        CollectionImported,
        Failed,
    };
    Status status = Status::Failed;
    QString message;
    QString transportUrl;
    QString addonName;
    QString configureUrl; // for ConfigurationRequired
    int imported = 0;
    int skipped = 0;
    int failed = 0;
};

class AddonManager final : public QObject
{
    Q_OBJECT

public:
    explicit AddonManager(AddonClient *client, const QString &storagePath, QObject *parent = nullptr);

    const QList<InstalledAddon> &addons() const { return addons_; }
    // Enabled addons, in order: the list request planning uses.
    QList<Descriptor> activeDescriptors() const;
    const InstalledAddon *find(const QString &transportUrl) const;
    int indexOf(const QString &transportUrl) const;

    // Install from anything a user can paste: a manifest URL (configured or
    // not, stremio://, legacy /stremio/v1), a URL whose response carries an
    // X-Stremio-Addon header, or a JSON collection URL.
    void install(const QString &input, std::function<void(const InstallOutcome &)> done);
    // Defaults are identified by manifest id, preserving configured URLs.
    void installMissing(QList<QPair<QString, QString>> defaults,
                        std::function<void(const InstallOutcome &)> done);
    // Install a descriptor (from an addon catalog): the manifest is fetched
    // fresh from its transport URL first, as Stremio's AddonDetails does.
    void installFromTransportUrl(const QString &transportUrl, std::function<void(const InstallOutcome &)> done);
    // Import a collection: [descriptor], {addons}, {result:{addons}},
    // {addonCollection:{addons}} or a list of manifest URLs.
    // Entries with a valid embedded manifest are added directly; the others
    // are fetched. `done` runs once every entry is settled.
    void importCollection(const QJsonValue &json, std::function<void(const InstallOutcome &)> done);
    QJsonArray exportCollection() const;

    bool remove(const QString &transportUrl);
    bool setEnabled(const QString &transportUrl, bool enabled);
    bool move(const QString &transportUrl, int toIndex);
    // Refetch the manifest(s) and upgrade in place (UpgradeAddon).
    void refresh(const QString &transportUrl, std::function<void(bool ok, const QString &error)> done = {});
    void refreshAll();

    // Adds a descriptor whose manifest is already known.
    InstallOutcome addDescriptor(const Descriptor &descriptor);

    void load();
    bool save() const;

signals:
    void changed();

private:
    void installManifestUrl(const QString &transportUrl, std::function<void(const InstallOutcome &)> done,
                            std::function<void()> onFailureFallback = {});
    void installFromDetection(const QString &url, std::function<void(const InstallOutcome &)> done,
                              const QString &previousError);

    AddonClient *client_;
    QString storagePath_;
    QList<InstalledAddon> addons_;
};

QString installStatusName(InstallOutcome::Status status);

} // namespace stremio
