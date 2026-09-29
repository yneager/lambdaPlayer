#pragma once

// HTTP access to addons. Mirrors stremio-core-web's fetch semantics
// (stremio-core-web/src/env.rs): redirects are followed, only HTTP 200/201 is
// a success, the body must be JSON. Caching follows the HTTP headers the SDK
// derives from cacheMaxAge/staleRevalidate/staleError (QNetworkDiskCache),
// plus a short in-memory cache of parsed responses so navigating back does
// not refetch, like stremio-core reusing loaded ResourceLoadables. Failures
// are never cached. Each request has its own timeout (15 s, as Debrify and
// Cove use), so one slow addon never blocks the others.

#include "stremio/common.h"
#include "stremio/manifest.h"
#include "stremio/resources.h"

#include <QDateTime>
#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QPointer>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace stremio {

struct FetchError
{
    enum class Kind { None, Network, Timeout, HttpStatus, InvalidJson, UnexpectedResponse, Transport, Cancelled };
    Kind kind = Kind::None;
    int httpStatus = 0;
    QString message;

    bool isError() const { return kind != Kind::None; }
    QString toString() const;
};

struct ResourceResult
{
    ResourceRequest request;
    std::optional<ResourceResponse> response;
    FetchError error;
    bool fromCache = false;
};

struct ManifestResult
{
    QString transportUrl;
    std::optional<Manifest> manifest;
    FetchError error;
};

struct JsonResult
{
    QString url;
    QJsonValue json;
    QString stremioAddonHeader; // X-Stremio-Addon (stremio-addon-client detectFromURL)
    QString contentType;
    FetchError error;
};

class AddonClient final : public QObject
{
    Q_OBJECT

public:
    explicit AddonClient(QObject *parent = nullptr);
    ~AddonClient() override;

    void setDiskCacheDirectory(const QString &directory);
    void setTimeout(int milliseconds) { timeoutMs_ = milliseconds; }
    int timeout() const { return timeoutMs_; }
    void setUserAgent(const QByteArray &userAgent) { userAgent_ = userAgent; }
    QNetworkAccessManager *networkAccessManager() const { return network_; }

    // The callback runs on the GUI thread, only while `context` is alive.
    // `force` bypasses the in-memory and HTTP caches (a user "refresh").
    QNetworkReply *fetchResource(const ResourceRequest &request, QObject *context,
                                 std::function<void(const ResourceResult &)> callback, bool force = false);
    QNetworkReply *fetchManifest(const QString &transportUrl, QObject *context,
                                 std::function<void(const ManifestResult &)> callback, bool force = false);
    QNetworkReply *fetchJson(const QString &url, QObject *context,
                             std::function<void(const JsonResult &)> callback, int timeoutMs = 0);

    // Aborts a request; its callback reports FetchError::Kind::Cancelled.
    static void cancel(QNetworkReply *reply);

    void clearMemoryCache();
    // Drops cached responses of one addon (after its manifest changed).
    void invalidateAddon(const QString &transportUrl);

private:
    struct CacheEntry
    {
        ResourceResponse response;
        QDateTime expires;
    };

    QNetworkReply *get(const QString &url, bool force, int timeoutMs);
    static FetchError replyError(QNetworkReply *reply, bool timedOut);
    // Removes the disk-cache entry behind a reply that was served from the
    // cache (true if one was removed), so the caller can refetch it.
    bool evictUnreadableCacheEntry(QNetworkReply *reply);

    QNetworkAccessManager *network_ = nullptr;
    int timeoutMs_ = 15000;
    QByteArray userAgent_;
    QHash<QString, CacheEntry> memoryCache_;
};

} // namespace stremio
