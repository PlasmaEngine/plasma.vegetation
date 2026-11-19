#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Math/Random.h>
#include <VegetationPlugin/Placement/VegetationScatter.h>

namespace
{
  /// Most cells of the neighbour grid; a larger region gets larger cells.
  constexpr plUInt32 s_uiMaxGridCells = 4u * 1024u * 1024u;

  /// Top-up darts per wanted point.
  constexpr plUInt32 s_uiTopUpDartsPerPoint = 8;

  /// Uniform grid of linked point lists over the region grown by the largest spacing.
  struct NeighbourGrid
  {
    plVec2 m_vOrigin = plVec2::MakeZero();
    float m_fCell = 1.0f;
    float m_fInvCell = 1.0f;
    plInt32 m_iCountX = 1;
    plInt32 m_iCountY = 1;
    plDynamicArray<plInt32> m_Heads;
    plDynamicArray<plInt32> m_Next;
    plDynamicArray<plVec2> m_Positions;
    plDynamicArray<float> m_Spacings;

    void Init(const plVec2& vMin, const plVec2& vMax, float fMinCell)
    {
      const plVec2 vSize = vMax - vMin;
      const float fArea = plMath::Max(vSize.x * vSize.y, 0.0001f);
      m_fCell = plMath::Max(plMath::Max(fMinCell, 0.05f), plMath::Sqrt(fArea / static_cast<float>(s_uiMaxGridCells)));
      m_fInvCell = 1.0f / m_fCell;
      m_vOrigin = vMin;
      m_iCountX = plMath::Max(1, static_cast<plInt32>(plMath::Ceil(vSize.x * m_fInvCell)));
      m_iCountY = plMath::Max(1, static_cast<plInt32>(plMath::Ceil(vSize.y * m_fInvCell)));
      m_Heads.SetCountUninitialized(static_cast<plUInt32>(m_iCountX * m_iCountY));
      for (plInt32& iHead : m_Heads)
        iHead = -1;
    }

    void CellOf(const plVec2& vPosition, plInt32& out_iX, plInt32& out_iY) const
    {
      out_iX = plMath::Clamp(static_cast<plInt32>(plMath::Floor((vPosition.x - m_vOrigin.x) * m_fInvCell)), 0, m_iCountX - 1);
      out_iY = plMath::Clamp(static_cast<plInt32>(plMath::Floor((vPosition.y - m_vOrigin.y) * m_fInvCell)), 0, m_iCountY - 1);
    }

    void Insert(const plVec2& vPosition, float fSpacing)
    {
      plInt32 iX, iY;
      CellOf(vPosition, iX, iY);
      const plUInt32 uiCell = static_cast<plUInt32>(iY * m_iCountX + iX);

      m_Next.PushBack(m_Heads[uiCell]);
      m_Heads[uiCell] = static_cast<plInt32>(m_Positions.GetCount());
      m_Positions.PushBack(vPosition);
      m_Spacings.PushBack(fSpacing);
    }

    /// Whether a point with this spacing keeps the pair spacing to every stored point. fReach is the largest pair
    /// spacing it can have.
    bool IsFree(const plVec2& vPosition, float fSpacing, float fReach) const
    {
      const plInt32 iRing = plMath::Max(1, static_cast<plInt32>(plMath::Ceil(fReach * m_fInvCell)));

      plInt32 iX, iY;
      CellOf(vPosition, iX, iY);

      for (plInt32 y = plMath::Max(iY - iRing, 0); y <= plMath::Min(iY + iRing, m_iCountY - 1); ++y)
      {
        for (plInt32 x = plMath::Max(iX - iRing, 0); x <= plMath::Min(iX + iRing, m_iCountX - 1); ++x)
        {
          for (plInt32 i = m_Heads[y * m_iCountX + x]; i >= 0; i = m_Next[i])
          {
            const float fPair = plVegetationScatter::GetPairSpacing(fSpacing, m_Spacings[i]);
            if ((m_Positions[i] - vPosition).GetLengthSquared() < fPair * fPair)
              return false;
          }
        }
      }

      return true;
    }
  };

  /// Coarse buckets of clearing indices; each clearing sits in every bucket its grown disc touches.
  struct ClearingGrid
  {
    plVec2 m_vOrigin = plVec2::MakeZero();
    float m_fInvCell = 1.0f;
    plInt32 m_iCountX = 0;
    plInt32 m_iCountY = 0;
    plDynamicArray<plUInt32> m_Start; ///< per bucket, into m_Items; one extra entry at the end
    plDynamicArray<plUInt32> m_Items;
    plArrayPtr<const plVegetationScatterClearing> m_Clearings;

    void Init(const plVec2& vMin, const plVec2& vMax, plArrayPtr<const plVegetationScatterClearing> clearings, float fGrow)
    {
      m_Clearings = clearings;
      if (clearings.IsEmpty())
        return;

      const plVec2 vSize = vMax - vMin;
      const float fCell = plMath::Max(16.0f, plMath::Sqrt(plMath::Max(vSize.x * vSize.y, 1.0f) / 65536.0f));
      m_fInvCell = 1.0f / fCell;
      m_vOrigin = vMin;
      m_iCountX = plMath::Max(1, static_cast<plInt32>(plMath::Ceil(vSize.x * m_fInvCell)));
      m_iCountY = plMath::Max(1, static_cast<plInt32>(plMath::Ceil(vSize.y * m_fInvCell)));

      const plUInt32 uiBuckets = static_cast<plUInt32>(m_iCountX * m_iCountY);
      plDynamicArray<plUInt32> counts;
      counts.SetCount(uiBuckets + 1);

      // two passes: count, then fill
      for (plUInt32 uiPass = 0; uiPass < 2; ++uiPass)
      {
        if (uiPass == 1)
        {
          m_Start.SetCount(uiBuckets + 1);
          plUInt32 uiSum = 0;
          for (plUInt32 b = 0; b < uiBuckets; ++b)
          {
            m_Start[b] = uiSum;
            uiSum += counts[b];
            counts[b] = m_Start[b];
          }
          m_Start[uiBuckets] = uiSum;
          m_Items.SetCountUninitialized(uiSum);
        }

        for (plUInt32 c = 0; c < clearings.GetCount(); ++c)
        {
          const float fRadius = clearings[c].m_fRadius + fGrow;
          plInt32 iX0, iY0, iX1, iY1;
          if (!Range(clearings[c].m_vCenter - plVec2(fRadius), clearings[c].m_vCenter + plVec2(fRadius), iX0, iY0, iX1, iY1))
            continue;

          for (plInt32 y = iY0; y <= iY1; ++y)
          {
            for (plInt32 x = iX0; x <= iX1; ++x)
            {
              const plUInt32 b = static_cast<plUInt32>(y * m_iCountX + x);
              if (uiPass == 0)
                ++counts[b];
              else
                m_Items[counts[b]++] = c;
            }
          }
        }
      }
    }

    bool Range(const plVec2& vMin, const plVec2& vMax, plInt32& out_iX0, plInt32& out_iY0, plInt32& out_iX1, plInt32& out_iY1) const
    {
      out_iX0 = static_cast<plInt32>(plMath::Floor((vMin.x - m_vOrigin.x) * m_fInvCell));
      out_iY0 = static_cast<plInt32>(plMath::Floor((vMin.y - m_vOrigin.y) * m_fInvCell));
      out_iX1 = static_cast<plInt32>(plMath::Floor((vMax.x - m_vOrigin.x) * m_fInvCell));
      out_iY1 = static_cast<plInt32>(plMath::Floor((vMax.y - m_vOrigin.y) * m_fInvCell));
      if (out_iX1 < 0 || out_iY1 < 0 || out_iX0 >= m_iCountX || out_iY0 >= m_iCountY)
        return false;

      out_iX0 = plMath::Max(out_iX0, 0);
      out_iY0 = plMath::Max(out_iY0, 0);
      out_iX1 = plMath::Min(out_iX1, m_iCountX - 1);
      out_iY1 = plMath::Min(out_iY1, m_iCountY - 1);
      return true;
    }

    bool IsCleared(const plVec2& vPosition, float fAvoidance) const
    {
      if (m_Clearings.IsEmpty())
        return false;

      const plInt32 iX = static_cast<plInt32>(plMath::Floor((vPosition.x - m_vOrigin.x) * m_fInvCell));
      const plInt32 iY = static_cast<plInt32>(plMath::Floor((vPosition.y - m_vOrigin.y) * m_fInvCell));
      if (iX < 0 || iY < 0 || iX >= m_iCountX || iY >= m_iCountY)
        return false;

      const plUInt32 b = static_cast<plUInt32>(iY * m_iCountX + iX);
      for (plUInt32 i = m_Start[b]; i < m_Start[b + 1]; ++i)
      {
        const plVegetationScatterClearing& clearing = m_Clearings[m_Items[i]];
        const float fRadius = clearing.m_fRadius + fAvoidance;
        if ((clearing.m_vCenter - vPosition).GetLengthSquared() < fRadius * fRadius)
          return true;
      }

      return false;
    }
  };
} // namespace

void plVegetationScatter::Scatter(const plVegetationScatterDesc& desc, plDynamicArray<plVegetationScatterPoint>& out_points)
{
  if (desc.m_Species.IsEmpty())
    return;

  const plVec2 vMin = desc.m_vMin.CompMin(desc.m_vMax);
  const plVec2 vMax = desc.m_vMin.CompMax(desc.m_vMax);
  const plVec2 vSize = vMax - vMin;
  const float fArea = vSize.x * vSize.y;
  if (fArea <= 0.0f || desc.m_fDensity <= 0.0f)
    return;

  const plUInt32 uiWanted = static_cast<plUInt32>(plMath::Min(static_cast<double>(desc.m_fDensity) * fArea + 0.5, static_cast<double>(desc.m_uiMaxPoints)));
  if (uiWanted == 0)
    return;

  float fWeightSum = 0.0f;
  float fMaxSpacing = 0.0f;
  float fMaxAvoidance = 0.0f;
  for (const auto& species : desc.m_Species)
  {
    fWeightSum += plMath::Max(species.m_fWeight, 0.0f);
    fMaxSpacing = plMath::Max(fMaxSpacing, species.m_fMinSpacing);
    fMaxAvoidance = plMath::Max(fMaxAvoidance, species.m_fClearingAvoidance);
  }

  float fMaxObstacleSpacing = 0.0f;
  for (const auto& obstacle : desc.m_Obstacles)
    fMaxObstacleSpacing = plMath::Max(fMaxObstacleSpacing, obstacle.m_fMinSpacing);

  // Obstacles farther outside than the widest pair spacing cannot reject anything.
  const float fMaxReach = GetPairSpacing(fMaxSpacing, plMath::Max(fMaxSpacing, fMaxObstacleSpacing));
  const plVec2 vGridMin = vMin - plVec2(fMaxReach);
  const plVec2 vGridMax = vMax + plVec2(fMaxReach);

  NeighbourGrid grid;
  grid.Init(vGridMin, vGridMax, fMaxReach);

  for (const auto& obstacle : desc.m_Obstacles)
  {
    const plVec2& p = obstacle.m_vPosition;
    if (p.x >= vGridMin.x && p.y >= vGridMin.y && p.x <= vGridMax.x && p.y <= vGridMax.y)
      grid.Insert(p, obstacle.m_fMinSpacing);
  }

  ClearingGrid clearings;
  clearings.Init(vMin, vMax, desc.m_Clearings, fMaxAvoidance);

  plRandom rng;
  rng.Initialize(desc.m_uiSeed);

  plUInt32 uiAccepted = 0;

  auto PickSpecies = [&]() -> plUInt32
  {
    const plUInt32 uiCount = desc.m_Species.GetCount();
    if (uiCount == 1)
      return 0;

    if (fWeightSum <= 0.0f)
      return plMath::Min(static_cast<plUInt32>(rng.FloatZeroToOneExclusive() * uiCount), uiCount - 1);

    float fPick = rng.FloatZeroToOneExclusive() * fWeightSum;
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      const float fWeight = plMath::Max(desc.m_Species[i].m_fWeight, 0.0f);
      if (fWeight <= 0.0f)
        continue;

      fPick -= fWeight;
      if (fPick <= 0.0f)
        return i;
    }

    // rounding: the last type with weight
    for (plUInt32 i = uiCount; i > 0; --i)
    {
      if (desc.m_Species[i - 1].m_fWeight > 0.0f)
        return i - 1;
    }
    return 0;
  };

  auto TryCandidate = [&](const plVec2& vPosition)
  {
    const plUInt32 uiSpecies = PickSpecies();
    const plVegetationScatterSpecies& species = desc.m_Species[uiSpecies];

    if (desc.m_RegionMask.IsValid() && !desc.m_RegionMask(vPosition))
      return;

    if (clearings.IsCleared(vPosition, plMath::Max(species.m_fClearingAvoidance, 0.0f)))
      return;

    const float fSpacing = plMath::Max(species.m_fMinSpacing, 0.0f);
    if (!grid.IsFree(vPosition, fSpacing, GetPairSpacing(fSpacing, plMath::Max(fMaxSpacing, fMaxObstacleSpacing))))
      return;

    if (desc.m_Accept.IsValid() && !desc.m_Accept(vPosition, uiSpecies))
      return;

    grid.Insert(vPosition, fSpacing);

    plVegetationScatterPoint& point = out_points.ExpandAndGetRef();
    point.m_vPosition = vPosition;
    point.m_uiSpecies = uiSpecies;
    ++uiAccepted;
  };

  // pass 1: one jittered candidate per stratum, strata in shuffled order
  {
    const float fStratum = plMath::Sqrt(fArea / static_cast<float>(uiWanted));
    const plUInt32 uiCountX = plMath::Max(1u, static_cast<plUInt32>(plMath::Ceil(vSize.x / fStratum)));
    const plUInt32 uiCountY = plMath::Max(1u, static_cast<plUInt32>(plMath::Ceil(vSize.y / fStratum)));
    const plVec2 vStratum(vSize.x / uiCountX, vSize.y / uiCountY);

    plDynamicArray<plUInt32> order;
    order.SetCountUninitialized(uiCountX * uiCountY);
    for (plUInt32 i = 0; i < order.GetCount(); ++i)
      order[i] = i;

    for (plUInt32 i = order.GetCount(); i > 1; --i)
    {
      const plUInt32 j = plMath::Min(static_cast<plUInt32>(rng.FloatZeroToOneExclusive() * i), i - 1);
      plMath::Swap(order[i - 1], order[j]);
    }

    for (plUInt32 uiStratum : order)
    {
      if (uiAccepted >= uiWanted)
        break;

      const plUInt32 x = uiStratum % uiCountX;
      const plUInt32 y = uiStratum / uiCountX;
      const float fX = static_cast<float>(x) + rng.FloatZeroToOneExclusive();
      const float fY = static_cast<float>(y) + rng.FloatZeroToOneExclusive();
      TryCandidate(vMin + plVec2(fX * vStratum.x, fY * vStratum.y));
    }
  }

  // Pass 2: uniform darts fill in what spacing or the accept test turned down. Near the packing limit of the spacing
  // most darts miss, so there are several per wanted point; a dart that fails the spacing test costs a few distance
  // checks, the accept callback only runs for darts that pass.
  const plUInt32 uiMaxDarts = uiWanted * s_uiTopUpDartsPerPoint;
  for (plUInt32 uiDart = 0; desc.m_bTopUp && uiDart < uiMaxDarts && uiAccepted < uiWanted; ++uiDart)
  {
    TryCandidate(vMin + plVec2(rng.FloatZeroToOneExclusive() * vSize.x, rng.FloatZeroToOneExclusive() * vSize.y));
  }
}
