#include "stremio/addonclient.h"

#include "stremio/legacytransport.h"
#include "stremio/transport.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

namespace stremio {

namespace {

constexpr auto kCancelledProperty = "lambdaCancelled";

int memoryCacheSeconds(const ResourceRequest &request, const ResourceResponse &response)
{
    // Streams and subtitles can carry short-lived (debrid) links.
    const bool shortLived = request.path.resource == QLatin1String(kStreamResource)
        || request.path.resource == QLatin1String(kSubtitlesResource);
    const int fallback = shortLived ? 60 : 300;
    if (response.cacheMaxAge) {
        return int(qMin<qint64>(*response.cacheMaxAge, 3600));
    }
    return fallback;
}

std::optional<QJsonValue> parseJsonBody(const QByteArray &body, FetchError &error)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error.kind = FetchError::Kind::InvalidJson;
        const QByteArray start = body.left(200).trimmed();
        error.message = start.startsWith('<')
            ? QStringLiteral("The addon returned a web page instead of JSON")
            : QStringLiteral("Invalid JSON: %1").arg(parseError.errorString());
        return std::nullopt;
    }
    if (document.isObject()) {
        return QJsonValue(document.object());
    }
    if (document.isArray()) {
        return QJsonValue(document.array());
    }
    return QJsonValue();
}

} // namespace

QString FetchError::toString() const
{
    switch (kind) {
    case Kind::None: return {};
    case Kind::HttpStatus: return QStringLiteral("HTTP %1%2").arg(httpStatus).arg(message.isEmpty() ? QString() : QStringLiteral(": ") + message);
    case Kind::Timeout: return QStringLiteral("Timed out");
    case Kind::Cancelled: return QStringLiteral("Cancelled");
    default: return message;
    }
}

AddonClient::AddonClient(QObject *parent)
    : QObject(parent)
    , network_(new QNetworkAccessManager(this))
{
    network_->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    userAgent_ = QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) LAMBDA-Player/")
        + QCoreApplication::applicationVersion().toLatin1();
}

AddonClient::~AddonClient() = default;

void AddonClient::setDiskCacheDirectory(const QString &directory)
{
    auto *cache = new QNetworkDiskCache(network_);
    cache->setCacheDirectory(directory);
    cache->setMaximumCacheSize(64LL * 1024 * 1024);
    network_->setCache(cache);
}

QNetworkReply *AddonClient::get(const QString &url, bool force, int timeoutMs)
{
    QNetworkRequest request(QUrl(url, QUrl::TolerantMode));
    request.setTransferTimeout(timeoutMs > 0 ? timeoutMs : timeoutMs_);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent_);
    request.setRawHeader("Accept", "application/json, */*");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         force ? QNetworkRequest::AlwaysNetwork : QNetworkRequest::PreferNetwork);
    QNetworkReply *reply = network_->get(request);
    // Freed even when the requesting page (the callback context) is gone.
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    return reply;
}

void AddonClient::cancel(QNetworkReply *reply)
{
    if (reply && reply->isRunning()) {
        reply->setProperty(kCancelledProperty, true);
        reply->abort();
    }
}

FetchError AddonClient::replyError(QNetworkReply *reply, bool timedOut)
{
    FetchError error;
    if (reply->property(kCancelledProperty).toBool()) {
        error.kind = FetchError::Kind::Cancelled;
        return error;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (timedOut || reply->error() == QNetworkReply::TimeoutError
        || (reply->error() == QNetworkReply::OperationCanceledError && status == 0)) {
        error.kind = FetchError::Kind::Timeout;
        return error;
    }
    if (status != 0 && status != 200 && status != 201) {
        error.kind = FetchError::Kind::HttpStatus;
        error.httpStatus = status;
        error.message = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        return error;
    }
    if (reply->error() != QNetworkReply::NoError) {
        error.kind = FetchError::Kind::Network;
        error.message = reply->errorString();
        return error;
    }
    return error;
}

QNetworkReply *AddonClient::fetchResource(const ResourceRequest &request, QObject *context,
                                          std::function<void(const ResourceResult &)> callback, bool force)
{
    const QString key = request.key();
    if (!force) {
        const auto cached = memoryCache_.constFind(key);
        if (cached != memoryCache_.cend() && cached->expires > QDateTime::currentDateTimeUtc()) {
            ResourceResult result;
            result.request = request;
            result.response = cached->response;
            result.fromCache = true;
            QPointer<QObject> guard(context);
            QMetaObject::invokeMethod(this, [guard, callback, result] {
                if (guard) {
                    callback(result);
                }
            }, Qt::QueuedConnection);
            return nullptr;
        }
    }

    QString transportError;
    const QString url = resourceUrl(request.base, request.path, &transportError);
    if (url.isEmpty()) {
        ResourceResult result;
        result.request = request;
        result.error.kind = FetchError::Kind::Transport;
        result.error.message = transportError;
        QPointer<QObject> guard(context);
        QMetaObject::invokeMethod(this, [guard, callback, result] {
            if (guard) {
                callback(result);
            }
        }, Qt::QueuedConnection);
        return nullptr;
    }

    QNetworkReply *reply = get(url, force, 0);
    const bool legacyTransport = transportKind(request.base) == TransportKind::Legacy;
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, context ? context : this, [this, reply, request, callback, key, legacyTransport, force, guard] {
        reply->deleteLater();
        ResourceResult result;
        result.request = request;
        result.error = replyError(reply, false);
        if (!result.error.isError()) {
            const auto json = parseJsonBody(reply->readAll(), result.error);
            if (!json && !force && evictUnreadableCacheEntry(reply)) {
                // A damaged disk-cache entry would otherwise be served until it
                // expires (max-age is often a day): fetch it again once.
                fetchResource(request, guard, callback, true);
                return;
            }
            if (json) {
                QString parseError;
                result.response = legacyTransport
                    ? legacy::parseResourceResponse(request.path.resource, *json, &parseError)
                    : ResourceResponse::fromJson(*json, &parseError);
                if (!result.response) {
                    result.error.kind = FetchError::Kind::UnexpectedResponse;
                    result.error.message = parseError;
                } else {
                    result.fromCache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
                    memoryCache_.insert(key, {*result.response,
                                              QDateTime::currentDateTimeUtc().addSecs(memoryCacheSeconds(request, *result.response))});
                }
            }
        }
        callback(result);
    });
    return reply;
}

QNetworkReply *AddonClient::fetchManifest(const QString &transportUrl, QObject *context,
                                          std::function<void(const ManifestResult &)> callback, bool force)
{
    QNetworkReply *reply = get(manifestUrl(transportUrl), force, 0);
    const bool legacyTransport = transportKind(transportUrl) == TransportKind::Legacy;
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, context ? context : this, [this, reply, transportUrl, callback, legacyTransport, force, guard] {
        reply->deleteLater();
        ManifestResult result;
        result.transportUrl = transportUrl;
        result.error = replyError(reply, false);
        if (!result.error.isError()) {
            const auto json = parseJsonBody(reply->readAll(), result.error);
            if (!json && !force && evictUnreadableCacheEntry(reply)) {
                fetchManifest(transportUrl, guard, callback, true);
                return;
            }
            if (json) {
                QString parseError;
                result.manifest = legacyTransport ? legacy::parseManifestResponse(*json, &parseError)
                                                  : (json->isObject() ? Manifest::fromJson(json->toObject(), &parseError)
                                                                      : std::nullopt);
                if (!result.manifest) {
                    result.error.kind = FetchError::Kind::UnexpectedResponse;
                    result.error.message = parseError.isEmpty() ? QStringLiteral("Not an addon manifest") : parseError;
                }
            }
        }
        callback(result);
    });
    return reply;
}

QNetworkReply *AddonClient::fetchJson(const QString &url, QObject *context,
                                      std::function<void(const JsonResult &)> callback, int timeoutMs)
{
    QNetworkReply *reply = get(url, true, timeoutMs);
    connect(reply, &QNetworkReply::finished, context ? context : this, [reply, url, callback] {
        reply->deleteLater();
        JsonResult result;
        result.url = url;
        result.error = replyError(reply, false);
        result.stremioAddonHeader = QString::fromUtf8(reply->rawHeader("X-Stremio-Addon"));
        result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (!result.error.isError()) {
            const auto json = parseJsonBody(reply->readAll(), result.error);
            if (json) {
                result.json = *json;
            } else if (!result.stremioAddonHeader.isEmpty()) {
                result.error = {}; // the header alone identifies the addon
            }
        }
        callback(result);
    });
    return reply;
}

bool AddonClient::evictUnreadableCacheEntry(QNetworkReply *reply)
{
    if (!reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool() || !network_->cache()) return false;
    return network_->cache()->remove(reply->url());
}

void AddonClient::clearMemoryCache()
{
    memoryCache_.clear();
}

void AddonClient::invalidateAddon(const QString &transportUrl)
{
    const QString prefix = transportUrl + QLatin1Char('\n');
    for (auto it = memoryCache_.begin(); it != memoryCache_.end();) {
        it = it.key().startsWith(prefix) ? memoryCache_.erase(it) : std::next(it);
    }
}

} // namespace stremio
