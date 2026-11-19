#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Types/Delegate.h>
#include <VegetationPlugin/VegetationPluginDLL.h>

/// \brief One type taking part in a scatter.
struct plVegetationScatterSpecies
{
  float m_fWeight = 1.0f;             ///< relative share of the candidates
  float m_fMinSpacing = 0.0f;         ///< between two points of this type; between types the mean of both
  float m_fClearingAvoidance = 0.0f;  ///< extra distance kept from every clearing's edge
};

/// \brief A point that is already there (an instance of the layer) and keeps new points away.
struct plVegetationScatterObstacle
{
  PL_DECLARE_POD_TYPE();

  plVec2 m_vPosition;
  float m_fMinSpacing; ///< the spacing of the obstacle's own type
};

/// \brief A disc no point may fall into, grown by the species' clearing avoidance.
struct plVegetationScatterClearing
{
  PL_DECLARE_POD_TYPE();

  plVec2 m_vCenter;
  float m_fRadius;
};

struct plVegetationScatterPoint
{
  PL_DECLARE_POD_TYPE();

  plVec2 m_vPosition;
  plUInt32 m_uiSpecies;
};

/// \brief Poisson-disk scatter of several types over a rectangle at a target density.
///
/// Stratified dart throwing: the rectangle is cut into one stratum per wanted point, each stratum offers one jittered
/// candidate in shuffled order, then uniform darts top up what spacing rejected. A candidate is kept when it lies inside
/// the optional region mask, outside every clearing (plus its type's avoidance), at least the pair's spacing away from
/// every kept point and obstacle, and the optional accept callback agrees (the brush raycasts the ground there). Work is
/// bounded: at most m_uiMaxPoints points, one candidate per stratum plus at most eight darts per wanted point, and the
/// neighbour grid has at most 4M cells. When the spacing does not leave room for the density (dart throwing packs discs
/// to about 55 % of the plane), spacing wins and fewer points come out. The same description and seed give the same
/// points.
struct PL_VEGETATIONPLUGIN_DLL plVegetationScatterDesc
{
  plVec2 m_vMin = plVec2::MakeZero();
  plVec2 m_vMax = plVec2::MakeZero();
  float m_fDensity = 0.0f;           ///< wanted points per square metre over the whole rectangle
  plUInt32 m_uiMaxPoints = 250000;
  plUInt32 m_uiSeed = 0;

  /// After the strata, throw uniform darts until the wanted count is reached (or as many darts as wanted points were
  /// thrown). Off for a brush stamp, where falloff and spacing are meant to thin the stamp.
  bool m_bTopUp = true;

  plArrayPtr<const plVegetationScatterSpecies> m_Species;
  plArrayPtr<const plVegetationScatterObstacle> m_Obstacles;
  plArrayPtr<const plVegetationScatterClearing> m_Clearings;

  /// Optional: false drops the candidate (outside a brush disc, rejected by falloff).
  plDelegate<bool(const plVec2&)> m_RegionMask;

  /// Optional: called last for a candidate that passed every other test; false drops it. A true answer is final, so a
  /// caller may record what it computed for the point: the points come out in the order they were accepted.
  plDelegate<bool(const plVec2&, plUInt32)> m_Accept;
};

class PL_VEGETATIONPLUGIN_DLL plVegetationScatter
{
public:
  /// \brief Appends the accepted points to out_points.
  static void Scatter(const plVegetationScatterDesc& desc, plDynamicArray<plVegetationScatterPoint>& out_points);

  /// \brief The spacing two points must keep: the mean of both types' spacing.
  static float GetPairSpacing(float fSpacingA, float fSpacingB) { return 0.5f * (fSpacingA + fSpacingB); }
};
