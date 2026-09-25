#include "QtHelpers.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

static void qtMessageFilter(QtMsgType type, const QMessageLogContext& /*ctx*/, const QString& msg)
{
    if (type == QtWarningMsg && msg.contains("setHighDpiScaleFactorRoundingPolicy"))
        return;
    switch (type) {
    case QtDebugMsg:    spdlog::debug("[Qt] {}", msg.toStdString()); break;
    case QtInfoMsg:     spdlog::info("[Qt] {}", msg.toStdString()); break;
    case QtWarningMsg:  spdlog::warn("[Qt] {}", msg.toStdString()); break;
    case QtCriticalMsg: spdlog::error("[Qt] {}", msg.toStdString()); break;
    case QtFatalMsg:    spdlog::critical("[Qt] {}", msg.toStdString()); break;
    }
}

void rt::installQtMessageFilter()
{
    qInstallMessageHandler(qtMessageFilter);
}

QString rt::findProjectRoot()
{
    QDir dir(QCoreApplication::applicationDirPath());

    for (int i = 0; i < 5; ++i)
    {
        if (dir.exists("CMakeLists.txt") &&
            dir.exists("assets") &&
            dir.exists("shaders"))
        {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) break;
    }

    // Installed layout: app dir itself contains assets/ (e.g. Program Files)
    QDir appDir(QCoreApplication::applicationDirPath());
    if (appDir.exists("assets")) {
        return appDir.absolutePath();
    }

    // Also check the parent of the app dir (some layouts bundle in a subfolder)
    QDir parentDir(QCoreApplication::applicationDirPath());
    if (parentDir.cdUp() && parentDir.exists("assets")) {
        return parentDir.absolutePath();
    }

    return QDir::currentPath();
}

QString rt::userDataDir()
{
    // Use QStandardPaths for cross-platform writable data location.
    // On Windows this resolves to %LOCALAPPDATA%/ROUNDTABLE/.
    QString path = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);

    // Create the directory if it doesn't exist yet.
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(".");
        spdlog::info("Created user data directory: {}", path.toStdString());
    }

    return path;
}

QString rt::bundledAssetsDir()
{
    return QDir(findProjectRoot()).filePath(QStringLiteral("assets"));
}

QString rt::downloadedCharacterAssetsDir()
{
    // Downloaded characters live with the bundled ones in the program's own
    // assets/ folder.  They used to go to %LOCALAPPDATA%, where COMPOSE found
    // them but the timeline renderer (SpineEngine, bundled root only) did
    // not, so their shots rendered black.
    return bundledAssetsDir();
}

QString rt::findCharacterDirectory(const QString& characterName)
{
    const QString relativePath = QStringLiteral("characters/") + characterName;

    const QString userPath = QDir(downloadedCharacterAssetsDir()).filePath(relativePath);
    if (QDir(userPath).exists())
        return QDir(userPath).absolutePath();

    const QString bundledPath = QDir(bundledAssetsDir()).filePath(relativePath);
    if (QDir(bundledPath).exists())
        return QDir(bundledPath).absolutePath();

    return {};
}

void rt::rescanCharacterModels(ModelManager* modelManager)
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!modelManager) return;

    modelManager->scan(QDir::toNativeSeparators(bundledAssetsDir()).toStdString());
    modelManager->scanAdditional(
        QDir::toNativeSeparators(downloadedCharacterAssetsDir()).toStdString());
#else
    Q_UNUSED(modelManager);
#endif
}
