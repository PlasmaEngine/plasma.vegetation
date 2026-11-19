#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetDocumentManager.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAssetObjects.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

namespace
{
  /// Document type and output tag of the engine's Foliage Composite asset, whose merged mesh output is a static mesh
  /// with a composite table. Its last transform writes build statistics under the STATS output tag.
  constexpr plStringView s_sCompositeDocumentType = "Foliage Composite"_plsv;
  constexpr plStringView s_sCompositeStatsTag = "STATS"_plsv;
  constexpr plUInt8 s_uiCompositeStatsVersion = 1;

  /// Reads the numbers the summary shows from a version 1 statistics file; any other version fails.
  plResult ReadCompositeStats(plStreamReader& inout_stream, plVegetationCompositeInfo& out_info)
  {
    plUInt8 uiVersion = 0;
    inout_stream >> uiVersion;
    if (uiVersion != s_uiCompositeStatsVersion)
      return PL_FAILURE;

    plUInt32 uiParts = 0;
    inout_stream >> uiParts;
    if (uiParts > 1u << 20)
      return PL_FAILURE;

    for (plUInt32 i = 0; i < uiParts; ++i)
    {
      plString sName;
      plUInt32 uiTriangles = 0, uiInstances = 0;
      plUInt64 uiBytes = 0;
      inout_stream >> sName >> uiTriangles >> uiInstances >> uiBytes;
    }

    plUInt32 uiJoints = 0, uiVoxelGroups = 0;
    plUInt64 uiFlatTriangles = 0, uiStoredTriangles = 0;
    inout_stream >> uiJoints >> out_info.m_uiBones >> out_info.m_uiPartInstances >> uiFlatTriangles >> uiStoredTriangles >> out_info.m_uiMergedTriangles >> uiVoxelGroups;
    inout_stream >> out_info.m_fSwitchError >> out_info.m_fRadius;

    out_info.m_uiParts = uiParts;
    return PL_SUCCESS;
  }

  /// The properties object a sub-object (Render, Ray Tracing) belongs to, or the object itself.
  const plDocumentObject* GetPropertiesObject(const plDocumentObject* pObject)
  {
    while (pObject != nullptr && pObject->GetType() != plGetStaticRTTI<plVegetationTypeAssetProperties>())
      pObject = pObject->GetParent();
    return pObject;
  }

  /// The document keeps the Composite summary current; it is set exactly while LOD 0 is a composite.
  bool IsComposite(const plDocumentObject* pProperties)
  {
    if (pProperties == nullptr)
      return false;

    const plVariant value = pProperties->GetTypeAccessor().GetValue("Composite");
    return value.IsA<plString>() && !value.Get<plString>().IsEmpty();
  }
} // namespace

plVegetationCompositeInfo plVegetationCompositeInfo::Query(plStringView sMeshGuid)
{
  plVegetationCompositeInfo info;

  plUuid guid;
  if (sMeshGuid.IsEmpty() || !plConversionUtils::TryConvertStringToUuid(sMeshGuid, guid))
    return info;

  plString sStatsFile;
  {
    auto pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid);
    if (pSubAsset == nullptr || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
      return info;

    plAssetInfo* pAssetInfo = pSubAsset->m_pAssetInfo;
    if (pAssetInfo->m_pDocumentTypeDescriptor->m_sDocumentTypeName != s_sCompositeDocumentType)
      return info;

    info.m_bComposite = true;
    sStatsFile = pAssetInfo->GetManager()->GetAbsoluteOutputFileName(pAssetInfo->m_pDocumentTypeDescriptor, pAssetInfo->m_Path.GetAbsolutePath(), s_sCompositeStatsTag);
  }

  plFileReader file;
  if (file.Open(sStatsFile).Succeeded())
  {
    plVegetationCompositeInfo stats = info;
    if (ReadCompositeStats(file, stats).Succeeded())
    {
      info = stats;
      info.m_bHasStats = true;
    }
  }

  return info;
}

plString plVegetationCompositeInfo::GetSummary() const
{
  if (!m_bComposite)
    return {};

  if (!m_bHasStats)
    return "Composite tree. Transform the composite asset to see its parts and bones.";

  plStringBuilder s;
  s.SetFormat("{} parts, {} part instances, {} bones, switch error {} m, radius {} m, merged {} triangles", m_uiParts, m_uiPartInstances, m_uiBones, plArgF(m_fSwitchError, 3), plArgF(m_fRadius, 1), m_uiMergedTriangles);
  return s;
}

void plVegetationTypeAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  const plRTTI* pType = e.m_pObject->GetType();
  auto& props = *e.m_pPropertyStates;

  if (pType == plGetStaticRTTI<plVegetationTypeAssetProperties>())
  {
    props["Composite"].m_Visibility = IsComposite(e.m_pObject) ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    return;
  }

  // A composite switches between its parts and its merged mesh by itself: there are no LODs to crossfade, no impostor
  // and no other LOD to trace.
  if (pType == plGetStaticRTTI<plVegetationTypeAssetRender>())
  {
    if (IsComposite(GetPropertiesObject(e.m_pObject)))
    {
      props["LodCrossfade"].m_Visibility = plPropertyUiState::Invisible;
      props["UseImpostor"].m_Visibility = plPropertyUiState::Invisible;
    }
  }
  else if (pType == plGetStaticRTTI<plVegetationTypeAssetRayTracing>())
  {
    if (IsComposite(GetPropertiesObject(e.m_pObject)))
    {
      props["Lod"].m_Visibility = plPropertyUiState::Invisible;
    }
  }
}

void plVegetationTypeAssetProperties::ContainerMetaStateEventHandler(plContainerElementMetaStateEvent& e)
{
  if (e.m_pObject->GetType() != plGetStaticRTTI<plVegetationTypeAssetProperties>() || plStringView(e.m_szProperty) != "Lods")
    return;

  if (!IsComposite(e.m_pObject))
    return;

  const plInt32 iCount = e.m_pObject->GetTypeAccessor().GetCount("Lods");
  for (plInt32 i = 1; i < iCount; ++i)
  {
    (*e.m_pContainerElementStates)[plVariant(static_cast<plUInt32>(i))].m_Visibility = plPropertyUiState::Invisible;
  }
}

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationTypeAssetLod, plNoBase, 1, plRTTIDefaultAllocator<plVegetationTypeAssetLod>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Mesh", m_sMesh)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Mesh_Static")),
    PL_MEMBER_PROPERTY("ScreenSize", m_fScreenSize)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 2.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationTypeAssetWind, plNoBase, 1, plRTTIDefaultAllocator<plVegetationTypeAssetWind>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("BendStiffness", m_fBendStiffness)->AddAttributes(new plDefaultValueAttribute(0.35f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("BendHeight", m_fBendHeight)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.05f, 50.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("BranchAmplitude", m_fBranchAmplitude)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("BranchFrequency", m_fBranchFrequency)->AddAttributes(new plDefaultValueAttribute(1.2f), new plClampValueAttribute(0.05f, 8.0f), new plSuffixAttribute(" Hz")),
    PL_MEMBER_PROPERTY("EdgeAmplitude", m_fEdgeAmplitude)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("EdgeFrequency", m_fEdgeFrequency)->AddAttributes(new plDefaultValueAttribute(6.0f), new plClampValueAttribute(0.5f, 20.0f), new plSuffixAttribute(" Hz")),
    PL_MEMBER_PROPERTY("PhaseScale", m_fPhaseScale)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 4.0f), new plSuffixAttribute(" s")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationTypeAssetTouch, plNoBase, 1, plRTTIDefaultAllocator<plVegetationTypeAssetTouch>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("TouchStrength", m_fTouchStrength)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Recovery", m_fRecovery)->AddAttributes(new plDefaultValueAttribute(1.5f), new plClampValueAttribute(0.1f, 10.0f), new plSuffixAttribute(" /s")),
    PL_MEMBER_PROPERTY("Crush", m_fCrush)->AddAttributes(new plDefaultValueAttribute(0.3f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("CollisionPrefab", m_sCollisionPrefab)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Prefab")),
    PL_MEMBER_PROPERTY("CollisionRadius", m_fCollisionRadius)->AddAttributes(new plDefaultValueAttribute(15.0f), new plClampValueAttribute(1.0f, 200.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("TouchTag", m_sTouchTag),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationTypeAssetPlacement, plNoBase, 1, plRTTIDefaultAllocator<plVegetationTypeAssetPlacement>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("MinScale", m_fMinScale)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_MEMBER_PROPERTY("MaxScale", m_fMaxScale)->AddAttributes(new plDefaultValueAttribute(1.25f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_MEMBER_PROPERTY("AlignToNormal", m_fAlignToNormal)->AddAttributes(new plDefaultValueAttribute(0.6f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("MaxSlope", m_MaxSlope)->AddAttributes(new plDefaultValueAttribute(plAngle::MakeFromDegree(45.0f)), new plClampValueAttribute(plAngle::MakeFromDegree(0.0f), plAngle::MakeFromDegree(90.0f))),
    PL_MEMBER_PROPERTY("Sink", m_fSink)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(-1.0f, 1.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MinSpacing", m_fMinSpacing)->AddAttributes(new plDefaultValueAttribute(0.4f), new plClampValueAttribute(0.0f, 50.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ColorGradient", m_sColorGradient)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_ColorGradient")),
    PL_MEMBER_PROPERTY("HueVariation", m_fHueVariation)->AddAttributes(new plDefaultValueAttribute(0.05f), new plClampValueAttribute(0.0f, 0.5f)),
    PL_MEMBER_PROPERTY("DensityWeight", m_fDensityWeight)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f)),
    PL_MEMBER_PROPERTY("ClearingAvoidance", m_fClearingAvoidance)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" m")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationTypeAssetRender, plNoBase, 1, plRTTIDefaultAllocator<plVegetationTypeAssetRender>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("CullDistance", m_fCullDistance)->AddAttributes(new plDefaultValueAttribute(120.0f), new plClampValueAttribute(1.0f, 5000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ShadowCullDistance", m_fShadowCullDistance)->AddAttributes(new plDefaultValueAttribute(40.0f), new plClampValueAttribute(0.0f, 5000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ThinningStart", m_fThinningStart)->AddAttributes(new plDefaultValueAttribute(0.6f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("LodCrossfade", m_fLodCrossfade)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 0.5f)),
    PL_MEMBER_PROPERTY("CastShadows", m_bCastShadows)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("UseImpostor", m_bUseImpostor),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationTypeAssetRayTracing, plNoBase, 1, plRTTIDefaultAllocator<plVegetationTypeAssetRayTracing>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("AffectsGI", plVegetationRayTracingMode, m_Mode),
    PL_MEMBER_PROPERTY("Lod", m_iLod)->AddAttributes(new plDefaultValueAttribute(0), new plClampValueAttribute(0, static_cast<plInt32>(plVegetationMaxLods) - 1)),
    PL_MEMBER_PROPERTY("Range", m_fRange)->AddAttributes(new plClampValueAttribute(0.0f, 5000.0f), new plSuffixAttribute(" m")),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeAssetProperties, 1, plRTTIDefaultAllocator<plVegetationTypeAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Lods", m_Lods),
    PL_MEMBER_PROPERTY("Composite", m_sComposite)->AddAttributes(new plReadOnlyAttribute(), new plTemporaryAttribute()),
    PL_ARRAY_MEMBER_PROPERTY("Materials", m_Materials)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
    PL_MEMBER_PROPERTY("Wind", m_Wind),
    PL_MEMBER_PROPERTY("Touch", m_Touch),
    PL_MEMBER_PROPERTY("Placement", m_Placement),
    PL_MEMBER_PROPERTY("Render", m_Render),
    PL_MEMBER_PROPERTY("RayTracing", m_RayTracing),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationTypeAssetProperties::plVegetationTypeAssetProperties() = default;
plVegetationTypeAssetProperties::~plVegetationTypeAssetProperties() = default;

void plVegetationTypeAssetProperties::FillDescriptor(plVegetationTypeResourceDescriptor& out_desc) const
{
  out_desc.m_Lods.Clear();
  for (const auto& lod : m_Lods)
  {
    if (lod.m_sMesh.IsEmpty())
      continue;

    auto& d = out_desc.m_Lods.ExpandAndGetRef();
    d.m_sMesh = lod.m_sMesh;
    d.m_fScreenSize = lod.m_fScreenSize;
  }

  out_desc.m_Materials = m_Materials;

  out_desc.m_Wind.m_fBendStiffness = m_Wind.m_fBendStiffness;
  out_desc.m_Wind.m_fBendHeight = m_Wind.m_fBendHeight;
  out_desc.m_Wind.m_fBranchAmplitude = m_Wind.m_fBranchAmplitude;
  out_desc.m_Wind.m_fBranchFrequency = m_Wind.m_fBranchFrequency;
  out_desc.m_Wind.m_fEdgeAmplitude = m_Wind.m_fEdgeAmplitude;
  out_desc.m_Wind.m_fEdgeFrequency = m_Wind.m_fEdgeFrequency;
  out_desc.m_Wind.m_fPhaseScale = m_Wind.m_fPhaseScale;
  out_desc.m_Wind.m_fTouchStrength = m_Touch.m_fTouchStrength;

  out_desc.m_Touch.m_fRecovery = m_Touch.m_fRecovery;
  out_desc.m_Touch.m_fCrush = m_Touch.m_fCrush;
  out_desc.m_Touch.m_sCollisionPrefab = m_Touch.m_sCollisionPrefab;
  out_desc.m_Touch.m_fCollisionRadius = m_Touch.m_fCollisionRadius;
  out_desc.m_Touch.m_sTouchTag = m_Touch.m_sTouchTag;

  out_desc.m_Placement.m_fMinScale = m_Placement.m_fMinScale;
  out_desc.m_Placement.m_fMaxScale = m_Placement.m_fMaxScale;
  out_desc.m_Placement.m_fAlignToNormal = m_Placement.m_fAlignToNormal;
  out_desc.m_Placement.m_MaxSlope = m_Placement.m_MaxSlope;
  out_desc.m_Placement.m_fSink = m_Placement.m_fSink;
  out_desc.m_Placement.m_fMinSpacing = m_Placement.m_fMinSpacing;
  out_desc.m_Placement.m_sColorGradient = m_Placement.m_sColorGradient;
  out_desc.m_Placement.m_fHueVariation = m_Placement.m_fHueVariation;
  out_desc.m_Placement.m_fDensityWeight = m_Placement.m_fDensityWeight;
  out_desc.m_Placement.m_fClearingAvoidance = m_Placement.m_fClearingAvoidance;

  out_desc.m_Render.m_fCullDistance = m_Render.m_fCullDistance;
  out_desc.m_Render.m_fShadowCullDistance = m_Render.m_fShadowCullDistance;
  out_desc.m_Render.m_fThinningStart = m_Render.m_fThinningStart;
  out_desc.m_Render.m_fLodCrossfade = m_Render.m_fLodCrossfade;
  out_desc.m_Render.m_bCastShadows = m_Render.m_bCastShadows;
  out_desc.m_Render.m_bUseImpostor = m_Render.m_bUseImpostor;

  out_desc.m_RayTracing.m_Mode = m_RayTracing.m_Mode;
  out_desc.m_RayTracing.m_iLod = m_RayTracing.m_iLod;
  out_desc.m_RayTracing.m_fRange = m_RayTracing.m_fRange;
}
