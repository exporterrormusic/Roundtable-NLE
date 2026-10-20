/*
 * AudioSyncHandlers.cpp - UI event handlers and transcription callbacks for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "audiosync/AutoSync.h"
#include "ai/Transcriber.h"
#include "ai/ScriptMatcher.h"
#include "audio/AudioEngine.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QDir>
#include <functional>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSettings>
#include <QMessageBox>
#include <QSignalBlocker>

#include "Settings.h"

namespace rt {

void AudioSync::onLoadScriptClicked()
{
    // ── Build the combined popup dialog ──────────────────────────────────
    QDialog dlg(this);
    dlg.setWindowTitle("Load Script");
    dlg.setFixedSize(520, 260);
    dlg.setStyleSheet(QStringLiteral(
        "QDialog { background: %1; }")
        .arg(Theme::hex(Theme::colors().surface0)));

    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    // URL / file path field
    auto* urlLabel = new QLabel("Script URL or file path:");
    urlLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 600; color: %1;")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    layout->addWidget(urlLabel);

    auto* urlRow = new QHBoxLayout;
    urlRow->setSpacing(8);

    auto* urlEdit = new QLineEdit;
    urlEdit->setPlaceholderText("Google Docs URL or local file path...");
    urlEdit->setMinimumHeight(40);
    urlEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; padding: 8px 12px; font-size: 13px; }"
        "QLineEdit:focus { border-color: %5; }")
        .arg(Theme::hex(Theme::colors().inputBg))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().inputBorder))
        .arg(Theme::metrics().radiusSm)
        .arg(Theme::hex(Theme::colors().accent)));
    urlRow->addWidget(urlEdit, 1);

    auto* browseBtn = new QPushButton("Browse...");
    browseBtn->setFixedHeight(40);
    browseBtn->setCursor(Qt::PointingHandCursor);
    browseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; padding: 8px 16px; font-size: 13px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::metrics().radiusSm)
        .arg(Theme::hex(Theme::colors().surface3)));
    connect(browseBtn, &QPushButton::clicked, &dlg, [urlEdit]() {
        QString path = QFileDialog::getOpenFileName(
            nullptr, "Select Script File",
            QString(), "Script Files (*.txt *.json *.html);;All Files (*)");
        if (!path.isEmpty())
            urlEdit->setText(path);
    });
    urlRow->addWidget(browseBtn);
    layout->addLayout(urlRow);

    // Script name field
    auto* nameLabel = new QLabel("Script name:");
    nameLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 600; color: %1;")
        .arg(Theme::hex(Theme::colors().textPrimary)));
    layout->addWidget(nameLabel);

    auto* nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText("e.g. Episode 5 Scene 3");
    nameEdit->setMinimumHeight(40);
    nameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; padding: 8px 12px; font-size: 13px; }"
        "QLineEdit:focus { border-color: %5; }")
        .arg(Theme::hex(Theme::colors().inputBg))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().inputBorder))
        .arg(Theme::metrics().radiusSm)
        .arg(Theme::hex(Theme::colors().accent)));
    layout->addWidget(nameEdit);

    // Auto-fill name when URL changes
    // For Google Docs URLs, also asynchronously fetch the real document title.
    QObject::connect(urlEdit, &QLineEdit::textChanged, &dlg,
        [urlEdit, nameEdit, &dlg](const QString& text) {
        // Sync fallback — set placeholder name from URL
        if (!nameEdit->isModified()) {
            QString defaultName = AudioSync::displayNameForScriptUrl(text);
            if (!text.startsWith("http://") && !text.startsWith("https://")) {
                QFileInfo fi(text);
                QString stem = fi.completeBaseName();
                if (!stem.isEmpty() && stem.length() <= 50)
                    defaultName = stem;
            }
            nameEdit->setText(defaultName);
        }

        // Async — fetch the real document title for Google Docs URLs
        QRegularExpression gdRe(R"(docs\.google\.com/document/d/([^/\?]+))");
        auto gdMatch = gdRe.match(text);
        if (!gdMatch.hasMatch()) return;
        QString docId = gdMatch.captured(1);

        auto* manager = new QNetworkAccessManager(&dlg);
        QNetworkRequest request{QUrl(
            QString("https://docs.google.com/document/d/%1/").arg(docId))};
        request.setRawHeader("User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");

        auto* reply = manager->get(request);
        // Use QPointer guards — manager and reply are parented to the
        // stack-local QDialog, so they are destroyed if the user closes
        // the dialog before the HTTP request completes.  Without QPointer,
        // the lambda below would call deleteLater() on already-freed
        // objects → heap corruption (0xC0000374).
        QPointer<QNetworkAccessManager> mgrGuard(manager);
        QPointer<QNetworkReply> replyGuard(reply);
        connect(reply, &QNetworkReply::finished, &dlg,
                [mgrGuard, replyGuard, nameEdit]() {
            if (replyGuard) replyGuard->deleteLater();
            if (mgrGuard)   mgrGuard->deleteLater();
            if (!replyGuard || replyGuard->error() != QNetworkReply::NoError) return;
            if (nameEdit->isModified()) return; // user already typed a name

            std::string html = replyGuard->readAll().toStdString();
            std::string title = AudioSync::extractHtmlTitle(html);
            if (title.empty()) return;

            // Strip trailing " - Google Docs" suffix
            static const std::string suffix = " - Google Docs";
            auto pos = title.rfind(suffix);
            if (pos != std::string::npos) title.resize(pos);
            if (title.empty()) return;

            nameEdit->setText(QString::fromStdString(title));
        });
    });

    layout->addStretch();

    // OK / Cancel buttons
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btnBox->button(QDialogButtonBox::Ok)->setText("Load");
    btnBox->button(QDialogButtonBox::Ok)->setMinimumHeight(40);
    btnBox->button(QDialogButtonBox::Cancel)->setMinimumHeight(40);
    btnBox->button(QDialogButtonBox::Ok)->setCursor(Qt::PointingHandCursor);
    btnBox->button(QDialogButtonBox::Cancel)->setCursor(Qt::PointingHandCursor);
    btnBox->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none;"
        "  border-radius: %3px; padding: 8px 24px; font-size: %4px; font-weight: 700; }"
        "QPushButton:hover { background: %5; }"
        "QPushButton[text=\"Cancel\"] { background: %6; color: %7; }"
        "QPushButton[text=\"Cancel\"]:hover { background: %8; }")
        .arg(Theme::hex(Theme::colors().primaryBtnBg))
        .arg(Theme::hex(Theme::colors().textBright))
        .arg(Theme::metrics().radiusSm)
        .arg(Theme::typography().sizeSmall)
        .arg(Theme::hex(Theme::colors().primaryBtnHover))
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().surface3)));
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    // ── Show dialog ──────────────────────────────────────────────────────
    if (dlg.exec() != QDialog::Accepted)
        return;

    QString urlToUse = urlEdit->text().trimmed();
    QString chosenName = nameEdit->text().trimmed();

    if (urlToUse.isEmpty()) return;
    if (chosenName.isEmpty()) {
        // Fall back to auto-generated name
        chosenName = displayNameForScriptUrl(urlToUse);
        if (!urlToUse.startsWith("http://") && !urlToUse.startsWith("https://")) {
            QFileInfo fi(urlToUse);
            QString stem = fi.completeBaseName();
            if (!stem.isEmpty() && stem.length() <= 50)
                chosenName = stem;
        }
        if (chosenName.isEmpty()) return;
    }

    m_lastScriptSource = urlToUse;
    m_pendingSessionName = chosenName.toStdString();

    if (urlToUse.startsWith("http://") || urlToUse.startsWith("https://")) {
        addToScriptHistory(urlToUse);
        fetchScriptFromUrl(urlToUse);
    } else {
        loadScript(urlToUse.toStdString(), urlToUse.toStdString());
    }
}

void AudioSync::fetchScriptFromUrl(const QString& url)
{
    QString docId;
    bool isGoogleDocs = url.contains("docs.google.com/document");
    if (isGoogleDocs) {
        QRegularExpression re(R"(/d/([^/\?]+))");
        auto match = re.match(url);
        if (match.hasMatch())
            docId = match.captured(1);
    }

    // Build a list of URLs to try in order.
    // For Google Docs, attempt 0 is the document page itself (to extract the
    // real document title from its <title> tag), followed by export formats.
    QStringList urlsToTry;
    if (isGoogleDocs && !docId.isEmpty()) {
        urlsToTry << QString("https://docs.google.com/document/d/%1/").arg(docId)          // title page
                  << QString("https://docs.google.com/document/d/%1/export?format=html").arg(docId)
                  << QString("https://docs.google.com/document/d/%1/export?format=txt").arg(docId);
    } else {
        urlsToTry << url;
    }

    m_scriptStatus->setText("Fetching script...");
    m_scriptStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().textSecondary)));
    m_loadScriptBtn->setEnabled(false);

    struct FetchState {
        int attemptIndex = 0;
        QNetworkAccessManager* manager = nullptr;
    };
    // shared_ptr ensures FetchState outlives any in-flight lambda that
    // captures it — avoids the raw delete / deleteLater() ordering race.
    auto state = std::make_shared<FetchState>();

    // Capture the original URL for session key purposes
    QString originalUrl = url;

    // Use a shared_ptr<std::function> so recursive async calls stay valid
    // even after fetchScriptFromUrl returns (avoids dangling references to
    // local variables urlsToTry and tryNextUrl).
    auto sharedTryNextUrl = std::make_shared<std::function<void()>>();
    *sharedTryNextUrl = [this, state, urlsToTry, sharedTryNextUrl, docId, isGoogleDocs, originalUrl]() {
        if (state->attemptIndex >= static_cast<int>(urlsToTry.size())) {
            m_loadScriptBtn->setEnabled(true);
            return;
        }

        bool isTitlePage = (isGoogleDocs && state->attemptIndex == 0);

        QString currentUrl = urlsToTry[static_cast<size_t>(state->attemptIndex)];
        spdlog::info("Fetching script (attempt {}/{}): {}", state->attemptIndex + 1,
                     urlsToTry.size(), currentUrl.toStdString());

        state->manager = new QNetworkAccessManager(this);
        QNetworkRequest request{QUrl(currentUrl)};
        request.setRawHeader("User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");

        auto* reply = state->manager->get(request);
        reply->setProperty("attemptIndex", state->attemptIndex);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, state, urlsToTry, sharedTryNextUrl, isGoogleDocs, originalUrl, isTitlePage]() {
            int attempt = reply->property("attemptIndex").toInt();
            reply->deleteLater();
            state->manager->deleteLater();
            state->manager = nullptr;

            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                std::string content = data.toStdString();

                // ── Title page (attempt 0 for Google Docs) ──────────────
                // Extract the document title from the <title> tag, then
                // advance to the next attempt to fetch the actual content.
                if (isTitlePage) {
                    std::string docTitle = extractHtmlTitle(content);
                    if (!docTitle.empty()) {
                        // Strip trailing " - Google Docs" or " - Google Docs (something)" suffix
                        static const std::string suffix = " - Google Docs";
                        auto pos = docTitle.rfind(suffix);
                        if (pos != std::string::npos)
                            docTitle.resize(pos);
                        if (!docTitle.empty()) {
                            std::string cur = m_pendingSessionName;
                            if (cur.find("Google Doc") != std::string::npos)
                                m_pendingSessionName = std::move(docTitle);
                        }
                    }
                    spdlog::info("Google Docs title page fetched, advancing to content export");
                    state->attemptIndex = attempt + 1;
                    (*sharedTryNextUrl)();
                    return;
                }

                spdlog::info("Downloaded script: {} bytes", content.size());
                m_loadScriptBtn->setEnabled(true);
                // Pass the original URL as the session key, not the raw content
                loadScript(content, originalUrl.toStdString());
                return;
            }

            // Authentication error for Google Docs — try next format
            if (reply->error() == QNetworkReply::AuthenticationRequiredError &&
                isGoogleDocs && attempt < static_cast<int>(urlsToTry.size()) - 1)
            {
                spdlog::warn("Google Docs {} requires auth, trying next format",
                             isTitlePage ? "title page" : "export");
                state->attemptIndex = attempt + 1;
                (*sharedTryNextUrl)();
                return;
            }

            QString errMsg = reply->errorString();
            spdlog::error("Failed to fetch script from URL: {}", errMsg.toStdString());
            m_scriptStatus->setText(QString("Error: %1").arg(errMsg));
            m_scriptStatus->setStyleSheet(QString("color: %1;").arg(Theme::hex(Theme::colors().error)));
            m_loadScriptBtn->setEnabled(true);
        });
    };

    (*sharedTryNextUrl)();
}

void AudioSync::onImportAudioClicked()
{
    // Restore last used directory from QSettings
    auto settings = rt::appSettings();
    QString lastDir = settings.value("AudioSync/lastImportDir", QString()).toString();

    // Fall back to member variable if QSettings is empty
    if (lastDir.isEmpty() && !m_lastImportDir.isEmpty())
        lastDir = m_lastImportDir;

    // Fall back to user's home directory if still empty
    if (lastDir.isEmpty())
        lastDir = QDir::homePath();

    // Video files import their audio track (FFmpeg) for transcription; the
    // Match tab then shows video previews for them.
    QStringList paths = QFileDialog::getOpenFileNames(this, "Import Audio or Video Files",
        lastDir,
        "Audio or Video (*.wav *.mp3 *.flac *.ogg *.m4a *.mp4 *.mov *.mxf *.mkv *.m4v *.avi *.webm);;"
        "Audio Files (*.wav *.mp3 *.flac *.ogg *.m4a);;"
        "Video Files (*.mp4 *.mov *.mxf *.mkv *.m4v *.avi *.webm);;All Files (*)");
    if (!paths.isEmpty()) {
        // Save the directory for next time
        QString dir = QFileInfo(paths.first()).absolutePath();
        m_lastImportDir = dir;
        settings.setValue("AudioSync/lastImportDir", dir);
        settings.sync();
        importAudioFiles(paths);
    }
}

void AudioSync::onTranscribeClicked()
{
    startTranscription();
}

bool AudioSync::isCrisperWhisperSelected() const
{
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    if (!m_modelCombo) return false;
    const QString modelId = m_modelCombo->currentData().isValid()
        ? m_modelCombo->currentData().toString()
        : m_modelCombo->currentText();
    return modelId == QStringLiteral("crisperwhisper-2-large-personal");
#else
    return false;
#endif
}

void AudioSync::selectDefaultWhisperModel()
{
    if (!m_modelCombo) return;
    const QSignalBlocker blocker(m_modelCombo);
    const QString modelId = QString::fromUtf8(whisperModelName(kDefaultWhisperModel));
    const int index = m_modelCombo->findData(modelId);
    if (index >= 0)
        m_modelCombo->setCurrentIndex(index);
    else
        m_modelCombo->setCurrentText(modelId);
}

bool AudioSync::ensureCrisperWhisperConsent()
{
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    if (!isCrisperWhisperSelected()) return true;

    QSettings settings;
    const QString consentKey =
        QStringLiteral("transcription/crisperWhisperPersonalConsent");
    const QString consent = settings.value(consentKey).toString();
    const bool legacyAccepted = settings.value(
        QStringLiteral("transcription/crisperWhisperPersonalAccepted"), false).toBool();
    if (consent == QStringLiteral("accepted") || legacyAccepted) {
        if (consent != QStringLiteral("accepted"))
            settings.setValue(consentKey, QStringLiteral("accepted"));
        return true;
    }

    QMessageBox notice(this);
    notice.setIcon(QMessageBox::Information);
    notice.setWindowTitle(QStringLiteral("Enable CrisperWhisper for personal use?"));
    notice.setText(QStringLiteral("CrisperWhisper 2 model weights and their outputs are "
                                  "licensed for non-commercial use only."));
    notice.setInformativeText(QStringLiteral(
        "Roundtable does not include the model. The first transcription will download it "
        "to your local Hugging Face cache. Enable this option only for personal, "
        "non-commercial use."));
    auto* enable = notice.addButton(QStringLiteral("Enable for Personal Use"),
                                    QMessageBox::AcceptRole);
    notice.addButton(QMessageBox::Cancel);
    notice.exec();

    const bool accepted = notice.clickedButton() == enable;
    settings.setValue(consentKey, accepted
        ? QStringLiteral("accepted") : QStringLiteral("declined"));
    settings.setValue(
        QStringLiteral("transcription/crisperWhisperPersonalAccepted"), accepted);
    settings.sync();
    return accepted;
#else
    return true;
#endif
}

void AudioSync::onCancelTranscriptionClicked()
{
    if (!m_transcriptionRunner->isActive() || m_transcriptionRunner->isCancelling())
        return;

    // Completed files keep their results; no additional files are launched.
    m_transcriptionRunner->cancel();
    m_transcribeStatus->setText(QStringLiteral("Cancelling transcription..."));
    updateTranscriptionControls();
}

void AudioSync::onAutoSyncClicked()
{
    // Auto-Sync snaps boundaries to the speech in the samples: wait for the
    // background decodes rather than run on partial audio.
    loadAudioSamples();   // re-queue anything missing, e.g. a cancelled load
    if (audioSamplesPending()) {
        whenAudioSamplesReady(QStringLiteral("Auto-Sync"), [this]() { onAutoSyncClicked(); });
        return;
    }
    // Snapshot the editable values for this run. Both sync implementations
    // preserve confirmed clips, including their existing boundaries.
    if (m_syncFrontPaddingSpin)
        m_syncFrontPaddingSec = m_syncFrontPaddingSpin->value();
    if (m_syncEndPaddingSpin)
        m_syncEndPaddingSec = m_syncEndPaddingSpin->value();

    // Show a progress dialog so the user knows sync is working
    m_syncProgress = new QProgressDialog("Syncing...", QString(), 0, 100, this);
    m_syncProgress->setWindowTitle("Auto-Sync");
    m_syncProgress->setWindowModality(Qt::WindowModal);
    m_syncProgress->setMinimumDuration(0);
    m_syncProgress->setValue(0);
    m_syncProgress->show();
    QCoreApplication::processEvents();

    runAutoSync();

    if (m_syncProgress) {
        m_syncProgress->setValue(100);
        m_syncProgress->close();
        m_syncProgress->deleteLater();
        m_syncProgress = nullptr;
    }
}

void AudioSync::onExportClicked()
{
    emit exportRequested();
}

void AudioSync::onReExportClicked()
{
    emit reExportRequested();
}

// ─── Effective ranges (ported from Python _get_effective_ranges) ─────────────


void AudioSync::onTranscriptionProgress(float percent, const QString& status)
{
    float clampedPercent = std::clamp(percent, 0.0f, 100.0f);
    size_t runTotal = std::max<size_t>(m_transcriptionRunner->total(), 1);
    double overall = (static_cast<double>(m_transcriptionRunner->completed())
                    + static_cast<double>(clampedPercent) / 100.0)
                    / static_cast<double>(runTotal) * 100.0;
    m_progressBar->setValue(static_cast<int>(std::clamp(overall, 0.0, 99.0)));

    size_t fileNumber = std::min(m_transcriptionRunner->completed() + 1, runTotal);
    m_transcribeStatus->setText(QString("File %1/%2: %3")
        .arg(fileNumber)
        .arg(runTotal)
        .arg(status));
    emit transcriptionProgress(percent, status);
}

void AudioSync::onTranscriptionFileStarted(const std::string& path, size_t number, size_t total)
{
    if (ScriptSession* target = transcriptionSession())
        target->audioPath = path;
    m_transcribeStatus->setText(QString("Transcribing file %1/%2...").arg(number).arg(total));
}

void AudioSync::onTranscriptionFileFinished(const std::string& path,
                                            const TranscriptionResult& result)
{
    ScriptSession* target = transcriptionSession();
    if (!target)
        return;   // its script was closed or replaced mid-run
    const auto& paths = target->audioPaths;
    const auto it = std::find(paths.begin(), paths.end(), path);
    if (it == paths.end())
        return;
    if (target->allTranscriptionResults.size() < paths.size())
        target->allTranscriptionResults.resize(paths.size());
    target->allTranscriptionResults[static_cast<size_t>(it - paths.begin())] = result;
    if (target == &session())
        refreshTranscribeFileList();
}

void AudioSync::onTranscriptionFileFailed(const QString& error)
{
    m_transcribeStatus->setText(QStringLiteral("Transcription error: %1").arg(error));
}

void AudioSync::finishTranscriptionRun(bool cancelled)
{
    const size_t completed = m_transcriptionRunner->completed();
    const size_t total = m_transcriptionRunner->total();
    const bool hadSuccess = m_transcriptionRunner->hadSuccess();
    const QString error = m_transcriptionRunner->error();
    ScriptSession* target = transcriptionSession();
    m_transcriptionSessionKey.reset();

    m_progressBar->setVisible(false);
    updateTranscriptionControls();

    if (hadSuccess && target)
        applyFinishedTranscriptions(*target);

    const ScriptSession& shown = target ? *target : session();
    if (cancelled) {
        m_transcribeStatus->setText(QString("Transcription cancelled (%1 of %2 file(s) completed)")
            .arg(completed).arg(total));
        m_transcribeStatus->setStyleSheet(QString("color: %1;")
            .arg(Theme::hex(Theme::colors().warning)));
    } else if (!error.isEmpty()) {
        m_transcribeStatus->setText(hadSuccess
            ? QString("Transcription completed with errors (%1/%2): %3")
                  .arg(completed).arg(total).arg(error)
            : QStringLiteral("Error: %1").arg(error));
        m_transcribeStatus->setStyleSheet(QString("color: %1;")
            .arg(Theme::hex(Theme::colors().warning)));
        emit transcriptionFailed(error);
    } else if (shown.clips.empty()) {
        m_transcribeStatus->setText(QStringLiteral("Transcription returned no segments"));
        m_transcribeStatus->setStyleSheet(QString("color: %1;")
            .arg(Theme::hex(Theme::colors().warning)));
    } else {
        m_progressBar->setValue(100);
        m_transcribeStatus->setText(
            QString("Transcribed %1 segments from %2 file(s)")
                .arg(shown.clips.size()).arg(shown.audioPaths.size()));
        m_transcribeStatus->setStyleSheet(QString("color: %1;")
            .arg(Theme::hex(Theme::colors().success)));
    }
}

ScriptSession* AudioSync::transcriptionSession()
{
    if (!m_transcriptionSessionKey)
        return nullptr;
    if (*m_transcriptionSessionKey == m_activeScriptKey)
        return &session();
    auto it = m_scriptSessions.find(*m_transcriptionSessionKey);
    return it != m_scriptSessions.end() ? &it->second : nullptr;
}

void AudioSync::detachTranscriptionRun()
{
    m_transcriptionSessionKey.reset();
    onCancelTranscriptionClicked();
}

void AudioSync::applyFinishedTranscriptions(ScriptSession& target)
{
    audiosync::appendClipsFromTranscriptions(syncSourceOf(target), target.clips);
    if (target.script && !target.script->lines.empty())
        audiosync::mergeSegmentsToMatchScript(syncSourceOf(target), target.clips);

    if (target.clips.empty())
        return;
    target.transcriptionDone = true;
    if (&target != &session()) {
        // A script that is not open: its views are built when it is opened.
        populateScriptSessionList();
        return;
    }
    populateClipList();
    updateWorkflowState();
    emit transcriptionFinished(static_cast<int>(target.clips.size()));
}

void AudioSync::setTranscriptionsForTesting(std::vector<TranscriptionResult> results)
{
    session().allTranscriptionResults = std::move(results);
    applyFinishedTranscriptions(session());
}

void AudioSync::setTranscribeFunctionForTesting(TranscriptionRunner::TranscribeFn transcribe)
{
    m_transcribeForTesting = std::move(transcribe);
}

void AudioSync::updateTranscriptionControls()
{
    const bool active = m_transcriptionRunner->isActive();
    const bool cancelling = m_transcriptionRunner->isCancelling();

    if (m_transcribeBtn)
        m_transcribeBtn->setEnabled(!active);
    if (m_cancelTranscriptionBtn) {
        m_cancelTranscriptionBtn->setVisible(active);
        m_cancelTranscriptionBtn->setEnabled(active && !cancelling);
        m_cancelTranscriptionBtn->setText(cancelling
            ? QStringLiteral("Cancelling...")
            : QStringLiteral("Cancel"));
    }
    if (m_modelCombo)
        m_modelCombo->setEnabled(!active);
    if (m_importAudioBtn)
        m_importAudioBtn->setEnabled(!active);
    if (m_removeAudioBtn)
        m_removeAudioBtn->setEnabled(!active);
    if (m_audioSortCombo)
        m_audioSortCombo->setEnabled(!active);
    if (m_audioFileList)
        m_audioFileList->setEnabled(!active);
    if (m_transcribeFileList)
        m_transcribeFileList->setEnabled(!active);
    if (m_clearSelectedTranscriptionBtn)
        m_clearSelectedTranscriptionBtn->setEnabled(!active);
    if (m_clearAllTranscriptionsBtn)
        m_clearAllTranscriptionsBtn->setEnabled(!active);
}

// ─── Clear transcription ──────────────────────────────────────────────────────

void AudioSync::clearTranscriptionForFile(size_t fileIndex)
{
    if (fileIndex >= session().allTranscriptionResults.size()) return;

    session().allTranscriptionResults[fileIndex] = TranscriptionResult{};

    // Remove clips that came from this file
    const std::string& path = session().audioPaths[fileIndex];
    auto it = std::remove_if(session().clips.begin(), session().clips.end(),
        [&path](const SyncClip& clip) {
            return clip.sourceFile == path;
        });
    session().clips.erase(it, session().clips.end());

    refreshTranscribeFileList();
    populateClipList();
    populateLeftList();
    updateWorkflowState();
    m_transcribeStatus->setText(
        QString("Cleared transcription for file %1").arg(fileIndex + 1));
}

void AudioSync::clearAllTranscriptions()
{
    if (session().allTranscriptionResults.empty()) return;

    for (auto& r : session().allTranscriptionResults)
        r = TranscriptionResult{};

    session().clips.clear();

    refreshTranscribeFileList();
    populateClipList();
    populateLeftList();
    updateWorkflowState();
    m_transcribeStatus->setText("All transcriptions cleared");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Standalone gap closing
// ═════════════════════════════════════════════════════════════════════════════

void AudioSync::closeInterClipGaps()
{
    if (session().clips.empty()) return;
    const auto result = audiosync::closeInterClipGaps(syncSource(), session().clips);
    if (result.tooFewToClose) {
        if (m_transcribeStatus)
            m_transcribeStatus->setText(
                result.trimmed > 0
                    ? QString("Trimmed %1 clips").arg(result.trimmed)
                    : QString("No changes needed"));
        if (result.trimmed > 0) populateCards();
        return;
    }
    if (result.gapsClosed > 0 || result.trimmed > 0)
        populateCards();
    if (m_transcribeStatus)
        m_transcribeStatus->setText(
            QString("Trimmed %1 clips, closed %2 gaps").arg(result.trimmed).arg(result.gapsClosed));
}

} // namespace rt
