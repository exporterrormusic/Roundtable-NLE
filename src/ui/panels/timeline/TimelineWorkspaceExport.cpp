#include "panels/timeline/TimelineWorkspace.h"

#include "panels/export/ExportRenderSession.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

namespace rt {

std::shared_ptr<ExportRenderSession>
TimelineWorkspace::createExportRenderSession()
{
    ExportRenderSession::Dependencies dependencies;
    dependencies.mediaPool = m_mediaPool;
    dependencies.mediaSourceService = m_mediaSourceService;
    dependencies.modelManager = m_modelManager;
    dependencies.shotPresetManager = m_shotPresetManager;
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_modelManager)
        dependencies.assetsDir = m_modelManager->assetsDir();
#endif
    return std::make_shared<ExportRenderSession>(std::move(dependencies));
}

} // namespace rt
