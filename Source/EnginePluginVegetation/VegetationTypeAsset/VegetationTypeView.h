#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessViewContext.h>

class plVegetationTypeContext;

class plVegetationTypeViewContext : public plEngineProcessViewContext
{
public:
  plVegetationTypeViewContext(plVegetationTypeContext* pContext);
  ~plVegetationTypeViewContext();

  bool UpdateThumbnailCamera(const plBoundingBoxSphere& bounds);

protected:
  virtual plViewHandle CreateView() override;

  plVegetationTypeContext* m_pVegetationContext;
};
