#pragma once

#include <Core/ResourceManager/Resource.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Strings/String.h>
#include <RendererCore/Material/MaterialResource.h>
#include <VegetationPlugin/VegetationDeclarations.h>

/// \brief A normalized texture rectangle and its strand size multiplier.
struct PL_VEGETATIONPLUGIN_DLL plGrassAtlasRect
{
  plVec4 m_vTexMulAdd = plVec4(1, 1, 0, 0);
  float m_fSize = 1.0f;

  bool operator==(const plGrassAtlasRect& other) const { return m_vTexMulAdd == other.m_vTexMulAdd && m_fSize == other.m_fSize; }
};
PL_DECLARE_REFLECTABLE_TYPE(PL_VEGETATIONPLUGIN_DLL, plGrassAtlasRect);

/// \brief Every look, simulation and distance setting of one grass type.
struct PL_VEGETATIONPLUGIN_DLL plGrassSettings
{
  // Strands
  float m_fStrandsPerSquareMeter = 120.0f;
  plUInt32 m_uiSegmentCount = 1;   ///< 1..10
  plUInt32 m_uiBillboardCount = 1; ///< 1..10
  plUInt32 m_uiRandomSeed = 1;
  float m_fLength = 1.0f;
  float m_fWidth = 1.0f;
  float m_fRandomness = 0.2f;
  float m_fUniformity = 1.0f; ///< Spatial frequency of the atlas selection noise.
  plHybridArray<plGrassAtlasRect, 4> m_AtlasRects;
  plString m_sMaterial; ///< Material resource id; empty uses the default grass material.

  // Procedural blades are opt-in so existing textured cards keep their appearance.
  bool m_bProceduralBlades = false;
  float m_fBladeWidth = 0.06f; ///< Width relative to length, multiplied by Width.
  float m_fBladeTilt = 0.15f;
  float m_fBladeBend = 0.35f;
  float m_fBladeSideCurve = 0.08f;
  float m_fClumpSize = 1.5f; ///< Source-space metres.
  float m_fClumpStrength = 0.65f;
  float m_fClumpColorVariation = 0.15f;
  float m_fBladeNormalRoundness = 0.65f;
  bool m_bAdaptiveDensity = true; ///< Procedural blades: thin stored painted roots with distance.
  float m_fFarDensity = 0.01f;    ///< Minimum distance density, before the final fade.
  bool m_bFarGroundCover = true;
  float m_fGroundCoverStartDistance = 60.0f;
  float m_fGroundCoverMaxSize = 2.0f; ///< Maximum world-space footprint diameter on the sampled tangent plane.

  // Simulation
  float m_fStiffness = 0.5f;
  float m_fDrag = 0.1f;
  float m_fGravityPower = 0.0f;
  float m_fWindInfluence = 1.0f;
  bool m_bCameraBend = true;

  // Interaction. The GPU simulation bends strands only with the world's touch map; it does not query physics.
  float m_fTouchCrush = 0.6f;    ///< 0..1, how far touched strands are pressed down.
  float m_fTouchRecovery = 1.5f; ///< Per second, how fast touched strands recover.

  // Distances
  float m_fViewDistance = 200.0f;
  float m_fSimulationDistance = 40.0f;
  float m_fLodStartDistance = 15.0f;
  float m_fShadowDistance = 25.0f;
  bool m_bCastShadows = true; ///< Off excludes this type from every shadow view; Vegetation.Grass.Shadows must also allow it.
  float m_fPatchSize = 8.0f;

  /// \brief Clamps every value into its valid range and replaces non-finite values with defaults.
  void Sanitize();

  /// \brief True when switching a grown source from these settings to \a other changes its strands: the values the
  /// strand count, the generated static data, the bounds margin or the state layout depend on. Every other value is
  /// read from the source table each frame and takes effect through plGrassGpuManager::UpdateSourceSettings.
  bool RequiresRegrow(const plGrassSettings& other) const;

  /// \brief Writes the current (plGrassTypeResourceDescriptor version 3) layout.
  void Save(plStreamWriter& inout_stream) const;

  /// \brief Reads descriptor versions 1 through 3.
  plResult Load(plStreamReader& inout_stream, plUInt8 uiVersion);
};

/// \brief Everything the editor writes for one grass type; the runtime resource holds it verbatim.
struct PL_VEGETATIONPLUGIN_DLL plGrassTypeResourceDescriptor
{
  plGrassSettings m_Settings;

  void Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);
};

/// \brief One grass type: strand look, simulation, interaction and distance settings plus the material.
class PL_VEGETATIONPLUGIN_DLL plGrassTypeResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassTypeResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plGrassTypeResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plGrassTypeResource, plGrassTypeResourceDescriptor);

public:
  plGrassTypeResource();

  const plGrassTypeResourceDescriptor& GetDescriptor() const { return m_Descriptor; }
  const plGrassSettings& GetSettings() const { return m_Descriptor.m_Settings; }

  /// \brief The type's material, or an invalid handle when the default grass material applies.
  const plMaterialResourceHandle& GetMaterial() const { return m_hMaterial; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  void ResolveHandles();

  plGrassTypeResourceDescriptor m_Descriptor;
  plMaterialResourceHandle m_hMaterial;
};
