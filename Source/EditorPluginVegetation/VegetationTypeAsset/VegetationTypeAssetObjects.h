#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/String.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

struct plPropertyMetaStateEvent;
struct plContainerElementMetaStateEvent;
class plDocumentObject;

/// \brief What the editor knows about a foliage composite mesh asset without the engine process: whether the asset is
/// one (its document type) and the numbers its last transform wrote beside its outputs.
struct PL_EDITORPLUGINVEGETATION_DLL plVegetationCompositeSummary
{
  bool m_bComposite = false;
  bool m_bHasStats = false; ///< false until the composite has been transformed by this engine version
  plUInt32 m_uiParts = 0;
  plUInt32 m_uiPartInstances = 0;
  plUInt32 m_uiBones = 0;
  plUInt32 m_uiMergedTriangles = 0;
  float m_fSwitchError = 0.0f;
  float m_fRadius = 0.0f;

  /// \brief Looks up the mesh asset (guid string) in the asset curator; reads the build statistics of a composite.
  static plVegetationCompositeSummary Query(plStringView sMeshGuid);

  /// \brief One line for the property grid.
  plString GetSummary() const;
};
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
  float m_fDensityWeight = 1.0f;
  float m_fClearingAvoidance = 0.0f;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetPlacement);

struct plVegetationTypeAssetRender
{
  float m_fCullDistance = 120.0f;
  float m_fShadowCullDistance = 40.0f;
  float m_fThinningStart = 0.6f;
  float m_fLodCrossfade = 0.1f;
  bool m_bCastShadows = true;
  bool m_bUseImpostor = false;
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetRender);

/// How the ray tracing scene (GI, reflections, ray traced shadows) sees the type.
struct plVegetationTypeAssetRayTracing
{
  plEnum<plVegetationRayTracingMode> m_Mode;
  plInt8 m_iLod = 0;     ///< clamped to the type's LODs
  float m_fRange = 0.0f; ///< 0: the cull distance
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plVegetationTypeAssetRayTracing);

/// The reflected content of a Vegetation Type document.
class plVegetationTypeAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeAssetProperties, plReflectedClass);

public:
  plVegetationTypeAssetProperties();
  ~plVegetationTypeAssetProperties();

  /// \brief Copies the document properties into the runtime descriptor written at transform time.
  void FillDescriptor(plVegetationTypeResourceDescriptor& out_desc) const;

  /// \brief Hides what does nothing for a composite tree (LOD 1-3, crossfade, impostor) and shows the composite
  /// summary instead. Registered with plPropertyMetaState by the plugin.
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);
  static void ContainerMetaStateEventHandler(plContainerElementMetaStateEvent& e);

  plHybridArray<plVegetationTypeAssetLod, 4> m_Lods;
  plString m_sComposite; ///< Read-only summary of a composite LOD 0; not saved, the document keeps it current.
  plHybridArray<plString, 4> m_Materials;
  plVegetationTypeAssetWind m_Wind;
  plVegetationTypeAssetTouch m_Touch;
  plVegetationTypeAssetPlacement m_Placement;
  plVegetationTypeAssetRender m_Render;
  plVegetationTypeAssetRayTracing m_RayTracing;
};
