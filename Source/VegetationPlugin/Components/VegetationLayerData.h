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

/// \brief The content of a plVegetationLayerComponent: instances grouped by type, stored as one binary blob property
/// so a paint stroke is a single undoable property change. Shared by the runtime component, the editor paint tool and
/// the engine-process brush evaluation.
class PL_VEGETATIONPLUGIN_DLL plVegetationLayerData
{
public:
  plHybridArray<plVegetationLayerEntry, 4> m_Entries;

  void Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);

  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);

  plVegetationLayerEntry& GetOrAddEntry(plStringView sType);
  const plVegetationLayerEntry* FindEntry(plStringView sType) const;

  plUInt32 GetInstanceCount() const;
  bool IsEmpty() const { return GetInstanceCount() == 0; }

  /// \brief Appends every instance of other (same space assumed).
  void Append(const plVegetationLayerData& other);

  /// \brief Removes instances inside the sphere. Instances inside innerRadius are removed with probability
  /// fStrength, between inner and outer radius the probability fades to zero. An empty types list matches all types.
  /// vAxisScale is the length of one layer-space unit along each axis: distances are measured as offset * vAxisScale,
  /// so a world-space sphere stays exact under a non-uniformly scaled layer. Returns the number removed.
  plUInt32 RemoveInSphere(const plVec3& vCenter, float fRadius, float fFalloff, float fStrength, plArrayPtr<const plString> types, plUInt32 uiSeed, const plVec3& vAxisScale = plVec3(1.0f));

  /// \brief Drops empty entries.
  void Compact();
};
