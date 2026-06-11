/*
 * ProjectBin.cpp - Premiere Pro-style media browser panel.
 * Step 16
 */

#include "QtHelpers.h"
#include "panels/project/ProjectBin.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"
#include "dialogs/SequenceDialog.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QColorDialog>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainterPath>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTreeWidgetItem>
#include <QImage>
#include <QRegularExpression>

#include <map>
#include <set>

#include "panels/project/ProjectBinInternal.h"

namespace rt {
// All member function definitions have been moved to the extracted files above.

} // namespace rt