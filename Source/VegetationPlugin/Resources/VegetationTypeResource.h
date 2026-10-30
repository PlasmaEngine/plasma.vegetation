#pragma once

#include <Core/Curves/ColorGradientResource.h>
#include <Core/ResourceManager/Resource.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Strings/String.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <VegetationPlugin/VegetationDeclarations.h>

/// \brief Wind response of a type, mirrored into plVegetationWindProfile in the shader.
struct PL_VEGETATIONPLUGIN_DLL plVegetationWindSettings
{
  float m_fBendStiffness = 0.35f;
  float m_fBendHeight = 2.0f;
  float m_fBranchAmplitude = 0.5f;
  float m_fBranchFrequency = 1.2f;
  float m_fEdgeAmplitude = 0.5f;
  float m_fEdgeFrequency = 6.0f;
  float m_fPhaseScale = 0.8f;
  float m_fTouchStrength = 0.8f;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief How the type reacts to characters and what blocks them.
struct PL_VEGETATIONPLUGIN_DLL plVegetationTouchSettings
{
  float m_fRecovery = 1.5f;        ///< Per second, how fast the interaction map recovers under this type.
  float m_fCrush = 0.3f;           ///< 0..1, how far touched vertices are pressed down.
  plString m_sCollisionPrefab;     ///< Prefab instantiated for instances near a touch source; empty = no collision.
  float m_fCollisionRadius = 15.0f;
  plString m_sTouchTag;            ///< Tag reported in plMsgVegetationTouched.

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief Default placement rules; a placement source may tighten them.
struct PL_VEGETATIONPLUGIN_DLL plVegetationPlacementSettings
{
  float m_fMinScale = 0.8f;
  float m_fMaxScale = 1.25f;
  float m_fAlignToNormal = 0.6f;
  plAngle m_MaxSlope = plAngle::MakeFromDegree(45.0f);
  float m_fSink = 0.05f;
  float m_fMinSpacing = 0.4f;
  plString m_sColorGradient; ///< Optional plColorGradientResource sampled by the instance colour seed.
  float m_fHueVariation = 0.05f;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief Distances and flags for the renderer.
struct PL_VEGETATIONPLUGIN_DLL plVegetationRenderSettings
{
  float m_fCullDistance = 120.0f;
  float m_fShadowCullDistance = 40.0f;
  float m_fThinningStart = 0.6f;  ///< Fraction of the cull distance where stochastic thinning begins.
  float m_fLodCrossfade = 0.1f;   ///< Fraction of an LOD's screen-size range used for the dithered crossfade.
  bool m_bCastShadows = true;
  bool m_bAffectsGI = false;
  bool m_bUseImpostor = false;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief Everything the editor writes for one vegetation type; the runtime resource holds it verbatim.
struct PL_VEGETATIONPLUGIN_DLL plVegetationTypeResourceDescriptor
{
  struct Lod
  {
    plString m_sMesh;           ///< Mesh resource id (asset guid).
    float m_fScreenSize = 1.0f; ///< Screen-height fraction at which this level turns on; descending per LOD.
  };

  plHybridArray<Lod, plVegetationMaxLods> m_Lods;
  plHybridArray<plString, 4> m_Materials; ///< Per material slot override; empty string keeps the mesh material.
  plVegetationWindSettings m_Wind;
  plVegetationTouchSettings m_Touch;
  plVegetationPlacementSettings m_Placement;
  plVegetationRenderSettings m_Render;

  void Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);
};

/// \brief One plant type: LOD meshes, material overrides, wind, touch, placement and render settings.
class PL_VEGETATIONPLUGIN_DLL plVegetationTypeResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plVegetationTypeResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plVegetationTypeResource, plVegetationTypeResourceDescriptor);

public:
  plVegetationTypeResource();

  const plVegetationTypeResourceDescriptor& GetDescriptor() const { return m_Descriptor; }

  plUInt32 GetLodCount() const { return m_LodMeshes.GetCount(); }
  const plMeshResourceHandle& GetLodMesh(plUInt32 uiLod) const { return m_LodMeshes[uiLod]; }
  float GetLodScreenSize(plUInt32 uiLod) const { return m_Descriptor.m_Lods[uiLod].m_fScreenSize; }

  /// \brief Material override for a slot, or an invalid handle to use the mesh material.
  plMaterialResourceHandle GetMaterialOverride(plUInt32 uiSlot) const;

  const plColorGradientResourceHandle& GetColorGradient() const { return m_hColorGradient; }

  /// \brief Radius of LOD0's bounding sphere at scale 1; 1 until the mesh has loaded.
  float GetBoundingRadius() const;

  /// \brief Picks the LOD for a projected screen-height fraction, highest detail first.
  plUInt32 SelectLod(float fScreenSize) const;

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  void ResolveHandles();

  plVegetationTypeResourceDescriptor m_Descriptor;
  plHybridArray<plMeshResourceHandle, plVegetationMaxLods> m_LodMeshes;
  plHybridArray<plMaterialResourceHandle, 4> m_MaterialOverrides;
  plColorGradientResourceHandle m_hColorGradient;
  mutable float m_fBoundingRadius = 0.0f;
};
