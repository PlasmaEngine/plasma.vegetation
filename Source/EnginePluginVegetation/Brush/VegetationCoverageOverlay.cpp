#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <Core/World/World.h>
#include <EnginePluginVegetation/Brush/VegetationCoverageOverlay.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>

namespace
{
  /// Upper bound of drawn texels; beyond it the overlay shows the first tiles only.
  constexpr plUInt32 s_uiMaxOverlayTexels = 65536;
} // namespace

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plVegetationCoverageOverlayComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plHiddenAttribute(),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plVegetationCoverageOverlayComponent::plVegetationCoverageOverlayComponent() = default;
plVegetationCoverageOverlayComponent::~plVegetationCoverageOverlayComponent() = default;

void plVegetationCoverageOverlayComponent::SetLayer(plComponentHandle hLayer)
{
  if (m_hLayer == hLayer)
    return;

  m_hLayer = hLayer;
  m_uiBuiltCounter = plInvalidIndex;
  m_Triangles.Clear();
}

void plVegetationCoverageOverlayComponent::Update()
{
  const plVegetationLayerComponent* pLayer = nullptr;
  if (!GetWorld()->TryGetComponent(m_hLayer, pLayer) || !pLayer->IsActiveAndInitialized())
    return;

  const plTransform layerTransform = pLayer->GetOwner()->GetGlobalTransform();
  if (m_uiBuiltCounter != pLayer->GetGrassCoverageChangeCounter() || !m_BuiltTransform.IsEqual(layerTransform, 0.0001f))
  {
    Rebuild(*pLayer);
  }

  if (!m_Triangles.IsEmpty())
  {
    plDebugRenderer::DrawSolidTriangles(GetWorld(), m_Triangles, plColor::White);
  }
}

void plVegetationCoverageOverlayComponent::Rebuild(const plVegetationLayerComponent& layer)
{
  m_uiBuiltCounter = layer.GetGrassCoverageChangeCounter();
  m_BuiltTransform = layer.GetOwner()->GetGlobalTransform();
  m_Triangles.Clear();

  const plGrassCoverageData& coverage = layer.GetGrassCoverage();
  const float fTexel = coverage.m_fTexelSize;
  const float fHalf = fTexel * 0.46f; // small gaps keep the grid readable
  const float fLift = 0.03f;          // above the surface to avoid z-fighting

  plUInt32 uiTexels = 0;

  for (const plGrassCoverageEntry& entry : coverage.m_Entries)
  {
    const float fHue = static_cast<float>(plGrassCoverageData::GetTypeSeed(entry.m_sType) % 360u);
    const plColor typeColor = plColor::MakeHSV(fHue, 0.65f, 0.95f);
    const plColor discontinuousColor(1.0f, 0.45f, 0.05f);

    for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      const plGrassCoverageTile& tile = it.Value();

      for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
      {
        const plGrassCoverageTexel& texel = tile.m_Texels[i];
        if (texel.m_uiDensity == 0)
          continue;

        if (++uiTexels > s_uiMaxOverlayTexels)
          return;

        const plInt32 iX = tile.m_iX * 16 + static_cast<plInt32>(i % 16);
        const plInt32 iY = tile.m_iY * 16 + static_cast<plInt32>(i / 16);
        const plVec2 vCenter = coverage.GetTexelCenter(iX, iY);
        const float fHeight = tile.GetTexelHeight(i) + fLift;

        // the quad lies in the texel's surface plane so slopes and ramps read correctly
        const plVec3 n = plGrassCoverageTexel::DecodeNormal(texel.m_uiNormal);
        const float fNz = plMath::Max(n.z, 0.2f);
        auto Corner = [&](float dx, float dy)
        {
          const float z = fHeight - (n.x * dx + n.y * dy) / fNz;
          return m_BuiltTransform.TransformPosition(plVec3(vCenter.x + dx, vCenter.y + dy, z));
        };

        const plVec3 v0 = Corner(-fHalf, -fHalf);
        const plVec3 v1 = Corner(fHalf, -fHalf);
        const plVec3 v2 = Corner(fHalf, fHalf);
        const plVec3 v3 = Corner(-fHalf, fHalf);

        plColor color = (texel.m_uiFlags & plGrassCoverageTexelFlags::Discontinuous) ? discontinuousColor : typeColor;
        color.a = 0.15f + (texel.m_uiDensity / 255.0f) * 0.7f;

        // debug primitives are back-face culled: both windings, so exactly one of each pair shows from any side
        auto AddTriangle = [&](const plVec3& a, const plVec3& b, const plVec3& c)
        {
          plDebugRenderer::Triangle& t = m_Triangles.ExpandAndGetRef();
          t.m_position[0] = a;
          t.m_position[1] = b;
          t.m_position[2] = c;
          t.m_color = color;
        };

        AddTriangle(v0, v1, v2);
        AddTriangle(v0, v2, v3);
        AddTriangle(v0, v2, v1);
        AddTriangle(v0, v3, v2);
      }
    }
  }
}
