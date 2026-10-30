#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/String.h>

struct plVegetationTypeResourceDescriptor;

/// One level of detail: a mesh and the screen-height fraction at which it turns on.
struct plVegetationTypeAssetLod
{
  plString m_sMesh;
  float m_fScreenSize = 1.0f;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetLod);

struct plVegetationTypeAssetWind
{
  float m_fBendStiffness = 0.35f;
  float m_fBendHeight = 2.0f;
  float m_fBranchAmplitude = 0.5f;
  float m_fBranchFrequency = 1.2f;
  float m_fEdgeAmplitude = 0.5f;
  float m_fEdgeFrequency = 6.0f;
  float m_fPhaseScale = 0.8f;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetWind);

struct plVegetationTypeAssetTouch
{
  float m_fTouchStrength = 0.8f;
  float m_fRecovery = 1.5f;
  float m_fCrush = 0.3f;
  plString m_sCollisionPrefab;
  float m_fCollisionRadius = 15.0f;
  plString m_sTouchTag;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetTouch);

struct plVegetationTypeAssetPlacement
{
  float m_fMinScale = 0.8f;
  float m_fMaxScale = 1.25f;
  float m_fAlignToNormal = 0.6f;
  plAngle m_MaxSlope = plAngle::MakeFromDegree(45.0f);
  float m_fSink = 0.05f;
  float m_fMinSpacing = 0.4f;
  plString m_sColorGradient;
  float m_fHueVariation = 0.05f;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetPlacement);

struct plVegetationTypeAssetRender
{
  float m_fCullDistance = 120.0f;
  float m_fShadowCullDistance = 40.0f;
  float m_fThinningStart = 0.6f;
  float m_fLodCrossfade = 0.1f;
  bool m_bCastShadows = true;
  bool m_bAffectsGI = false;
  bool m_bUseImpostor = false;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetRender);

/// The reflected content of a Vegetation Type document.
class plVegetationTypeAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeAssetProperties, plReflectedClass);

public:
  plVegetationTypeAssetProperties();
  ~plVegetationTypeAssetProperties();

  /// \brief Copies the document properties into the runtime descriptor written at transform time.
  void FillDescriptor(plVegetationTypeResourceDescriptor& out_desc) const;

  plHybridArray<plVegetationTypeAssetLod, 4> m_Lods;
  plHybridArray<plString, 4> m_Materials;
  plVegetationTypeAssetWind m_Wind;
  plVegetationTypeAssetTouch m_Touch;
  plVegetationTypeAssetPlacement m_Placement;
  plVegetationTypeAssetRender m_Render;
};
