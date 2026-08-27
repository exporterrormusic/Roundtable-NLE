/*
 * AudioSync.cpp ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 5-step audio sync workflow panel implementation.
 */

#include "panels/audio/AudioSync.h"

#include "ai/Transcriber.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "spine/ShotPreset.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "widgets/MiniWaveformWidget.h"
#include "widgets/ManualMatchDialog.h"
#include "Theme.h"
#include "QtHelpers.h"

#include <spdlog/spdlog.h>

#include <QBoxLayout>
#include <QCoreApplication>
#include <QDir>
#include <QAction>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QHeaderView>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QTimer>
#include <QTextStream>
#include <QUrl>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QPainter>

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <sstream>
#include <filesystem>

//  TranscriptionWorker
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

namespace rt {

TranscriptionWorker::TranscriptionWorker(Transcriber* transcriber, QObject* parent)
    : QObject(parent)
    , m_transcriber(transcriber)
{
}

void TranscriptionWorker::process()
{
    if (!m_transcriber) {
        emit errorOccurred("Transcriber not initialized");
        emit finished(false);
        return;
    }

    try {

    if (m_cancelRequested.load(std::memory_order_acquire)) {
        emit finished(false);
        return;
    }

    auto progress = [this](float pct, const std::string& status) {
        emit progressChanged(pct, QString::fromStdString(status));
    };

    // Load model on the background thread (downloads from HuggingFace
    // if not cached — can take a while on slow connections).  Previously
    // this was done on the UI thread, freezing the app for the entire
    // download duration.
    if (!m_transcriber->isModelLoaded() || m_transcriber->currentModel() != m_modelSize) {
        emit progressChanged(0.0f, QStringLiteral("Loading transcription model..."));
        if (!m_transcriber->loadModel(m_modelSize, progress)) {
            emit errorOccurred(QString::fromStdString(
                m_transcriber->lastError().empty()
                    ? "Failed to load Whisper model"
                    : m_transcriber->lastError()));
            emit finished(false);
            return;
        }
    }

    // A cancellation request may arrive while loadModel() is starting. The
    // Transcriber resets its backend cancellation flag at model boundaries,
    // so retain this worker-owned token and check it before inference.
    if (m_cancelRequested.load(std::memory_order_acquire)) {
        m_transcriber->cancelAsync();
        emit finished(false);
        return;
    }

    m_result = m_transcriber->transcribe(m_audioPath, m_language, progress);

    if (!m_result.succeeded()) {
        if (m_result.status != TranscriptionStatus::Cancelled) {
            emit errorOccurred(QString::fromStdString(
                m_result.error.message.empty()
                    ? "Transcription failed"
                    : m_result.error.message));
        }
        emit finished(false);
    } else {
        emit finished(true);
    }
    } catch (const std::exception& error) {
        emit errorOccurred(QStringLiteral("Transcription exception: %1")
            .arg(QString::fromUtf8(error.what())));
        emit finished(false);
    } catch (...) {
        emit errorOccurred(QStringLiteral("Unknown transcription exception"));
        emit finished(false);
    }
}

// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  AudioSync ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â construction
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

AudioSync::AudioSync(QWidget* parent)
    : QWidget(parent)
    , m_transcriber(std::make_unique<Transcriber>())
{
    // Point whisper model downloads to writable user data directory
    m_transcriber->setModelsDirectory(
        (rt::userDataDir() + "/models").toStdString());

    // Trim-drag undo debounce timer
    m_trimDebounceTimer = new QTimer(this);
    m_trimDebounceTimer->setSingleShot(true);
    m_trimDebounceTimer->setInterval(400);
    connect(m_trimDebounceTimer, &QTimer::timeout, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_trimDebounceClipIdx < m_clips.size()) {
            auto ci   = m_trimDebounceClipIdx;
            double oS = m_preTrimStart, oE = m_preTrimEnd;
            int    oM = m_preTrimMatchState;
            double nS = m_clips[ci].start, nE = m_clips[ci].end;
            int    nM = m_clips[ci].matchState;
            if (m_commandStack) {
                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                    "Trim audio clip",
                    [this, ci, nS, nE, nM]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        if (ci < m_clips.size()) {
                            m_clips[ci].start = nS; m_clips[ci].end = nE;
                            m_clips[ci].matchState = nM;
                            if (auto* wf = waveformForClip(static_cast<int>(ci)))
                                wf->setTrimRange(nS, nE);
                            populateLeftList();
                            updateCardMatchStyle(ci);
                            updateSmartBar();
                        }
                    },
                    [this, ci, oS, oE, oM]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        if (ci < m_clips.size()) {
                            m_clips[ci].start = oS; m_clips[ci].end = oE;
                            m_clips[ci].matchState = oM;
                            if (auto* wf = waveformForClip(static_cast<int>(ci)))
                                wf->setTrimRange(oS, oE);
                            populateLeftList();
                            updateCardMatchStyle(ci);
                            updateSmartBar();
                        }
                    }
                ));
            }
            // Refresh card colors after drag completes
            populateLeftList();
            updateCardMatchStyle(ci);
            updateSmartBar();
        }
    });

    setupUi();
    updateWorkflowState();
}

AudioSync::~AudioSync()
{
    m_destroying.store(true);
    ++m_transcriptionRunId; // Invalidate queued callbacks from the active run.
    m_transcriptionState = TranscriptionState::Cancelling;

    QPointer<QThread> thread = m_workerThread;
    if (thread) {
        m_transcriber->cancelAsync();
        thread->quit();

        // QThread's destructor aborts the process if the thread is still
        // running. Cancellation is cooperative for both whisper.cpp and the
        // CrisperWhisper pipe worker, so wait for it to finish rather than
        // allowing QObject child destruction to hit that fatal path.
        if (thread->isRunning())
            thread->wait();

        QObject::disconnect(thread, nullptr, this, nullptr);
        if (m_worker)
            delete m_worker.data();
        m_worker = nullptr;
        delete thread.data();
        m_workerThread = nullptr;
    }
}

void AudioSync::setCommandStack(CommandStack* stack)
{
    m_commandStack = stack;
}

void AudioSync::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;
}

void AudioSync::runClipsMutationWithUndo(const std::string& description,
                                          std::function<void()> mutate,
                                          std::function<void()> rebuild)
{
    auto oldClips = m_clips;
    if (mutate) mutate();
    if (m_clips == oldClips) {
        // No observable change — refresh UI but don't pollute undo history.
        if (rebuild) rebuild();
        return;
    }
    auto newClips = m_clips;
    auto apply = [this, rebuild](std::vector<SyncClip> clips) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        m_clips = std::move(clips);
        if (rebuild) rebuild();
    };
    if (rebuild) rebuild();
    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            description,
            [apply, newClips]() mutable { apply(std::move(newClips)); },
            [apply, oldClips]() mutable { apply(std::move(oldClips)); }));
    }
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Event filter: route spacebar / J / K / L to the selected clip ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

// Helper: walk up from a widget to find which clip index it belongs to.
int AudioSync::clipRowForWidget(QWidget* widget) const
{
    if (!widget) return -1;
    // Walk up through card widgets
    QWidget* w = widget;
    while (w) {
        auto widgets = m_cardWidgets;  // local copy — protect against vector modification
        for (size_t i = 0; i < widgets.size(); ++i) {
            if (widgets[i] == w && i < m_cardClipIndices.size()) {
                return m_cardClipIndices[i];
            }
        }
        w = w->parentWidget();
    }
    return -1;
}

// Helper: find the MiniWaveformWidget for a given clip index via card mapping.
MiniWaveformWidget* AudioSync::waveformForClip(int clipIdx) const
{
    if (clipIdx < 0) return nullptr;
    for (size_t i = 0; i < m_cardClipIndices.size(); ++i) {
        if (m_cardClipIndices[i] == clipIdx && i < m_cardWaveforms.size()) {
            return m_cardWaveforms[i];
        }
    }
    return nullptr;
}

bool AudioSync::eventFilter(QObject* watched, QEvent* event)
{
    // While ManualMatchDialog is open, don't intercept any events
    if (m_manualMatchOpen)
        return QWidget::eventFilter(watched, event);

    // ── Right-click context menu on transcribe file list ────────────────
    if (watched == m_transcribeFileList && event->type() == QEvent::ContextMenu) {
        auto* ctxEvent = static_cast<QContextMenuEvent*>(event);
        QListWidgetItem* item = m_transcribeFileList->itemAt(ctxEvent->pos());
        if (!item) return QWidget::eventFilter(watched, event);
        int row = m_transcribeFileList->row(item);
        if (row < 0 || row >= static_cast<int>(m_audioPaths.size()))
            return QWidget::eventFilter(watched, event);

        // "Transcribed" if it has segments OR clips (clips can outlive segments
        // across state restores — match startTranscription's skip rule).
        const std::string& rowPath = m_audioPaths[static_cast<size_t>(row)];
        bool transcribed =
            (row < static_cast<int>(m_allTranscriptionResults.size()) &&
             !m_allTranscriptionResults[static_cast<size_t>(row)].segments.empty());
        if (!transcribed)
            for (const auto& c : m_clips)
                if (c.sourceFile == rowPath) { transcribed = true; break; }

        QMenu menu(m_transcribeFileList);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 8px 24px; border-radius: 4px; }"
            "QMenu::item:selected { background: %4; color: %5; }"
            "QMenu::item:disabled { color: %6; }"
            "QMenu::separator { height: 1px; background: %7; margin: 4px 8px; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accentDim))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().textTertiary))
            .arg(Theme::hex(Theme::colors().borderLight)));

        QAction* resetAction = menu.addAction(
            QStringLiteral("\U0001F504  Reset Transcription"));
        resetAction->setEnabled(transcribed);
        menu.addSeparator();
        QAction* resetAllAction = menu.addAction(
            QStringLiteral("Reset All Transcriptions"));

        QAction* chosen = menu.exec(m_transcribeFileList->mapToGlobal(ctxEvent->pos()));
        if (chosen == resetAction)
            clearTranscriptionForFile(static_cast<size_t>(row));
        else if (chosen == resetAllAction)
            clearAllTranscriptions();
        return true;
    }

    // ── Right-click context menu on script session list ─────────────────
    if (watched == m_scriptSessionList && event->type() == QEvent::ContextMenu) {
        auto* ctxEvent = static_cast<QContextMenuEvent*>(event);
        QPoint pos = ctxEvent->pos();
        QListWidgetItem* item = m_scriptSessionList->itemAt(pos);
        if (!item) return QWidget::eventFilter(watched, event);

        int row = m_scriptSessionList->row(item);
        QString key = item->data(Qt::UserRole).toString();

        QMenu menu(m_scriptSessionList);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 6px; padding: 4px; }"
            "QMenu::item { padding: 8px 24px; border-radius: 4px; }"
            "QMenu::item:selected { background: %4; color: %5; }"
            "QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accentDim))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().borderLight)));

        QAction* switchAction = menu.addAction(QStringLiteral("\U0001F517  Switch to this script"));
        menu.addSeparator();

        // Show "Sync with GDrive" only for URL-based scripts
        bool isUrl = key.startsWith("http://") || key.startsWith("https://");
        QAction* syncAction = nullptr;
        if (isUrl) {
            syncAction = menu.addAction(QStringLiteral("\U0001F504  Sync with GDrive"));
            menu.addSeparator();
        }

        QAction* renameAction = menu.addAction(QStringLiteral("\u270F  Rename"));
        QAction* deleteAction = menu.addAction(QStringLiteral("\u2716  Delete"));

        QAction* chosen = menu.exec(ctxEvent->globalPos());
        if (chosen == switchAction) {
            if (!key.isEmpty())
                switchToScript(key.toStdString());
        } else if (chosen == syncAction) {
            if (!key.isEmpty()) {
                // Switch to this session first, then fetch the latest from URL
                switchToScript(key.toStdString());
                m_scriptStatus->setText("Syncing with GDrive...");
                fetchScriptFromUrl(key);
            }
        } else if (chosen == renameAction) {
            // Find the history entry by URL and rename
            QString historyPath = rt::userDataDir() + QStringLiteral("/script_history.txt");
            QStringList lines;
            QFile rf(historyPath);
            if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&rf);
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (!line.isEmpty()) lines.append(line);
                }
                rf.close();
            }
            int foundIdx = -1;
            for (int i = 0; i < lines.size(); ++i) {
                int sep = lines[i].lastIndexOf('|');
                QString url = (sep >= 0) ? lines[i].mid(sep + 1).trimmed() : lines[i].trimmed();
                if (url == key) { foundIdx = i; break; }
            }
            if (foundIdx >= 0)
                renameScriptHistoryEntry(foundIdx);
        } else if (chosen == deleteAction) {
            // Remove session and delete history entry
            m_scriptSessions.erase(key.toStdString());
            QString historyPath = rt::userDataDir() + QStringLiteral("/script_history.txt");
            QStringList lines;
            QFile rf(historyPath);
            if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&rf);
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (!line.isEmpty()) lines.append(line);
                }
                rf.close();
            }
            int foundIdx = -1;
            for (int i = 0; i < lines.size(); ++i) {
                int sep = lines[i].lastIndexOf('|');
                QString url = (sep >= 0) ? lines[i].mid(sep + 1).trimmed() : lines[i].trimmed();
                if (url == key) { foundIdx = i; break; }
            }
            if (foundIdx >= 0)
                deleteScriptHistoryEntry(foundIdx);
            if (m_activeScriptKey == key.toStdString())
                clearCurrentSession();
            populateScriptSessionList();
        }
        return true;
    }

    // ---- Right-click context menu on import / transcribe file lists -----
    {
        auto* ctxEv = (event->type() == QEvent::ContextMenu)
            ? static_cast<QContextMenuEvent*>(event) : nullptr;
        QListWidget* list = nullptr;
        if (ctxEv && m_audioFileList) {
            auto* w = qobject_cast<QWidget*>(watched);
            if (w && (w == m_audioFileList || m_audioFileList->isAncestorOf(w)))
                list = m_audioFileList;
        }
        if (!list && ctxEv && m_transcribeFileList) {
            auto* w = qobject_cast<QWidget*>(watched);
            if (w && (w == m_transcribeFileList || m_transcribeFileList->isAncestorOf(w)))
                list = m_transcribeFileList;
        }
        if (list) {
            // Map context menu position from the watched widget's coords to the list's
            auto* w = qobject_cast<QWidget*>(watched);
            QPoint mappedPos = (w && w != list)
                ? list->mapFromGlobal(ctxEv->globalPos())
                : ctxEv->pos();
            QListWidgetItem* item = list->itemAt(mappedPos);
            if (!item) return QWidget::eventFilter(watched, event);
            int row = list->row(item);
            if (row < 0 || row >= static_cast<int>(m_audioPaths.size()))
                return QWidget::eventFilter(watched, event);
            bool hasTx = (row < static_cast<int>(m_allTranscriptionResults.size()) &&
                          !m_allTranscriptionResults[row].segments.empty());
            QMenu menu(list);
            menu.setStyleSheet(QStringLiteral(
                "QMenu { background: %1; color: %2; border: 1px solid %3;"
                "  border-radius: 6px; padding: 4px; }"
                "QMenu::item { padding: 8px 24px; border-radius: 4px; }"
                "QMenu::item:selected { background: %4; color: %5; }"
                "QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }")
                .arg(Theme::hex(Theme::colors().surface2))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().border))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().borderLight)));
            if (list == m_audioFileList) {
                QAction* trAct = menu.addAction(QStringLiteral("\u25B6  Transcribe"));
                menu.addSeparator();
                QAction* relinkAct = menu.addAction(QStringLiteral("\U0001F517  Re-link..."));
                QAction* rmAct = menu.addAction(QStringLiteral("\u2716  Remove File"));
                menu.addSeparator();
                QAction* expAct = menu.addAction(QStringLiteral("\U0001F4C2  Show in Explorer"));
                QAction* ch = menu.exec(ctxEv->globalPos());
                if (ch == trAct) {
                    showAudioSidePanel(2);
                    startSingleFileTranscription(static_cast<size_t>(row));
                } else if (ch == relinkAct) {
                    relinkAudioFile(row);
                } else if (ch == rmAct) {
                    removeFileFromSync(row);
                } else if (ch == expAct) {
                    QString fp = QString::fromStdString(m_audioPaths[static_cast<size_t>(row)]);
                    if (QFileInfo::exists(fp))
                        QProcess::startDetached("explorer", {"/select,", QDir::toNativeSeparators(fp)});
                }
            } else {
                QAction* trAct = menu.addAction(hasTx
                    ? QStringLiteral("\U0001F504  Re-transcribe")
                    : QStringLiteral("\u25B6  Transcribe"));
                QAction* clAct = hasTx ? menu.addAction(QStringLiteral("\U0001F5D1  Clear Transcription")) : nullptr;
                menu.addSeparator();
                QAction* rmAct = menu.addAction(QStringLiteral("\u2716  Remove File"));
                QAction* expAct = menu.addAction(QStringLiteral("\U0001F4C2  Show in Explorer"));
                QAction* ch = menu.exec(ctxEv->globalPos());
                if (ch == trAct) {
                    startSingleFileTranscription(static_cast<size_t>(row));
                } else if (ch == clAct) {
                    clearTranscriptionForFile(static_cast<size_t>(row));
                } else if (ch == rmAct) {
                    removeFileFromSync(row);
                } else if (ch == expAct) {
                    QString fp = QString::fromStdString(m_audioPaths[static_cast<size_t>(row)]);
                    if (QFileInfo::exists(fp))
                        QProcess::startDetached("explorer", {"/select,", QDir::toNativeSeparators(fp)});
                }
            }
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            auto* w = qobject_cast<QWidget*>(watched);
            if (w) {
                // Walk up to find the card frame
                QWidget* card = w;
                while (card && !card->objectName().startsWith("scriptCard_"))
                    card = card->parentWidget();
                if (card) {
                    // Find card index
                    auto widgets = m_cardWidgets;  // local copy — protect against vector modification
                    for (size_t i = 0; i < widgets.size(); ++i) {
                        if (widgets[i] == card) {
                            // Set selected clip
                            if (i < m_cardClipIndices.size() && m_cardClipIndices[i] >= 0)
                                m_selectedClipIdx = m_cardClipIndices[i];
                            // Apply card highlight (no scroll ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â only highlight)
                            if (i < m_cardScriptLineNums.size()) {
                                // Remove previous highlight
                                if (m_highlightedCard) {
                                    m_highlightedCard->setGraphicsEffect(nullptr);
                                    m_highlightedCard = nullptr;
                                }
                                auto* cardFrame = qobject_cast<QFrame*>(card);
                                if (cardFrame) {
                                    m_highlightedCard = cardFrame;
                                    auto* glow = new QGraphicsDropShadowEffect(cardFrame);
                                    glow->setBlurRadius(18);
                                    glow->setColor(Theme::colors().accent);
                                    glow->setOffset(0, 0);
                                    cardFrame->setGraphicsEffect(glow);
                                }
                            }
                            // Sync left list selection
                            if (m_leftScriptList && i < m_cardScriptLineNums.size()) {
                                auto scriptLineNum = m_cardScriptLineNums[i];
                                for (int r = 0; r < m_leftScriptList->count(); ++r) {
                                    auto* item = m_leftScriptList->item(r);
                                    if (item &&
                                        item->data(Qt::UserRole).toInt() == scriptLineNum) {
                                        m_leftScriptList->blockSignals(true);
                                        m_leftScriptList->setCurrentRow(r);
                                        m_leftScriptList->blockSignals(false);
                                        break;
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const int key = ke->key();

        // Don't steal keys from editable text widgets
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            if (qobject_cast<QLineEdit*>(w) || qobject_cast<QComboBox*>(w))
                return QWidget::eventFilter(watched, event);
        }

        // Resolve the target clip row.  Priority:
        //   1. m_selectedClipIdx  (set reliably by waveform click / play btn)
        //   2. Walk widget hierarchy from the watched widget
        //   3. QListWidget::currentRow()
        auto resolveRow = [&]() -> int {
            if (m_selectedClipIdx >= 0 &&
                static_cast<size_t>(m_selectedClipIdx) < m_clips.size())
                return m_selectedClipIdx;
            int r = clipRowForWidget(qobject_cast<QWidget*>(watched));
            if (r >= 0) return r;
            return -1;
        };

        // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Space key ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
        // Always handle Space here.  Never pass through to the widgetÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢s
        // keyPressEventÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Âfocus can end up on the QListWidget itself rather
        // than on the MiniWaveformWidget, making the per-widget handler
        // unreliable.
        if (key == Qt::Key_Space) {
            int row = resolveRow();
            if (row >= 0 && static_cast<size_t>(row) < m_clips.size()) {
                spdlog::debug("AudioSync::eventFilter  Space ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ clip {}", row);
                m_selectedClipIdx = row;
                togglePlayClip(static_cast<size_t>(row));
                return true;  // consumed
            }
            return QWidget::eventFilter(watched, event);
        }

        // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ J / K / L / I / O keys ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
        if (key == Qt::Key_J || key == Qt::Key_K || key == Qt::Key_L ||
            key == Qt::Key_I || key == Qt::Key_O)
        {
            if (m_forwardingKey)
                return false;  // inside sendEvent ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â let it reach the target widget

            int row = resolveRow();
            if (row < 0 || static_cast<size_t>(row) >= m_clips.size())
                return QWidget::eventFilter(watched, event);

            auto* wf = waveformForClip(row);
            if (wf) {
                m_forwardingKey = true;
                QCoreApplication::sendEvent(wf, ke);
                m_forwardingKey = false;
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Safety-net: catch Space that propagates up from unfiltered children ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
void AudioSync::keyPressEvent(QKeyEvent* event)
{
    if (m_manualMatchOpen) { QWidget::keyPressEvent(event); return; }

    // Ctrl+Z / Ctrl+Shift+Z (undo / redo)
    if (event->key() == Qt::Key_Z && (event->modifiers() & Qt::ControlModifier)) {
        if (m_commandStack) {
            if (event->modifiers() & Qt::ShiftModifier)
                m_commandStack->redo();
            else
                m_commandStack->undo();
            event->accept();
            return;
        }
    }
    // Ctrl+Y (redo)
    if (event->key() == Qt::Key_Y && (event->modifiers() & Qt::ControlModifier)) {
        if (m_commandStack) {
            m_commandStack->redo();
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        int row = (m_selectedClipIdx >= 0 &&
                   static_cast<size_t>(m_selectedClipIdx) < m_clips.size())
                      ? m_selectedClipIdx
                      : -1;
        if (row >= 0 && static_cast<size_t>(row) < m_clips.size()) {
            m_selectedClipIdx = row;
            togglePlayClip(static_cast<size_t>(row));
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  UI setup
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â


// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  Workflow actions
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

void AudioSync::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_cardsDirty) {
        // populateScriptList() already rebuilds both the left list and the
        // cards; calling populateClipList() afterwards rebuilt every card a
        // second time. Build once.
        populateLeftList();
        populateCards();
        m_cardsDirty = false;
    }
}

bool AudioSync::loadScript(const std::string& pathOrContent,
                           const std::string& sourceIdentifier)
{
    try {
        // Save the current session before loading a new script
        saveCurrentSession();

        // Preserve the raw text content so we can restore the script
        // offline without re-fetching from URL on project open.
        m_scriptRawContent = pathOrContent;

        auto parsed = Script::load(pathOrContent);
        if (parsed.isEmpty()) {
            spdlog::warn("AudioSync: Script is empty");
            m_scriptStatus->setText("No lines found");
            m_pendingSessionName.clear();
            return false;
        }

        // Determine session key: use sourceIdentifier if provided, else pathOrContent
        std::string sessionKey = sourceIdentifier.empty() ? pathOrContent : sourceIdentifier;
        if (sessionKey.empty()) sessionKey = "script_" + std::to_string(parsed.lineCount());

        // If this exact source was loaded before, update the session with
        // the new script content instead of ignoring the download.
        if (m_scriptSessions.count(sessionKey)) {
            spdlog::info("AudioSync: Re-loading URL '{}' — updating existing session",
                         sessionKey);
            m_pendingSessionName.clear();
            updateExistingSessionScript(sessionKey,
                std::make_unique<Script>(std::move(parsed)));

            // Update UI — switchToScript returns early if same session,
            // so refresh manually if this is the active session.
            if (sessionKey == m_activeScriptKey) {
                // saveCurrentSession() (at the top of loadScript) moved the
                // live working state — m_clips, m_allTranscriptionResults,
                // m_audioPaths, m_audioSamples, etc. — into the session, and
                // updateExistingSessionScript() only updated the *session*.
                // Restore the (remapped) state into the live members or the
                // clip list, transcribe list and auto-sync all see empty data
                // and the in-progress sync is silently wiped.
                restoreSession(sessionKey);
                m_syncDone = false;

                // Rebuild the audio file list widget from the restored paths
                // (it was never cleared, so it would otherwise drift from
                // m_audioPaths after the move/restore round-trip).
                if (m_audioFileList) {
                    m_audioFileList->blockSignals(true);
                    m_audioFileList->clear();
                    for (const auto& ap : m_audioPaths)
                        addAudioFileListItem(QString::fromStdString(ap));
                    m_audioFileList->blockSignals(false);
                }
                if (m_audioStatus)
                    m_audioStatus->setText(m_audioPaths.empty()
                        ? "No files imported"
                        : QString("%1 file(s)").arg(m_audioPaths.size()));

                populateScriptFilter();
                populateScriptList();
                if (!m_clips.empty())
                    populateClipList();
                refreshTranscribeFileList();
                updateWorkflowState();
                m_scriptStatus->setText(QString("%1 lines, %2 characters")
                    .arg(m_script ? m_script->lineCount() : 0)
                    .arg(m_script ? m_script->characters.size() : 0));
                populateScriptSessionList();
            } else {
                switchToScript(sessionKey);
            }
            return true;
        }

        // Preserve script source and audio/clip state before clearing
        // (clearCurrentSession wipes m_audioPaths, m_clips etc., which may
        // have been restored from the project blob before the async script
        // download finished).
        QString savedSource = m_lastScriptSource;
        auto savedAudioPaths = std::move(m_audioPaths);
        auto savedAudioPath = std::move(m_audioPath);
        auto savedClips = std::move(m_clips);
        auto savedAudioSamples = std::move(m_audioSamples);
        auto savedLineAudioFile = std::move(m_lineAudioFile);

        clearCurrentSession();
        m_lastScriptSource = savedSource;
        m_activeScriptKey = sessionKey;

        m_script = std::make_unique<Script>(std::move(parsed));
        m_scriptLoaded = true;

        // Restore audio/clip state that was saved before clearing
        if (!savedAudioPaths.empty()) {
            m_audioPaths = std::move(savedAudioPaths);
            m_audioPath = std::move(savedAudioPath);
            m_clips = std::move(savedClips);
            m_audioSamples = std::move(savedAudioSamples);
            m_lineAudioFile = std::move(savedLineAudioFile);
            m_audioImported = true;
            if (m_audioStatus)
                m_audioStatus->setText(QString("%1 file(s)").arg(m_audioPaths.size()));
            // Rebuild audio file list widget (cleared by clearCurrentSession)
            if (m_audioFileList) {
                m_audioFileList->blockSignals(true);
                m_audioFileList->clear();
                for (const auto& ap : m_audioPaths)
                    addAudioFileListItem(QString::fromStdString(ap));
                m_audioFileList->blockSignals(false);
            }
            // Rebuild card widgets since clearCurrentSession cleared them
            populateScriptList();
            populateClipList();
        }

        // Determine display name
        auto& session = m_scriptSessions[sessionKey];
        if (!m_pendingSessionName.empty()) {
            // Interactive load — use user-chosen name
            session.displayName = m_pendingSessionName;
            m_pendingSessionName.clear();
        } else {
            // Restore or auto — derive from source
            session.displayName = displayNameForScriptUrl(QString::fromStdString(sessionKey)).toStdString();
        }
        session.sourceUrl = sessionKey;

        populateScriptFilter();
        if (!m_restoring) {
            populateScriptList();
            updateWorkflowState();
        }

        m_scriptStatus->setText(QString("%1 lines, %2 characters")
            .arg(m_script->lineCount())
            .arg(m_script->characters.size()));

        populateScriptSessionList();

        spdlog::info("AudioSync: Loaded script session '{}' with {} lines",
                     session.displayName, m_script->lineCount());
        emit scriptLoaded(static_cast<int>(m_script->lineCount()));
        emit voiceContextChanged();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("AudioSync: Failed to load script: {}", e.what());
        m_scriptStatus->setText(QString("Error: %1").arg(e.what()));
        m_pendingSessionName.clear();
        return false;
    }
}

bool AudioSync::importAudio(const std::string& audioPath)
{
    if (audioPath.empty()) return false;

    // One imported path owns exactly one transcription-result slot. Avoid a
    // duplicate row if the same file is selected twice.
    if (std::find(m_audioPaths.begin(), m_audioPaths.end(), audioPath)
        != m_audioPaths.end()) {
        spdlog::info("AudioSync: Audio already imported: {}", audioPath);
        return true;
    }

    // Repair any legacy size mismatch before appending the new parallel slot.
    m_allTranscriptionResults.resize(m_audioPaths.size());

    m_audioPath = audioPath;
    m_audioPaths.push_back(audioPath);
    m_allTranscriptionResults.emplace_back();
    m_audioImported = true;
    if (m_audioPathEdit) m_audioPathEdit->setText(QString::fromStdString(audioPath));
    m_audioStatus->setText(QString("%1 file(s)").arg(m_audioPaths.size()));

    // Load audio samples FIRST so the list item can display a waveform
    loadAudioSamples();

    // Add to the audio file list widget
    if (m_audioFileList) {
        addAudioFileListItem(QString::fromStdString(audioPath));
    }
    refreshTranscribeFileList();

    updateWorkflowState();

    spdlog::info("AudioSync: Imported audio: {}", audioPath);
    emit audioImported(QString::fromStdString(audioPath));
    return true;
}

void AudioSync::importAudioFiles(const QStringList& paths)
{
    for (const auto& path : paths)
        importAudio(path.toStdString());
}

void AudioSync::startTranscription()
{
    if (m_transcriptionState != TranscriptionState::Idle || m_workerThread) {
        spdlog::warn("AudioSync: Ignoring Transcribe request while run {} is active",
                     m_transcriptionRunId);
        return;
    }

    if (m_audioPaths.empty()) {
        m_transcribeStatus->setText("No audio files imported");
        return;
    }

    // Model loading is now deferred to the background worker thread
    // (TranscriptionWorker::process) to avoid blocking the UI during
    // download from HuggingFace.

    // Preserve existing transcription results — only transcribe new files
    if (m_allTranscriptionResults.size() < m_audioPaths.size()) {
        m_allTranscriptionResults.resize(m_audioPaths.size());
    }

    // A file counts as already transcribed if it has stored segments OR has
    // already produced clips. Clips persist across some state restores (e.g.
    // loadScript's preserve block) where the raw segments do not — without the
    // clip check those files would be needlessly re-transcribed, which is the
    // "Transcribe All re-does everything" bug.
    std::unordered_set<std::string> filesWithClips;
    for (const auto& c : m_clips)
        filesWithClips.insert(c.sourceFile);

    // Build list of file indices that still need transcription
    std::vector<size_t> pendingIndices;
    for (size_t i = 0; i < m_audioPaths.size(); ++i) {
        const bool hasSegments = !m_allTranscriptionResults[i].segments.empty();
        const bool hasClips    = filesWithClips.count(m_audioPaths[i]) > 0;
        if (!hasSegments && !hasClips)
            pendingIndices.push_back(i);
    }

    if (pendingIndices.empty()) {
        m_transcribeStatus->setText("All files already transcribed");
        m_transcribeStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().success)));
        return;
    }

    beginTranscriptionRun(std::move(pendingIndices));
}

void AudioSync::startSingleFileTranscription(size_t index)
{
    if (m_transcriptionState != TranscriptionState::Idle || m_workerThread) {
        spdlog::warn("AudioSync: Ignoring per-file Transcribe request while run {} is active",
                     m_transcriptionRunId);
        if (m_transcribeStatus)
            m_transcribeStatus->setText(QStringLiteral("A transcription is already in progress"));
        return;
    }
    if (index >= m_audioPaths.size())
        return;

    if (m_allTranscriptionResults.size() < m_audioPaths.size())
        m_allTranscriptionResults.resize(m_audioPaths.size());
    beginTranscriptionRun({index});
}

void AudioSync::beginTranscriptionRun(std::vector<size_t> indices)
{
    if (m_transcriptionState != TranscriptionState::Idle || m_workerThread || indices.empty())
        return;

    indices.erase(std::remove_if(indices.begin(), indices.end(), [this](size_t index) {
        return index >= m_audioPaths.size();
    }), indices.end());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty())
        return;

    m_pendingTranscriptionIndices = std::move(indices);
    m_transcriptionRunTotal = m_pendingTranscriptionIndices.size();
    m_transcriptionRunCompleted = 0;
    m_transcriptionRunHadSuccess = false;
    m_transcriptionRunError.clear();
    m_transcriptionState = TranscriptionState::Running;
    const uint64_t runId = ++m_transcriptionRunId;
    m_currentTranscriptionIndex = m_pendingTranscriptionIndices.front();

    m_transcribeStatus->setText(QString("Transcribing file 1/%1...")
        .arg(m_transcriptionRunTotal));
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    updateTranscriptionControls();

    spdlog::info("AudioSync: Starting transcription run {} with {} file(s)",
                 runId, m_transcriptionRunTotal);
    emit transcriptionStarted();
    startTranscriptionForFile(m_currentTranscriptionIndex, runId);
}

void AudioSync::startTranscriptionForFile(size_t index, uint64_t runId)
{
    if (runId != m_transcriptionRunId
        || m_transcriptionState == TranscriptionState::Idle
        || index >= m_audioPaths.size()) {
        return;
    }

    // A new worker is launched only after the previous QThread::finished
    // callback has cleared these pointers. Never replace an active worker.
    if (m_workerThread || m_worker) {
        spdlog::error("AudioSync: Refusing to replace active worker in run {}", runId);
        return;
    }

    m_audioPath = m_audioPaths[index];

    auto* thread = new QThread;
    auto* worker = new TranscriptionWorker(m_transcriber.get());
    m_workerThread = thread;
    m_worker = worker;
    worker->setAudioPath(m_audioPath);
    {
        const QString modelId = m_modelCombo->currentData().isValid()
            ? m_modelCombo->currentData().toString()
            : m_modelCombo->currentText();
        auto modelName = modelId.toStdString();
        worker->setModelSize(whisperModelFromName(modelName));
    }
    worker->moveToThread(thread);

    const QPointer<TranscriptionWorker> safeWorker(worker);
    const QPointer<QThread> safeThread(thread);
    connect(thread, &QThread::started, worker, &TranscriptionWorker::process);
    connect(worker, &TranscriptionWorker::progressChanged, this,
            [this, runId, safeWorker](float percent, const QString& status) {
                if (runId == m_transcriptionRunId && safeWorker == m_worker)
                    onTranscriptionProgress(percent, status);
            }, Qt::QueuedConnection);
    connect(worker, &TranscriptionWorker::errorOccurred, this,
            [this, runId, safeWorker](const QString& error) {
                if (safeWorker == m_worker)
                    handleTranscriptionError(runId, error);
            }, Qt::QueuedConnection);
    connect(worker, &TranscriptionWorker::finished, this,
            [this, runId, safeWorker, safeThread](bool success) {
                if (safeWorker == m_worker)
                    handleTranscriptionFinished(runId, safeWorker, success);
                // The worker slot has returned after this signal. Asking the
                // event loop to quit is safe even for a stale/invalidated run.
                if (safeThread)
                    safeThread->quit();
            }, Qt::QueuedConnection);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this,
            [this, runId, safeThread] {
                handleTranscriptionThreadFinished(runId, safeThread);
            }, Qt::QueuedConnection);

    thread->start();
}

int AudioSync::scriptLineCount() const
{
    return m_script ? static_cast<int>(m_script->lineCount()) : 0;
}

QStringList AudioSync::scriptCharacters() const
{
    QStringList result;
    if (m_script) {
        for (const auto& ch : m_script->characters)
            result.append(QString::fromStdString(ch));
    }
    // MATCH can retain valid character-tagged clips in restored/legacy
    // projects even if the parsed Script character cache is incomplete.
    for (const auto& clip : m_clips) {
        const QString character = QString::fromUtf8(clip.character).trimmed();
        if (!character.isEmpty() && !result.contains(character, Qt::CaseInsensitive))
            result.append(character);
    }
    return result;
}

} // namespace rt
