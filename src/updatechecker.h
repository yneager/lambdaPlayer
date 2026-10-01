#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <QProcess>

class UpdateChecker final : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(const QString &currentTag, QObject *parent = nullptr,
                           const QUrl &endpoint = QUrl("https://api.github.com/repos/yneager/lambdaPlayer/releases?per_page=100"));
    QJsonObject state() const;
    static bool newerThan(const QString &candidate, const QString &current);
    static QJsonObject selectRelease(const QJsonArray &releases, const QString &current, bool includeTests);
    static QJsonObject selectFeed(const QJsonObject &feed, const QString &current, bool includeTests);
    void start();
public slots:
    void check(bool manual = true);
    void setIncludeTests(bool enabled);
    void dismiss();
    void download();
    void releaseNotes();
    void cancelDownload();
signals:
    void changed(const QJsonObject &state);
    void notify(const QString &message, bool warning);
    void restartRequested();
private:
    void publish();
    QString currentTag_;
    QUrl endpoint_;
    QNetworkAccessManager network_;
    QJsonObject available_;
    QString status_ = "idle";
    bool includeTests_ = false;
    bool busy_ = false;
    int progress_ = 0;
    QNetworkReply *downloadReply_ = nullptr;
    void failUpdate(const QString &message);
    void prepareUpdate(const QString &job);
};
