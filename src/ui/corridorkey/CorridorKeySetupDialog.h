/*
 * CorridorKeySetupDialog — explains what CorridorKey downloads (and from
 * whom, how big, where it goes, what GPU it needs), asks once, then shows the
 * setup's progress. Modeless: the setup keeps running when it is hidden.
 */

#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace rt {

class CorridorKeyService;

class CorridorKeySetupDialog final : public QDialog
{
    Q_OBJECT

public:
    CorridorKeySetupDialog(CorridorKeyService* service, QWidget* parent);

private:
    void sync();

    CorridorKeyService* m_service;
    QString        m_gpuProblem;
    QLabel*        m_gpuLabel{nullptr};
    QLabel*        m_stageLabel{nullptr};
    QProgressBar*  m_progress{nullptr};
    QPushButton*   m_installButton{nullptr};
    QPushButton*   m_cancelButton{nullptr};
    QPushButton*   m_closeButton{nullptr};
};

} // namespace rt
