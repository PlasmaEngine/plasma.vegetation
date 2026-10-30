#pragma once

#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>

using plGrassRenderComponentManager = plComponentManager<class plGrassRenderComponent, plBlockStorageType::FreeList>;

/// \brief Internal render owner of one grass source. Created by plGrassSurfaceComponent and plVegetationWorldModule on
/// a generated child object, never placed by hand and never serialized. Its bounds drive spatial culling; for every
/// view within reach it extracts one plGrassRenderData that plGrassRenderer draws from the GPU strand pool.
class PL_VEGETATIONPLUGIN_DLL plGrassRenderComponent : public plRenderComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGrassRenderComponent, plRenderComponent, plGrassRenderComponentManager);

public:
  plGrassRenderComponent();
  ~plGrassRenderComponent();

  /// \brief Everything extraction needs. Call from the world update only. Without \a bCastShadows shadow views skip it.
  void SetSource(plGrassSourceHandle hSource, const plMaterialResourceHandle& hMaterial, plUInt32 uiSelectionId, float fViewDistance, float fShadowDistance, bool bCastShadows);

  /// \brief Owner-space bounds of every strand pose; triggers a bounds update when they change.
  void SetLocalBounds(const plBoundingBox& bounds);

  plGrassSourceHandle GetSource() const { return m_hSource; }

protected:
  virtual void OnDeactivated() override;
  virtual plResult GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg) override;
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

private:
  plGrassSourceHandle m_hSource;
  plUInt32 m_uiSourceSlot = plInvalidIndex;
  plMaterialResourceHandle m_hMaterial;
  plUInt32 m_uiSelectionId = 0;
  float m_fViewDistance = 200.0f;
  float m_fShadowDistance = 25.0f;
  bool m_bCastShadows = true;
  plBoundingBox m_LocalBounds = plBoundingBox::MakeInvalid();
  mutable plInstanceDataOffset m_InstanceDataOffset;
};
