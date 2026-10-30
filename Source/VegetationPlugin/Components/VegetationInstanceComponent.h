#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <VegetationPlugin/VegetationDeclarations.h>

struct plMsgTransformChanged;

using plVegetationInstanceComponentManager = plComponentManagerSimple<class plVegetationInstanceComponent, plComponentUpdateType::Always>;

/// \brief One hand-placed plant. Registers a single instance of its vegetation type with the world's vegetation
/// module and follows the owner's transform, so it renders, sways and bends like painted or procedural plants.
class PL_VEGETATIONPLUGIN_DLL plVegetationInstanceComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plVegetationInstanceComponent, plComponent, plVegetationInstanceComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plVegetationInstanceComponent

public:
  plVegetationInstanceComponent();
  ~plVegetationInstanceComponent();

  void SetTypeFile(const char* szFile); // [ property ]
  const char* GetTypeFile() const;      // [ property ]

  void SetType(const plVegetationTypeResourceHandle& hType);
  const plVegetationTypeResourceHandle& GetType() const { return m_hType; }

  void SetColorSeed(plUInt8 uiSeed); // [ property ]
  plUInt8 GetColorSeed() const { return m_uiColorSeed; } // [ property ]

  void SetCastShadows(bool b); // [ property ]
  bool GetCastShadows() const { return m_bCastShadows; } // [ property ]

  void SetTouchBending(bool b); // [ property ]
  bool GetTouchBending() const { return m_bTouchBending; } // [ property ]

  /// \brief Re-registers when marked dirty. Runs in the manager's update where the world is writable; property
  /// setters and transform changes only mark the component.
  void Update();

private:
  void OnMsgTransformChanged(plMsgTransformChanged& msg);
  void Register();
  void Unregister();

  plVegetationTypeResourceHandle m_hType;
  plUInt8 m_uiColorSeed = 0;
  bool m_bCastShadows = true;
  bool m_bTouchBending = true;
  bool m_bDirty = false;

  plVegetationTileHandle m_hTile;
};
