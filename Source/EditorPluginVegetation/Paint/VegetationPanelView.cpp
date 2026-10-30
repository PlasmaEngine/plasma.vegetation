#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/Paint/VegetationBrushSettings.h>
#include <EditorPluginVegetation/Paint/VegetationPanel.h>
#include <EditorPluginVegetation/Paint/VegetationPanelView.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Math/ColorScheme.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>

namespace
{
  using namespace plEditorUi;

  constexpr float s_fToolRow = 31.0f;
  constexpr float s_fLabelWidth = 72.0f;
  constexpr float s_fValueWidth = 54.0f;
  constexpr float s_fLine = 22.0f;
  constexpr float s_fWheelStep = 40.0f;
  constexpr const char* s_szToolIcon = ":/EditorPluginVegetation/Paint.svg";

  const plHashedString s_sScroll = plMakeHashedString("Veg.Scroll");
  const plHashedString s_sHintDraw = plMakeHashedString("Veg.HintDraw");

  /// One brush slider: its range, snapping and how its value reads.
  struct SliderInfo
  {
    const char* m_szLabel;
    float m_fMin;
    float m_fMax;
    float m_fStep;
    bool m_bMeters;
  };

  constexpr SliderInfo s_Sliders[] = {
    {"Radius", 0.5f, 30.0f, 0.1f, true},
    {"Density", 0.05f, 1.0f, 0.01f, false},
    {"Falloff", 0.0f, 1.0f, 0.01f, false},
    {"Strength", 0.05f, 1.0f, 0.01f, false},
    {"Length", 0.0f, 1.0f, 0.01f, false},
  };

  const char* s_szModes[] = {"Paint", "Erase", "Thin", "Place"};
  const char* s_szModeTips[] = {"Left-drag scatters the checked plants and paints the checked Grass Types (1)", "Left-drag removes the checked types (2)", "Left-drag thins toward the density value (3)", "Click places one instance of the checked plants; unavailable for Grass Types (4)"};
  const char* s_szFields[] = {"Veg.MaxSlope", "Veg.MinScale", "Veg.MaxScale"};

  float& SliderValue(plUInt32 uiSlider)
  {
    auto& s = plVegetationBrush::s_Settings;
    switch (uiSlider)
    {
      case 0:
        return s.m_fRadius;
      case 1:
        return s.m_fDensity;
      case 2:
        return s.m_fFalloff;
      case 3:
        return s.m_fStrength;
      default:
        return s.m_fLength;
    }
  }

  plString Key(plStringView sPrefix, plUInt32 uiIndex)
  {
    plStringBuilder s;
    s.SetFormat("{}.{}", sPrefix, uiIndex);
    return s;
  }

  void Place(plUiElement* pElement, const plRectFloat& rect)
  {
    pElement->m_vAnchorMin = pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(rect.x, rect.y, rect.width, rect.height);
  }

  plUiPanel* Rule(plUiElement* pParent, const plRectFloat& rect)
  {
    plUiPanel* pRule = Panel(pParent);
    Place(pRule, rect);
    pRule->m_Color = Line();
    return pRule;
  }

  plColorGammaUB WithAlpha(plColorGammaUB color, plUInt8 uiAlpha)
  {
    color.a = uiAlpha;
    return color;
  }

  float LabelWidth(plStringView sText, plUInt32 uiSize = FontSize - 1)
  {
    return plMath::Ceil(MeasureText(sText, uiSize));
  }

  /// A text button with an optional icon in front; primary on the selection fill with an accent border; disabled
  /// with dim text.
  plUiButton* ToolButton(plUiElement* pParent, const plRectFloat& rect, plStringView sLabel, plStringView sAction, plStringView sIcon = {}, bool bPrimary = false, bool bEnabled = true)
  {
    plUiButton* pButton = TextButton(pParent, sLabel, sAction);
    Place(pButton, rect);

    if (bPrimary)
    {
      pButton->m_Color = Selection();
      pButton->m_BorderColor = WithAlpha(Accent(), 115);
    }

    if (!bEnabled)
    {
      pButton->m_Color = Field();
      pButton->m_HoverColor = Field();
      pButton->m_PressedColor = Field();
    }

    auto* pText = static_cast<plUiText*>(pButton->m_Children[0]);
    if (!bEnabled)
      pText->m_Color = Dim();

    if (!sIcon.IsEmpty())
    {
      Fill(pText, plVec4(26, 0, 6, 0));
      pText->m_HAlign = plUiTextHAlign::Left;

      plUiImage* pImage = ButtonIcon(pButton, sIcon);
      pImage->m_vAnchorMin = pImage->m_vAnchorMax = plVec2(0, 0.5f);
      pImage->m_vOffsets = plVec4(8, -7, 14, 14);
      pImage->m_Color = bPrimary ? Accent() : Muted();
      if (!bEnabled)
        pImage->m_Color.a = 90;
    }

    return pButton;
  }

  float ToolButtonWidth(plStringView sLabel, bool bIcon)
  {
    return LabelWidth(sLabel) + (bIcon ? 38.0f : 22.0f);
  }

  /// A check box (style pass D9) with its label; the whole row toggles.
  plUiButton* CheckRow(plUiElement* pParent, const plRectFloat& rect, plStringView sLabel, plStringView sDetail, plStringView sAction, bool bChecked)
  {
    plUiButton* pButton = Button(pParent, "", sAction);
    Place(pButton, rect);
    pButton->ClearChildren();
    pButton->m_Color = plColorGammaUB(0, 0, 0, 0);
    pButton->m_hStyle = GetGhostStyle();

    plUiPanel* pBox = Panel(pButton);
    pBox->m_vAnchorMin = pBox->m_vAnchorMax = plVec2(0, 0.5f);
    pBox->m_vOffsets = plVec4(0, -7, 14, 14);
    pBox->m_Color = bChecked ? Selection() : Field();
    pBox->m_fBorderWidth = 1.0f;
    pBox->m_BorderColor = bChecked ? WithAlpha(Accent(), 153) : Border();
    pBox->m_vCornerRadius = plVec4(3);

    if (bChecked)
    {
      plUiImage* pTick = SvgImage(pBox, IconPath(Icon::Check));
      Fill(pTick, plVec4(1, 1, 1, 1));
      pTick->m_Color = Accent();
    }

    const float fLabel = LabelWidth(sLabel);
    plUiText* pText = Text(pButton, sLabel);
    pText->m_vAnchorMin = plVec2(0, 0);
    pText->m_vAnchorMax = plVec2(0, 1);
    pText->m_vOffsets = plVec4(22, 0, fLabel + 4, 0);
    pText->m_uiFontSize = FontSize - 1;
    pText->m_Color = Muted();

    if (!sDetail.IsEmpty())
    {
      plUiText* pDetail = Text(pButton, sDetail);
      Fill(pDetail, plVec4(22 + fLabel + 8, 0, 0, 0));
      pDetail->m_hFont = GetCodeFont();
      pDetail->m_uiFontSize = MinFontSize + 1;
      pDetail->m_Color = Dim();
    }

    return pButton;
  }

  /// A dropdown: a field well with the choice and a chevron.
  plUiButton* Dropdown(plUiElement* pParent, const plRectFloat& rect, plStringView sValue, plStringView sAction)
  {
    plUiButton* pButton = Button(pParent, "", sAction);
    Place(pButton, rect);
    pButton->ClearChildren();
    pButton->m_Color = Field();
    pButton->m_HoverColor = Raised();
    pButton->m_fBorderWidth = 1.0f;
    pButton->m_BorderColor = Line();
    pButton->m_vCornerRadius = plVec4(4);

    plUiText* pText = Text(pButton, sValue);
    Fill(pText, plVec4(8, 0, 22, 0));
    pText->m_uiFontSize = FontSize - 1;

    plUiImage* pChevron = ButtonIcon(pButton, Icon::Expand);
    pChevron->m_vAnchorMin = pChevron->m_vAnchorMax = plVec2(1, 0.5f);
    pChevron->m_vOffsets = plVec4(-18, -6, 12, 12);
    pChevron->m_Color = Dim();
    return pButton;
  }

  /// A key cap made of elements, for buttons: a plate with a hairline and the key in the code face.
  void KeyCapElement(plUiElement* pParent, float fRight, plStringView sKey)
  {
    const float fWidth = plMath::Max(plMath::Ceil(MeasureCodeText(sKey, MinFontSize)) + 8.0f, 16.0f);

    plUiPanel* pCap = Panel(pParent);
    pCap->m_vAnchorMin = pCap->m_vAnchorMax = plVec2(1, 0.5f);
    pCap->m_vOffsets = plVec4(fRight - fWidth, -8, fWidth, 16);
    pCap->m_Color = Plate();
    pCap->m_fBorderWidth = 1.0f;
    pCap->m_BorderColor = Border();
    pCap->m_vCornerRadius = plVec4(3);

    plUiText* pText = Text(pCap, sKey);
    Fill(pText);
    pText->m_hFont = GetCodeFont();
    pText->m_uiFontSize = MinFontSize;
    pText->m_Color = Muted();
    pText->m_HAlign = plUiTextHAlign::Center;
  }

  /// A piece of the key hint: words, or a key drawn as a key cap.
  struct HintToken
  {
    const char* m_szText;
    bool m_bKey;
  };

  constexpr HintToken s_Hint[] = {
    {"[", true}, {" ", false}, {"]", true}, {" radius \xC2\xB7 ", false},
    {"Shift", true}, {"+", false}, {"[", true}, {" ", false}, {"]", true}, {" density \xC2\xB7 ", false},
    {"Ctrl", true}, {"-drag erases in any mode \xC2\xB7 ", false},
    {"1", true}, {"-", false}, {"4", true}, {" switch modes", false},
  };

  float HintTokenWidth(const HintToken& token)
  {
    return token.m_bKey ? plEditorUiDraw::KeyCapWidth(token.m_szText) : plMath::Ceil(MeasureText(token.m_szText, FontSize - 2));
  }
} // namespace

plVegetationPanelView::plVegetationPanelView(plDocument* pDocument)
  : plEditorUiContent("VegetationPanel")
{
  m_pPanel = PL_DEFAULT_NEW(plVegetationPanel, pDocument);

  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plVegetationPanelView::OnPointer, this));

  m_pPanel->SetView(this);
}

plVegetationPanelView::~plVegetationPanelView()
{
  // the panel's logic goes first: its document events must not reach a view that is going
  m_pPanel.Clear();

  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plVegetationPanelView::Refresh()
{
  if (!IsReady())
    return;

  if (GetStructureKey() != m_sBuiltKey)
    m_bRebuild = true;
  else
    UpdateBindings();
}

plString plVegetationPanelView::GetStructureKey() const
{
  // everything the document shows as elements rather than bound values
  const auto& s = plVegetationBrush::s_Settings;

  plStringBuilder key;
  key.SetFormat("{}|{}|{}|{}|{}|{}|{}|{}|", m_pPanel->IsToolActive(), m_pPanel->GetLayerIndex(), static_cast<plUInt32>(s.m_Mode), s.IsPlaceAvailable(), s.GetSelectedGrassCount() > 0, s.m_bAlignToNormal, m_pPanel->CanResnap(), m_pPanel->CanClearCoverage());

  for (const plString& sLayer : m_pPanel->GetLayerNames())
    key.Append(sLayer, "/");

  for (const auto& type : s.m_Types)
  {
    plStringBuilder count;
    if (!type.m_bGrass)
      count.SetFormat("{}", m_pPanel->GetInstanceCount(type.m_sGuid));
    key.Append(type.m_sGuid, type.m_bSelected ? "+" : "-", count, ";");
  }

  key.Append("|", m_pPanel->GetStatistics());
  return key;
}

void plVegetationPanelView::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (m_bRebuild || GetSurface()->GetLogicalSize() != m_vBuiltSize)
    BuildDocument();

  PollSliders();
  PollFields();
}

void plVegetationPanelView::BuildDocument()
{
  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  m_sBuiltKey = GetStructureKey();

  const float fWidth = plMath::Max(m_vBuiltSize.x, 200.0f);
  const auto& s = plVegetationBrush::s_Settings;
  auto& board = GetSurface()->GetBlackboard();
  const plColorGammaUB sectionColor = plColorScheme::GetCategoryColor("Effects", plColorScheme::CategoryColorUsage::BorderColor);

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();

  // everything scrolls inside one offset panel
  plUiOffsetPanel* pContent = PL_DEFAULT_NEW(plUiOffsetPanel);
  pRoot->m_Children.PushBack(pContent);
  pContent->m_sOffsetBinding = s_sScroll.GetView();
  pContent->m_Color = plColorGammaUB(0, 0, 0, 0);

  float y = 0.0f;
  auto Section = [&](plStringView sTitle, plStringView sCountBinding = {})
  {
    plUiPanel* pHeader = SectionHeader(pContent, sTitle, sectionColor, sCountBinding);
    pHeader->m_vOffsets.y = y;
    y += SectionHeaderHeight;
    return pHeader;
  };

  // the tool
  {
    const bool bActive = m_pPanel->IsToolActive();
    const char* szLabel = bActive ? "Vegetation Tool active" : "Activate Vegetation Tool";
    ToolButton(pContent, plRectFloat(6, y + 3, ToolButtonWidth(szLabel, true), ControlHeight), szLabel, "Veg.Tool", s_szToolIcon, bActive);
    SetToolTip(board, "Veg.Tool", "Same as the Vegetation button in the toolbar. Esc leaves the tool.");
    y += s_fToolRow;
    Rule(pContent, plRectFloat(0, y - 1, fWidth, 1));
  }

  // target layer
  {
    Section("Target layer");

    const float fNew = ToolButtonWidth("New", true);
    m_LayerRect = plRectFloat(6, y + 3, fWidth - 12 - fNew - 4, ControlHeight);
    const plArrayPtr<const plString> layers = m_pPanel->GetLayerNames();
    const plInt32 iLayer = m_pPanel->GetLayerIndex();
    Dropdown(pContent, m_LayerRect, iLayer >= 0 && iLayer < static_cast<plInt32>(layers.GetCount()) ? layers[iLayer].GetView() : plStringView(), "Veg.Layer");
    ToolButton(pContent, plRectFloat(fWidth - 6 - fNew, y + 3, fNew, ControlHeight), "New", "Veg.NewLayer", IconPath(Icon::Add));
    SetToolTip(board, "Veg.NewLayer", "Adds a Vegetation Layer object to the scene and targets it.");
    y += s_fToolRow - 1;
  }

  // mode: two by two, each with its key
  {
    Section("Mode");

    const float fButton = (fWidth - 16.0f - 4.0f) * 0.5f;
    for (plUInt32 i = 0; i < 4; ++i)
    {
      const bool bPlace = i == plVegetationBrushMode::Place;
      const bool bEnabled = !bPlace || s.IsPlaceAvailable();
      const plRectFloat rect(8 + (i % 2) * (fButton + 4), y + 6 + (i / 2) * (ControlHeight + 4), fButton, ControlHeight);
      const plString sAction = Key("Veg.Mode", i);

      plUiButton* pButton = ToolButton(pContent, rect, s_szModes[i], sAction, {}, s.m_Mode == i, bEnabled);
      auto* pText = static_cast<plUiText*>(pButton->m_Children[0]);
      Fill(pText, plVec4(10, 0, 30, 0));
      pText->m_HAlign = plUiTextHAlign::Left;

      plStringBuilder key;
      key.SetFormat("{}", i + 1);
      KeyCapElement(pButton, -6.0f, key);
      SetToolTip(board, sAction, s_szModeTips[i]);
    }
    y += 6 + 2 * ControlHeight + 4 + 6;

    if (!s.IsPlaceAvailable())
    {
      plUiText* pWarning = Text(pContent, "Place needs a plant type. Grass Types are painted as coverage.");
      Place(pWarning, plRectFloat(10, y, fWidth - 20, 18));
      pWarning->m_uiFontSize = FontSize - 2;
      pWarning->m_Color = Warning();
      y += 22.0f;
    }
  }

  // brush values
  {
    Section("Brush");
    y += 6.0f;

    const bool bLength = s.GetSelectedGrassCount() > 0;
    const float fSlider = plMath::Max(fWidth - 20.0f - s_fLabelWidth - s_fValueWidth - 12.0f, 40.0f);

    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_Sliders); ++i)
    {
      if (i == 4 && !bLength)
        continue;

      plUiText* pLabel = Text(pContent, s_Sliders[i].m_szLabel);
      Place(pLabel, plRectFloat(10, y, s_fLabelWidth, s_fLine));
      pLabel->m_uiFontSize = FontSize - 1;
      pLabel->m_Color = Muted();

      const plString sBinding = Key("Veg.Slider", i);
      plUiSlider* pSlider = PL_DEFAULT_NEW(plUiSlider);
      pContent->m_Children.PushBack(pSlider);
      pSlider->m_sName = sBinding;
      Place(pSlider, plRectFloat(10 + s_fLabelWidth + 6, y + s_fLine * 0.5f - 4.0f, fSlider, 8));
      pSlider->m_fMin = s_Sliders[i].m_fMin;
      pSlider->m_fMax = s_Sliders[i].m_fMax;
      pSlider->m_sValueBinding = sBinding;
      pSlider->m_Color = Raised();
      pSlider->m_FillColor = Border();
      pSlider->m_HandleColor = Muted();
      pSlider->m_vCornerRadius = plVec4(4);
      if (i == 4)
        SetToolTip(board, sBinding, "Length multiplier Paint writes into grass coverage.");

      plUiText* pValue = Text(pContent, "", Key("Veg.SliderText", i));
      Place(pValue, plRectFloat(fWidth - 10 - s_fValueWidth, y, s_fValueWidth, s_fLine));
      pValue->m_hFont = GetCodeFont();
      pValue->m_uiFontSize = FontSize - 2;
      pValue->m_HAlign = plUiTextHAlign::Right;
      pValue->m_Color = Foreground();

      y += s_fLine + 6.0f;
    }

    // max slope and the scale range
    {
      plUiText* pLabel = Text(pContent, "Max slope");
      Place(pLabel, plRectFloat(10, y, s_fLabelWidth, ControlHeight));
      pLabel->m_uiFontSize = FontSize - 1;
      pLabel->m_Color = Muted();
      NumberField(pContent, plRectFloat(10 + s_fLabelWidth + 6, y, 90, ControlHeight), s_szFields[0], "Veg.MaxSlopeText", "Veg.MaxSlopeSubmit", {}, "\xC2\xB0");
      y += ControlHeight + 6.0f;

      plUiText* pScale = Text(pContent, "Scale");
      Place(pScale, plRectFloat(10, y, s_fLabelWidth, ControlHeight));
      pScale->m_uiFontSize = FontSize - 1;
      pScale->m_Color = Muted();

      const float x = 10 + s_fLabelWidth + 6;
      NumberField(pContent, plRectFloat(x, y, 70, ControlHeight), s_szFields[1], "Veg.MinScaleText", "Veg.MinScaleSubmit");
      plUiText* pTo = Text(pContent, "to");
      Place(pTo, plRectFloat(x + 74, y, 18, ControlHeight));
      pTo->m_uiFontSize = FontSize - 1;
      pTo->m_Color = Dim();
      NumberField(pContent, plRectFloat(x + 94, y, 70, ControlHeight), s_szFields[2], "Veg.MaxScaleText", "Veg.MaxScaleSubmit");
      y += ControlHeight + 6.0f;
    }

    CheckRow(pContent, plRectFloat(10, y, fWidth - 20, 20), "Align to surface normal", {}, "Veg.Align", s.m_bAlignToNormal);
    y += 26.0f;

    // the key hint wraps; it is drawn
    {
      float x = 0.0f;
      plUInt32 uiLines = 1;
      for (const HintToken& token : s_Hint)
      {
        const float fToken = HintTokenWidth(token);
        if (x + fToken > fWidth - 20.0f && x > 0.0f)
        {
          x = 0.0f;
          ++uiLines;
        }
        x += fToken;
      }

      m_fHintTop = y;
      m_fHintHeight = uiLines * s_fLine;

      plUiPanel* pHint = Panel(pContent);
      Place(pHint, plRectFloat(10, y, fWidth - 20, m_fHintHeight));
      pHint->m_Color = plColorGammaUB(0, 0, 0, 0);
      plEditorUi::DrawList(pHint, "Veg.Hint", s_sHintDraw.GetView());
      y += m_fHintHeight + 6.0f;
    }
  }

  // types: plants and grass
  {
    plUiPanel* pHeader = Section("Types", "Veg.TypeCount");
    const float fRefresh = ToolButtonWidth("Refresh", false);
    plUiButton* pRefresh = ToolButton(pHeader, plRectFloat(0, 3, fRefresh, 20), "Refresh", "Veg.Refresh");
    pRefresh->m_vAnchorMin = pRefresh->m_vAnchorMax = plVec2(1, 0);
    pRefresh->m_vOffsets = plVec4(-8 - fRefresh, 3, fRefresh, 20);
    SetToolTip(board, "Veg.Refresh", "Re-reads the Vegetation Type and Grass Type assets of the project.");

    y += 4.0f;
    const float fTypesTop = y;

    for (const bool bGrass : {false, true})
    {
      plUiText* pGroup = Text(pContent, bGrass ? "Grass" : "Plants");
      Place(pGroup, plRectFloat(10, y, fWidth - 20, 20));
      pGroup->m_uiFontSize = FontSize - 1;
      pGroup->m_Color = Foreground();
      y += 20.0f;

      for (plUInt32 i = 0; i < s.m_Types.GetCount(); ++i)
      {
        const auto& type = s.m_Types[i];
        if (type.m_bGrass != bGrass)
          continue;

        plStringBuilder detail;
        if (bGrass)
          detail = "(coverage)";
        else
          detail.SetFormat("({} instances)", m_pPanel->GetInstanceCount(type.m_sGuid));

        const plString sAction = Key("Veg.Type", i);
        CheckRow(pContent, plRectFloat(10, y, fWidth - 20, 20), type.m_sName, detail, sAction, type.m_bSelected);
        SetToolTip(board, sAction, "Checked plants are painted as instances, mixed evenly; checked Grass Types are painted as coverage. Erase and Thin act on the checked types only; none checked = all.");
        y += 20.0f;
      }
    }

    PL_IGNORE_UNUSED(fTypesTop);
    y += 6.0f;
  }

  // coverage tools
  {
    Rule(pContent, plRectFloat(0, y, fWidth, 1));
    const float fResnap = ToolButtonWidth("Resnap Coverage", true);
    const float fClear = ToolButtonWidth("Clear Coverage", true);
    ToolButton(pContent, plRectFloat(6, y + 4, fResnap, ControlHeight), "Resnap Coverage", "Veg.Resnap", IconPath(Icon::Reset), false, m_pPanel->CanResnap());
    ToolButton(pContent, plRectFloat(6 + fResnap + 4, y + 4, fClear, ControlHeight), "Clear Coverage", "Veg.Clear", IconPath(Icon::Clear), false, m_pPanel->CanClearCoverage());
    SetToolTip(board, "Veg.Resnap", "Re-raycasts every painted grass texel of the target layer onto the current collision, keeping density and length. One undo step.");
    SetToolTip(board, "Veg.Clear", "Removes all grass coverage of the target layer. One undo step.");
    y += s_fToolRow + 1;
    Rule(pContent, plRectFloat(0, y - 1, fWidth, 1));
  }

  // statistics
  {
    y += 6.0f;
    plStringBuilder stats = m_pPanel->GetStatistics();
    plHybridArray<plStringView, 8> lines;
    stats.Split(false, lines, "\n");
    for (const plStringView& sLine : lines)
    {
      plUiText* pLine = Text(pContent, sLine);
      Place(pLine, plRectFloat(10, y, fWidth - 20, 15));
      pLine->m_hFont = GetCodeFont();
      pLine->m_uiFontSize = MinFontSize + 1;
      pLine->m_Color = Muted();
      y += 15.0f;
    }
    y += 8.0f;
  }

  m_fContentHeight = y;
  Place(pContent, plRectFloat(0, 0, fWidth, m_fContentHeight));

  SetDocument(Finish(pRoot));

  ClampScroll();
  UpdateBindings();
  DrawHint();
}

void plVegetationPanelView::UpdateBindings()
{
  if (!IsReady())
    return;

  const auto& s = plVegetationBrush::s_Settings;
  auto& board = GetSurface()->GetBlackboard();

  plStringBuilder text;
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_Sliders); ++i)
  {
    const float fValue = SliderValue(i);
    board.SetEntryValue(Key("Veg.Slider", i), fValue);

    if (s_Sliders[i].m_bMeters)
      text.SetFormat("{} m", plArgF(fValue, 1));
    else
      text.SetFormat("{}", plArgF(fValue, 2));
    board.SetEntryValue(Key("Veg.SliderText", i), plString(text));
  }

  // the field being typed in keeps its text
  const char* szFieldBindings[] = {"Veg.MaxSlopeText", "Veg.MinScaleText", "Veg.MaxScaleText"};
  const float fieldValues[] = {s.m_MaxSlope.GetDegree(), s.m_fMinScale, s.m_fMaxScale};
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Field::Count); ++i)
  {
    if (m_iFocusedField == static_cast<plInt32>(i))
      continue;

    text.SetFormat("{}", plArgF(fieldValues[i], i == 0 ? 0 : 2));
    board.SetEntryValue(szFieldBindings[i], plString(text));
  }

  text.SetFormat("{}", s.m_Types.GetCount());
  board.SetEntryValue("Veg.TypeCount", plString(text));
}

void plVegetationPanelView::PollSliders()
{
  // a dragged slider writes into its entry; the brush follows with the snapped value
  auto& board = GetSurface()->GetBlackboard();
  bool bChanged = false;

  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_Sliders); ++i)
  {
    const plVariant value = board.GetEntryValue(plTempHashedString(Key("Veg.Slider", i)), SliderValue(i));
    const float fRaw = value.ConvertTo<float>();
    const float fSnapped = plMath::Clamp(plMath::Round(fRaw / s_Sliders[i].m_fStep) * s_Sliders[i].m_fStep, s_Sliders[i].m_fMin, s_Sliders[i].m_fMax);

    if (plMath::Abs(fSnapped - SliderValue(i)) > s_Sliders[i].m_fStep * 0.25f)
    {
      SliderValue(i) = fSnapped;
      bChanged = true;
    }
  }

  if (bChanged)
    plVegetationBrush::NotifySettingsChanged();
}

void plVegetationPanelView::PollFields()
{
  // a number field commits when focus leaves it
  const plStringView sFocused = GetSurface()->GetCanvas().GetFocusedElementName();

  plInt32 iFocused = -1;
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Field::Count); ++i)
  {
    if (sFocused == s_szFields[i])
      iFocused = static_cast<plInt32>(i);
  }

  if (m_iFocusedField >= 0 && m_iFocusedField != iFocused)
  {
    const Field field = static_cast<Field>(m_iFocusedField);
    m_iFocusedField = iFocused;
    CommitField(field);
    return;
  }

  m_iFocusedField = iFocused;
}

void plVegetationPanelView::CommitField(Field field)
{
  const char* szFieldBindings[] = {"Veg.MaxSlopeText", "Veg.MinScaleText", "Veg.MaxScaleText"};
  const plVariant value = GetSurface()->GetBlackboard().GetEntryValue(plTempHashedString(szFieldBindings[static_cast<plUInt32>(field)]));

  double fValue = 0.0;
  if (!value.IsA<plString>() || plConversionUtils::StringToFloat(value.Get<plString>(), fValue).Failed())
  {
    UpdateBindings();
    return;
  }

  auto& s = plVegetationBrush::s_Settings;
  switch (field)
  {
    case Field::MaxSlope:
      s.m_MaxSlope = plAngle::MakeFromDegree(plMath::Clamp(static_cast<float>(fValue), 0.0f, 90.0f));
      break;
    case Field::MinScale:
      s.m_fMinScale = plMath::Clamp(static_cast<float>(fValue), 0.01f, 50.0f);
      s.m_fMaxScale = plMath::Max(s.m_fMaxScale, s.m_fMinScale);
      break;
    case Field::MaxScale:
      s.m_fMaxScale = plMath::Max(plMath::Clamp(static_cast<float>(fValue), 0.01f, 50.0f), s.m_fMinScale);
      break;
    default:
      break;
  }

  plVegetationBrush::NotifySettingsChanged();
  UpdateBindings();
}

void plVegetationPanelView::DrawHint()
{
  if (!IsReady())
    return;

  const float fWidth = plMath::Max(GetSurface()->GetLogicalSize().x, 200.0f) - 20.0f;
  m_HintWriter.Clear();

  float x = 0.0f;
  float y = 0.0f;
  for (const HintToken& token : s_Hint)
  {
    const float fToken = HintTokenWidth(token);
    if (x + fToken > fWidth && x > 0.0f)
    {
      x = 0.0f;
      y += s_fLine;
    }

    if (token.m_bKey)
      plEditorUiDraw::KeyCap(m_HintWriter, plVec2(x, y + (s_fLine - plEditorUiDraw::KeyCapHeight) * 0.5f), token.m_szText);
    else
      m_HintWriter.Text(plRectFloat(x, y, fToken + 2.0f, s_fLine), token.m_szText, Dim(), FontSize - 2);

    x += fToken;
  }

  m_HintWriter.Publish(GetSurface()->GetBlackboard(), s_sHintDraw);
}

void plVegetationPanelView::ClampScroll()
{
  const float fMax = plMath::Max(m_fContentHeight - GetSurface()->GetLogicalSize().y, 0.0f);
  m_fScroll = plMath::Clamp(m_fScroll, 0.0f, fMax);
  GetSurface()->GetBlackboard().SetEntryValue(s_sScroll, plVec2(0.0f, -m_fScroll));
}

void plVegetationPanelView::OnPointer(const plEditorUiPointerEvent& e)
{
  if (e.m_Type != plEditorUiPointerEvent::Type::Wheel)
    return;

  m_fScroll -= e.m_fWheel * s_fWheelStep;
  ClampScroll();
}

void plVegetationPanelView::OpenLayerMenu()
{
  const plArrayPtr<const plString> layers = m_pPanel->GetLayerNames();

  plEditorUiMenu menu;
  menu.m_fMinWidth = plMath::Max(m_LayerRect.width, 120.0f);
  for (plUInt32 i = 0; i < layers.GetCount(); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(layers[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = static_cast<plInt32>(i) == m_pPanel->GetLayerIndex();
  }

  const plVec2 vPosition(m_LayerRect.x, m_LayerRect.y + m_LayerRect.height + 2.0f - m_fScroll);
  plEditorUiMenuView::Show(*GetSurface(), vPosition, menu, [this](plUInt32 uiPicked)
    { m_pPanel->SetLayerIndex(static_cast<plInt32>(uiPicked)); });
}

void plVegetationPanelView::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();
  auto& s = plVegetationBrush::s_Settings;

  if (sId == "Veg.Tool")
  {
    m_pPanel->ToggleTool();
  }
  else if (sId == "Veg.Layer")
  {
    OpenLayerMenu();
  }
  else if (sId == "Veg.NewLayer")
  {
    m_pPanel->AddLayer();
  }
  else if (sId.StartsWith("Veg.Mode."))
  {
    plInt32 iMode = 0;
    plConversionUtils::StringToInt(sId.GetSubString(9, 2), iMode).IgnoreResult();
    const auto mode = static_cast<plVegetationBrushMode::Enum>(plMath::Clamp(iMode, 0, 3));
    if (mode == plVegetationBrushMode::Place && !s.IsPlaceAvailable())
      return;

    s.m_Mode = mode;
    plVegetationBrush::NotifySettingsChanged();
  }
  else if (sId == "Veg.Align")
  {
    s.m_bAlignToNormal = !s.m_bAlignToNormal;
    plVegetationBrush::NotifySettingsChanged();
  }
  else if (sId.StartsWith("Veg.Type."))
  {
    plInt32 iType = 0;
    plConversionUtils::StringToInt(sId.GetSubString(9, 3), iType).IgnoreResult();
    if (iType >= 0 && iType < static_cast<plInt32>(s.m_Types.GetCount()))
    {
      s.m_Types[iType].m_bSelected = !s.m_Types[iType].m_bSelected;
      m_pPanel->OnTypesChanged();
    }
  }
  else if (sId == "Veg.Refresh")
  {
    m_pPanel->RefreshTypes();
  }
  else if (sId == "Veg.Resnap")
  {
    m_pPanel->ResnapCoverage();
  }
  else if (sId == "Veg.Clear")
  {
    m_pPanel->ClearCoverage();
  }
  else if (sId == "Veg.MaxSlopeSubmit")
  {
    CommitField(Field::MaxSlope);
  }
  else if (sId == "Veg.MinScaleSubmit")
  {
    CommitField(Field::MinScale);
  }
  else if (sId == "Veg.MaxScaleSubmit")
  {
    CommitField(Field::MaxScale);
  }
}
