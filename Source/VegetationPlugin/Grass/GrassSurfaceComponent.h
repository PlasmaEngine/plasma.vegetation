#pragma once

#include <Core/ResourceManager/ResourceManager.h>
#include <Core/World/World.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

class PL_VEGETATIONPLUGIN_DLL plGrassSurfaceComponentManager : public plComponentManager<class plGrassSurfaceComponent, plBlockStorageType::Compact>
{
public:
  plGrassSurfaceComponentManager(plWorld* pWorld);
  virtual void Initialize() override;

private:
  void Update(const plWorldModule::UpdateContext& context);

  // Last applied grass shadow settings; a change forces every component to invalidate its cached shadows.
  bool m_bShadowsEnabled = true;
  float m_fShadowDistanceScale = 1.0f;
};

/// Grows textured, segmented grass cards on a mesh asset in owner-local space.
/// Every look, simulation and distance setting comes from the Grass Type; without one the type defaults apply.
/// The strand count is the mesh's world-space area (triangles with a non-zero vertex length) times the type's
/// strands per square metre, capped at 100000. Roots are sampled on the CPU and uploaded into the world's GPU strand
/// pool (plGrassGpuManager), which simulates, culls and draws them. A generated child object carries the render owner.
class PL_VEGETATIONPLUGIN_DLL plGrassSurfaceComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plGrassSurfaceComponent, plComponent, plGrassSurfaceComponentManager);

public:
  plGrassSurfaceComponent();
  ~plGrassSurfaceComponent();
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  // Setters only flag a rebuild; the next world update performs it.
  void SetMesh(const plMeshResourceHandle& hMesh);
  const plMeshResourceHandle& GetMesh() const { return m_hMesh; }
  PL_ADD_RESOURCEHANDLE_ACCESSORS_WITH_SETTER(Mesh, m_hMesh, SetMesh);

  void SetGrassType(const plGrassTypeResourceHandle& hType);
  const plGrassTypeResourceHandle& GetGrassType() const { return m_hGrassType; }
  PL_ADD_RESOURCEHANDLE_ACCESSORS_WITH_SETTER(GrassType, m_hGrassType, SetGrassType);

  plUInt32 VertexLengths_GetCount() const { return m_VertexLengths.GetCount(); }
  float VertexLengths_GetValue(plUInt32 uiIndex) const { return m_VertexLengths[uiIndex]; }
  void VertexLengths_SetValue(plUInt32 uiIndex, float fValue);
  void VertexLengths_Insert(plUInt32 uiIndex, float fValue);
  void VertexLengths_Remove(plUInt32 uiIndex);

  /// \brief The settings of the last rebuild, copied from the Grass Type.
  const plGrassSettings& GetSettings() const { return m_Settings; }

  /// \brief Strands grown by the last rebuild and uploaded into the pool.
  plUInt32 GetStrandCount() const { return m_uiStrandCount; }

  /// \brief The type's material, or the default grass material.
  plMaterialResourceHandle GetMaterial() const;

  /// \brief True when the type's Cast Shadows and Vegetation.Grass.Shadows both allow shadows.
  bool CastsShadows() const;

  static constexpr plUInt32 MaxStrandCount = 100000;

private:
  friend class plGrassSurfaceComponentManager;
  void Update(bool bShadowSettingsChanged);
  bool UpdateTypeSettings();
  void Rebuild();
  void ReleaseGpu();
  void UpdateRenderObject();
  void InvalidateCachedShadows(bool bForce = false);

  plMeshResourceHandle m_hMesh;
  plCpuMeshResourceHandle m_hCpuMesh;
  plUInt32 m_uiMeshChangeCounter = 0;
  plGrassTypeResourceHandle m_hGrassType;
  plUInt32 m_uiTypeChangeCounter = 0;
  bool m_bTypeChanged = true;
  plGrassSettings m_Settings;
  plMaterialResourceHandle m_hTypeMaterial;
  plMaterialResourceHandle m_hDefaultMaterial;
  plDynamicArray<float> m_VertexLengths;
  bool m_bRebuild = true;
  float m_fTextureAspect = 1.0f;
  plVec3 m_vBuiltScale = plVec3(1.0f);

  plUInt32 m_uiStrandCount = 0;
  plBoundingBox m_LocalBounds = plBoundingBox::MakeInvalid(); ///< owner space, every strand pose
  plGrassSourceHandle m_hSource;
  plTransform m_UploadedTransform = plTransform::MakeIdentity();
  plGameObjectHandle m_hRenderObject;
  plComponentHandle m_hRenderComponent;
  plBoundingBox m_LastCasterBox = plBoundingBox::MakeInvalid();
};
