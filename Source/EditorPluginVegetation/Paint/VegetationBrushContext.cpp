#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <EditorFramework/InputContexts/EditorInputEvent.h>
#include <EditorPluginVegetation/Paint/VegetationBrushContext.h>
#include <EditorPluginVegetation/Paint/VegetationBrushSettings.h>
#include <EditorPluginVegetation/Paint/VegetationPaintEditTool.h>
#include <EditorUi/Widgets.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationBrushContext, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

namespace
{
  /// The ring in the accent colour, or in the danger colour while the brush erases; the falloff ring is fainter.
  plColor RingColor(bool bErase, bool bInner)
  {
    plColorGammaUB color = bErase ? plEditorUi::Danger() : plEditorUi::Accent();
    if (bInner)
      color.a = 120;
    return color;
  }
} // namespace

plVegetationBrushContext::plVegetationBrushContext()
{
  m_hRing.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, RingColor(false, false), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hInnerRing.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, RingColor(false, true), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);

  SetVisible(false);
  SetTransformation(plTransform::MakeIdentity());
}

plVegetationBrushContext::~plVegetationBrushContext() = default;

void plVegetationBrushContext::OnSetOwner(plEngineDocumentWindow* pOwnerWindow, plEngineViewWidget* pOwnerView)
{
  PL_IGNORE_UNUSED(pOwnerView);

  // The ring handles only reach the engine process as sync objects of the document.
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hRing);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hInnerRing);
}

void plVegetationBrushContext::OnVisibleChanged(bool bVisible)
{
  m_hRing.SetVisible(bVisible && m_bHasSurface);
  m_hInnerRing.SetVisible(bVisible && m_bHasSurface);

  if (!bVisible)
  {
    m_bDragging = false;
    m_bHasSurface = false;

    if (m_pTool != nullptr)
      m_pTool->HidePlacePreview();
  }
}

void plVegetationBrushContext::OnTransformationChanged(const plTransform& transform)
{
  PL_IGNORE_UNUSED(transform);
}

void plVegetationBrushContext::DoFocusLost(bool bCancel)
{
  PL_IGNORE_UNUSED(bCancel);

  if (m_bDragging && m_pTool != nullptr)
  {
    m_pTool->EndStroke();
  }

  m_bDragging = false;
}

void plVegetationBrushContext::UpdateStatusBarText(plEngineDocumentWindow* pWindow)
{
  const auto& s = plVegetationBrush::s_Settings;
  const char* szMode = "Paint";
  switch (m_bEraseHeld ? plVegetationBrushMode::Erase : s.m_Mode)
  {
    case plVegetationBrushMode::Erase:
      szMode = "Erase";
      break;
    case plVegetationBrushMode::Thin:
      szMode = "Thin";
      break;
    case plVegetationBrushMode::Place:
      szMode = "Place";
      break;
    default:
      break;
  }

  plStringBuilder sText;
  sText.SetFormat("Vegetation \xC2\xB7 {} \xC2\xB7 radius {} m \xC2\xB7 density {} \xC2\xB7 [ ] radius \xC2\xB7 Ctrl-drag erases \xC2\xB7 Esc leaves the tool", szMode, plArgF(s.m_fRadius, 1), plArgF(s.m_fDensity, 2));
  pWindow->SetPermanentStatusBarMsg(sText);
}

bool plVegetationBrushContext::UpdateCursor(const plVec2I32& vViewPos)
{
  plEngineViewWidget* pView = GetOwnerView();
  if (pView == nullptr)
    return false;

  const plObjectPickingResult& res = pView->PickObject(static_cast<plUInt16>(vViewPos.x), static_cast<plUInt16>(vViewPos.y));

  // The picking pass reports the surface under the cursor even when nothing maps back to a document object
  // (existing vegetation, engine-only geometry): any hit short of the far plane is a paintable surface.
  bool bHitSurface = res.m_PickedObject.IsValid() || res.m_PickedComponent.IsValid();
  if (!bHitSurface && !res.m_vPickedPosition.IsNaN() && pView->m_pViewConfig != nullptr)
  {
    const float fFar = pView->m_pViewConfig->m_Camera.GetFarPlane();
    bHitSurface = (res.m_vPickedPosition - res.m_vPickingRayStart).GetLengthSquared() < fFar * fFar * 0.9f;
  }

  if (bHitSurface)
  {
    m_vCursor = res.m_vPickedPosition;
    m_vCursorNormal = res.m_vPickedNormal;
    if (m_vCursorNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).Failed())
      m_vCursorNormal.Set(0, 0, 1);
    m_bHasSurface = true;
  }
  else
  {
    // No geometry under the cursor: fall back to the ground plane so painting on an empty scene still works.
    plVec3 vPoint;
    if (pView->PickPlane(static_cast<plUInt16>(vViewPos.x), static_cast<plUInt16>(vViewPos.y), pView->GetFallbackPickingPlane(m_vCursor), vPoint).Succeeded())
    {
      m_vCursor = vPoint;
      m_vCursorNormal.Set(0, 0, 1);
      m_bHasSurface = true;
    }
    else
    {
      m_bHasSurface = false;
    }
  }

  UpdateRing();
  return m_bHasSurface;
}

void plVegetationBrushContext::UpdateRing()
{
  if (!m_bHasSurface || !IsVisible())
  {
    m_hRing.SetVisible(false);
    m_hInnerRing.SetVisible(false);
    return;
  }

  const auto& s = plVegetationBrush::s_Settings;
  const float fRadius = s.m_fRadius;
  const float fInner = fRadius * (1.0f - plMath::Clamp(s.m_fFalloff, 0.0f, 1.0f));

  const bool bErase = m_bEraseHeld || s.m_Mode == plVegetationBrushMode::Erase;
  m_hRing.SetColor(RingColor(bErase, false));
  m_hInnerRing.SetColor(RingColor(bErase, true));

  plVec3 vT1 = m_vCursorNormal.GetOrthogonalVector().GetNormalized();
  plVec3 vT2 = m_vCursorNormal.CrossRH(vT1).GetNormalized();

  auto BuildCircle = [&](float fR, plHybridArray<plVec3, 68>& out_lines)
  {
    out_lines.Clear();
    constexpr plUInt32 uiSegments = 32;
    for (plUInt32 i = 0; i < uiSegments; ++i)
    {
      const plAngle a0 = plAngle::MakeFromRadian((i / (float)uiSegments) * 2.0f * plMath::Pi<float>());
      const plAngle a1 = plAngle::MakeFromRadian(((i + 1) / (float)uiSegments) * 2.0f * plMath::Pi<float>());
      out_lines.PushBack(m_vCursor + (vT1 * plMath::Cos(a0) + vT2 * plMath::Sin(a0)) * fR + m_vCursorNormal * 0.02f);
      out_lines.PushBack(m_vCursor + (vT1 * plMath::Cos(a1) + vT2 * plMath::Sin(a1)) * fR + m_vCursorNormal * 0.02f);
    }
  };

  plHybridArray<plVec3, 68> lines;
  BuildCircle(fRadius, lines);
  m_hRing.SetLines(lines);
  m_hRing.SetTransformation(plTransform::MakeIdentity());
  m_hRing.SetVisible(true);

  if (fInner > 0.01f && fInner < fRadius - 0.01f)
  {
    BuildCircle(fInner, lines);
    m_hInnerRing.SetLines(lines);
    m_hInnerRing.SetTransformation(plTransform::MakeIdentity());
    m_hInnerRing.SetVisible(true);
  }
  else
  {
    m_hInnerRing.SetVisible(false);
  }
}

void plVegetationBrushContext::SetEraseHeld(bool bHeld)
{
  if (m_bEraseHeld == bHeld)
    return;

  m_bEraseHeld = bHeld;
  UpdateRing();

  if (GetOwnerWindow() != nullptr)
    UpdateStatusBarText(GetOwnerWindow());
}

plEditorInput plVegetationBrushContext::DoKeyReleaseEvent(const plEditorInputEvent& e)
{
  if (e.m_Key == plEditorPlatformKey::Control)
    SetEraseHeld(false);

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plVegetationBrushContext::DoKeyPressEvent(const plEditorInputEvent& e)
{
  auto& s = plVegetationBrush::s_Settings;

  if (e.m_Key == plEditorPlatformKey::Control)
  {
    SetEraseHeld(true);
    return plEditorInput::MayBeHandledByOthers;
  }

  if (e.m_Key == plEditorPlatformKey::Escape)
  {
    if (m_bDragging && m_pTool != nullptr)
    {
      m_pTool->EndStroke();
      m_bDragging = false;
    }

    if (m_pTool != nullptr)
    {
      m_pTool->Deactivate();
    }
    return plEditorInput::WasExclusivelyHandled;
  }

  if (e.IsLetter('[') || e.IsLetter(']'))
  {
    const bool bShift = e.HasModifier(plEditorInputEvent::Shift);
    const float fStep = bShift ? 0.05f : 0.5f;
    const float fSign = e.IsLetter(']') ? 1.0f : -1.0f;
    if (bShift)
    {
      s.m_fDensity = plMath::Clamp(s.m_fDensity + fSign * fStep, 0.05f, 1.0f);
    }
    else
    {
      s.m_fRadius = plMath::Clamp(s.m_fRadius + fSign * fStep, 0.5f, 30.0f);
    }

    plVegetationBrush::NotifySettingsChanged();
    UpdateRing();
    UpdateStatusBarText(GetOwnerWindow());
    return plEditorInput::WasExclusivelyHandled;
  }

  if (e.GetLetter() >= '1' && e.GetLetter() <= '4' && e.m_uiModifiers == 0)
  {
    const auto mode = static_cast<plVegetationBrushMode::Enum>(e.GetLetter() - '1');
    if (mode == plVegetationBrushMode::Place && !s.IsPlaceAvailable())
      return plEditorInput::WasExclusivelyHandled;

    s.m_Mode = mode;
    plVegetationBrush::NotifySettingsChanged();
    UpdateStatusBarText(GetOwnerWindow());
    return plEditorInput::WasExclusivelyHandled;
  }

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plVegetationBrushContext::DoMousePressEvent(const plEditorInputEvent& e)
{
  // camera navigation always wins
  if (e.HasModifier(plEditorInputEvent::Alt) || e.m_uiButton != plEditorInputEvent::LeftButton)
    return plEditorInput::MayBeHandledByOthers;

  if (m_pTool == nullptr || !UpdateCursor(e.GetPosition()))
    return plEditorInput::WasExclusivelyHandled;

  m_bEraseOverride = e.HasModifier(plEditorInputEvent::Control);

  const auto mode = m_bEraseOverride ? plVegetationBrushMode::Erase : plVegetationBrush::s_Settings.m_Mode;

  if (mode == plVegetationBrushMode::Place)
  {
    m_pTool->PlaceSingle(m_vCursor, m_vCursorNormal);
    return plEditorInput::WasExclusivelyHandled;
  }

  m_bDragging = true;
  m_vLastStamp = m_vCursor;
  m_pTool->BeginStroke(mode);
  m_pTool->StrokeSegment(m_vCursor, m_vCursorNormal);

  return plEditorInput::WasExclusivelyHandled;
}

plEditorInput plVegetationBrushContext::DoMouseReleaseEvent(const plEditorInputEvent& e)
{
  if (e.m_uiButton != plEditorInputEvent::LeftButton)
    return plEditorInput::MayBeHandledByOthers;

  if (m_bDragging && m_pTool != nullptr)
  {
    m_pTool->EndStroke();
  }

  m_bDragging = false;
  m_bEraseOverride = false;
  return plEditorInput::WasExclusivelyHandled;
}

plEditorInput plVegetationBrushContext::DoMouseMoveEvent(const plEditorInputEvent& e)
{
  if (e.HasModifier(plEditorInputEvent::Alt) && !m_bDragging)
    return plEditorInput::MayBeHandledByOthers;

  // Ctrl may have been pressed while another view had the keyboard
  SetEraseHeld(e.HasModifier(plEditorInputEvent::Control));
  UpdateCursor(e.GetPosition());

  if (m_bDragging && m_bHasSurface && m_pTool != nullptr)
  {
    // Stamp spacing is half a radius so strokes stay continuous without flooding the engine with segments.
    const float fSpacing = plMath::Max(plVegetationBrush::s_Settings.m_fRadius * 0.5f, 0.1f);
    if ((m_vCursor - m_vLastStamp).GetLengthSquared() >= fSpacing * fSpacing)
    {
      m_vLastStamp = m_vCursor;
      m_pTool->StrokeSegment(m_vCursor, m_vCursorNormal);
    }
  }
  else if (m_pTool != nullptr)
  {
    // Place mode shows a ghost of the plant at the cursor; Ctrl turns the click into an erase, so no ghost then.
    const bool bPlace = plVegetationBrush::s_Settings.m_Mode == plVegetationBrushMode::Place && !e.HasModifier(plEditorInputEvent::Control);
    if (bPlace && m_bHasSurface)
    {
      m_pTool->UpdatePlacePreview(m_vCursor, m_vCursorNormal);
    }
    else
    {
      m_pTool->HidePlacePreview();
    }
  }

  return m_bDragging ? plEditorInput::WasExclusivelyHandled : plEditorInput::MayBeHandledByOthers;
}
