/*
 * Settings.h — Centralized QSettings factory.
 *
 * Dev builds (ROUNDTABLE_DEV_BUILD) use ("ROUNDTABLE", "NLE-Dev") so they
 * don't contaminate the installed version's settings (recent files, workspace
 * layout, last project path, etc.).  Release builds use ("ROUNDTABLE", "NLE").
 */

#pragma once

#include <QSettings>

namespace rt {

/// Returns a QSettings instance scoped to the correct application key.
/// Dev builds use "NLE-Dev"; installed releases use "NLE".
inline QSettings appSettings()
{
#ifdef ROUNDTABLE_DEV_BUILD
    constexpr auto application = "NLE-Dev";
#else
    constexpr auto application = "NLE";
#endif
    // UI tests select a temporary INI store. The two-name constructor
    // always uses native settings, including the user's Windows registry.
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, "ROUNDTABLE", application);
}

} // namespace rt
