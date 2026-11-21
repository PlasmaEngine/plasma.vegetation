#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Math/Quat.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/VariantType.h>
#include <VegetationPlugin/VegetationPluginDLL.h>

/// \brief One painted instance, in the layer object's local space.
struct PL_VEGETATIONPLUGIN_DLL plVegetationLayerInstance
{
  PL_DECLARE_POD_TYPE();

  plVec3 m_vPosition = plVec3::MakeZero();
  plQuat m_qRotation = plQuat::MakeIdentity();
  float m_fScale = 1.0f;
  plUInt8 m_uiColorSeed = 0;
};

/// \brief All painted instances of one vegetation type in a layer.
struct PL_VEGETATIONPLUGIN_DLL plVegetationLayerEntry
{
  plString m_sType; ///< Vegetation type resource id (asset guid).
  plDynamicArray<plVegetationLayerInstance> m_Instances;
};

/// \brief A clearing painted on a layer: a disc in the layer object's local xy plane where Paint and Fill place no
/// plants. Types keep their Clearing Avoidance distance from its edge.
struct PL_VEGETATIONPLUGIN_DLL plVegetationLayerClearing
{
  PL_DECLARE_POD_TYPE();

  plVec2 m_vCenter = plVec2::MakeZero();
  float m_fRadius = 1.0f;
};

/// \brief The content of a plVegetationLayerComponent: instances grouped by type and the layer's clearings, stored as
/// one binary blob property so a paint stroke is a single undoable property change. Shared by the runtime component,
/// the editor paint tool and the engine-process brush evaluation.
class PL_VEGETATIONPLUGIN_DLL plVegetationLayerData
{
public:
  plHybridArray<plVegetationLayerEntry, 4> m_Entries;
  plDynamicArray<plVegetationLayerClearing> m_Clearings; ///< layer space

  void Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);

  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);

  plVegetationLayerEntry& GetOrAddEntry(plStringView sType);
  const plVegetationLayerEntry* FindEntry(plStringView sType) const;

  plUInt32 GetInstanceCount() const;
  bool IsEmpty() const { return GetInstanceCount() == 0; }

  /// \brief Appends every instance and clearing of other (same space assumed).
  void Append(const plVegetationLayerData& other);

  /// \brief Adds a clearing unless an existing one already covers it. Returns whether it was added.
  bool AddClearing(const plVec2& vCenter, float fRadius);

  /// \brief Removes the clearings whose centre lies inside the disc. vAxisScale works as in RemoveInSphere. Returns the
  /// number removed.
  plUInt32 RemoveClearings(const plVec2& vCenter, float fRadius, const plVec2& vAxisScale = plVec2(1.0f));

  /// \brief Removes instances inside the sphere. Instances inside innerRadius are removed with probability
  /// fStrength, between inner and outer radius the probability fades to zero. An empty types list matches all types.
  /// vAxisScale is the length of one layer-space unit along each axis: distances are measured as offset * vAxisScale,
  /// so a world-space sphere stays exact under a non-uniformly scaled layer. Returns the number removed.
  plUInt32 RemoveInSphere(const plVec3& vCenter, float fRadius, float fFalloff, float fStrength, plArrayPtr<const plString> types, plUInt32 uiSeed, const plVec3& vAxisScale = plVec3(1.0f));

  /// \brief Drops empty entries.
  void Compact();
};
