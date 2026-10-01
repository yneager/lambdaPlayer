#include "stremio/addonmanager.h"

#include "stremio/addonurl.h"
#include "stremio/json.h"
#include "stremio/transport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#include <memory>
#include <algorithm>

namespace stremio {

void AddonManager::installMissing(QList<QPair<QString, QString>> defaults,
                                std::function<void(const InstallOutcome &)> done)
{
    auto pending = std::make_shared<QList<QPair<QString, QString>>>(std::move(defaults));
    auto summary = std::make_shared<InstallOutcome>();
    summary->status = InstallOutcome::Status::CollectionImported;
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakNext = next;
    *next = [this, pending, summary, done, weakNext] {
        auto next = weakNext.lock();
        while (!pending->isEmpty()) {
            const auto entry = pending->takeFirst();
            const bool installed = std::any_of(addons_.cbegin(), addons_.cend(), [&entry](const InstalledAddon &addon) {
                return addon.descriptor.manifest.id == entry.first;
            });
            if (installed) { ++summary->skipped; continue; }
            install(entry.second, [summary, next](const InstallOutcome &outcome) {
                if (outcome.status == InstallOutcome::Status::Installed || outcome.status == InstallOutcome::Status::Updated)
                    ++summary->imported;
                else if (outcome.status == InstallOutcome::Status::AlreadyInstalled)
                    ++summary->skipped;
                else { ++summary->failed; summary->message = outcome.message; }
                (*next)();
            });
            return;
        }
        if (summary->failed) {
            summary->status = InstallOutcome::Status::Failed;
            summary->message = QStringLiteral("%1 installed, %2 already present, %3 failed. Retry to install missing addons. %4")
                .arg(summary->imported).arg(summary->skipped).arg(summary->failed).arg(summary->message);
        } else {
            summary->message = QStringLiteral("All installed: Cinemeta, OpenSubtitles v3 and Torrentio.");
        }
        done(*summary);
    };
    (*next)();
}

QString installStatusName(InstallOutcome::Status status)
{
    switch (status) {
    case InstallOutcome::Status::Installed: return QStringLiteral("installed");
    case InstallOutcome::Status::Updated: return QStringLiteral("updated");
    case InstallOutcome::Status::AlreadyInstalled: return QStringLiteral("alreadyInstalled");
    case InstallOutcome::Status::ConfigurationRequired: return QStringLiteral("configurationRequired");
    case InstallOutcome::Status::CollectionImported: return QStringLiteral("collectionImported");
    case InstallOutcome::Status::Failed: return QStringLiteral("failed");
    }
    return {};
}

namespace {

InstallOutcome failed(const QString &message)
{
    InstallOutcome outcome;
    outcome.status = InstallOutcome::Status::Failed;
    outcome.message = message;
    return outcome;
}

// Debrify _extractAddonDescriptors + stremio-addon-client collections.
QJsonArray collectionEntries(const QJsonValue &json)
{
    if (json.isArray()) {
        return json.toArray();
    }
    const QJsonObject object = json.toObject();
    if (object.value(QStringLiteral("addons")).isArray()) {
        return object.value(QStringLiteral("addons")).toArray();
    }
    const QJsonObject result = object.value(QStringLiteral("result")).toObject();
    if (result.value(QStringLiteral("addons")).isArray()) {
        return result.value(QStringLiteral("addons")).toArray();
    }
    for (const char *key : {"addonCollection", "addon_collection"}) {
        const QJsonObject collection = object.value(QLatin1String(key)).toObject();
        if (collection.value(QStringLiteral("addons")).isArray()) {
            return collection.value(QStringLiteral("addons")).toArray();
        }
    }
    return {};
}

bool isCollection(const QJsonValue &json)
{
    return json.isArray() || !collectionEntries(json).isEmpty();
}

} // namespace

AddonManager::AddonManager(AddonClient *client, const QString &storagePath, QObject *parent)
    : QObject(parent)
    , client_(client)
    , storagePath_(storagePath)
{
}

QList<Descriptor> AddonManager::activeDescriptors() const
{
    QList<Descriptor> result;
    for (const InstalledAddon &addon : addons_) {
        if (addon.enabled) {
            result.append(addon.descriptor);
        }
    }
    return result;
}

int AddonManager::indexOf(const QString &transportUrl) const
{
    for (int i = 0; i < addons_.size(); ++i) {
        if (addons_[i].descriptor.transportUrl == transportUrl) {
            return i;
        }
    }
    return -1;
}

const InstalledAddon *AddonManager::find(const QString &transportUrl) const
{
    const int index = indexOf(transportUrl);
    return index >= 0 ? &addons_[index] : nullptr;
}

InstallOutcome AddonManager::addDescriptor(const Descriptor &descriptor)
{
    InstallOutcome outcome;
    outcome.transportUrl = descriptor.transportUrl;
    outcome.addonName = descriptor.manifest.name;
    if (descriptor.manifest.behaviorHints.configurationRequired) {
        outcome.status = InstallOutcome::Status::ConfigurationRequired;
        outcome.configureUrl = configureUrl(descriptor.transportUrl);
        outcome.message = QStringLiteral("%1 needs to be configured before it can be installed.").arg(descriptor.manifest.name);
        return outcome;
    }
    const int index = indexOf(descriptor.transportUrl);
    if (index >= 0) {
        InstalledAddon &existing = addons_[index];
        if (existing.descriptor.manifest.raw == descriptor.manifest.raw) {
            outcome.status = InstallOutcome::Status::AlreadyInstalled;
            outcome.message = QStringLiteral("%1 is already installed.").arg(descriptor.manifest.name);
            return outcome;
        }
        existing.descriptor = descriptor;
        existing.lastError.clear();
        existing.updated = QDateTime::currentDateTimeUtc();
        client_->invalidateAddon(descriptor.transportUrl);
        outcome.status = InstallOutcome::Status::Updated;
        outcome.message = QStringLiteral("%1 was updated.").arg(descriptor.manifest.name);
    } else {
        InstalledAddon addon;
        addon.descriptor = descriptor;
        addon.updated = QDateTime::currentDateTimeUtc();
        addons_.append(addon);
        outcome.status = InstallOutcome::Status::Installed;
        outcome.message = QStringLiteral("%1 was installed.").arg(descriptor.manifest.name);
    }
    save();
    emit changed();
    return outcome;
}

void AddonManager::install(const QString &input, std::function<void(const InstallOutcome &)> done)
{
    InstallUrl url;
    QString error;
    if (!normalizeInstallUrl(input, url, &error)) {
        done(failed(error));
        return;
    }
    if (url.isLegacy || url.endsWithManifest) {
        installManifestUrl(url.manifestUrl, done);
        return;
    }
    if (url.looksLikeJsonFile) {
        installFromDetection(url.url, done, {});
        return;
    }
    // Configured URLs pasted without /manifest.json (Debrify, Nuvio): try the
    // manifest first, then look at the URL itself (X-Stremio-Addon header,
    // collections) like stremio-addon-client's detectFromURL.
    const QString raw = url.url;
    installManifestUrl(url.manifestUrl, done, [this, raw, done] {
        installFromDetection(raw, done, {});
    });
}

void AddonManager::installFromTransportUrl(const QString &transportUrl, std::function<void(const InstallOutcome &)> done)
{
    installManifestUrl(transportUrl, done);
}

void AddonManager::installManifestUrl(const QString &transportUrl, std::function<void(const InstallOutcome &)> done,
                                      std::function<void()> onFailureFallback)
{
    client_->fetchManifest(transportUrl, this, [this, transportUrl, done, onFailureFallback](const ManifestResult &result) {
        if (!result.manifest) {
            if (onFailureFallback) {
                onFailureFallback();
                return;
            }
            done(failed(QStringLiteral("Could not load the addon manifest (%1).").arg(result.error.toString())));
            return;
        }
        Descriptor descriptor;
        descriptor.manifest = *result.manifest;
        descriptor.transportUrl = transportUrl;
        done(addDescriptor(descriptor));
    }, true);
}

void AddonManager::installFromDetection(const QString &url, std::function<void(const InstallOutcome &)> done,
                                        const QString &previousError)
{
    client_->fetchJson(url, this, [this, url, done, previousError](const JsonResult &result) {
        if (!result.stremioAddonHeader.isEmpty()) {
            const QString target = QUrl(url).resolved(QUrl(result.stremioAddonHeader)).toString(QUrl::FullyEncoded);
            installManifestUrl(target, done);
            return;
        }
        if (result.error.isError()) {
            done(failed(previousError.isEmpty()
                            ? QStringLiteral("Could not load the addon (%1).").arg(result.error.toString())
                            : previousError));
            return;
        }
        if (isCollection(result.json)) {
            importCollection(result.json, done);
            return;
        }
        if (result.json.isObject() && result.json.toObject().contains(QStringLiteral("id"))) {
            done(failed(QStringLiteral("This URL serves an addon manifest, but addon URLs must end with /manifest.json.")));
            return;
        }
        done(failed(QStringLiteral("No Stremio addon was found at this URL.")));
    });
}

void AddonManager::importCollection(const QJsonValue &json, std::function<void(const InstallOutcome &)> done)
{
    const QJsonArray entries = collectionEntries(json);
    auto summary = std::make_shared<InstallOutcome>();
    summary->status = InstallOutcome::Status::CollectionImported;
    auto pending = std::make_shared<int>(1); // released after the loop
    auto finishOne = [this, summary, pending, done] {
        if (--(*pending) > 0) {
            return;
        }
        summary->message = QStringLiteral("Imported %1 addon(s)").arg(summary->imported);
        if (summary->skipped) summary->message += QStringLiteral(", %1 already installed").arg(summary->skipped);
        if (summary->failed) summary->message += QStringLiteral(", %1 failed").arg(summary->failed);
        summary->message += QLatin1Char('.');
        if (summary->imported == 0 && summary->skipped == 0) {
            summary->status = InstallOutcome::Status::Failed;
            if (summary->failed == 0) {
                summary->message = QStringLiteral("No Stremio addons were found in this collection.");
            }
        }
        if (done) {
            done(*summary);
        }
    };
    auto record = [summary](const InstallOutcome &outcome) {
        switch (outcome.status) {
        case InstallOutcome::Status::Installed:
        case InstallOutcome::Status::Updated:
            ++summary->imported;
            break;
        case InstallOutcome::Status::AlreadyInstalled:
            ++summary->skipped;
            break;
        default:
            ++summary->failed;
            break;
        }
    };

    for (const QJsonValue &entry : entries) {
        QString rawUrl;
        QJsonObject object;
        if (entry.isString()) {
            rawUrl = entry.toString();
        } else if (entry.isObject()) {
            object = entry.toObject();
            for (const char *key : {"transportUrl", "transport_url", "manifestUrl", "manifest_url"}) {
                if (object.value(QLatin1String(key)).isString()) {
                    rawUrl = object.value(QLatin1String(key)).toString();
                    break;
                }
            }
        }
        InstallUrl url;
        if (rawUrl.isEmpty() || !normalizeInstallUrl(rawUrl, url)) {
            ++summary->failed;
            continue;
        }
        const QString transportUrl = url.manifestUrl;
        if (indexOf(transportUrl) >= 0) {
            ++summary->skipped;
            continue;
        }
        if (object.value(QStringLiteral("manifest")).isObject()) {
            QJsonObject descriptorJson = object;
            descriptorJson.insert(QStringLiteral("transportUrl"), transportUrl);
            if (auto descriptor = Descriptor::fromJson(descriptorJson)) {
                record(addDescriptor(*descriptor));
                continue;
            }
        }
        ++(*pending);
        installManifestUrl(transportUrl, [record, finishOne](const InstallOutcome &outcome) {
            record(outcome);
            finishOne();
        });
    }
    finishOne();
}

QJsonArray AddonManager::exportCollection() const
{
    QJsonArray array;
    for (const InstalledAddon &addon : addons_) {
        array.append(addon.descriptor.toJson());
    }
    return array;
}

bool AddonManager::remove(const QString &transportUrl)
{
    const int index = indexOf(transportUrl);
    if (index < 0) {
        return false;
    }
    addons_.removeAt(index);
    client_->invalidateAddon(transportUrl);
    save();
    emit changed();
    return true;
}

bool AddonManager::setEnabled(const QString &transportUrl, bool enabled)
{
    const int index = indexOf(transportUrl);
    if (index < 0 || addons_[index].enabled == enabled) {
        return false;
    }
    addons_[index].enabled = enabled;
    save();
    emit changed();
    return true;
}

bool AddonManager::move(const QString &transportUrl, int toIndex)
{
    const int from = indexOf(transportUrl);
    if (from < 0) {
        return false;
    }
    toIndex = qBound(0, toIndex, int(addons_.size()) - 1);
    if (from == toIndex) {
        return false;
    }
    addons_.move(from, toIndex);
    save();
    emit changed();
    return true;
}

void AddonManager::refresh(const QString &transportUrl, std::function<void(bool, const QString &)> done)
{
    client_->fetchManifest(transportUrl, this, [this, transportUrl, done](const ManifestResult &result) {
        const int index = indexOf(transportUrl);
        if (index < 0) {
            if (done) done(false, QStringLiteral("Addon is no longer installed."));
            return;
        }
        InstalledAddon &addon = addons_[index];
        QString error;
        if (!result.manifest) {
            error = result.error.toString();
        } else if (result.manifest->behaviorHints.configurationRequired) {
            // UpgradeAddon refuses configurationRequired manifests: keep the old one.
            error = QStringLiteral("The addon now requires configuration.");
        } else {
            if (addon.descriptor.manifest.raw != result.manifest->raw) {
                client_->invalidateAddon(transportUrl);
            }
            addon.descriptor.manifest = *result.manifest;
            addon.updated = QDateTime::currentDateTimeUtc();
        }
        addon.lastError = error;
        save();
        emit changed();
        if (done) done(error.isEmpty(), error);
    }, true);
}

void AddonManager::refreshAll()
{
    for (const InstalledAddon &addon : std::as_const(addons_)) {
        refresh(addon.descriptor.transportUrl);
    }
}

void AddonManager::load()
{
    addons_.clear();
    QFile file(storagePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &value : root.value(QStringLiteral("addons")).toArray()) {
        const QJsonObject entry = value.toObject();
        auto descriptor = Descriptor::fromJson(entry.value(QStringLiteral("descriptor")).toObject());
        if (!descriptor || indexOf(descriptor->transportUrl) >= 0) {
            continue;
        }
        InstalledAddon addon;
        addon.descriptor = *descriptor;
        addon.enabled = entry.value(QStringLiteral("enabled")).toBool(true);
        addon.lastError = entry.value(QStringLiteral("lastError")).toString();
        addon.updated = QDateTime::fromString(entry.value(QStringLiteral("updated")).toString(), Qt::ISODate);
        addons_.append(addon);
    }
}

bool AddonManager::save() const
{
    QJsonArray list;
    for (const InstalledAddon &addon : addons_) {
        list.append(QJsonObject{
            {"descriptor", addon.descriptor.toJson()},
            {"enabled", addon.enabled},
            {"lastError", addon.lastError},
            {"updated", addon.updated.toString(Qt::ISODate)},
        });
    }
    QDir().mkpath(QFileInfo(storagePath_).absolutePath());
    QSaveFile file(storagePath_);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"addons", list}}).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace stremio
