#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/World/World.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginVegetation/Brush/VegetationBrushHandler.h>
#include <EnginePluginVegetation/Brush/VegetationCoverageOverlay.h>
#include <Foundation/Math/Random.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>
#include <VegetationPlugin/Placement/VegetationScatter.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

plHashTable<plUuid, plVegetationBrushHandler::Ghost> plVegetationBrushHandler::s_Ghosts;
plHashTable<plUuid, plVegetationBrushHandler::Overlay> plVegetationBrushHandler::s_Overlays;
plHashTable<plUuid, plVegetationBrushHandler::StrokePreview> plVegetationBrushHandler::s_Previews;

namespace
{
  /// Casts vertical (layer-space) columns against static collision. Positions and results are in layer space; the
  /// cast runs in world space along the layer's up axis.
  struct GrassColumnCaster
  {
    const plPhysicsWorldModuleInterface* m_pPhysics = nullptr;
    plPhysicsQueryParameters m_Params = plPhysicsQueryParameters(0, plPhysicsShapeType::Static);
    plTransform m_Layer = plTransform::MakeIdentity();
    plTransform m_InvLayer = plTransform::MakeIdentity();
    plVec3 m_vUp = plVec3(0, 0, 1); ///< layer up in world space
    float m_fReach = 0.5f;          ///< world units above and below the reference point

    bool m_bHasPlane = false; ///< brush: reference points are projected onto the pick plane first
    plVec3 m_vPlanePoint = plVec3::MakeZero();
    plVec3 m_vPlaneNormal = plVec3(0, 0, 1);

    void Init(plWorld* pWorld, const plTransform& layer, float fReach)
    {
      m_pPhysics = pWorld->GetModuleReadOnly<plPhysicsWorldModuleInterface>();
      m_Layer = layer;
      m_InvLayer = layer.GetInverse();
      m_vUp = layer.m_qRotation * plVec3(0, 0, 1);
      if (m_vUp.NormalizeIfNotZero(plVec3(0, 0, 1)).Failed())
        m_vUp.Set(0, 0, 1);
      m_fReach = fReach;
    }

    /// Returns true on a collision hit. Without a hit the outputs are the reference point (on the pick plane when
    /// there is one) and the plane normal, or false with outputs untouched when there is no plane either.
    bool Cast(float x, float y, float fReferenceZ, float& out_fHeight, plVec3& out_vNormal) const
    {
      plVec3 vPoint = m_Layer.TransformPosition(plVec3(x, y, fReferenceZ));

      if (m_bHasPlane)
      {
        const float fDenom = m_vUp.Dot(m_vPlaneNormal);
        if (plMath::Abs(fDenom) > 0.05f)
        {
          vPoint += m_vUp * ((m_vPlanePoint - vPoint).Dot(m_vPlaneNormal) / fDenom);
        }
      }

      if (m_pPhysics != nullptr)
      {
        plPhysicsCastResult hit;
        if (m_pPhysics->Raycast(hit, vPoint + m_vUp * m_fReach, -m_vUp, m_fReach * 2.0f, m_Params))
        {
          out_fHeight = m_InvLayer.TransformPosition(hit.m_vPosition).z;
          out_vNormal = (m_InvLayer.m_qRotation * hit.m_vNormal).GetNormalized();
          return true;
        }
      }

      if (m_bHasPlane)
      {
        out_fHeight = m_InvLayer.TransformPosition(vPoint).z;
        out_vNormal = (m_InvLayer.m_qRotation * m_vPlaneNormal).GetNormalized();
      }

      return false;
    }
  };

  float GetDiscontinuityThreshold(float fTexelSize)
  {
    return plMath::Max(0.15f, fTexelSize * 0.5f);
  }

  /// Lazily evaluated centre and corner samples of the texels under one brush stamp, shared by every grass type of the
  /// request so each column is cast once.
  struct GrassStampSampler
  {
    struct Centre
    {
      bool m_bDone = false;
      bool m_bValid = false;
      plGrassCoverageSample m_Sample;
    };

    struct Corner
    {
      bool m_bDone = false;
      float m_fHeight = 0.0f;
    };

    const GrassColumnCaster* m_pCaster = nullptr;
    float m_fTexel = 0.5f;
    float m_fReferenceZ = 0.0f;
    float m_fCosMaxSlope = 0.0f;
    bool m_bRejectMisses = false; ///< Fill: a column without ground gets no grass (the brush falls back to its pick plane)
    plInt32 m_iX0 = 0;
    plInt32 m_iY0 = 0;
    plInt32 m_iCountX = 0;
    plInt32 m_iCountY = 0;
    plDynamicArray<Centre> m_Centres;
    plDynamicArray<Corner> m_Corners;

    float GetCorner(plInt32 iX, plInt32 iY, float fFallback)
    {
      Corner& c = m_Corners[(iY - m_iY0) * (m_iCountX + 1) + (iX - m_iX0)];
      if (!c.m_bDone)
      {
        c.m_bDone = true;
        plVec3 vNormal;
        c.m_fHeight = fFallback;
        m_pCaster->Cast(iX * m_fTexel, iY * m_fTexel, m_fReferenceZ, c.m_fHeight, vNormal);
      }
      return c.m_fHeight;
    }

    bool Sample(const plVec2& vPosition, plGrassCoverageSample& out_sample)
    {
      const plInt32 iX = static_cast<plInt32>(plMath::Floor(vPosition.x / m_fTexel));
      const plInt32 iY = static_cast<plInt32>(plMath::Floor(vPosition.y / m_fTexel));
      if (iX < m_iX0 || iY < m_iY0 || iX >= m_iX0 + m_iCountX || iY >= m_iY0 + m_iCountY)
        return false;

      Centre& centre = m_Centres[(iY - m_iY0) * m_iCountX + (iX - m_iX0)];
      if (!centre.m_bDone)
      {
        centre.m_bDone = true;
        centre.m_Sample.m_fHeight = m_fReferenceZ;
        const bool bHit = m_pCaster->Cast(vPosition.x, vPosition.y, m_fReferenceZ, centre.m_Sample.m_fHeight, centre.m_Sample.m_vNormal);

        centre.m_bValid = (bHit || !m_bRejectMisses) && centre.m_Sample.m_vNormal.z >= m_fCosMaxSlope;
        if (centre.m_bValid)
        {
          // four-corner test: corners that disagree mark the texel, which keeps its centre sample
          const float fH = centre.m_Sample.m_fHeight;
          const float h0 = GetCorner(iX, iY, fH);
          const float h1 = GetCorner(iX + 1, iY, fH);
          const float h2 = GetCorner(iX, iY + 1, fH);
          const float h3 = GetCorner(iX + 1, iY + 1, fH);
          const float fMin = plMath::Min(plMath::Min(h0, h1), plMath::Min(h2, h3));
          const float fMax = plMath::Max(plMath::Max(h0, h1), plMath::Max(h2, h3));
          centre.m_Sample.m_bDiscontinuous = (fMax - fMin) > GetDiscontinuityThreshold(m_fTexel);
        }
      }

      out_sample = centre.m_Sample;
      return centre.m_bValid;
    }
  };

  /// Most plants one Fill places; a larger request is cut to this many and says so in the log.
  constexpr plUInt32 s_uiMaxFillInstances = 250000;

  /// Fill casts top-down from this far above the highest picked corner.
  constexpr float s_fFillCastHeight = 1000.0f;

  /// Placement rules of one checked plant type for a request.
  struct SpeciesRules
  {
    plString m_sType;
    float m_fWeight = 1.0f;
    float m_fMinSpacing = 0.4f;
    float m_fClearingAvoidance = 0.0f;
    float m_fMinScale = 0.8f;
    float m_fMaxScale = 1.25f;
    float m_fAlignToNormal = 0.6f;
    float m_fSink = 0.05f;
    float m_fCosMaxSlope = 0.7f;
  };

  bool GetTypePlacement(plStringView sType, plVegetationPlacementSettings& out_placement)
  {
    if (sType.IsEmpty())
      return false;

    plVegetationTypeResourceHandle hType = plResourceManager::LoadResource<plVegetationTypeResource>(sType);
    plResourceLock<plVegetationTypeResource> pType(hType, plResourceAcquireMode::BlockTillLoaded_NeverFail);
    if (pType.GetAcquireResult() != plResourceAcquireResult::Final)
      return false;

    out_placement = pType->GetPlacement();
    return true;
  }

  /// Density weight and clearing avoidance always come from the type; slope, scale, spacing, sink and alignment from
  /// the type unless the brush overrides them.
  void ResolveSpecies(const plVegetationStrokeRequest& request, plDynamicArray<SpeciesRules>& out_species)
  {
    out_species.Clear();

    for (plUInt32 i = 0; i < request.m_Types.GetCount(); ++i)
    {
      SpeciesRules& rules = out_species.ExpandAndGetRef();
      rules.m_sType = request.m_Types[i];

      const float fPanelWeight = i < request.m_Weights.GetCount() ? plMath::Max(request.m_Weights[i], 0.0f) : 1.0f;

      plVegetationPlacementSettings placement;
      const bool bType = GetTypePlacement(rules.m_sType, placement);

      rules.m_fWeight = fPanelWeight * (bType ? plMath::Max(placement.m_fDensityWeight, 0.0f) : 1.0f);
      rules.m_fClearingAvoidance = bType ? plMath::Max(placement.m_fClearingAvoidance, 0.0f) : 0.0f;

      if (request.m_bUseTypeRules && bType)
      {
        rules.m_fMinSpacing = placement.m_fMinSpacing;
        rules.m_fMinScale = placement.m_fMinScale;
        rules.m_fMaxScale = placement.m_fMaxScale;
        rules.m_fAlignToNormal = placement.m_fAlignToNormal;
        rules.m_fSink = placement.m_fSink;
        rules.m_fCosMaxSlope = plMath::Cos(placement.m_MaxSlope);
      }
      else
      {
        rules.m_fMinSpacing = request.m_fMinSpacing;
        rules.m_fMinScale = request.m_fMinScale;
        rules.m_fMaxScale = request.m_fMaxScale;
        rules.m_fAlignToNormal = request.m_fAlignToNormal;
        rules.m_fSink = request.m_fSink;
        rules.m_fCosMaxSlope = plMath::Cos(request.m_MaxSlope);
      }

      rules.m_fMinSpacing = plMath::Max(rules.m_fMinSpacing, 0.0f);
      rules.m_fMaxScale = plMath::Max(rules.m_fMaxScale, rules.m_fMinScale);
    }
  }

  plUInt32 PickSpecies(plArrayPtr<const SpeciesRules> species, plRandom& ref_rng)
  {
    float fWeightSum = 0.0f;
    for (const auto& rules : species)
      fWeightSum += rules.m_fWeight;

    if (fWeightSum <= 0.0f)
      return 0;

    float fPick = ref_rng.FloatZeroToOneExclusive() * fWeightSum;
    plUInt32 uiPick = 0;
    for (plUInt32 i = 0; i < species.GetCount(); ++i)
    {
      if (species[i].m_fWeight <= 0.0f)
        continue;

      uiPick = i;
      fPick -= species[i].m_fWeight;
      if (fPick <= 0.0f)
        break;
    }
    return uiPick;
  }

  plVegetationLayerInstance MakeInstance(const SpeciesRules& rules, const plVec3& vPosition, const plVec3& vSurfaceNormal, plRandom& ref_rng)
  {
    const plVec3 vUp(0, 0, 1);
    plVec3 vAlign = plMath::Lerp(vUp, vSurfaceNormal, plMath::Clamp(rules.m_fAlignToNormal, 0.0f, 1.0f));
    if (vAlign.NormalizeIfNotZero(vUp).Failed())
      vAlign = vUp;

    const plQuat qAlign = plQuat::MakeShortestRotation(vUp, vAlign);
    const plQuat qYaw = plQuat::MakeFromAxisAndAngle(vUp, plAngle::MakeFromRadian(ref_rng.FloatZeroToOneExclusive() * 2.0f * plMath::Pi<float>()));

    plVegetationLayerInstance inst;
    inst.m_vPosition = vPosition - vSurfaceNormal * rules.m_fSink;
    inst.m_qRotation = qAlign * qYaw;
    inst.m_fScale = plMath::Lerp(rules.m_fMinScale, rules.m_fMaxScale, ref_rng.FloatZeroToOneExclusive());
    inst.m_uiColorSeed = static_cast<plUInt8>(ref_rng.UInt() & 0xFF);
    return inst;
  }

  /// The existing plants of a layer in world space, each with the spacing its type keeps.
  struct ExistingPlants
  {
    plDynamicArray<plVec3> m_Positions;
    plDynamicArray<float> m_Spacings;
    plDynamicArray<plUInt32> m_Species; ///< index into the request's species, plInvalidIndex for an unchecked type
    float m_fMaxSpacing = 0.0f;

    /// bLayerSpace keeps the positions in the layer's local space instead of transforming them to world space.
    void Gather(const plVegetationLayerData* pData, const plVegetationStrokeRequest& request, plArrayPtr<const SpeciesRules> species, bool bLayerSpace = false)
    {
      if (pData == nullptr)
        return;

      for (const auto& entry : pData->m_Entries)
      {
        if (entry.m_Instances.IsEmpty())
          continue;

        plUInt32 uiSpecies = plInvalidIndex;
        float fSpacing = request.m_fMinSpacing;
        for (plUInt32 s = 0; s < species.GetCount(); ++s)
        {
          if (species[s].m_sType == entry.m_sType)
          {
            uiSpecies = s;
            fSpacing = species[s].m_fMinSpacing;
          }
        }

        if (uiSpecies == plInvalidIndex && request.m_bUseTypeRules)
        {
          plVegetationPlacementSettings placement;
          if (GetTypePlacement(entry.m_sType, placement))
            fSpacing = placement.m_fMinSpacing;
        }

        fSpacing = plMath::Max(fSpacing, 0.0f);
        m_fMaxSpacing = plMath::Max(m_fMaxSpacing, fSpacing);

        for (const auto& inst : entry.m_Instances)
        {
          m_Positions.PushBack(bLayerSpace ? inst.m_vPosition : request.m_LayerTransform.TransformPosition(inst.m_vPosition));
          m_Spacings.PushBack(fSpacing);
          m_Species.PushBack(uiSpecies);
        }
      }
    }
  };

  float GetMaxSpacing(plArrayPtr<const SpeciesRules> species)
  {
    float fMax = 0.0f;
    for (const auto& rules : species)
      fMax = plMath::Max(fMax, rules.m_fMinSpacing);
    return fMax;
  }
} // namespace

void plVegetationBrushHandler::Startup()
{
  plEngineProcessDocumentContext::s_ToolConfigEvents.AddEventHandler(OnToolConfig);
}

void plVegetationBrushHandler::Shutdown()
{
  plEngineProcessDocumentContext::s_ToolConfigEvents.RemoveEventHandler(OnToolConfig);
  s_Ghosts.Clear();
  s_Overlays.Clear();
  s_Previews.Clear();
}

void plVegetationBrushHandler::OnToolConfig(const plEngineProcessToolConfigEvent& e)
{
  if (e.m_pContext == nullptr || e.m_pMsg == nullptr)
    return;

  if (e.m_pMsg->m_sWhatToDo == plVegetationStrokeRequest::s_szWhatToDo)
  {
    HandleStroke(e);
  }
  else if (e.m_pMsg->m_sWhatToDo == plVegetationResnapRequest::s_szWhatToDo)
  {
    HandleResnap(e);
  }
  else if (e.m_pMsg->m_sWhatToDo == plVegetationOverlayMsg::s_szWhatToDo)
  {
    HandleOverlay(e);
  }
  else if (e.m_pMsg->m_sWhatToDo == plVegetationDropPreviewMsg::s_szWhatToDo)
  {
    HandleDropPreview(e);
  }
}

void plVegetationBrushHandler::HandleStroke(const plEngineProcessToolConfigEvent& e)
{
  if (!e.m_pMsg->m_PayloadValue.IsA<plDataBuffer>())
    return;

  plVegetationStrokeRequest request;
  if (request.FromBuffer(e.m_pMsg->m_PayloadValue.Get<plDataBuffer>()).Failed())
    return;

  if (request.m_bPreview)
  {
    UpdateGhost(e.m_pContext, request);
    return;
  }

  plVegetationStrokeResult result;
  result.m_LayerGuid = request.m_LayerGuid;
  result.m_uiStrokeId = request.m_uiStrokeId;
  result.m_uiSegment = request.m_uiSegment;
  result.m_GrassDelta.m_fTexelSize = plGrassCoverageData::ClampTexelSize(request.m_fTexelSize);

  plWorld* pWorld = e.m_pContext->GetWorld();
  if (pWorld != nullptr)
  {
    PL_LOCK(pWorld->GetWriteMarker());

    // The target layer as it stands: with a running stroke's preview it already holds the stroke's earlier stamps.
    const plVegetationLayerData* pExisting = nullptr;
    {
      const plComponentHandle hLayer = request.m_LayerGuid.IsValid() ? e.m_pContext->GetContext().m_ComponentMap.GetHandle(request.m_LayerGuid) : plComponentHandle();
      plVegetationLayerComponent* pLayer = nullptr;
      if (pWorld->TryGetComponent(hLayer, pLayer))
        pExisting = &pLayer->GetLayerData();
    }

    if (request.m_uiMode == plVegetationBrushMode::Paint || request.m_uiMode == plVegetationBrushMode::Place)
    {
      EvaluatePaint(pWorld, request, result, pExisting);
      EvaluateGrass(pWorld, request, result);
    }
    else if (request.m_uiMode == plVegetationBrushMode::Fill)
    {
      EvaluateFill(pWorld, request, result, pExisting);
    }

    if (request.m_bStrokePreview)
    {
      MirrorStroke(e.m_pContext, pWorld, request, result);
    }
  }

  // Erase and Thin are evaluated by the editor; those stamps only feed the preview.
  if (request.m_bMirrorOnly)
    return;

  plSimpleDocumentConfigMsgToEditor msg;
  msg.m_DocumentGuid = e.m_pMsg->m_DocumentGuid;
  msg.m_sWhatToDo = plVegetationStrokeResult::s_szWhatToDo;

  plDataBuffer buffer;
  result.ToBuffer(buffer);
  msg.m_PayloadValue = buffer;

  e.m_pContext->SendProcessMessage(&msg);
}

void plVegetationBrushHandler::EvaluatePaint(plWorld* pWorld, const plVegetationStrokeRequest& request, plVegetationStrokeResult& out_result, const plVegetationLayerData* pExisting)
{
  if (request.m_Types.IsEmpty())
    return;

  const plPhysicsWorldModuleInterface* pPhysics = pWorld->GetModuleReadOnly<plPhysicsWorldModuleInterface>();
  const plPhysicsQueryParameters params(0, plPhysicsShapeType::Static);

  plHybridArray<SpeciesRules, 8> species;
  ResolveSpecies(request, species);

  plVec3 vNormal = request.m_vNormal;
  if (vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).Failed())
    vNormal.Set(0, 0, 1);

  const float fRadius = plMath::Max(request.m_fRadius, 0.05f);

  // Snaps a point of the pick's tangent plane to collision within a short reach around that plane. A long cast would
  // fall through the picked surface onto whatever lies below it (the floor under a greybox, for instance) whenever the
  // picked mesh has no collider of its own. Without a hit the point stays on the plane.
  auto Snap = [&](const plVec3& vPoint, float fReach, plVec3& out_vPosition, plVec3& out_vNormal)
  {
    out_vPosition = vPoint;
    out_vNormal = vNormal;

    plPhysicsCastResult hit;
    if (pPhysics != nullptr && pPhysics->Raycast(hit, vPoint + vNormal * fReach, -vNormal, fReach * 2.0f, params))
    {
      out_vPosition = hit.m_vPosition;
      out_vNormal = hit.m_vNormal;
    }
  };

  // Place stamps exactly one instance at the pick, where the artist clicked: no spacing, no clearings.
  if (request.m_uiMode == plVegetationBrushMode::Place)
  {
    plRandom rng;
    rng.Initialize(request.m_uiSeed);

    plVec3 vPosition, vSurfaceNormal;
    Snap(request.m_vCenter, 0.5f, vPosition, vSurfaceNormal);

    const plUInt32 uiSpecies = PickSpecies(species, rng);
    if (vSurfaceNormal.z < species[uiSpecies].m_fCosMaxSlope)
      return;

    out_result.m_Added.GetOrAddEntry(species[uiSpecies].m_sType).m_Instances.PushBack(MakeInstance(species[uiSpecies], vPosition, vSurfaceNormal, rng));
    return;
  }

  // Paint scatters on the pick's tangent plane: 2D coordinates along two tangents around the pick.
  const plVec3 vTangent1 = vNormal.GetOrthogonalVector().GetNormalized();
  const plVec3 vTangent2 = vNormal.CrossRH(vTangent1).GetNormalized();
  auto ToPlane = [&](const plVec3& vPoint)
  {
    const plVec3 v = vPoint - request.m_vCenter;
    return plVec2(v.Dot(vTangent1), v.Dot(vTangent2));
  };

  ExistingPlants existing;
  existing.Gather(pExisting, request, species);

  const float fReach = plVegetationScatter::GetPairSpacing(GetMaxSpacing(species), plMath::Max(GetMaxSpacing(species), existing.m_fMaxSpacing));

  plDynamicArray<plVegetationScatterObstacle> obstacles;
  for (plUInt32 i = 0; i < existing.m_Positions.GetCount(); ++i)
  {
    if ((existing.m_Positions[i] - request.m_vCenter).GetLength() > fRadius + fReach + 1.0f)
      continue;

    plVegetationScatterObstacle& obstacle = obstacles.ExpandAndGetRef();
    obstacle.m_vPosition = ToPlane(existing.m_Positions[i]);
    obstacle.m_fMinSpacing = existing.m_Spacings[i];
  }

  // Clearings are discs in the layer plane, taken at the height of the pick.
  plDynamicArray<plVegetationScatterClearing> clearings;
  if (pExisting != nullptr && !pExisting->m_Clearings.IsEmpty())
  {
    const float fLocalZ = request.m_LayerTransform.GetInverse().TransformPosition(request.m_vCenter).z;
    const plVec2 vAxisScale = request.m_LayerTransform.m_vScale.Abs().GetAsVec2();
    const float fScale = 0.5f * (vAxisScale.x + vAxisScale.y);

    for (const auto& clearing : pExisting->m_Clearings)
    {
      const plVec3 vCenter = request.m_LayerTransform.TransformPosition(plVec3(clearing.m_vCenter.x, clearing.m_vCenter.y, fLocalZ));
      const float fWorldRadius = clearing.m_fRadius * fScale;
      if ((vCenter - request.m_vCenter).GetLength() > fRadius + fWorldRadius + 100.0f)
        continue;

      plVegetationScatterClearing& c = clearings.ExpandAndGetRef();
      c.m_vCenter = ToPlane(vCenter);
      c.m_fRadius = fWorldRadius;
    }
  }

  plHybridArray<plVegetationScatterSpecies, 8> scatterSpecies;
  for (const auto& rules : species)
  {
    plVegetationScatterSpecies& s = scatterSpecies.ExpandAndGetRef();
    s.m_fWeight = rules.m_fWeight;
    s.m_fMinSpacing = rules.m_fMinSpacing;
    s.m_fClearingAvoidance = rules.m_fClearingAvoidance;
  }

  // Falloff: candidates in the outer ring survive with a probability that fades to zero at the edge.
  plRandom maskRng;
  maskRng.Initialize(request.m_uiSeed ^ 0x9E3779B9u);
  const float fFalloff = plMath::Clamp(request.m_fFalloff, 0.0f, 1.0f);
  const float fInner = 1.0f - fFalloff;

  plDynamicArray<plVec3> hitPositions;
  plDynamicArray<plVec3> hitNormals;
  const float fSnapReach = plMath::Max(0.25f, fRadius * 0.5f);

  plVegetationScatterDesc desc;
  desc.m_vMin = plVec2(-fRadius);
  desc.m_vMax = plVec2(fRadius);
  desc.m_fDensity = request.m_fDensity * 1.5f; // 1.5 per m^2 at density 1
  desc.m_uiMaxPoints = 652;                     // about 512 inside the disc
  desc.m_bTopUp = false;                        // falloff and spacing thin a stamp; nothing makes up for it
  desc.m_uiSeed = request.m_uiSeed;
  desc.m_Species = scatterSpecies;
  desc.m_Obstacles = obstacles;
  desc.m_Clearings = clearings;
  desc.m_RegionMask = [&](const plVec2& v) -> bool
  {
    const float fRing = v.GetLength() / fRadius;
    if (fRing > 1.0f)
      return false;
    if (fRing > fInner && fFalloff > 0.0f && maskRng.FloatZeroToOneExclusive() > (1.0f - fRing) / fFalloff)
      return false;
    return true;
  };
  desc.m_Accept = [&](const plVec2& v, plUInt32 uiSpecies) -> bool
  {
    plVec3 vPosition, vSurfaceNormal;
    Snap(request.m_vCenter + vTangent1 * v.x + vTangent2 * v.y, fSnapReach, vPosition, vSurfaceNormal);
    if (vSurfaceNormal.z < species[uiSpecies].m_fCosMaxSlope)
      return false;

    hitPositions.PushBack(vPosition);
    hitNormals.PushBack(vSurfaceNormal);
    return true;
  };

  plDynamicArray<plVegetationScatterPoint> points;
  plVegetationScatter::Scatter(desc, points);

  plRandom rng;
  rng.Initialize(request.m_uiSeed + 1);

  for (plUInt32 i = 0; i < points.GetCount(); ++i)
  {
    const SpeciesRules& rules = species[points[i].m_uiSpecies];
    out_result.m_Added.GetOrAddEntry(rules.m_sType).m_Instances.PushBack(MakeInstance(rules, hitPositions[i], hitNormals[i], rng));
  }
}

namespace
{
  /// Fill works in the layer's local space: the region, the clearings and the coverage are there. Spacing and
  /// distances are world metres, so they shrink by the layer's average xy scale; the ground is cast along the layer's
  /// up axis from far above the region.
  struct FillSpace
  {
    plVec3 m_vMin = plVec3::MakeZero();
    plVec3 m_vMax = plVec3::MakeZero();
    float m_fScale = 1.0f;        ///< world metres per layer unit in xy
    float m_fAreaScale = 1.0f;    ///< world m^2 per layer unit^2
    float m_fReferenceZ = 0.0f;
    GrassColumnCaster m_Caster;
    bool m_bHasPhysics = false;

    void Init(plWorld* pWorld, const plVegetationStrokeRequest& request)
    {
      m_vMin = request.m_vFillMin.CompMin(request.m_vFillMax);
      m_vMax = request.m_vFillMin.CompMax(request.m_vFillMax);

      const plVec2 vAxisScale = request.m_LayerTransform.m_vScale.Abs().GetAsVec2().CompMax(plVec2(0.0001f));
      m_fScale = 0.5f * (vAxisScale.x + vAxisScale.y);
      m_fAreaScale = vAxisScale.x * vAxisScale.y;
      m_fReferenceZ = 0.5f * (m_vMin.z + m_vMax.z);

      const float fUpScale = plMath::Max(plMath::Abs(request.m_LayerTransform.m_vScale.z), 0.0001f);
      m_Caster.Init(pWorld, request.m_LayerTransform, s_fFillCastHeight + 0.5f * (m_vMax.z - m_vMin.z) * fUpScale);
      m_bHasPhysics = m_Caster.m_pPhysics != nullptr;
    }

    /// Ground under a layer-space column; without physics the reference height and the layer's up.
    bool Cast(const plVec2& v, float& out_fHeight, plVec3& out_vNormal) const
    {
      if (!m_bHasPhysics)
      {
        out_fHeight = m_fReferenceZ;
        out_vNormal.Set(0, 0, 1);
        return true;
      }

      return m_Caster.Cast(v.x, v.y, m_fReferenceZ, out_fHeight, out_vNormal);
    }
  };

  void FillPlants(const plVegetationStrokeRequest& request, const FillSpace& space, const plVegetationLayerData* pExisting, plVegetationStrokeResult& out_result);
  void FillGrass(const plVegetationStrokeRequest& request, const FillSpace& space, const plVegetationLayerData* pExisting, plVegetationStrokeResult& out_result);
} // namespace

void plVegetationBrushHandler::EvaluateFill(plWorld* pWorld, const plVegetationStrokeRequest& request, plVegetationStrokeResult& out_result, const plVegetationLayerData* pExisting)
{
  if (request.m_Types.IsEmpty() && request.m_GrassTypes.IsEmpty())
    return;

  PL_LOG_BLOCK("Vegetation Fill");

  FillSpace space;
  space.Init(pWorld, request);
  if (space.m_vMax.x <= space.m_vMin.x || space.m_vMax.y <= space.m_vMin.y)
    return;

  // With Replace the region's plants of the fill types are gone already, so they neither count nor keep spacing.
  plVegetationLayerData replaced;
  if (request.m_bFillReplace && pExisting != nullptr)
  {
    replaced = *pExisting;
    request.ApplyFillReplace(replaced, nullptr);
    pExisting = &replaced;
  }

  if (!request.m_Types.IsEmpty())
    FillPlants(request, space, pExisting, out_result);

  if (!request.m_GrassTypes.IsEmpty())
    FillGrass(request, space, pExisting, out_result);
}

namespace
{
void FillPlants(const plVegetationStrokeRequest& request, const FillSpace& space, const plVegetationLayerData* pExisting, plVegetationStrokeResult& out_result)
{
  plHybridArray<SpeciesRules, 8> species;
  ResolveSpecies(request, species);

  const plVec3 vMin = space.m_vMin;
  const plVec3 vMax = space.m_vMax;
  const plVec2 vSize = (vMax - vMin).GetAsVec2();
  const double fArea = static_cast<double>(vSize.x) * vSize.y * space.m_fAreaScale; // world m^2
  const float fInvScale = 1.0f / space.m_fScale;

  ExistingPlants existing;
  existing.Gather(pExisting, request, species, true);

  const float fReach = fInvScale * plVegetationScatter::GetPairSpacing(GetMaxSpacing(species), plMath::Max(GetMaxSpacing(species), existing.m_fMaxSpacing));

  // Existing plants of the checked types inside the rectangle count toward the density; everything nearby keeps its
  // spacing.
  plUInt32 uiExistingInside = 0;
  plDynamicArray<plVegetationScatterObstacle> obstacles;
  for (plUInt32 i = 0; i < existing.m_Positions.GetCount(); ++i)
  {
    const plVec2 v = existing.m_Positions[i].GetAsVec2();
    if (v.x < vMin.x - fReach || v.y < vMin.y - fReach || v.x > vMax.x + fReach || v.y > vMax.y + fReach)
      continue;

    if (existing.m_Species[i] != plInvalidIndex && v.x >= vMin.x && v.y >= vMin.y && v.x <= vMax.x && v.y <= vMax.y)
      ++uiExistingInside;

    plVegetationScatterObstacle& obstacle = obstacles.ExpandAndGetRef();
    obstacle.m_vPosition = v;
    obstacle.m_fMinSpacing = existing.m_Spacings[i] * fInvScale;
  }

  // clearings are layer-space discs already
  plDynamicArray<plVegetationScatterClearing> clearings;
  if (pExisting != nullptr)
  {
    for (const auto& clearing : pExisting->m_Clearings)
    {
      plVegetationScatterClearing& c = clearings.ExpandAndGetRef();
      c.m_vCenter = clearing.m_vCenter;
      c.m_fRadius = clearing.m_fRadius;
    }
  }

  const double fWanted = plMath::Max(0.0, static_cast<double>(request.m_fTreesPerHectare) * fArea / 10000.0 - uiExistingInside);
  if (fWanted < 0.5)
  {
    plLog::Info("The rectangle already holds {} plants of the checked types; nothing to fill.", uiExistingInside);
    return;
  }

  if (fWanted > s_uiMaxFillInstances)
    plLog::Warning("Fill asks for {} plants; placing at most {}. Fill a smaller rectangle or lower Trees / ha.", static_cast<plUInt64>(fWanted), s_uiMaxFillInstances);

  plHybridArray<plVegetationScatterSpecies, 8> scatterSpecies;
  for (const auto& rules : species)
  {
    plVegetationScatterSpecies& s = scatterSpecies.ExpandAndGetRef();
    s.m_fWeight = rules.m_fWeight;
    s.m_fMinSpacing = rules.m_fMinSpacing * fInvScale;
    s.m_fClearingAvoidance = rules.m_fClearingAvoidance * fInvScale;
  }

  const plTransform& layer = request.m_LayerTransform;

  plDynamicArray<plVec3> hitPositions;
  plDynamicArray<plVec3> hitNormals;
  hitPositions.Reserve(static_cast<plUInt32>(plMath::Min(fWanted, static_cast<double>(s_uiMaxFillInstances))));
  hitNormals.Reserve(hitPositions.GetCapacity());

  plUInt32 uiMissed = 0;
  plUInt32 uiTooSteep = 0;

  plVegetationScatterDesc desc;
  desc.m_vMin = vMin.GetAsVec2();
  desc.m_vMax = vMax.GetAsVec2();
  desc.m_fDensity = static_cast<float>(fWanted / (static_cast<double>(vSize.x) * vSize.y)); // per layer unit^2
  desc.m_uiMaxPoints = s_uiMaxFillInstances;
  desc.m_uiSeed = request.m_uiSeed;
  desc.m_Species = scatterSpecies;
  desc.m_Obstacles = obstacles;
  desc.m_Clearings = clearings;
  desc.m_Accept = [&](const plVec2& v, plUInt32 uiSpecies) -> bool
  {
    float fHeight = 0.0f;
    plVec3 vLocalNormal(0, 0, 1);
    if (!space.Cast(v, fHeight, vLocalNormal))
    {
      ++uiMissed;
      return false;
    }

    const plVec3 vPosition = layer.TransformPosition(plVec3(v.x, v.y, fHeight));
    plVec3 vSurfaceNormal = layer.m_qRotation * vLocalNormal;
    vSurfaceNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();

    if (vSurfaceNormal.z < species[uiSpecies].m_fCosMaxSlope)
    {
      ++uiTooSteep;
      return false;
    }

    hitPositions.PushBack(vPosition);
    hitNormals.PushBack(vSurfaceNormal);
    return true;
  };

  const plTime start = plTime::Now();

  plDynamicArray<plVegetationScatterPoint> points;
  plVegetationScatter::Scatter(desc, points);

  plRandom rng;
  rng.Initialize(request.m_uiSeed + 1);

  for (plUInt32 i = 0; i < points.GetCount(); ++i)
  {
    const SpeciesRules& rules = species[points[i].m_uiSpecies];
    out_result.m_Added.GetOrAddEntry(rules.m_sType).m_Instances.PushBack(MakeInstance(rules, hitPositions[i], hitNormals[i], rng));
  }

  plLog::Info("Placed {} of {} plants over {} ha in {} ms ({} casts without ground, {} too steep).", points.GetCount(), static_cast<plUInt64>(fWanted + 0.5), plArgF(fArea / 10000.0, 2), plArgF((plTime::Now() - start).GetMilliseconds(), 0), uiMissed, uiTooSteep);
}

/// Most coverage texels one Fill writes per grass type.
constexpr plUInt64 s_uiMaxFillTexels = 16ull * 1024ull * 1024ull;

/// Texels per side of one block: the column cache of a block is bounded, columns are shared by the grass types.
constexpr plInt32 s_iFillTexelBlock = 256;

void FillGrass(const plVegetationStrokeRequest& request, const FillSpace& space, const plVegetationLayerData* pExisting, plVegetationStrokeResult& out_result)
{
  plGrassCoverageData& delta = out_result.m_GrassDelta;
  delta.m_fTexelSize = plGrassCoverageData::ClampTexelSize(request.m_fTexelSize);

  plInt32 iX0, iY0, iX1, iY1;
  if (!delta.GetTexelRect(space.m_vMin.GetAsVec2(), space.m_vMax.GetAsVec2(), iX0, iY0, iX1, iY1))
    return;

  const plUInt64 uiTexels = static_cast<plUInt64>(iX1 - iX0 + 1) * static_cast<plUInt64>(iY1 - iY0 + 1);
  if (uiTexels > s_uiMaxFillTexels)
  {
    plLog::Warning("Grass Fill covers {} texels; filling at most {}. Fill a smaller region or raise the layer's Grass Texel Size.", uiTexels, s_uiMaxFillTexels);
    iY1 = iY0 + static_cast<plInt32>(s_uiMaxFillTexels / static_cast<plUInt64>(iX1 - iX0 + 1)) - 1;
  }

  // the same clearings as plants, without avoidance: grass types have none
  plDynamicArray<plVegetationScatterClearing> clearings;
  if (pExisting != nullptr)
  {
    for (const auto& clearing : pExisting->m_Clearings)
    {
      plVegetationScatterClearing& c = clearings.ExpandAndGetRef();
      c.m_vCenter = clearing.m_vCenter;
      c.m_fRadius = clearing.m_fRadius;
    }
  }

  // The engine's grass sampler, with the fill's long reach: centre and corner columns per texel, slope test,
  // discontinuity flag.
  const plTime start = plTime::Now();
  plUInt32 uiWritten = 0;

  for (plInt32 iBlockY = iY0; iBlockY <= iY1; iBlockY += s_iFillTexelBlock)
  {
    for (plInt32 iBlockX = iX0; iBlockX <= iX1; iBlockX += s_iFillTexelBlock)
    {
      GrassStampSampler sampler;
      sampler.m_pCaster = &space.m_Caster;
      sampler.m_fTexel = delta.m_fTexelSize;
      sampler.m_fReferenceZ = space.m_fReferenceZ;
      sampler.m_fCosMaxSlope = plMath::Cos(request.m_MaxSlope);
      sampler.m_bRejectMisses = true;
      sampler.m_iX0 = iBlockX;
      sampler.m_iY0 = iBlockY;
      sampler.m_iCountX = plMath::Min(s_iFillTexelBlock, iX1 - iBlockX + 1);
      sampler.m_iCountY = plMath::Min(s_iFillTexelBlock, iY1 - iBlockY + 1);
      sampler.m_Centres.SetCount(sampler.m_iCountX * sampler.m_iCountY);
      sampler.m_Corners.SetCount((sampler.m_iCountX + 1) * (sampler.m_iCountY + 1));

      GrassStampSampler* pSampler = &sampler;
      const FillSpace* pSpace = &space;
      const plDynamicArray<plVegetationScatterClearing>* pClearings = &clearings;
      const plGrassCoverageData::SampleFunc func = [pSampler, pSpace, pClearings](const plVec2& vPosition, plGrassCoverageSample& out_sample) -> bool
      {
        for (const auto& c : *pClearings)
        {
          if ((c.m_vCenter - vPosition).GetLengthSquared() < c.m_fRadius * c.m_fRadius)
            return false;
        }

        if (!pSpace->m_bHasPhysics)
        {
          out_sample.m_fHeight = pSpace->m_fReferenceZ;
          out_sample.m_vNormal.Set(0, 0, 1);
          out_sample.m_bDiscontinuous = false;
          return true;
        }

        return pSampler->Sample(vPosition, out_sample);
      };

      for (const plString& sType : request.m_GrassTypes)
      {
        if (!sType.IsEmpty())
          uiWritten += delta.PaintTexelRect(sType, iBlockX, iBlockY, iBlockX + sampler.m_iCountX - 1, iBlockY + sampler.m_iCountY - 1, request.m_fDensity, request.m_fLength, func);
      }
    }
  }

  plLog::Info("Filled {} grass texels of {} type(s) in {} ms.", uiWritten, request.m_GrassTypes.GetCount(), plArgF((plTime::Now() - start).GetMilliseconds(), 0));
}
} // namespace

void plVegetationBrushHandler::EvaluateGrass(plWorld* pWorld, const plVegetationStrokeRequest& request, plVegetationStrokeResult& out_result)
{
  // Place stamps plants only; Erase and Thin act on the coverage the editor already holds.
  if (request.m_GrassTypes.IsEmpty() || request.m_uiMode != plVegetationBrushMode::Paint)
    return;

  plGrassCoverageData& delta = out_result.m_GrassDelta;
  delta.m_fTexelSize = plGrassCoverageData::ClampTexelSize(request.m_fTexelSize);

  const float fRadius = plMath::Max(request.m_fRadius, 0.05f);

  // Same short reach around the pick plane as instances, so a mesh without collision does not let the cast fall
  // through onto whatever lies below it.
  GrassColumnCaster caster;
  caster.Init(pWorld, request.m_LayerTransform, plMath::Max(0.25f, fRadius * 0.5f));
  caster.m_bHasPlane = true;
  caster.m_vPlanePoint = request.m_vCenter;
  caster.m_vPlaneNormal = request.m_vNormal;
  if (caster.m_vPlaneNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).Failed())
    caster.m_vPlaneNormal.Set(0, 0, 1);

  const plVec3 vLocalCenter = caster.m_InvLayer.TransformPosition(request.m_vCenter);

  plGrassCoverageStamp stamp;
  stamp.m_vCenter.Set(vLocalCenter.x, vLocalCenter.y);
  // The world-space brush disc is an ellipse on a non-uniformly scaled layer.
  stamp.m_fRadius = fRadius;
  stamp.m_vAxisScale = request.m_LayerTransform.m_vScale.Abs().GetAsVec2();
  stamp.m_fFalloff = request.m_fFalloff;
  stamp.m_fDensity = request.m_fDensity;
  stamp.m_fStrength = request.m_fStrength;
  stamp.m_fLength = request.m_fLength;
  const plVec2 vLayerRadius = stamp.GetLayerRadius();

  GrassStampSampler sampler;
  sampler.m_pCaster = &caster;
  sampler.m_fTexel = delta.m_fTexelSize;
  sampler.m_fReferenceZ = vLocalCenter.z;
  sampler.m_fCosMaxSlope = plMath::Cos(request.m_MaxSlope);
  sampler.m_iX0 = delta.ToTexelCoord(stamp.m_vCenter.x - vLayerRadius.x);
  sampler.m_iY0 = delta.ToTexelCoord(stamp.m_vCenter.y - vLayerRadius.y);
  sampler.m_iCountX = delta.ToTexelCoord(stamp.m_vCenter.x + vLayerRadius.x) - sampler.m_iX0 + 1;
  sampler.m_iCountY = delta.ToTexelCoord(stamp.m_vCenter.y + vLayerRadius.y) - sampler.m_iY0 + 1;
  if (sampler.m_iCountX <= 0 || sampler.m_iCountY <= 0 || sampler.m_iCountX * sampler.m_iCountY > 1024 * 1024)
    return;

  sampler.m_Centres.SetCount(sampler.m_iCountX * sampler.m_iCountY);
  sampler.m_Corners.SetCount((sampler.m_iCountX + 1) * (sampler.m_iCountY + 1));

  GrassStampSampler* pSampler = &sampler;
  const plGrassCoverageData::SampleFunc func = [pSampler](const plVec2& vPosition, plGrassCoverageSample& out_sample) -> bool
  { return pSampler->Sample(vPosition, out_sample); };

  for (const plString& sType : request.m_GrassTypes)
  {
    if (!sType.IsEmpty())
    {
      delta.PaintInDisc(sType, stamp, func);
    }
  }
}

void plVegetationBrushHandler::EvaluateResnap(plWorld* pWorld, const plVegetationResnapRequest& request, plVegetationResnapResult& out_result)
{
  plGrassCoverageData coverage;
  if (coverage.FromBuffer(request.m_Coverage).Failed())
  {
    out_result.m_Coverage = request.m_Coverage;
    return;
  }

  const float fTexel = coverage.m_fTexelSize;

  // Columns are cast around each texel's stored height, far enough to find ground that moved since painting.
  GrassColumnCaster caster;
  caster.Init(pWorld, request.m_LayerTransform, plMath::Max(2.0f, fTexel * 4.0f));

  for (plGrassCoverageEntry& entry : coverage.m_Entries)
  {
    for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      plGrassCoverageTile& tile = it.Value();

      for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
      {
        plGrassCoverageTexel& texel = tile.m_Texels[i];
        if (texel.m_uiDensity == 0)
          continue;

        const plInt32 iX = tile.m_iX * 16 + static_cast<plInt32>(i % 16);
        const plInt32 iY = tile.m_iY * 16 + static_cast<plInt32>(i / 16);
        const plVec2 vCenter = coverage.GetTexelCenter(iX, iY);
        const float fReference = tile.GetTexelHeight(i);

        float fHeight = fReference;
        plVec3 vNormal(0, 0, 1);
        if (!caster.Cast(vCenter.x, vCenter.y, fReference, fHeight, vNormal))
          continue;

        float fMin = fHeight;
        float fMax = fHeight;
        for (plUInt32 c = 0; c < 4; ++c)
        {
          float fCorner = fHeight;
          plVec3 vCornerNormal;
          caster.Cast((iX + (c & 1)) * fTexel, (iY + (c >> 1)) * fTexel, fReference, fCorner, vCornerNormal);
          fMin = plMath::Min(fMin, fCorner);
          fMax = plMath::Max(fMax, fCorner);
        }

        tile.SetTexelHeight(i, fHeight);
        plGrassCoverageTexel::EncodeNormal(vNormal, texel.m_uiNormal);
        texel.m_uiFlags = (fMax - fMin) > GetDiscontinuityThreshold(fTexel) ? plGrassCoverageTexelFlags::Discontinuous : 0;
        ++out_result.m_uiResnappedTexels;
      }
    }
  }

  coverage.ToBuffer(out_result.m_Coverage);
}

void plVegetationBrushHandler::HandleResnap(const plEngineProcessToolConfigEvent& e)
{
  if (!e.m_pMsg->m_PayloadValue.IsA<plDataBuffer>())
    return;

  plVegetationResnapRequest request;
  if (request.FromBuffer(e.m_pMsg->m_PayloadValue.Get<plDataBuffer>()).Failed())
    return;

  plVegetationResnapResult result;
  result.m_LayerGuid = request.m_LayerGuid;
  result.m_uiRequestId = request.m_uiRequestId;

  plWorld* pWorld = e.m_pContext->GetWorld();
  if (pWorld != nullptr)
  {
    PL_LOCK(pWorld->GetWriteMarker());
    EvaluateResnap(pWorld, request, result);
  }
  else
  {
    result.m_Coverage = request.m_Coverage;
  }

  plSimpleDocumentConfigMsgToEditor msg;
  msg.m_DocumentGuid = e.m_pMsg->m_DocumentGuid;
  msg.m_sWhatToDo = plVegetationResnapResult::s_szWhatToDo;

  plDataBuffer buffer;
  result.ToBuffer(buffer);
  msg.m_PayloadValue = buffer;

  e.m_pContext->SendProcessMessage(&msg);
}

void plVegetationBrushHandler::HandleOverlay(const plEngineProcessToolConfigEvent& e)
{
  plWorld* pWorld = e.m_pContext->GetWorld();
  if (pWorld == nullptr)
    return;

  plUuid layerGuid;
  if (e.m_pMsg->m_PayloadValue.IsA<plUuid>())
  {
    layerGuid = e.m_pMsg->m_PayloadValue.Get<plUuid>();
  }

  PL_LOCK(pWorld->GetWriteMarker());

  Overlay& overlay = s_Overlays[e.m_pContext->GetDocumentGuid()];

  plGameObject* pObject = nullptr;
  if (overlay.m_uiWorldIndex != pWorld->GetIndex() || !pWorld->TryGetObject(overlay.m_hObject, pObject))
  {
    overlay = Overlay();
    overlay.m_uiWorldIndex = pWorld->GetIndex();
    pObject = nullptr;
  }

  const plComponentHandle hLayer = layerGuid.IsValid() ? e.m_pContext->GetContext().m_ComponentMap.GetHandle(layerGuid) : plComponentHandle();

  if (hLayer.IsInvalidated())
  {
    if (pObject != nullptr)
    {
      pWorld->DeleteObjectNow(overlay.m_hObject);
    }
    s_Overlays.Remove(e.m_pContext->GetDocumentGuid());
    return;
  }

  if (pObject == nullptr)
  {
    plGameObjectDesc desc;
    desc.m_sName.Assign("VegetationCoverageOverlay");
    desc.m_Tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("Editor")); // never exported
    overlay.m_hObject = pWorld->CreateObject(desc, pObject);

    plVegetationCoverageOverlayComponent* pComponent = nullptr;
    overlay.m_hComponent = plVegetationCoverageOverlayComponent::CreateComponent(pObject, pComponent);
  }

  plVegetationCoverageOverlayComponent* pComponent = nullptr;
  if (pWorld->TryGetComponent(overlay.m_hComponent, pComponent))
  {
    pComponent->SetLayer(hLayer);
  }
}

void plVegetationBrushHandler::RestoreLayer(plWorld* pWorld, const StrokePreview& preview)
{
  plVegetationLayerComponent* pLayer = nullptr;
  if (pWorld->TryGetComponent(preview.m_hLayer, pLayer))
  {
    pLayer->ClearStrokePreview();
  }
}

void plVegetationBrushHandler::MirrorStroke(plEngineProcessDocumentContext* pContext, plWorld* pWorld, const plVegetationStrokeRequest& request, const plVegetationStrokeResult& result)
{
  const plComponentHandle hLayer = request.m_LayerGuid.IsValid() ? pContext->GetContext().m_ComponentMap.GetHandle(request.m_LayerGuid) : plComponentHandle();

  plVegetationLayerComponent* pLayer = nullptr;
  if (!pWorld->TryGetComponent(hLayer, pLayer))
    return;

  StrokePreview& preview = s_Previews[pContext->GetDocumentGuid()];

  if (preview.m_uiWorldIndex != pWorld->GetIndex() || preview.m_uiStrokeId != request.m_uiStrokeId || preview.m_hLayer != hLayer)
  {
    // Another stroke, layer or world: the old preview goes back to its committed blobs and this stroke starts from
    // the layer as it stands now.
    RestoreLayer(pWorld, preview);

    preview = StrokePreview();
    preview.m_uiWorldIndex = pWorld->GetIndex();
    preview.m_uiStrokeId = request.m_uiStrokeId;
    preview.m_hLayer = hLayer;
    preview.m_Data = pLayer->GetLayerData();
    preview.m_Grass = pLayer->GetGrassCoverage();

    // the editor paints new texels on the layer's Grass Texel Size, so the preview works on the same grid
    preview.m_Grass.Resample(plGrassCoverageData::ClampTexelSize(request.m_fTexelSize));
  }

  if (request.m_uiMode == plVegetationBrushMode::Paint)
  {
    // world space -> layer space, the same way the editor merges the result into its stroke data
    const plTransform inv = request.m_LayerTransform.GetInverse();
    const float fLayerScale = request.m_LayerTransform.m_vScale.GetLength() / plMath::Sqrt(3.0f);
    const float fInvScale = fLayerScale > 0.0001f ? 1.0f / fLayerScale : 1.0f;

    for (const auto& entry : result.m_Added.m_Entries)
    {
      if (entry.m_Instances.IsEmpty())
        continue;

      auto& target = preview.m_Data.GetOrAddEntry(entry.m_sType);
      for (const auto& inst : entry.m_Instances)
      {
        plVegetationLayerInstance local = inst;
        local.m_vPosition = inv.TransformPosition(inst.m_vPosition);
        local.m_qRotation = inv.m_qRotation * inst.m_qRotation;
        local.m_fScale = inst.m_fScale * fInvScale;
        target.m_Instances.PushBack(local);
      }
    }

    if (!result.m_GrassDelta.IsEmpty())
    {
      preview.m_Grass.MergeDelta(result.m_GrassDelta).IgnoreResult();
    }
  }
  else if (request.m_uiMode == plVegetationBrushMode::Erase || request.m_uiMode == plVegetationBrushMode::Thin)
  {
    // The same removal the editor ran on its stroke data, with the seed it used.
    const bool bNoneChecked = request.m_Types.IsEmpty() && request.m_GrassTypes.IsEmpty();

    const plTransform inv = request.m_LayerTransform.GetInverse();
    const plVec3 vLocalCenter = inv.TransformPosition(request.m_vCenter);
    const plVec3 vAxisScale = request.m_LayerTransform.m_vScale.Abs().CompMax(plVec3(0.0001f));

    if (bNoneChecked || !request.m_Types.IsEmpty())
    {
      preview.m_Data.RemoveInSphere(vLocalCenter, request.m_fRadius, request.m_fFalloff, request.GetPlantRemovalStrength(), request.m_Types, request.m_uiSeed, vAxisScale);
    }

    if (bNoneChecked || !request.m_GrassTypes.IsEmpty())
    {
      plGrassCoverageStamp stamp;
      stamp.m_vCenter.Set(vLocalCenter.x, vLocalCenter.y);
      stamp.m_fRadius = request.m_fRadius;
      stamp.m_vAxisScale = vAxisScale.GetAsVec2();
      stamp.m_fFalloff = request.m_fFalloff;
      stamp.m_fDensity = request.m_fDensity;
      stamp.m_fStrength = request.m_fStrength;

      if (request.m_uiMode == plVegetationBrushMode::Erase)
      {
        preview.m_Grass.EraseInDisc(request.m_GrassTypes, stamp);
      }
      else
      {
        preview.m_Grass.ThinInDisc(request.m_GrassTypes, stamp);
      }
    }
  }
  else if (request.m_uiMode == plVegetationBrushMode::Clearing)
  {
    // the same stamp the editor applied to its stroke data
    request.ApplyClearingStamp(preview.m_Data);
  }
  else
  {
    return;
  }

  pLayer->SetStrokePreview(preview.m_Data, preview.m_Grass);
}

void plVegetationBrushHandler::HandleDropPreview(const plEngineProcessToolConfigEvent& e)
{
  StrokePreview* pPreview = s_Previews.GetValue(e.m_pContext->GetDocumentGuid());
  if (pPreview == nullptr)
    return;

  plWorld* pWorld = e.m_pContext->GetWorld();
  if (pWorld != nullptr && pWorld->GetIndex() == pPreview->m_uiWorldIndex)
  {
    PL_LOCK(pWorld->GetWriteMarker());
    RestoreLayer(pWorld, *pPreview);
  }

  s_Previews.Remove(e.m_pContext->GetDocumentGuid());
}

plMaterialResourceHandle plVegetationBrushHandler::GetGhostMaterial(const plMaterialResourceHandle& hBase)
{
  if (!hBase.IsValid())
    return hBase;

  plStringBuilder sName("VegetationGhost:", hBase.GetResourceID());

  plMaterialResourceHandle hGhost = plResourceManager::GetExistingResource<plMaterialResource>(sName);
  if (hGhost.IsValid())
    return hGhost;

  // The plant's own material, blended instead of masked, so the ghost reads as a see-through preview and stays
  // out of the editor's opaque picking.
  plMaterialResourceDescriptor desc;
  desc.m_hBaseMaterial = hBase;
  desc.m_RenderDataCategory = plDefaultRenderDataCategories::LitTransparent;

  plPermutationVar& blend = desc.m_PermutationVars.ExpandAndGetRef();
  blend.m_sName.Assign("BLEND_MODE");
  blend.m_sValue.Assign("BLEND_MODE_TRANSPARENT");

  return plResourceManager::CreateResource<plMaterialResource>(sName, std::move(desc), sName);
}

void plVegetationBrushHandler::UpdateGhost(plEngineProcessDocumentContext* pContext, const plVegetationStrokeRequest& request)
{
  plWorld* pWorld = pContext->GetWorld();
  if (pWorld == nullptr)
    return;

  PL_LOCK(pWorld->GetWriteMarker());

  Ghost& ghost = s_Ghosts[pContext->GetDocumentGuid()];

  plGameObject* pObject = nullptr;
  if (ghost.m_uiWorldIndex != pWorld->GetIndex() || !pWorld->TryGetObject(ghost.m_hObject, pObject))
  {
    ghost = Ghost();
    ghost.m_uiWorldIndex = pWorld->GetIndex();
    pObject = nullptr;
  }

  plVegetationStrokeResult result;
  if (!request.m_Types.IsEmpty())
  {
    plVegetationStrokeRequest single = request;
    single.m_uiMode = plVegetationBrushMode::Place;
    EvaluatePaint(pWorld, single, result);
  }

  const plVegetationLayerInstance* pInstance = nullptr;
  plStringView sType;
  for (const auto& entry : result.m_Added.m_Entries)
  {
    if (!entry.m_Instances.IsEmpty())
    {
      pInstance = &entry.m_Instances[0];
      sType = entry.m_sType;
      break;
    }
  }

  if (pInstance == nullptr)
  {
    // nothing to place here (empty palette, too steep): hide but keep the object for the next move
    if (pObject != nullptr)
      pObject->SetActiveFlag(false);
    return;
  }

  if (pObject == nullptr)
  {
    plGameObjectDesc desc;
    desc.m_sName.Assign("VegetationPlaceGhost");
    desc.m_bDynamic = true;
    ghost.m_hObject = pWorld->CreateObject(desc, pObject);

    plMeshComponent* pMesh = nullptr;
    ghost.m_hMesh = plMeshComponent::CreateComponent(pObject, pMesh);
    pMesh->SetColor(plColor(0.55f, 1.0f, 0.85f, 0.45f));
  }

  if (ghost.m_sType != sType)
  {
    ghost.m_sType = sType;
    ghost.m_Materials.Clear();

    plMeshComponent* pMesh = nullptr;
    if (pWorld->TryGetComponent(ghost.m_hMesh, pMesh))
    {
      plVegetationTypeResourceHandle hType = plResourceManager::LoadResource<plVegetationTypeResource>(sType);
      plResourceLock<plVegetationTypeResource> pType(hType, plResourceAcquireMode::BlockTillLoaded_NeverFail);

      plMeshResourceHandle hMesh;
      if (pType.GetAcquireResult() == plResourceAcquireResult::Final && pType->GetLodCount() > 0)
      {
        hMesh = pType->GetLodMesh(0);
      }

      pMesh->SetMesh(hMesh);

      if (hMesh.IsValid())
      {
        plResourceLock<plMeshResource> pMeshRes(hMesh, plResourceAcquireMode::BlockTillLoaded_NeverFail);
        if (pMeshRes.GetAcquireResult() == plResourceAcquireResult::Final)
        {
          plArrayPtr<const plMaterialResourceHandle> meshMaterials = pMeshRes->GetMaterials();
          for (plUInt32 uiSlot = 0; uiSlot < meshMaterials.GetCount(); ++uiSlot)
          {
            plMaterialResourceHandle hBase = pType->GetMaterialOverride(uiSlot);
            if (!hBase.IsValid())
              hBase = meshMaterials[uiSlot];

            const plMaterialResourceHandle hGhost = GetGhostMaterial(hBase);
            ghost.m_Materials.PushBack(hGhost);
            pMesh->SetMaterial(uiSlot, hGhost);
          }
        }
      }
    }
  }

  plTransform t;
  t.m_vPosition = pInstance->m_vPosition;
  t.m_qRotation = pInstance->m_qRotation;
  t.m_vScale.Set(pInstance->m_fScale);

  pObject->SetGlobalTransform(t);
  pObject->SetActiveFlag(true);
}
