#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <EnginePluginVegetation/EnginePluginVegetationDLL.h>
#include <Foundation/Containers/DynamicArray.h>
#include <RendererCore/Debug/DebugRenderer.h>

class plVegetationLayerComponent;

using plVegetationCoverageOverlayComponentManager = plComponentManagerSimple<class plVegetationCoverageOverlayComponent, plComponentUpdateType::Always>;

/// \brief Editor-only: draws the grass coverage of one plVegetationLayerComponent as tinted texel quads with the debug
/// renderer, alpha by density, one hue per grass type and orange for Discontinuous texels. Created by
/// plVegetationBrushHandler on an object tagged "Editor" while the Vegetation tool is active.
class PL_ENGINEPLUGINVEGETATION_DLL plVegetationCoverageOverlayComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plVegetationCoverageOverlayComponent, plComponent, plVegetationCoverageOverlayComponentManager);

public:
  plVegetationCoverageOverlayComponent();
  ~plVegetationCoverageOverlayComponent();

  void SetLayer(plComponentHandle hLayer);

  /// \brief Rebuilds the quads when the layer's coverage or transform changed and draws them for this frame.
  void Update();

private:
  void Rebuild(const plVegetationLayerComponent& layer);

  plComponentHandle m_hLayer;
  plUInt32 m_uiBuiltCounter = plInvalidIndex;
  plTransform m_BuiltTransform = plTransform::MakeIdentity();
  plDynamicArray<plDebugRenderer::Triangle> m_Triangles;
};
