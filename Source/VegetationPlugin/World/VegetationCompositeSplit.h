#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Types/ArrayPtr.h>
#include <VegetationPlugin/VegetationDeclarations.h>

struct plVegetationCellInstance;

/// \brief How one foliage composite tree of a cell is drawn.
struct plVegetationCompositeTreeState
{
  enum Enum : plUInt8
  {
    Hidden = 0, ///< Beyond the cull distance, or dropped by Vegetation.DensityScale.
    Merged = 1, ///< The composite's merged mesh.
    Parts = 2,  ///< The composite's parts, each an instance of its part mesh.

    MainMask = 3,
    CastsShadow = 4, ///< Within the shadow cull distance and not flagged NoShadow.
    ShadowParts = 8, ///< Parts in the main view and near enough to cast its shadow with parts too; else the merged mesh.
  };
};

/// \brief The distances a cell's composite trees are split by, all from the main camera.
struct plVegetationCompositeThresholds
{
  float m_fSwitchDistance = 0.0f;     ///< At instance scale 1: parts closer than this times the scale, merged beyond.
  float m_fCullDistance = 0.0f;       ///< Trees further away are hidden.
  float m_fShadowCullDistance = 0.0f; ///< Trees further away cast no shadow; at most m_fCullDistance.
  float m_fShadowPartsDistance = 0.0f; ///< At instance scale 1: shadows use parts closer than this, the merged mesh beyond.
  float m_fDensity = 1.0f;            ///< Vegetation.DensityScale: fraction of the non-hero trees kept, by colour seed.

  bool operator==(const plVegetationCompositeThresholds& other) const = default;
};

/// \brief Decides per tree of one cell group whether it shows its parts, its merged mesh or nothing, and says when that
/// changed, so the cell rebuilds its batches only then.
///
/// The cell's tree positions are bounded by a box. While the camera stays on the same side of every threshold for the
/// whole box (all trees nearer, or all further than it), no tree can have changed and nothing is evaluated per tree.
/// Only cells that straddle a threshold test each tree, and report a change only when a tree actually crossed one.
class PL_VEGETATIONPLUGIN_DLL plVegetationCompositeSplit
{
public:
  /// Forgets everything: the next Update evaluates every tree and reports a change.
  void Reset();

  /// Classifies the trees for the camera. Returns true when any tree's state differs from the last call.
  bool Update(plArrayPtr<const plVegetationCellInstance> instances, const plVec3& vCamera, const plVegetationCompositeThresholds& thresholds);

  /// Per tree (instance index): plVegetationCompositeTreeState bits.
  plArrayPtr<const plUInt8> GetStates() const { return m_States; }

  /// Trees in each main state after the last Update.
  plUInt32 GetCount(plVegetationCompositeTreeState::Enum state) const { return m_uiCounts[state & plVegetationCompositeTreeState::MainMask]; }

  /// Whether the last Update evaluated every tree (the cell straddled a threshold or inputs changed). For tests and stats.
  bool WasEvaluatedPerTree() const { return m_bEvaluatedPerTree; }

  /// Pure per-tree rule, exposed for tests: the state bits of a tree at a distance from the camera.
  static plUInt8 ClassifyTree(const plVegetationCellInstance& inst, float fDistance, const plVegetationCompositeThresholds& thresholds);

private:
  /// Per threshold, 0 all trees nearer, 1 all further, 2 mixed; packed two bits each.
  plUInt32 ComputeBandKey(const plVec3& vCamera, const plVegetationCompositeThresholds& thresholds) const;

  plDynamicArray<plUInt8> m_States;
  plBoundingBox m_Positions = plBoundingBox::MakeInvalid();
  float m_fMinScale = 1.0f;
  float m_fMaxScale = 1.0f;
  plUInt32 m_uiBandKey = 0xFFFFFFFF;
  plVec3 m_vCamera = plVec3::MakeZero();
  plVegetationCompositeThresholds m_Thresholds;
  bool m_bValid = false;
  bool m_bEvaluatedPerTree = false;
  plUInt32 m_uiCounts[3] = {};
};
