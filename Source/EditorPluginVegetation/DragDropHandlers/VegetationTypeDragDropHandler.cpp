#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/DragDrop/DragDropInfo.h>
#include <EditorPluginVegetation/DragDropHandlers/VegetationTypeDragDropHandler.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeComponentDragDropHandler, 1, plRTTIDefaultAllocator<plVegetationTypeComponentDragDropHandler>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

float plVegetationTypeComponentDragDropHandler::CanHandle(const plDragDropInfo* pInfo) const
{
  if (plComponentDragDropHandler::CanHandle(pInfo) == 0.0f)
    return 0.0f;

  return IsSpecificAssetType(pInfo, "Vegetation Type") ? 1.0f : 0.0f;
}

void plVegetationTypeComponentDragDropHandler::OnDragBegin(const plDragDropInfo* pInfo)
{
  plComponentDragDropHandler::OnDragBegin(pInfo);

  constexpr const char* szComponentType = "plVegetationInstanceComponent";
  constexpr const char* szPropertyName = "Type";

  if (pInfo->m_sTargetContext == "viewport")
  {
    CreateDropObject(pInfo->m_vDropPosition, szComponentType, szPropertyName, GetAssetGuidString(pInfo), plUuid(), -1);
  }
  else
  {
    if (!pInfo->m_bCtrlKeyDown && pInfo->m_iTargetObjectInsertChildIndex == -1) // dropped directly on a node
    {
      AttachComponentToObject(szComponentType, szPropertyName, GetAssetGuidString(pInfo), pInfo->m_TargetObject);
      m_DraggedObjects.PushBack(pInfo->m_TargetObject);
    }
    else
    {
      CreateDropObject(pInfo->m_vDropPosition, szComponentType, szPropertyName, GetAssetGuidString(pInfo), pInfo->m_TargetObject, pInfo->m_iTargetObjectInsertChildIndex);
    }
  }

  SelectCreatedObjects();
  BeginTemporaryCommands();
}
