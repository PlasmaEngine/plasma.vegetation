#pragma once

#include <EditorFramework/DragDrop/ComponentDragDropHandler.h>
#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>

/// Dropping a Vegetation Type asset into a scene creates an object with a plVegetationInstanceComponent.
class plVegetationTypeComponentDragDropHandler : public plComponentDragDropHandler
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeComponentDragDropHandler, plComponentDragDropHandler);

public:
  virtual float CanHandle(const plDragDropInfo* pInfo) const override;
  virtual void OnDragBegin(const plDragDropInfo* pInfo) override;
};
