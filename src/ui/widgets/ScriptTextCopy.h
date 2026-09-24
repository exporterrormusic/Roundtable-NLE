#pragma once

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLabel>

namespace rt {

/// Make displayed script dialogue behave like ordinary selectable text while
/// also offering a one-click way to copy the complete line for TTS prompts.
inline void enableScriptTextCopy(QLabel* label, const QString& completeLine)
{
    if (!label) return;

    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setFocusPolicy(Qt::ClickFocus);
    label->setCursor(Qt::IBeamCursor);
    label->setProperty("scriptTextCopyEnabled", true);

    auto* copyLine = new QAction(
        QCoreApplication::translate("ScriptTextCopy", "Copy line text"), label);
    copyLine->setObjectName(QStringLiteral("copyScriptLineTextAction"));
    QObject::connect(copyLine, &QAction::triggered, label, [completeLine]() {
        if (auto* clipboard = QGuiApplication::clipboard())
            clipboard->setText(completeLine);
    });
    label->addAction(copyLine);
    label->setContextMenuPolicy(Qt::ActionsContextMenu);
}

} // namespace rt
