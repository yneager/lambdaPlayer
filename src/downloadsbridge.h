#pragma once
#include <QObject>
#include <QJsonObject>
#include <QProcess>

// Qt host for the copied Motrix client and engine. The client owns task RPC.
class DownloadsBridge final : public QObject
{
    Q_OBJECT
public:
    explicit DownloadsBridge(QObject *parent = nullptr);
    ~DownloadsBridge() override;
public slots:
    QJsonObject start();
    QString chooseFolder();
    QJsonObject chooseTorrent();
    void receiveDrop(const QString &text);
    void reveal(const QString &path);
signals:
    void engineStopped();
    void downloadDropped(const QJsonObject &source);
private:
    QProcess process_;
    QJsonObject readTorrent(const QString &path);
    QString directory_, dataDirectory_, secret_;
    quint16 port_ = 0;
    bool encrypted_ = true;
};
