#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Graphics/Camera.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <VegetationPlugin/Grass/GrassRenderComponent.h>
#include <VegetationPlugin/Grass/GrassRenderer.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGrassRenderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plHiddenAttribute(),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plGrassRenderComponent::plGrassRenderComponent() = default;
plGrassRenderComponent::~plGrassRenderComponent() = default;

void plGrassRenderComponent::SetSource(plGrassSourceHandle hSource, const plMaterialResourceHandle& hMaterial, plUInt32 uiSelectionId, float fViewDistance, float fShadowDistance, bool bCastShadows)
{
  m_hSource = hSource;
  m_uiSourceSlot = hSource.IsInvalidated() ? plInvalidIndex : plGrassGpuManager::GetSourceSlot(GetWorld()->GetIndex(), hSource);
  m_hMaterial = hMaterial;
  m_uiSelectionId = uiSelectionId;
  m_fViewDistance = fViewDistance;
  m_fShadowDistance = fShadowDistance;
  m_bCastShadows = bCastShadows;
}

void plGrassRenderComponent::SetLocalBounds(const plBoundingBox& bounds)
{
  if (bounds.IsValid() == m_LocalBounds.IsValid() && (!bounds.IsValid() || bounds.IsEqual(m_LocalBounds, 0.0001f)))
    return;

  m_LocalBounds = bounds;
  if (IsActiveAndInitialized())
    TriggerLocalBoundsUpdate();
}

void plGrassRenderComponent::OnDeactivated()
{
  if (!m_InstanceDataOffset.IsInvalidated())
  {
    if (const plRenderDataManager* pManager = GetWorld()->GetModuleReadOnly<plRenderDataManager>())
      pManager->DeleteInstanceData(m_InstanceDataOffset);
    m_InstanceDataOffset = plInstanceDataOffset();
  }

  SUPER::OnDeactivated();
}

plResult plGrassRenderComponent::GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg)
{
  PL_IGNORE_UNUSED(ref_bAlwaysVisible);
  PL_IGNORE_UNUSED(ref_msg);

  if (!m_LocalBounds.IsValid())
    return PL_FAILURE;

  ref_bounds = plBoundingBoxSphere::MakeFromBox(m_LocalBounds);
  return PL_SUCCESS;
}

void plGrassRenderComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (!plGrassGpuManager::IsEnabled() || m_uiSourceSlot == plInvalidIndex || !m_hMaterial.IsValid())
    return;

  // Distances use the LOD camera, which is the main camera for shadow views too.
  const plVec3 vCameraPosition = msg.m_pView->GetLodCamera()->GetCenterPosition();
  const plBoundingBox box = GetOwner()->GetGlobalBounds().GetBox();
  const float fDistance = box.GetDistanceTo(vCameraPosition);
  if (fDistance > m_fViewDistance)
    return;

  if (msg.m_pView->GetCameraUsageHint() == plCameraUsageHint::Shadow)
  {
    if (!m_bCastShadows || !plGrassGpuManager::AreShadowsEnabled() || fDistance > m_fShadowDistance * plGrassGpuManager::GetShadowDistanceScale())
      return;
  }

  const plGALDynamicBufferHandle hInstanceBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, GetOwner()->IsDynamic(), GetOwner()->GetGlobalTransform(), m_InstanceDataOffset, m_uiSelectionId);

  plGrassRenderData* pData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plGrassRenderData>(GetOwner());
  pData->m_hMaterial = m_hMaterial;
  pData->m_hInstanceDataBuffer = hInstanceBuffer;
  pData->m_uiInstanceDataOffset = m_InstanceDataOffset.m_uiOffset;
  pData->m_uiMaterialDataOffset = plPackMaterialDataOffset(m_hMaterial, 0);
  pData->m_uiWorldIndex = GetWorld()->GetIndex();
  pData->m_uiSourceSlot = m_uiSourceSlot;
  pData->m_vGlobalPosition = box.GetCenter();
  pData->m_vLastGlobalPosition = pData->m_vGlobalPosition;
  pData->m_uiSortingKey = (plHashingUtils::StringHashTo32(m_hMaterial.GetResourceIDHash()) << 16) | (m_uiSourceSlot & 0xFFFF);

  const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(m_hMaterial);

  // Distance and shadow-distance tests are view-dependent, so the data is never cached.
  msg.AddRenderData(pData, category, plRenderData::Caching::Never);
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassRenderComponent);
