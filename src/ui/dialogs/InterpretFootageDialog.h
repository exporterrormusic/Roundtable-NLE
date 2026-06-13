/*
 * InterpretFootageDialog — Override FPS, alpha, and pixel aspect ratio
 * for imported media assets (Premiere Pro "Interpret Footage" equivalent).
 */

#pragma once

#include "project/AssetDatabase.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;

namespace rt {

class InterpretFootageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit InterpretFootageDialog(const FootageInterpretation& current,
                                    double nativeFps,
                                    bool isVFR = false,
                                    QWidget* parent = nullptr);

    [[nodiscard]] FootageInterpretation result() const;

private:
    QDoubleSpinBox* m_fpsSpin{nullptr};
    QComboBox*      m_alphaCombo{nullptr};
    QComboBox*      m_parCombo{nullptr};
    double          m_nativeFps{0.0};
};

} // namespace rt
