#pragma once

#include <Core/World/Declarations.h>
#include <EnginePluginVegetation/EnginePluginVegetationDLL.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Types/Uuid.h>
#include <RendererCore/Material/MaterialResource.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>

struct plEngineProcessToolConfigEvent;
struct plVegetationStrokeRequest;
struct plVegetationStrokeResult;
struct plVegetationResnapRequest;
struct plVegetationResnapResult;
class plEngineProcessDocumentContext;
class plWorld;

/// \brief Engine-process side of the paint tool: evaluates "Vegetation.Stroke" requests against the scene's
/// collision (with the pick plane as fallback) and answers with the instances to add and the grass coverage delta.
/// Place-mode preview requests instead move a translucent ghost of the plant to where the click would put it.
/// "Vegetation.Resnap" re-raycasts a layer's coverage, "Vegetation.Overlay" shows the coverage overlay of a layer.
///
/// Every stamp of a running stroke is also mirrored onto a stroke preview of the target layer, so the stroke renders
/// while the mouse is still down. "Vegetation.DropPreview" restores the layer from its committed blobs again.
class PL_ENGINEPLUGINVEGETATION_DLL plVegetationBrushHandler
{
public:
  static void Startup();
  static void Shutdown();

  /// \brief Places instances for one paint stamp. Exposed for tests.
  static void EvaluatePaint(plWorld* pWorld, const plVegetationStrokeRequest& request, plVegetationStrokeResult& out_result);

  /// \brief Samples the texels under a Paint stamp for every grass type of the request into out_result.m_GrassDelta.
  static void EvaluateGrass(plWorld* pWorld, const plVegetationStrokeRequest& request, plVegetationStrokeResult& out_result);

  /// \brief Re-raycasts every painted texel, keeping density and length. Texels without a hit keep their surface.
  static void EvaluateResnap(plWorld* pWorld, const plVegetationResnapRequest& request, plVegetationResnapResult& out_result);

private:
  static void OnToolConfig(const plEngineProcessToolConfigEvent& e);
  static void HandleStroke(const plEngineProcessToolConfigEvent& e);
  static void HandleResnap(const plEngineProcessToolConfigEvent& e);
  static void HandleOverlay(const plEngineProcessToolConfigEvent& e);
  static void HandleDropPreview(const plEngineProcessToolConfigEvent& e);
  static void UpdateGhost(plEngineProcessDocumentContext* pContext, const plVegetationStrokeRequest& request);

  /// \brief The stroke of one scene document as it looks so far, in the target layer's local space. Paint stamps add
  /// the instances and coverage the engine just evaluated, Erase and Thin repeat the removal the editor did with the
  /// same parameters and seed, so both sides end up with the same result.
  struct StrokePreview
  {
    plUInt32 m_uiWorldIndex = plInvalidIndex;
    plUInt32 m_uiStrokeId = 0;
    plComponentHandle m_hLayer;
    plVegetationLayerData m_Data;
    plGrassCoverageData m_Grass;
  };

  static void MirrorStroke(plEngineProcessDocumentContext* pContext, plWorld* pWorld, const plVegetationStrokeRequest& request, const plVegetationStrokeResult& result);

  /// \brief Puts the preview's layer back to the instances and coverage of its committed blobs.
  static void RestoreLayer(plWorld* pWorld, const StrokePreview& preview);

  static plHashTable<plUuid, StrokePreview> s_Previews;

  /// \brief The ghost of one scene document: a mesh object with the type's LOD0 mesh in translucent variants of its
  /// materials. Lives in the document's world, so it is recreated whenever that world is gone.
  struct Ghost
  {
    plUInt32 m_uiWorldIndex = plInvalidIndex;
    plGameObjectHandle m_hObject;
    plComponentHandle m_hMesh;
    plString m_sType;
    plHybridArray<plMaterialResourceHandle, 4> m_Materials;
  };

  static plMaterialResourceHandle GetGhostMaterial(const plMaterialResourceHandle& hBase);

  static plHashTable<plUuid, Ghost> s_Ghosts;

  /// \brief The coverage overlay of one scene document: an editor-tagged object whose component draws the target
  /// layer's texels.
  struct Overlay
  {
    plUInt32 m_uiWorldIndex = plInvalidIndex;
    plGameObjectHandle m_hObject;
    plComponentHandle m_hComponent;
  };

  static plHashTable<plUuid, Overlay> s_Overlays;
};
