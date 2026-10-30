#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAssetObjects.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassTypeAssetProperties, 2, plRTTIDefaultAllocator<plGrassTypeAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("StrandsPerSquareMeter", m_fStrandsPerSquareMeter)->AddAttributes(new plDefaultValueAttribute(120.0f), new plClampValueAttribute(0.0f, 10000.0f), new plGroupAttribute("Strands")),
    PL_MEMBER_PROPERTY("SegmentCount", m_uiSegmentCount)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 10)),
    PL_MEMBER_PROPERTY("BillboardCount", m_uiBillboardCount)->AddAttributes(new plDefaultValueAttribute(1), new plClampValueAttribute(1, 10)),
    PL_MEMBER_PROPERTY("RandomSeed", m_uiRandomSeed)->AddAttributes(new plDefaultValueAttribute(1)),
    PL_MEMBER_PROPERTY("Length", m_fLength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Width", m_fWidth)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("Randomness", m_fRandomness)->AddAttributes(new plDefaultValueAttribute(0.2f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("Uniformity", m_fUniformity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 100.0f)),
    PL_ARRAY_MEMBER_PROPERTY("AtlasRects", m_AtlasRects),
    PL_MEMBER_PROPERTY("Material", m_sMaterial)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
    PL_MEMBER_PROPERTY("ProceduralBlades", m_bProceduralBlades)->AddAttributes(new plDefaultValueAttribute(false), new plGroupAttribute("Procedural Blades")),
    PL_MEMBER_PROPERTY("BladeWidth", m_fBladeWidth)->AddAttributes(new plDefaultValueAttribute(0.06f), new plClampValueAttribute(0.001f, 1.0f)),
    PL_MEMBER_PROPERTY("BladeTilt", m_fBladeTilt)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 0.8f)),
    PL_MEMBER_PROPERTY("BladeBend", m_fBladeBend)->AddAttributes(new plDefaultValueAttribute(0.35f), new plClampValueAttribute(0.0f, 0.8f)),
    PL_MEMBER_PROPERTY("BladeSideCurve", m_fBladeSideCurve)->AddAttributes(new plDefaultValueAttribute(0.08f), new plClampValueAttribute(0.0f, 0.3f)),
    PL_MEMBER_PROPERTY("ClumpSize", m_fClumpSize)->AddAttributes(new plDefaultValueAttribute(1.5f), new plClampValueAttribute(0.1f, 100.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ClumpStrength", m_fClumpStrength)->AddAttributes(new plDefaultValueAttribute(0.65f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("ClumpColorVariation", m_fClumpColorVariation)->AddAttributes(new plDefaultValueAttribute(0.15f), new plClampValueAttribute(0.0f, 0.5f)),
    PL_MEMBER_PROPERTY("BladeNormalRoundness", m_fBladeNormalRoundness)->AddAttributes(new plDefaultValueAttribute(0.65f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("AdaptiveDensity", m_bAdaptiveDensity)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("FarDensity", m_fFarDensity)->AddAttributes(new plDefaultValueAttribute(0.01f), new plClampValueAttribute(1.0f / 256.0f, 1.0f)),
    PL_MEMBER_PROPERTY("FarGroundCover", m_bFarGroundCover)->AddAttributes(new plDefaultValueAttribute(true), new plGroupAttribute("Ground Cover")),
    PL_MEMBER_PROPERTY("GroundCoverStartDistance", m_fGroundCoverStartDistance)->AddAttributes(new plDefaultValueAttribute(60.0f), new plClampValueAttribute(1.0f, 10000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("GroundCoverMaxSize", m_fGroundCoverMaxSize)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.01f, 4.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Stiffness", m_fStiffness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 10.0f), new plGroupAttribute("Simulation")),
    PL_MEMBER_PROPERTY("Drag", m_fDrag)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("GravityPower", m_fGravityPower)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("WindInfluence", m_fWindInfluence)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("CameraBend", m_bCameraBend)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("TouchCrush", m_fTouchCrush)->AddAttributes(new plDefaultValueAttribute(0.6f), new plClampValueAttribute(0.0f, 1.0f), new plGroupAttribute("Interaction")),
    PL_MEMBER_PROPERTY("TouchRecovery", m_fTouchRecovery)->AddAttributes(new plDefaultValueAttribute(1.5f), new plClampValueAttribute(0.0f, 100.0f), new plSuffixAttribute(" /s")),
    PL_MEMBER_PROPERTY("ViewDistance", m_fViewDistance)->AddAttributes(new plDefaultValueAttribute(200.0f), new plClampValueAttribute(0.0f, 10000.0f), new plSuffixAttribute(" m"), new plGroupAttribute("Distances")),
    PL_MEMBER_PROPERTY("SimulationDistance", m_fSimulationDistance)->AddAttributes(new plDefaultValueAttribute(40.0f), new plClampValueAttribute(0.0f, 10000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("LodStartDistance", m_fLodStartDistance)->AddAttributes(new plDefaultValueAttribute(15.0f), new plClampValueAttribute(1.0f, 1000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("ShadowDistance", m_fShadowDistance)->AddAttributes(new plDefaultValueAttribute(25.0f), new plClampValueAttribute(0.0f, 10000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("CastShadows", m_bCastShadows)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("PatchSize", m_fPatchSize)->AddAttributes(new plDefaultValueAttribute(8.0f), new plClampValueAttribute(1.0f, 64.0f), new plSuffixAttribute(" m")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGrassTypeAssetProperties::plGrassTypeAssetProperties() = default;
plGrassTypeAssetProperties::~plGrassTypeAssetProperties() = default;

void plGrassTypeAssetProperties::FillDescriptor(plGrassTypeResourceDescriptor& out_desc) const
{
  plGrassSettings& s = out_desc.m_Settings;

  s.m_fStrandsPerSquareMeter = m_fStrandsPerSquareMeter;
  s.m_uiSegmentCount = m_uiSegmentCount;
  s.m_uiBillboardCount = m_uiBillboardCount;
  s.m_uiRandomSeed = m_uiRandomSeed;
  s.m_fLength = m_fLength;
  s.m_fWidth = m_fWidth;
  s.m_fRandomness = m_fRandomness;
  s.m_fUniformity = m_fUniformity;
  s.m_AtlasRects = m_AtlasRects;
  s.m_sMaterial = m_sMaterial;
  s.m_bProceduralBlades = m_bProceduralBlades;
  s.m_fBladeWidth = m_fBladeWidth;
  s.m_fBladeTilt = m_fBladeTilt;
  s.m_fBladeBend = m_fBladeBend;
  s.m_fBladeSideCurve = m_fBladeSideCurve;
  s.m_fClumpSize = m_fClumpSize;
  s.m_fClumpStrength = m_fClumpStrength;
  s.m_fClumpColorVariation = m_fClumpColorVariation;
  s.m_fBladeNormalRoundness = m_fBladeNormalRoundness;
  s.m_bAdaptiveDensity = m_bAdaptiveDensity;
  s.m_fFarDensity = m_fFarDensity;
  s.m_bFarGroundCover = m_bFarGroundCover;
  s.m_fGroundCoverStartDistance = m_fGroundCoverStartDistance;
  s.m_fGroundCoverMaxSize = m_fGroundCoverMaxSize;

  s.m_fStiffness = m_fStiffness;
  s.m_fDrag = m_fDrag;
  s.m_fGravityPower = m_fGravityPower;
  s.m_fWindInfluence = m_fWindInfluence;
  s.m_bCameraBend = m_bCameraBend;

  s.m_fTouchCrush = m_fTouchCrush;
  s.m_fTouchRecovery = m_fTouchRecovery;

  s.m_fViewDistance = m_fViewDistance;
  s.m_fSimulationDistance = m_fSimulationDistance;
  s.m_fLodStartDistance = m_fLodStartDistance;
  s.m_fShadowDistance = m_fShadowDistance;
  s.m_bCastShadows = m_bCastShadows;
  s.m_fPatchSize = m_fPatchSize;

  s.Sanitize();
}
