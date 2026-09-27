#include "DriftAssetStore.h"

#include "AppController.h"
#include "AssetLibrary.h"
#include "MarketClient.h"
#include "engine/FacePropCatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace {

constexpr int kFileTimeoutMs = 60000;

bool validId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"));
    return re.match(id).hasMatch();
}

bool validName(const QString &name)
{
    return !name.isEmpty() && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && name != QLatin1String(".") && name != QLatin1String("..");
}

} // namespace

struct DriftAssetStore::Install
{
    QString id;
    QString partialDir;
    QVariantList files;
    int next = 0;
    QPointer<QNetworkReply> reply;
};

DriftAssetStore::DriftAssetStore(MarketClient *market, AssetLibrary *library, AppController *app,
                                 QObject *parent)
    : QObject(parent)
    , m_market(market)
    , m_library(library)
    , m_app(app)
    , m_network(new QNetworkAccessManager(this))
{
}

QString DriftAssetStore::installRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/drift-assets");
}

void DriftAssetStore::refresh()
{
    if (m_loading)
        return;
    if (!m_market || !m_market->configured()) {
        m_error = tr("The marketplace is not available in this build.");
        emit errorChanged();
        return;
    }
    m_loading = true;
    emit loadingChanged();
    QNetworkReply *reply = m_market->signedGet(QStringLiteral("/assets"));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_loading = false;
        emit loadingChanged();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QString error = reply->error() == QNetworkReply::NoError && doc.isObject()
            ? QString()
            : tr("Could not load Drift Assets. Check your connection and try again.");
        if (error != m_error) {
            m_error = error;
            emit errorChanged();
        }
        if (error.isEmpty())
            applyPack(doc.object());
    });
}

void DriftAssetStore::applyPack(const QJsonObject &pack)
{
    m_categories.clear();
    m_assets.clear();
    m_index.clear();
    for (const QJsonValue &c : pack.value(QStringLiteral("categories")).toArray())
        m_categories.append(c.toObject().toVariantMap());
    for (const QJsonValue &v : pack.value(QStringLiteral("assets")).toArray()) {
        QVariantMap asset = v.toObject().toVariantMap();
        const QString id = asset.value(QStringLiteral("id")).toString();
        if (!validId(id) || m_index.contains(id))
            continue;
        m_index.insert(id, m_assets.size());
        m_assets.append(asset);
    }
    emit packChanged();
    ++m_revision;
    emit revisionChanged();
}

QVariantList DriftAssetStore::assetsIn(const QString &categoryId) const
{
    QVariantList out;
    for (const QVariant &a : m_assets) {
        if (a.toMap().value(QStringLiteral("category")).toString() == categoryId)
            out.append(a);
    }
    return out;
}

QVariantList DriftAssetStore::search(const QString &query) const
{
    const QStringList words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return {};
    QVariantList out;
    for (const QVariant &a : m_assets) {
        const QVariantMap asset = a.toMap();
        const QString hay = (asset.value(QStringLiteral("name")).toString() + QLatin1Char(' ')
                             + asset.value(QStringLiteral("tags")).toStringList().join(QLatin1Char(' '))
                             + QLatin1Char(' ') + asset.value(QStringLiteral("description")).toString()
                             + QLatin1Char(' ') + asset.value(QStringLiteral("category")).toString())
                                .toLower();
        if (std::all_of(words.cbegin(), words.cend(), [&](const QString &w) { return hay.contains(w); }))
            out.append(asset);
    }
    return out;
}

QVariantMap DriftAssetStore::assetById(const QString &id) const
{
    const int i = m_index.value(id, -1);
    return i < 0 ? QVariantMap() : m_assets.at(i).toMap();
}

QString DriftAssetStore::assetDir(const QVariantMap &asset) const
{
    return installRoot() + QLatin1Char('/') + asset.value(QStringLiteral("kind")).toString()
        + QLatin1Char('/') + asset.value(QStringLiteral("id")).toString();
}

bool DriftAssetStore::isInstalled(const QVariantMap &asset) const
{
    const QString id = asset.value(QStringLiteral("id")).toString();
    if (asset.value(QStringLiteral("kind")).toString() == QLatin1String("face-prop")) {
        const QList<FacePropEntry> props = facePropsSnapshot();
        return std::any_of(props.cbegin(), props.cend(),
                           [&](const FacePropEntry &p) { return p.id == id; });
    }
    const QString dir = assetDir(asset);
    for (const QVariant &f : asset.value(QStringLiteral("files")).toList()) {
        const QVariantMap file = f.toMap();
        const QFileInfo info(dir + QLatin1Char('/') + file.value(QStringLiteral("name")).toString());
        if (!info.isFile() || info.size() != file.value(QStringLiteral("size")).toLongLong())
            return false;
    }
    return true;
}

QString DriftAssetStore::state(const QString &id) const
{
    const QString s = m_states.value(id);
    if (!s.isEmpty() && s != QLatin1String("installed"))
        return s;
    const QVariantMap asset = assetById(id);
    return !asset.isEmpty() && isInstalled(asset) ? QStringLiteral("installed") : QStringLiteral("none");
}

QString DriftAssetStore::localPath(const QString &id) const
{
    const QVariantMap asset = assetById(id);
    if (asset.isEmpty() || !isInstalled(asset))
        return {};
    for (const QVariant &f : asset.value(QStringLiteral("files")).toList()) {
        const QVariantMap file = f.toMap();
        if (file.value(QStringLiteral("role")).toString() == QLatin1String("main"))
            return assetDir(asset) + QLatin1Char('/') + file.value(QStringLiteral("name")).toString();
    }
    return {};
}

void DriftAssetStore::setState(const QString &id, const QString &state)
{
    m_states.insert(id, state);
    ++m_revision;
    emit revisionChanged();
}

void DriftAssetStore::install(const QString &id)
{
    const QVariantMap asset = assetById(id);
    if (asset.isEmpty() || m_installs.contains(id))
        return;
    if (isInstalled(asset)) {
        finishInstall(id);
        return;
    }
    const QVariantList files = asset.value(QStringLiteral("files")).toList();
    for (const QVariant &f : files) {
        if (!validName(f.toMap().value(QStringLiteral("name")).toString())) {
            failInstall(id, tr("That asset could not be installed."));
            return;
        }
    }

    auto install = std::make_shared<Install>();
    install->id = id;
    install->files = files;
    install->partialDir = assetDir(asset) + QStringLiteral(".partial");
    QDir(install->partialDir).removeRecursively();
    if (!QDir().mkpath(install->partialDir)) {
        failInstall(id, tr("Could not write to the app data folder."));
        return;
    }
    m_installs.insert(id, install);
    setState(id, QStringLiteral("installing"));
    fetchNext(id);
}

void DriftAssetStore::fetchNext(const QString &id)
{
    const auto install = m_installs.value(id);
    if (!install)
        return;
    if (install->next >= install->files.size()) {
        const QString dir = assetDir(assetById(id));
        QDir(dir).removeRecursively();
        if (!QDir().rename(install->partialDir, dir)) {
            failInstall(id, tr("Could not write to the app data folder."));
            return;
        }
        m_installs.remove(id);
        finishInstall(id);
        return;
    }

    const QVariantMap file = install->files.at(install->next).toMap();
    QNetworkRequest request(file.value(QStringLiteral("url")).toUrl());
    request.setTransferTimeout(kFileTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    install->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, id, file, reply]() {
        reply->deleteLater();
        const auto install = m_installs.value(id);
        if (!install)
            return;
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            failInstall(id, tr("Could not download that asset. Check your connection and try again."));
            return;
        }
        const QString sha = QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        if (data.size() != file.value(QStringLiteral("size")).toLongLong()
            || sha != file.value(QStringLiteral("sha256")).toString()) {
            failInstall(id, tr("That download was damaged. Try again."));
            return;
        }
        QFile out(install->partialDir + QLatin1Char('/') + file.value(QStringLiteral("name")).toString());
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
            failInstall(id, tr("Could not write to the app data folder."));
            return;
        }
        out.close();
        ++install->next;
        fetchNext(id);
    });
}

void DriftAssetStore::finishInstall(const QString &id)
{
    const QVariantMap asset = assetById(id);
    if (asset.value(QStringLiteral("kind")).toString() == QLatin1String("face-prop")) {
        // A prop is installed once it is in the library; the staged copy has done its job.
        const QString staged = assetDir(asset);
        if (QFileInfo::exists(staged)) {
            const QVariantMap result = m_app ? m_app->importFaceProps(QUrl::fromLocalFile(staged))
                                             : QVariantMap();
            QDir(staged).removeRecursively();
            if (!result.value(QStringLiteral("installed")).toStringList().contains(id)) {
                failInstall(id, tr("Could not add that face prop."));
                return;
            }
        }
        setState(id, QStringLiteral("installed"));
        emit ready(id, QString());
        return;
    }

    const QStringList ids = m_library ? m_library->importLocalPaths({localPath(id)}) : QStringList();
    if (ids.isEmpty()) {
        failInstall(id, tr("Could not add that asset to the media bin."));
        return;
    }
    setState(id, QStringLiteral("installed"));
    emit ready(id, ids.first());
}

void DriftAssetStore::failInstall(const QString &id, const QString &message)
{
    if (const auto install = m_installs.take(id)) {
        if (install->reply)
            install->reply->abort();
        QDir(install->partialDir).removeRecursively();
    }
    setState(id, QStringLiteral("failed"));
    emit failed(id, message);
}
