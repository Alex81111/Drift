#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantList>

#include <memory>

class AppController;
class AssetLibrary;
class MarketClient;
class QNetworkAccessManager;
class QNetworkReply;

// Drift Assets: CutWire's own Lottie animations, face props and 3D objects, served by the
// marketplace as a versioned pack (GET /api/v1/assets). This is the Market's first page, so
// unlike the stock sources it needs no consent gate, no quota and no folder prompt: files are
// sha256-checked and installed into app data, then used straight from there.
//
// Lottie and 3D objects go into the media bin; face props are installed through
// AppController::importFaceProps so the Face Props picker lists them.
class DriftAssetStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY packChanged)
    Q_PROPERTY(QVariantList assets READ assets NOTIFY packChanged)
    // Bumped whenever any asset's install state changes; QML bindings on state() read it.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    DriftAssetStore(MarketClient *market, AssetLibrary *library, AppController *app,
                    QObject *parent = nullptr);

    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    QVariantList categories() const { return m_categories; }
    QVariantList assets() const { return m_assets; }
    int revision() const { return m_revision; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE QVariantList assetsIn(const QString &categoryId) const;
    // Name, tags and description, every word must match. Pack order.
    Q_INVOKABLE QVariantList search(const QString &query) const;
    Q_INVOKABLE QVariantMap assetById(const QString &id) const;
    // none | installing | installed | failed
    Q_INVOKABLE QString state(const QString &id) const;
    // Installs if needed, then: Lottie and objects are imported into the media bin and
    // `ready(id, binAssetId)` fires; face props are added to the prop library and `ready(id, "")`
    // fires. Safe to call again while an install is running.
    Q_INVOKABLE void install(const QString &id);
    // Absolute path of the installed main file (.json / .glb), empty when not installed.
    Q_INVOKABLE QString localPath(const QString &id) const;

    // Test seam and the network reply's destination: replaces the pack.
    void applyPack(const QJsonObject &pack);
    static QString installRoot();

signals:
    void loadingChanged();
    void errorChanged();
    void packChanged();
    void revisionChanged();
    void ready(const QString &id, const QString &binAssetId);
    void failed(const QString &id, const QString &message);

private:
    struct Install;

    QString assetDir(const QVariantMap &asset) const;
    bool isInstalled(const QVariantMap &asset) const;
    void fetchNext(const QString &id);
    void finishInstall(const QString &id);
    void failInstall(const QString &id, const QString &message);
    void setState(const QString &id, const QString &state);

    QPointer<MarketClient> m_market;
    QPointer<AssetLibrary> m_library;
    QPointer<AppController> m_app;
    QNetworkAccessManager *m_network = nullptr;

    bool m_loading = false;
    QString m_error;
    QVariantList m_categories;
    QVariantList m_assets;
    QHash<QString, int> m_index;
    QHash<QString, QString> m_states;
    QHash<QString, std::shared_ptr<Install>> m_installs;
    int m_revision = 0;
};
