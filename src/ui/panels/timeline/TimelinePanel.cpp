/*
 * TimelinePanel.cpp � Main timeline container � coordinator TU.
 * Step 12: Timeline Panel � Core UI
 *
 * All member function definitions have been moved to the extracted
 * sub-files below. This file retains the includes for build system
 * compatibility.
 *
 * Extracted files:
 *   TimelinePanelLayout.cpp      � setupLayout, ctor/dtor, resizeEvent
 *   TimelinePanelTracks.cpp      � rebuildTracks, ensureDefaultTracks, refreshTrackContents
 *   TimelinePanelZoomScroll.cpp  � zoom, wheel, scroll, playhead, ruler
 *   TimelinePanelPaintEvents.cpp � paintEvent, eventFilter, keyPressEvent
 *   TimelinePanelHitTest.cpp     � hitTestClip/Track/Edge, reorder helpers
 *   TimelinePanelCommands.cpp    � setCommandStack, setActiveTool, executeCommand
 *   TimelinePanelMenus.cpp       � context menus (previously extracted)
 *   TimelinePanelMedia.cpp       � waveform/thumbnail loading (previously extracted)
 *   TimelinePanelMouse.cpp       � mousePressEvent (previously extracted)
 *   TimelinePanelMouseDrag.cpp   � mouseMove/Release/DoubleClick (previously extracted)
 *   TimelinePanelDragDrop.cpp    � drag-and-drop (previously extracted) â€” Core UI
 */

#include "panels/timeline/TimelinePanel.h"
#include "Theme.h"
#include "widgets/TimelineRuler.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/NLEScrollBar.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/EditOperations.h"
#include "timeline/GraphicClip.h"
#include "command/CommandStack.h"
#include "command/commands/TrackCommands.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"
#include "ShortcutManager.h"
#include "audio/AudioFile.h"
#include "decode/VideoDecoder.h"

#include <QPixmap>
#include <QFontMetrics>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QCursor>
#include <QInputDialog>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QSplitter>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPointer>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QAction>
#include <QColorDialog>
#include <QPushButton>
#include <QToolTip>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTreeWidget>
#include <QTimer>

#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "timeline/Transition.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <limits>

#include <algorithm>
#include <cmath>
#include <thread>

#include "panels/timeline/TimelinePanelInternal.h"
#include "panels/timeline/PlayheadLineWidget.h"

namespace rt {
// All member function definitions have been moved to the extracted sub-files.

} // namespace rt
