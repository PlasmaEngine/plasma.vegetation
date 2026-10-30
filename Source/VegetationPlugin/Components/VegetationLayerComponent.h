#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>
#include <VegetationPlugin/VegetationDeclarations.h>

struct plMsgTransformChanged;

using plVegetationLayerComponentManager = plComponentManagerSimple<class plVegetationLayerComponent, plComponentUpdateType::Always>;

/// \brief A painted set of vegetation instances and grass coverage. The editor's Vegetation paint tool writes the Data
/// and GrassCoverage blobs; at runtime the component registers every instance with the world's vegetation module, in
/// the owner's local space so the whole layer moves with its object. Grass coverage lives on a texel grid in the
/// owner's local xy plane with GrassTexelSize metre texels.
class PL_VEGETATIONPLUGIN_DLL plVegetationLayerComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plVegetationLayerComponent, plComponent, plVegetationLayerComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plVegetationLayerComponent

public:
  plVegetationLayerComponent();
  ~plVegetationLayerComponent();

  void SetData(const plDataBuffer& data);                // [ property ]
  const plDataBuffer& GetData() const { return m_Data; } // [ property ]

  plUInt32 GetInstanceCount() const { return m_Parsed.GetInstanceCount(); } // [ property ]

  const plVegetationLayerData& GetLayerData() const { return m_Parsed; }

  void SetGrassCoverageBlob(const plDataBuffer& data);                         // [ property ] "GrassCoverage"
  const plDataBuffer& GetGrassCoverageBlob() const { return m_GrassCoverage; } // [ property ] "GrassCoverage"

  /// \brief Texel size of the grass coverage grid in metres, 0.25 .. 2: the grid the editor paints new texels on. The
  /// runtime grows the coverage on the grid stored in the GrassCoverage blob; the editor resamples the blob in the same
  /// transaction that changes this property.
  void SetGrassTexelSize(float fTexelSize);                    // [ property ]
  float GetGrassTexelSize() const { return m_fGrassTexelSize; } // [ property ]

  plUInt32 GetGrassTileCount() const { return m_GrassParsed.GetTileCount(); } // [ property ]

  /// \brief The parsed coverage. Its GetLastChangedTiles() / GetLastChangeWasFull() describe the latest change.
  const plGrassCoverageData& GetGrassCoverage() const { return m_GrassParsed; }

  /// \brief Increments whenever the grass coverage content or grid changes.
  plUInt32 GetGrassCoverageChangeCounter() const { return m_GrassParsed.GetChangeCounter(); }

  /// \brief Shows the result of a paint stroke that is still running on top of the committed blobs, so the stroke
  /// renders while the mouse is down. The Data and GrassCoverage blobs stay untouched, so nothing of this is saved.
  /// Only the engine process of the editor uses this.
  void SetStrokePreview(const plVegetationLayerData& data, const plGrassCoverageData& grass);

  /// \brief Drops a stroke preview and restores the instances and coverage of the committed blobs.
  void ClearStrokePreview();

  bool HasStrokePreview() const { return m_bStrokePreview; }

  /// \brief Re-registers when marked dirty; runs in the manager's update where the world is writable.
  void Update();

private:
  void OnMsgTransformChanged(plMsgTransformChanged& msg);
  void Register();
  void Unregister();

  plDataBuffer m_Data;
  plVegetationLayerData m_Parsed;
  plHybridArray<plVegetationTileHandle, 4> m_Tiles;
  bool m_bDirty = false;

  plDataBuffer m_GrassCoverage;
  plGrassCoverageData m_GrassParsed;
  float m_fGrassTexelSize = plGrassCoverageData::DefaultTexelSize;
  bool m_bStrokePreview = false; ///< the parsed data shows a running paint stroke, not the blobs
};
