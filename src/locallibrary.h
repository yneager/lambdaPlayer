#pragma once
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
class QWidget;

class LocalLibrary final : public QObject {
    Q_OBJECT
public:
    explicit LocalLibrary(QWidget *parent);
    static QJsonObject scan(const QString &folder);
    QString nextFile(const QString &path) const;
public slots:
    QJsonArray state() const;
    void chooseFolder();
    void refresh(const QString &folder);
    void remove(const QString &folder);
    void matchArtwork(const QString &folder);
    void play(const QString &folder, const QString &path);
signals:
    void changed(const QJsonArray &items);
    void playRequested(const QString &path);
    void notify(const QString &message, bool warning);
private:
    void importFolder(const QString &folder);
    void lookup(const QString &folder, const QString &title, bool interactive);
    void save();
    int indexOf(const QString &folder) const;
    QJsonArray items_;
    QNetworkAccessManager network_;
    QPointer<QWidget> dialogParent_;
};
