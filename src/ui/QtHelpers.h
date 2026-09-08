#pragma once
#include <QString>

namespace rt {

class ModelManager;

/// Custom Qt message handler that suppresses known harmless warnings.
void installQtMessageFilter();

/// Locate the project root directory by walking up from the executable.
QString findProjectRoot();

/// Resolve a writable user data directory (e.g. %LOCALAPPDATA%/ROUNDTABLE/).
/// Created on first call if it doesn't exist. Falls back to project root.
QString userDataDir();

/// Absolute path to application assets. Character content found here is only
/// supported for compatibility with downloads made by older releases.
QString bundledAssetsDir();

/// Asset root for writable, per-user character downloads.
/// Character files live below the returned path in characters/<name>/.
QString downloadedCharacterAssetsDir();

/// Find an existing character directory, preferring the writable user copy.
QString findCharacterDirectory(const QString& characterName);

/// Rebuild a catalog from legacy install-local and per-user character assets.
void rescanCharacterModels(ModelManager* modelManager);

} // namespace rt
