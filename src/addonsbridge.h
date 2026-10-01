#pragma once

// The Home page's view of the Stremio addon client ("stremio" on the
// QWebChannel). Protocol decisions (which addon gets which request, how
// results are read) stay in C++; the page renders the result slots it is sent.
// Every request made for a page carries a token so the page can cancel what
// it no longer shows (leaving Details, a new search, …).

#include "stremio/resources.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>

#include <optional>

class QNetworkReply;
class StremioBackend;
class AddonConfigureWindow;

class AddonsBridge final : public QObject
{
    Q_OBJECT

public:
    AddonsBridge(StremioBackend *backend, QWidget *dialogParent, QObject *parent = nullptr);

public slots:
    // Addons page
    QJsonObject state() const;
    void install(const QString &input, const QString &requestId);
    void installDefaults(const QString &requestId);
    void importCollectionText(const QString &text, const QString &requestId);
    void importCollectionFile(const QString &requestId);
    void exportCollection();
    bool remove(const QString &transportUrl);
    bool setEnabled(const QString &transportUrl, bool enabled);
    bool move(const QString &transportUrl, int toIndex);
    void refresh(const QString &transportUrl);
    void configure(const QString &transportUrl);
    void openExternal(const QString &url);
    void setStreamingServerUrl(const QString &url);
    void checkStreamingServer();
    void setCacheSize(double bytes);
    void chooseCacheLocation();
    void resetCacheLocation();
    void clearCache();
    QJsonArray addonCatalogRows() const;

    // Home / Search / Discover
    QJsonArray boardRows() const;
    QJsonArray searchRows(const QString &query) const;
    void loadCatalog(const QString &token, const QJsonObject &request, bool force);
    QJsonValue nextPage(const QJsonObject &request, int lastPageItems) const;
    QJsonObject discover(const QJsonValue &selected, const QJsonArray &pageSizes) const;

    // Details
    void loadMeta(const QString &token, const QString &type, const QString &id, bool force);
    void loadStreams(const QString &token, const QString &type, const QString &videoId,
                     const QString &metaToken, bool force);
    void play(const QJsonObject &stream, const QJsonObject &context);

    void cancel(const QString &token);

signals:
    void addonsChanged(const QJsonObject &state);
    void installResult(const QString &requestId, const QJsonObject &result);
    void catalogResult(const QString &token, const QJsonObject &result);
    void metaPlan(const QString &token, const QJsonArray &slotList);
    void metaResult(const QString &token, int index, const QJsonObject &result);
    void streamsPlan(const QString &token, const QJsonObject &plan);
    void streamsResult(const QString &token, int index, const QJsonObject &result);
    void playStatus(const QJsonObject &status);
    void streamingServerChanged(const QJsonObject &status);
    void notify(const QString &message, bool warning);
    // Before the engine cache is deleted (the player stops engine streams).
    void cacheClearing();

private:
    struct MetaSession
    {
        QList<stremio::ResourceRequest> requests;
        QList<std::optional<stremio::MetaItem>> items;
    };

    void track(const QString &token, QNetworkReply *reply);
    void emitState();
    QJsonObject streamingServerStatus() const;

    StremioBackend *backend_;
    bool installingDefaults_ = false;
    QPointer<QWidget> dialogParent_;
    QHash<QString, QList<QPointer<QNetworkReply>>> pending_;
    QHash<QString, MetaSession> metaSessions_;
    QPointer<AddonConfigureWindow> configureWindow_;
};
