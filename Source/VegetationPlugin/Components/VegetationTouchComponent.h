#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <VegetationPlugin/VegetationDeclarations.h>

using plVegetationTouchComponentManager = plComponentManagerSimple<class plVegetationTouchComponent, plComponentUpdateType::WhenSimulating>;

/// \brief Bends vegetation around its owner: attach to characters, vehicles, projectiles. Registers a touch sphere
/// with the vegetation module every frame; the module's touch map keeps the push and lets it recover.
class PL_VEGETATIONPLUGIN_DLL plVegetationTouchComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plVegetationTouchComponent, plComponent, plVegetationTouchComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plVegetationTouchComponent

public:
  plVegetationTouchComponent();
  ~plVegetationTouchComponent();

  void Update();

  float m_fRadius = 0.8f;            // [ property ]
  float m_fStrength = 1.0f;          // [ property ]
  float m_fVelocityInfluence = 0.5f; // [ property ]
  float m_fCrush = 0.5f;             // [ property ]

private:
  plVec3 m_vLastPosition = plVec3::MakeZero();
  bool m_bHasLastPosition = false;
};
