#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/SimdMath/SimdNoise.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Types/TagRegistry.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Lights/Implementation/VirtualShadowPool.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <VegetationPlugin/Grass/GrassRenderComponent.h>
#include <VegetationPlugin/Grass/GrassSurfaceComponent.h>

#include <Shaders/Vegetation/Grass/GrassConstants.h>
#include <Shaders/Vegetation/Grass/GrassGpuData.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plGrassSurfaceComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_RESOURCE_ACCESSOR_PROPERTY("Mesh", GetMesh, SetMesh)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Mesh_Static")),
    PL_RESOURCE_ACCESSOR_PROPERTY("GrassType", GetGrassType, SetGrassType)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Grass_Type")),
    PL_ARRAY_ACCESSOR_PROPERTY("VertexLengths", VertexLengths_GetCount, VertexLengths_GetValue, VertexLengths_SetValue, VertexLengths_Insert, VertexLengths_Remove),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Rendering/Vegetation"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE;
// clang-format on

namespace
{
  // Materials/Grass/GrassBase.plMaterialAsset
  constexpr const char* s_szDefaultGrassMaterial = "{ f4f1bf46-9929-d239-90ab-037fadeb9ecd }";
} // namespace

plGrassSurfaceComponent::plGrassSurfaceComponent() = default;
plGrassSurfaceComponent::~plGrassSurfaceComponent() = default;

void plGrassSurfaceComponent::SetMesh(const plMeshResourceHandle& hMesh)
{
  m_hMesh = hMesh;
  m_hCpuMesh.Invalidate();
  m_bRebuild = true;
}

void plGrassSurfaceComponent::SetGrassType(const plGrassTypeResourceHandle& hType)
{
  if (m_hGrassType == hType)
    return;

  m_hGrassType = hType;
  m_bTypeChanged = true;
  m_bRebuild = true;
}

plMaterialResourceHandle plGrassSurfaceComponent::GetMaterial() const
{
  return m_hTypeMaterial.IsValid() ? m_hTypeMaterial : m_hDefaultMaterial;
}

bool plGrassSurfaceComponent::CastsShadows() const
{
  return m_Settings.m_bCastShadows && plGrassGpuManager::AreShadowsEnabled();
}

void plGrassSurfaceComponent::OnActivated()
{
  SUPER::OnActivated();
  if (!m_hDefaultMaterial.IsValid())
    m_hDefaultMaterial = plResourceManager::LoadResource<plMaterialResource>(s_szDefaultGrassMaterial);
  m_bTypeChanged = true;
  m_bRebuild = true;
}

void plGrassSurfaceComponent::OnDeactivated()
{
  ReleaseGpu();
  m_hCpuMesh.Invalidate();
  SUPER::OnDeactivated();
}

void plGrassSurfaceComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();
  s << m_hMesh;
  s << m_hGrassType;
  s.WriteArray(m_VertexLengths).AssertSuccess();
}

void plGrassSurfaceComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();
  s >> m_hMesh;
  s >> m_hGrassType;
  s.ReadArray(m_VertexLengths).AssertSuccess();
  m_hCpuMesh.Invalidate();
  m_bTypeChanged = true;
  m_bRebuild = true;
}

bool plGrassSurfaceComponent::UpdateTypeSettings()
{
  if (!m_hGrassType.IsValid())
  {
    if (m_bTypeChanged)
    {
      if (!m_Settings.m_bCastShadows)
        InvalidateCachedShadows(true);
      m_Settings = plGrassSettings();
      m_hTypeMaterial.Invalidate();
      m_bTypeChanged = false;
      m_bRebuild = true;
    }
    return true;
  }

  plResourceLock<plGrassTypeResource> pType(m_hGrassType, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pType.GetAcquireResult() != plResourceAcquireResult::Final)
    return false;

  // Edits to the type asset reload the resource. Growth values regrow the strands; every other value is handed to the
  // grown source, which reads it from its table every frame.
  const plUInt32 uiCounter = pType->GetCurrentResourceChangeCounter();
  if (m_bTypeChanged || uiCounter != m_uiTypeChangeCounter)
  {
    plGrassSettings newSettings = pType->GetSettings();
    newSettings.Sanitize();
    plGrassSettings oldSettings = m_Settings;
    oldSettings.Sanitize();
    const bool bRegrow = m_bTypeChanged || m_hSource.IsInvalidated() || newSettings.RequiresRegrow(oldSettings) ||
                         plGrassGpuManager::UpdateSourceSettings(GetWorld()->GetIndex(), m_hSource, newSettings, m_fTextureAspect).Failed();

    // Cast Shadows changes what this caster contributes over its whole footprint, like the global shadow switch.
    const bool bCastShadowsChanged = newSettings.m_bCastShadows != oldSettings.m_bCastShadows;

    m_Settings = pType->GetSettings();
    m_hTypeMaterial = pType->GetMaterial();
    m_uiTypeChangeCounter = uiCounter;
    m_bTypeChanged = false;
    m_bRebuild |= bRegrow;

    if (bCastShadowsChanged)
      InvalidateCachedShadows(true);
  }
  return true;
}

void plGrassSurfaceComponent::Update(bool bShadowSettingsChanged)
{
  PL_PROFILE_SCOPE("Grass.Update");

  // A new shadow setting changes what this caster contributes over its whole footprint.
  if (bShadowSettingsChanged)
    InvalidateCachedShadows(true);

  const plTransform transform = GetOwner()->GetGlobalTransform();
  if (plMath::Abs(transform.m_vScale.x) < 0.00001f || plMath::Abs(transform.m_vScale.y) < 0.00001f || plMath::Abs(transform.m_vScale.z) < 0.00001f)
  {
    ReleaseGpu();
    m_bRebuild = true;
    return;
  }

  if (!UpdateTypeSettings())
  {
    ReleaseGpu();
    m_bRebuild = true;
    return;
  }

  // The strand count follows the world-space area, so a scale change regrows the strands.
  if (!transform.m_vScale.Abs().IsEqual(m_vBuiltScale, 0.0001f))
    m_bRebuild = true;

  if (m_hMesh.IsValid() && !m_hCpuMesh.IsValid())
    m_hCpuMesh = plResourceManager::LoadResource<plCpuMeshResource>(m_hMesh.GetResourceID());
  if (m_hCpuMesh.IsValid())
  {
    plResourceLock<plCpuMeshResource> pMesh(m_hCpuMesh, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    {
      ReleaseGpu();
      m_bRebuild = true;
      return;
    }
    m_bRebuild |= m_uiMeshChangeCounter != pMesh->GetCurrentResourceChangeCounter();
  }

  const plMaterialResourceHandle hMaterial = GetMaterial();
  float fAspect = 1.0f;
  if (hMaterial.IsValid())
  {
    plResourceLock<plMaterialResource> pMaterial(hMaterial, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    if (pMaterial.GetAcquireResult() == plResourceAcquireResult::Final)
    {
      auto hTexture = pMaterial->GetTexture2DBinding("BaseTexture");
      if (hTexture.IsValid())
      {
        plResourceLock<plTexture2DResource> pTexture(hTexture, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
        if (pTexture.GetAcquireResult() == plResourceAcquireResult::Final)
          fAspect = float(pTexture->GetWidth()) / plMath::Max(1u, pTexture->GetHeight());
      }
    }
  }
  m_bRebuild |= m_fTextureAspect != fAspect;
  m_fTextureAspect = fAspect;

  if (m_bRebuild)
    Rebuild();

  if (!m_hSource.IsInvalidated() && transform != m_UploadedTransform)
  {
    plGrassGpuManager::SetSourceTransform(GetWorld()->GetIndex(), m_hSource, transform);
    m_UploadedTransform = transform;
    InvalidateCachedShadows();
  }

  UpdateRenderObject();
}

void plGrassSurfaceComponent::Rebuild()
{
  m_bRebuild = false;
  m_uiStrandCount = 0;
  m_LocalBounds = plBoundingBox::MakeInvalid();
  m_vBuiltScale = GetOwner()->GetGlobalTransform().m_vScale.Abs();

  const plUInt32 uiWorldIndex = GetWorld()->GetIndex();
  plGrassGpuManager::Free(uiWorldIndex, m_hSource);

  PL_SCOPE_EXIT(InvalidateCachedShadows());

  if (!m_hCpuMesh.IsValid() || m_Settings.m_fStrandsPerSquareMeter <= 0 || m_Settings.m_fLength <= 0)
    return;

  plResourceLock<plCpuMeshResource> pMesh(m_hCpuMesh, plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    return;
  m_uiMeshChangeCounter = pMesh->GetCurrentResourceChangeCounter();
  const auto& buffer = pMesh->GetDescriptor().MeshBufferDesc();
  if (buffer.GetTopology() != plGALPrimitiveTopology::Triangles || buffer.GetVertexCount() < 3)
    return;

  const auto positions = buffer.GetPositionData();
  const auto indices = buffer.GetIndexBufferData();
  const auto GetIndex = [&](plUInt32 i) -> plUInt32
  {
    if (!buffer.HasIndexBuffer())
      return i;
    return buffer.Uses32BitIndices() ? reinterpret_cast<const plUInt32*>(indices.GetPtr())[i] : reinterpret_cast<const plUInt16*>(indices.GetPtr())[i];
  };
  const auto GetLength = [&](plUInt32 i) -> float
  {
    return i < m_VertexLengths.GetCount() ? plMath::Clamp(m_VertexLengths[i], 0.0f, 1.0f) : 1.0f;
  };

  // Only triangles whose vertex-length mask is non-zero grow strands.
  // Area weighting in world scale keeps the density even across unevenly tessellated or non-uniformly scaled meshes.
  struct Triangle
  {
    PL_DECLARE_POD_TYPE();
    plUInt32 m_uiIndices[3];
    float m_fCumulativeArea;
  };
  plDynamicArray<Triangle> triangles;
  float fArea = 0;
  for (plUInt32 t = 0; t < buffer.GetPrimitiveCount(); ++t)
  {
    Triangle tri = {{GetIndex(t * 3), GetIndex(t * 3 + 1), GetIndex(t * 3 + 2)}, 0};
    const auto* v = tri.m_uiIndices;
    if (v[0] >= positions.GetCount() || v[1] >= positions.GetCount() || v[2] >= positions.GetCount())
      continue;
    if (GetLength(v[0]) + GetLength(v[1]) + GetLength(v[2]) <= 0)
      continue;
    const plVec3 p0 = positions[v[0]].CompMul(m_vBuiltScale);
    const plVec3 p1 = positions[v[1]].CompMul(m_vBuiltScale);
    const plVec3 p2 = positions[v[2]].CompMul(m_vBuiltScale);
    const float fTriangleArea = 0.5f * (p1 - p0).CrossRH(p2 - p0).GetLength();
    if (!plMath::IsFinite(fTriangleArea) || fTriangleArea < 0.000001f)
      continue;
    fArea += fTriangleArea;
    tri.m_fCumulativeArea = fArea;
    triangles.PushBack(tri);
  }
  if (triangles.IsEmpty())
    return;

  const double fStrandCount = plMath::Round(static_cast<double>(fArea) * m_Settings.m_fStrandsPerSquareMeter);
  const plUInt32 uiStrandCount = static_cast<plUInt32>(plMath::Clamp(fStrandCount, 0.0, static_cast<double>(MaxStrandCount)));
  if (uiStrandCount == 0)
    return;

  const auto& atlasRects = m_Settings.m_AtlasRects;
  const plVec3 vScale = m_vBuiltScale;
  const float fScaleRatio = plMath::Max(vScale.x, vScale.y, vScale.z) / plMath::Max(0.00001f, plMath::Min(vScale.x, vScale.y, vScale.z));

  plRandom random;
  random.Initialize(m_Settings.m_uiRandomSeed);
  plSimdPerlinNoise atlasNoise(m_Settings.m_uiRandomSeed);

  plDynamicArray<plGrassStrandStatic> strands;
  strands.SetCount(uiStrandCount);
  for (plUInt32 i = 0; i < uiStrandCount; ++i)
  {
    const float fSample = static_cast<float>(random.DoubleZeroToOneExclusive()) * fArea;
    plUInt32 lo = 0, hi = triangles.GetCount() - 1;
    while (lo < hi)
    {
      const plUInt32 mid = (lo + hi) / 2;
      if (triangles[mid].m_fCumulativeArea < fSample)
        lo = mid + 1;
      else
        hi = mid;
    }
    const auto* v = triangles[lo].m_uiIndices;
    float f = static_cast<float>(random.DoubleZeroToOneExclusive());
    float g = static_cast<float>(random.DoubleZeroToOneExclusive());
    if (f + g > 1)
    {
      f = 1 - f;
      g = 1 - g;
    }
    const float h = 1 - f - g;
    const plVec3 vRoot = positions[v[0]] * h + positions[v[1]] * f + positions[v[2]] * g;
    plVec3 vNormal = (positions[v[1]] - positions[v[0]]).CrossRH(positions[v[2]] - positions[v[0]]).GetNormalized();
    if (buffer.GetVertexStreamConfig().HasNormal())
    {
      plVec3 normal = buffer.GetNormal(v[0]) * h + buffer.GetNormal(v[1]) * f + buffer.GetNormal(v[2]) * g;
      normal.NormalizeIfNotZero(vNormal).IgnoreResult();
      vNormal = normal;
    }
    const float fAngle = static_cast<float>(random.DoubleZeroToOneExclusive());

    plUInt32 uiAtlas = 0;
    float fAtlasSize = 1.0f;
    float fAtlasAspect = 1.0f;
    if (!atlasRects.IsEmpty())
    {
      // Smooth spatial variation gives nearby roots related atlas choices.
      const plVec3 p = vRoot * m_Settings.m_fUniformity;
      const float fNoise = atlasNoise.NoiseZeroToOne(plSimdVec4f(p.x), plSimdVec4f(p.y), plSimdVec4f(p.z)).x();
      uiAtlas = static_cast<plUInt32>(plMath::Saturate(fNoise) * 1000) % atlasRects.GetCount();
      fAtlasSize = plMath::Clamp(atlasRects[uiAtlas].m_fSize, 0.0f, 10.0f);
      const plVec4 uv = atlasRects[uiAtlas].m_vTexMulAdd;
      fAtlasAspect = plMath::Abs(uv.y) > 0.00001f ? plMath::Abs(uv.x / uv.y) : 1.0f;
    }

    const float fLength = m_Settings.m_fLength * plMath::Lerp(1.0f, static_cast<float>(random.DoubleZeroToOneExclusive()), m_Settings.m_fRandomness) *
                          (GetLength(v[0]) * h + GetLength(v[1]) * f + GetLength(v[2]) * g) * fAtlasSize;

    plGrassGpuManager::MakeStrandStatic(vRoot, vNormal, fLength, fAngle, uiAtlas, random.UInt(), strands[i]);
    strands[i].AtlasFlags |= GrassClumpSeed(vRoot.x, vRoot.y, m_Settings.m_fClumpSize, m_Settings.m_uiRandomSeed) << 16u;
    strands[i].Reserved = GrassPcgHash(strands[i].Seed ^ 0x27D4EB2Fu);

    // Includes every possible constrained pose, card width and camera bend.
    const float fRadius = (fLength * (m_Settings.m_bProceduralBlades ? (3.0f + m_Settings.m_fWidth * m_Settings.m_fBladeWidth * 3.0f) : (1.8f + m_Settings.m_fWidth * fAtlasAspect * m_fTextureAspect * 0.5f)) + (m_Settings.m_bProceduralBlades ? m_Settings.m_fGroundCoverMaxSize : 0.0f)) * fScaleRatio;
    m_LocalBounds.ExpandToInclude(plBoundingBox::MakeFromCenterAndHalfExtents(vRoot, plVec3(fRadius)));
  }

  const plTransform transform = GetOwner()->GetGlobalTransform();
  m_hSource = plGrassGpuManager::AllocateSource(uiWorldIndex, m_Settings, transform, m_fTextureAspect);
  m_UploadedTransform = transform;

  plBoundingBox rootBounds = plBoundingBox::MakeInvalid();
  for (const plGrassStrandStatic& strand : strands)
    rootBounds.ExpandToInclude(strand.Root);

  if (plGrassGpuManager::UploadRoots(uiWorldIndex, m_hSource, strands, rootBounds).Succeeded())
    m_uiStrandCount = uiStrandCount;
  else
    m_LocalBounds = plBoundingBox::MakeInvalid();
}

void plGrassSurfaceComponent::ReleaseGpu()
{
  plGrassGpuManager::Free(GetWorld()->GetIndex(), m_hSource);
  m_uiStrandCount = 0;
  m_LocalBounds = plBoundingBox::MakeInvalid();
  InvalidateCachedShadows();

  plGameObject* pObject = nullptr;
  if (GetWorld()->TryGetObject(m_hRenderObject, pObject))
  {
    // Deactivate immediately; deferred deletion is safe while the world iterates components.
    pObject->SetActiveFlag(false);
    GetWorld()->DeleteObjectDelayed(m_hRenderObject, false);
  }
  m_hRenderObject.Invalidate();
  m_hRenderComponent.Invalidate();
}

void plGrassSurfaceComponent::UpdateRenderObject()
{
  plGameObject* pObject = nullptr;
  plGrassRenderComponent* pComponent = nullptr;
  const bool bExists = GetWorld()->TryGetObject(m_hRenderObject, pObject) && GetWorld()->TryGetComponent(m_hRenderComponent, pComponent);

  if (m_hSource.IsInvalidated())
  {
    // A freed slot may be reused by another source; the render owner must stop drawing it.
    if (bExists)
    {
      pComponent->SetSource(plGrassSourceHandle(), plMaterialResourceHandle(), 0, 0.0f, 0.0f, false);
      pComponent->SetLocalBounds(plBoundingBox::MakeInvalid());
    }
    return;
  }

  if (!bExists)
  {
    plGameObjectDesc desc;
    desc.m_sName.Assign("Grass Render");
    desc.m_hParent = GetOwner()->GetHandle();
    desc.m_bDynamic = GetOwner()->IsDynamic();
    desc.m_Tags = GetOwner()->GetTags();
    desc.m_uiTeamID = GetOwner()->GetTeamID();
    m_hRenderObject = GetWorld()->CreateObject(desc, pObject);
    pObject->SetCreatedByPrefab();

    m_hRenderComponent = plGrassRenderComponent::CreateComponent(pObject, pComponent);
    pComponent->SetCreatedByPrefab();
  }

  // Shadow views extract only tagged objects; wind and touch move the casters every frame.
  // Without Vegetation.Grass.Shadows or the type's Cast Shadows the tags stay off, so no shadow view ever reaches the
  // render component.
  plTagSet tags = GetOwner()->GetTags();
  // Grass receives GI but must not contribute unstable thin geometry to GI captures.
  // Tag only the generated grass child, leaving the supporting mesh eligible for GI.
  tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("skipGI"));
  if (CastsShadows())
  {
    tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
    tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("VSMContinuousShadow"));
  }
  else
  {
    tags.Remove(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
    tags.Remove(plTagRegistry::GetGlobalRegistry().RegisterTag("VSMContinuousShadow"));
  }
  if (pObject->GetTags() != tags)
    pObject->SetTags(tags);

  pComponent->SetSource(m_hSource, GetMaterial(), plRenderComponent::GetUniqueIdForRendering(*this), m_Settings.m_fViewDistance, m_Settings.m_fShadowDistance, m_Settings.m_bCastShadows);
  pComponent->SetLocalBounds(m_LocalBounds);
}

void plGrassSurfaceComponent::InvalidateCachedShadows(bool bForce)
{
  plBoundingBox newBox = m_LocalBounds;
  if (newBox.IsValid())
    newBox.TransformFromOrigin(GetOwner()->GetGlobalTransform().GetAsMat4());

  // While grass casts no shadows nothing of it is cached, so content changes cost no invalidation.
  // The forced call on a shadow setting change still clears whatever the previous setting left behind.
  if (!bForce && !CastsShadows())
  {
    m_LastCasterBox = newBox;
    return;
  }

  plBoundingBox invalidate = newBox;
  if (m_LastCasterBox.IsValid())
  {
    if (invalidate.IsValid())
      invalidate.ExpandToInclude(m_LastCasterBox);
    else
      invalidate = m_LastCasterBox;
  }

  if (invalidate.IsValid())
    plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), invalidate);

  m_LastCasterBox = newBox;
}

void plGrassSurfaceComponent::VertexLengths_SetValue(plUInt32 uiIndex, float fValue)
{
  m_VertexLengths[uiIndex] = fValue;
  m_bRebuild = true;
}

void plGrassSurfaceComponent::VertexLengths_Insert(plUInt32 uiIndex, float fValue)
{
  m_VertexLengths.InsertAt(uiIndex, fValue);
  m_bRebuild = true;
}

void plGrassSurfaceComponent::VertexLengths_Remove(plUInt32 uiIndex)
{
  m_VertexLengths.RemoveAtAndCopy(uiIndex);
  m_bRebuild = true;
}

//////////////////////////////////////////////////////////////////////////

plGrassSurfaceComponentManager::plGrassSurfaceComponentManager(plWorld* pWorld)
  : plComponentManager(pWorld)
{
}

void plGrassSurfaceComponentManager::Initialize()
{
  SUPER::Initialize();
  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plGrassSurfaceComponentManager::Update, this);
  desc.m_Phase = plWorldUpdatePhase::PostTransform;
  desc.m_bOnlyUpdateWhenSimulating = false;
  RegisterUpdateFunction(desc);
}

void plGrassSurfaceComponentManager::Update(const plWorldModule::UpdateContext& context)
{
  // Grass.Shadows and Grass.ShadowDistanceScale take effect on the next update of every component.
  const bool bShadows = plGrassGpuManager::AreShadowsEnabled();
  const float fShadowDistanceScale = plGrassGpuManager::GetShadowDistanceScale();
  const bool bShadowSettingsChanged = bShadows != m_bShadowsEnabled || fShadowDistanceScale != m_fShadowDistanceScale;
  m_bShadowsEnabled = bShadows;
  m_fShadowDistanceScale = fShadowDistanceScale;

  for (auto it = m_ComponentStorage.GetIterator(context.m_uiFirstComponentIndex, context.m_uiComponentCount); it.IsValid(); ++it)
    if (it->IsActiveAndInitialized())
      it->Update(bShadowSettingsChanged);

  // The simulation distance reference and time step of this world's grass pool.
  plVec3 vCamera = plVec3::MakeZero();
  bool bHasCamera = false;
  if (const plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()))
  {
    if (const plCamera* pCamera = pView->GetCamera())
    {
      vCamera = pCamera->GetCenterPosition();
      bHasCamera = true;
    }
  }
  plGrassGpuManager::SubmitWorldFrame(GetWorld()->GetIndex(), vCamera, bHasCamera, GetWorld()->GetClock().GetTimeDiff(), GetWorld()->GetWorldSimulationEnabled());
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassSurfaceComponent);
