#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/String.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

/// The reflected content of a Grass Type document. Mirrors plGrassSettings one to one.
class plGrassTypeAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassTypeAssetProperties, plReflectedClass);

public:
  plGrassTypeAssetProperties();
  ~plGrassTypeAssetProperties();

  /// \brief Copies the document properties into the runtime descriptor written at transform time.
  void FillDescriptor(plGrassTypeResourceDescriptor& out_desc) const;

  // Strands
  float m_fStrandsPerSquareMeter = 120.0f;
  plUInt32 m_uiSegmentCount = 1;
  plUInt32 m_uiBillboardCount = 1;
  plUInt32 m_uiRandomSeed = 1;
  float m_fLength = 1.0f;
  float m_fWidth = 1.0f;
  float m_fRandomness = 0.2f;
  float m_fUniformity = 1.0f;
  plHybridArray<plGrassAtlasRect, 4> m_AtlasRects;
  plString m_sMaterial;

  bool m_bProceduralBlades = false;
  float m_fBladeWidth = 0.06f;
  float m_fBladeTilt = 0.15f;
  float m_fBladeBend = 0.35f;
  float m_fBladeSideCurve = 0.08f;
  float m_fClumpSize = 1.5f;
  float m_fClumpStrength = 0.65f;
  float m_fClumpColorVariation = 0.15f;
  float m_fBladeNormalRoundness = 0.65f;
  bool m_bAdaptiveDensity = true;
  float m_fFarDensity = 0.01f;
  bool m_bFarGroundCover = true;
  float m_fGroundCoverStartDistance = 60.0f;
  float m_fGroundCoverMaxSize = 2.0f;

  // Simulation
  float m_fStiffness = 0.5f;
  float m_fDrag = 0.1f;
  float m_fGravityPower = 0.0f;
  float m_fWindInfluence = 1.0f;
  bool m_bCameraBend = true;

  // Interaction
  float m_fTouchCrush = 0.6f;
  float m_fTouchRecovery = 1.5f;

  // Distances
  float m_fViewDistance = 200.0f;
  float m_fSimulationDistance = 40.0f;
  float m_fLodStartDistance = 15.0f;
  float m_fShadowDistance = 25.0f;
  bool m_bCastShadows = true;
  float m_fPatchSize = 8.0f;
};
