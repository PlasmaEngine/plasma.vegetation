#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessViewContext.h>

class plGrassTypeContext;

class plGrassTypeViewContext : public plEngineProcessViewContext
{
public:
  plGrassTypeViewContext(plGrassTypeContext* pContext);
  ~plGrassTypeViewContext();

  bool UpdateThumbnailCamera(const plBoundingBoxSphere& bounds);

protected:
  virtual plViewHandle CreateView() override;

  plGrassTypeContext* m_pGrassContext;
};
