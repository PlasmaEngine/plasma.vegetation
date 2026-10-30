#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/Paint/VegetationBrushSettings.h>

plVegetationBrushSettings plVegetationBrush::s_Settings;
plEvent<const plVegetationBrushEvent&> plVegetationBrush::s_Events;

plUInt32 plVegetationBrushSettings::GetSelectedTypeCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& type : m_Types)
  {
    if (type.m_bSelected)
      ++uiCount;
  }
  return uiCount;
}

plUInt32 plVegetationBrushSettings::GetSelectedPlantCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& type : m_Types)
  {
    if (type.m_bSelected && !type.m_bGrass)
      ++uiCount;
  }
  return uiCount;
}

plUInt32 plVegetationBrushSettings::GetSelectedGrassCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& type : m_Types)
  {
    if (type.m_bSelected && type.m_bGrass)
      ++uiCount;
  }
  return uiCount;
}

void plVegetationBrushSettings::FillRequestGrassTypes(plHybridArray<plString, 8>& out_types) const
{
  out_types.Clear();

  for (const auto& type : m_Types)
  {
    if (!type.m_bSelected || !type.m_bGrass || type.m_sGuid.IsEmpty())
      continue;

    if (out_types.GetCount() >= 8)
      break;

    out_types.PushBack(type.m_sGuid);
  }
}

void plVegetationBrushSettings::FillRequestTypes(plHybridArray<plString, 8>& out_types, plHybridArray<float, 8>& out_weights) const
{
  out_types.Clear();
  out_weights.Clear();

  for (const auto& type : m_Types)
  {
    if (!type.m_bSelected || type.m_bGrass || type.m_sGuid.IsEmpty())
      continue;

    if (out_types.GetCount() >= 8)
      break;

    out_types.PushBack(type.m_sGuid);
    out_weights.PushBack(type.m_fWeight);
  }
}

void plVegetationBrush::NotifySettingsChanged()
{
  plVegetationBrushEvent e;
  e.m_Type = plVegetationBrushEvent::Type::SettingsChanged;
  s_Events.Broadcast(e);
}

void plVegetationBrush::NotifyStrokeApplied()
{
  plVegetationBrushEvent e;
  e.m_Type = plVegetationBrushEvent::Type::StrokeApplied;
  s_Events.Broadcast(e);
}
