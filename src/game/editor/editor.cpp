/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <algorithm>

#include <base/color.h>
#include <base/system.h>

#if defined(CONF_FAMILY_UNIX)
#include <pthread.h>
#endif

#include <engine/client.h>
#include <engine/console.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/filecollection.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/components/camera.h>
#include <game/client/components/menu_background.h>
#include <game/client/gameclient.h>
#include <game/client/lineinput.h>
#include <game/client/render.h>
#include <game/client/ui.h>
#include <game/client/ui_listbox.h>
#include <game/client/ui_scrollregion.h>
#include <game/generated/client_data.h>
#include <game/localization.h>

#include "auto_map.h"
#include "editor.h"

#include <limits>

using namespace FontIcons;

static const char *VANILLA_IMAGES[] = {
	"bg_cloud1",
	"bg_cloud2",
	"bg_cloud3",
	"desert_doodads",
	"desert_main",
	"desert_mountains",
	"desert_mountains2",
	"desert_sun",
	"generic_deathtiles",
	"generic_unhookable",
	"grass_doodads",
	"grass_main",
	"jungle_background",
	"jungle_deathtiles",
	"jungle_doodads",
	"jungle_main",
	"jungle_midground",
	"jungle_unhookables",
	"moon",
	"mountains",
	"snow",
	"stars",
	"sun",
	"winter_doodads",
	"winter_main",
	"winter_mountains",
	"winter_mountains2",
	"winter_mountains3"};

bool CEditor::IsVanillaImage(const char *pImage)
{
	return std::any_of(std::begin(VANILLA_IMAGES), std::end(VANILLA_IMAGES), [pImage](const char *pVanillaImage) { return str_comp(pImage, pVanillaImage) == 0; });
}

static const char *FILETYPE_EXTENSIONS[CEditor::NUM_FILETYPES] = {
	".map",
	".png",
	".opus"};

const void *CEditor::ms_pUiGotContext;

enum
{
	BUTTON_CONTEXT = 1,
};

CEditorImage::~CEditorImage()
{
	m_pEditor->Graphics()->UnloadTexture(&m_Texture);
	free(m_pData);
	m_pData = nullptr;
}

CEditorSound::~CEditorSound()
{
	m_pEditor->Sound()->UnloadSample(m_SoundID);
	free(m_pData);
	m_pData = nullptr;
}

CLayerGroup::CLayerGroup()
{
	m_vpLayers.clear();
	m_aName[0] = 0;
	m_Visible = true;
	m_Collapse = false;
	m_GameGroup = false;
	m_OffsetX = 0;
	m_OffsetY = 0;
	m_ParallaxX = 100;
	m_ParallaxY = 100;
	m_CustomParallaxZoom = 0;
	m_ParallaxZoom = 100;

	m_UseClipping = 0;
	m_ClipX = 0;
	m_ClipY = 0;
	m_ClipW = 0;
	m_ClipH = 0;
}

template<typename T>
static void DeleteAll(std::vector<T> &vList)
{
	for(const auto &pItem : vList)
		delete pItem;
	vList.clear();
}

CLayerGroup::~CLayerGroup()
{
	DeleteAll(m_vpLayers);
}

void CLayerGroup::Convert(CUIRect *pRect)
{
	pRect->x += m_OffsetX;
	pRect->y += m_OffsetY;
}

void CLayerGroup::Mapping(float *pPoints)
{
	float ParallaxZoom = m_pMap->m_pEditor->m_PreviewZoom ? m_ParallaxZoom : 100.0f;

	m_pMap->m_pEditor->RenderTools()->MapScreenToWorld(
		m_pMap->m_pEditor->m_WorldOffsetX, m_pMap->m_pEditor->m_WorldOffsetY,
		m_ParallaxX, m_ParallaxY, ParallaxZoom, m_OffsetX, m_OffsetY,
		m_pMap->m_pEditor->Graphics()->ScreenAspect(), m_pMap->m_pEditor->m_WorldZoom, pPoints);

	pPoints[0] += m_pMap->m_pEditor->m_EditorOffsetX;
	pPoints[1] += m_pMap->m_pEditor->m_EditorOffsetY;
	pPoints[2] += m_pMap->m_pEditor->m_EditorOffsetX;
	pPoints[3] += m_pMap->m_pEditor->m_EditorOffsetY;
}

void CLayerGroup::MapScreen()
{
	float aPoints[4];
	Mapping(aPoints);
	m_pMap->m_pEditor->Graphics()->MapScreen(aPoints[0], aPoints[1], aPoints[2], aPoints[3]);
}

void CLayerGroup::Render()
{
	MapScreen();
	IGraphics *pGraphics = m_pMap->m_pEditor->Graphics();

	if(m_UseClipping)
	{
		float aPoints[4];
		m_pMap->m_pGameGroup->Mapping(aPoints);
		float x0 = (m_ClipX - aPoints[0]) / (aPoints[2] - aPoints[0]);
		float y0 = (m_ClipY - aPoints[1]) / (aPoints[3] - aPoints[1]);
		float x1 = ((m_ClipX + m_ClipW) - aPoints[0]) / (aPoints[2] - aPoints[0]);
		float y1 = ((m_ClipY + m_ClipH) - aPoints[1]) / (aPoints[3] - aPoints[1]);

		pGraphics->ClipEnable((int)(x0 * pGraphics->ScreenWidth()), (int)(y0 * pGraphics->ScreenHeight()),
			(int)((x1 - x0) * pGraphics->ScreenWidth()), (int)((y1 - y0) * pGraphics->ScreenHeight()));
	}

	for(auto &pLayer : m_vpLayers)
	{
		if(pLayer->m_Visible)
		{
			if(pLayer->m_Type == LAYERTYPE_TILES)
			{
				CLayerTiles *pTiles = static_cast<CLayerTiles *>(pLayer);
				if(pTiles->m_Game || pTiles->m_Front || pTiles->m_Tele || pTiles->m_Speedup || pTiles->m_Tune || pTiles->m_Switch)
					continue;
			}
			if(m_pMap->m_pEditor->m_ShowDetail || !(pLayer->m_Flags & LAYERFLAG_DETAIL))
				pLayer->Render();
		}
	}

	for(auto &pLayer : m_vpLayers)
	{
		if(pLayer->m_Visible && pLayer->m_Type == LAYERTYPE_TILES && pLayer != m_pMap->m_pGameLayer && pLayer != m_pMap->m_pFrontLayer && pLayer != m_pMap->m_pTeleLayer && pLayer != m_pMap->m_pSpeedupLayer && pLayer != m_pMap->m_pSwitchLayer && pLayer != m_pMap->m_pTuneLayer)
		{
			CLayerTiles *pTiles = static_cast<CLayerTiles *>(pLayer);
			if(pTiles->m_Game || pTiles->m_Front || pTiles->m_Tele || pTiles->m_Speedup || pTiles->m_Tune || pTiles->m_Switch)
			{
				pLayer->Render();
			}
		}
	}

	if(m_UseClipping)
		pGraphics->ClipDisable();
}

void CLayerGroup::AddLayer(CLayer *pLayer)
{
	m_pMap->OnModify();
	m_vpLayers.push_back(pLayer);
}

void CLayerGroup::DeleteLayer(int Index)
{
	if(Index < 0 || Index >= (int)m_vpLayers.size())
		return;
	delete m_vpLayers[Index];
	m_vpLayers.erase(m_vpLayers.begin() + Index);
	m_pMap->OnModify();
}

void CLayerGroup::DuplicateLayer(int Index)
{
	if(Index < 0 || Index >= (int)m_vpLayers.size())
		return;

	auto *pDup = m_vpLayers[Index]->Duplicate();
	m_vpLayers.insert(m_vpLayers.begin() + Index + 1, pDup);

	m_pMap->OnModify();
}

void CLayerGroup::GetSize(float *pWidth, float *pHeight) const
{
	*pWidth = 0;
	*pHeight = 0;
	for(const auto &pLayer : m_vpLayers)
	{
		float lw, lh;
		pLayer->GetSize(&lw, &lh);
		*pWidth = maximum(*pWidth, lw);
		*pHeight = maximum(*pHeight, lh);
	}
}

int CLayerGroup::SwapLayers(int Index0, int Index1)
{
	if(Index0 < 0 || Index0 >= (int)m_vpLayers.size())
		return Index0;
	if(Index1 < 0 || Index1 >= (int)m_vpLayers.size())
		return Index0;
	if(Index0 == Index1)
		return Index0;
	m_pMap->OnModify();
	std::swap(m_vpLayers[Index0], m_vpLayers[Index1]);
	return Index1;
}

void CEditorImage::AnalyseTileFlags()
{
	mem_zero(m_aTileFlags, sizeof(m_aTileFlags));

	int tw = m_Width / 16; // tilesizes
	int th = m_Height / 16;
	if(tw == th && m_Format == CImageInfo::FORMAT_RGBA)
	{
		unsigned char *pPixelData = (unsigned char *)m_pData;

		int TileID = 0;
		for(int ty = 0; ty < 16; ty++)
			for(int tx = 0; tx < 16; tx++, TileID++)
			{
				bool Opaque = true;
				for(int x = 0; x < tw; x++)
					for(int y = 0; y < th; y++)
					{
						int p = (ty * tw + y) * m_Width + tx * tw + x;
						if(pPixelData[p * 4 + 3] < 250)
						{
							Opaque = false;
							break;
						}
					}

				if(Opaque)
					m_aTileFlags[TileID] |= TILEFLAG_OPAQUE;
			}
	}
}

void CEditor::EnvelopeEval(int TimeOffsetMillis, int Env, ColorRGBA &Channels, void *pUser)
{
	CEditor *pThis = (CEditor *)pUser;
	if(Env < 0 || Env >= (int)pThis->m_Map.m_vpEnvelopes.size())
	{
		Channels = ColorRGBA();
		return;
	}

	CEnvelope *pEnv = pThis->m_Map.m_vpEnvelopes[Env];
	float t = pThis->m_AnimateTime;
	t *= pThis->m_AnimateSpeed;
	t += (TimeOffsetMillis / 1000.0f);
	pEnv->Eval(t, Channels);
}

/********************************************************
 OTHER
*********************************************************/

bool CEditor::DoEditBox(CLineInput *pLineInput, const CUIRect *pRect, float FontSize, int Corners, const char *pToolTip)
{
	if(UI()->LastActiveItem() == pLineInput)
		m_EditBoxActive = 2;
	UpdateTooltip(pLineInput, pRect, pToolTip);
	return UI()->DoEditBox(pLineInput, pRect, FontSize, Corners);
}

bool CEditor::DoClearableEditBox(CLineInput *pLineInput, const CUIRect *pRect, float FontSize, int Corners, const char *pToolTip)
{
	if(UI()->LastActiveItem() == pLineInput)
		m_EditBoxActive = 2;
	UpdateTooltip(pLineInput, pRect, pToolTip);
	return UI()->DoClearableEditBox(pLineInput, pRect, FontSize, Corners);
}

ColorRGBA CEditor::GetButtonColor(const void *pID, int Checked)
{
	if(Checked < 0)
		return ColorRGBA(0, 0, 0, 0.5f);

	switch(Checked)
	{
	case 8: // invisible
		return ColorRGBA(0, 0, 0, 0);
	case 7: // selected + game layers
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 0, 0, 0.4f);
		return ColorRGBA(1, 0, 0, 0.2f);

	case 6: // game layers
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 1, 1, 0.4f);
		return ColorRGBA(1, 1, 1, 0.2f);

	case 5: // selected + image/sound should be embedded
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 0, 0, 0.75f);
		return ColorRGBA(1, 0, 0, 0.5f);

	case 4: // image/sound should be embedded
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 0, 0, 1.0f);
		return ColorRGBA(1, 0, 0, 0.875f);

	case 3: // selected + unused image/sound
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 0, 1, 0.75f);
		return ColorRGBA(1, 0, 1, 0.5f);

	case 2: // unused image/sound
		if(UI()->HotItem() == pID)
			return ColorRGBA(0, 0, 1, 0.75f);
		return ColorRGBA(0, 0, 1, 0.5f);

	case 1: // selected
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 0, 0, 0.75f);
		return ColorRGBA(1, 0, 0, 0.5f);

	default: // regular
		if(UI()->HotItem() == pID)
			return ColorRGBA(1, 1, 1, 0.75f);
		return ColorRGBA(1, 1, 1, 0.5f);
	}
}

void CEditor::UpdateTooltip(const void *pID, const CUIRect *pRect, const char *pToolTip)
{
	if((UI()->MouseInside(pRect) && m_pTooltip) || (UI()->HotItem() == pID && pToolTip))
		m_pTooltip = pToolTip;
}

int CEditor::DoButton_Editor_Common(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	if(UI()->MouseInside(pRect))
	{
		if(Flags & BUTTON_CONTEXT)
			ms_pUiGotContext = pID;
	}

	UpdateTooltip(pID, pRect, pToolTip);
	return UI()->DoButtonLogic(pID, Checked, pRect);
}

int CEditor::DoButton_Editor(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	pRect->Draw(GetButtonColor(pID, Checked), IGraphics::CORNER_ALL, 3.0f);
	CUIRect NewRect = *pRect;
	UI()->DoLabel(&NewRect, pText, 10.0f, TEXTALIGN_MC);
	Checked %= 2;
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_Env(const void *pID, const char *pText, int Checked, const CUIRect *pRect, const char *pToolTip, ColorRGBA BaseColor, int Corners)
{
	float Bright = Checked ? 1.0f : 0.5f;
	float Alpha = UI()->HotItem() == pID ? 1.0f : 0.75f;
	ColorRGBA Color = ColorRGBA(BaseColor.r * Bright, BaseColor.g * Bright, BaseColor.b * Bright, Alpha);

	pRect->Draw(Color, Corners, 3.0f);
	UI()->DoLabel(pRect, pText, 10.0f, TEXTALIGN_MC);
	Checked %= 2;
	return DoButton_Editor_Common(pID, pText, Checked, pRect, 0, pToolTip);
}

int CEditor::DoButton_File(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	if(Checked)
		pRect->Draw(GetButtonColor(pID, Checked), IGraphics::CORNER_ALL, 3.0f);

	CUIRect Rect;
	pRect->VMargin(5.0f, &Rect);
	UI()->DoLabel(&Rect, pText, 10.0f, TEXTALIGN_ML);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_Menu(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	pRect->Draw(ColorRGBA(0.5f, 0.5f, 0.5f, 1.0f), IGraphics::CORNER_T, 3.0f);

	CUIRect Rect;
	pRect->VMargin(5.0f, &Rect);
	UI()->DoLabel(&Rect, pText, 10.0f, TEXTALIGN_ML);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_MenuItem(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	if(UI()->HotItem() == pID || Checked)
		pRect->Draw(GetButtonColor(pID, Checked), IGraphics::CORNER_ALL, 3.0f);

	CUIRect Rect;
	pRect->VMargin(5.0f, &Rect);
	UI()->DoLabel(&Rect, pText, 10.0f, TEXTALIGN_ML);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_Tab(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	pRect->Draw(GetButtonColor(pID, Checked), IGraphics::CORNER_T, 5.0f);
	UI()->DoLabel(pRect, pText, 10.0f, TEXTALIGN_MC);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_Ex(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip, int Corners, float FontSize)
{
	pRect->Draw(GetButtonColor(pID, Checked), Corners, 3.0f);
	UI()->DoLabel(pRect, pText, FontSize, TEXTALIGN_MC);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_FontIcon(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip, int Corners, float FontSize)
{
	pRect->Draw(GetButtonColor(pID, Checked), Corners, 3.0f);

	TextRender()->SetCurFont(TextRender()->GetFont(TEXT_FONT_ICON_FONT));
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING);
	UI()->DoLabel(pRect, pText, FontSize, TEXTALIGN_MC);
	TextRender()->SetRenderFlags(0);
	TextRender()->SetCurFont(nullptr);

	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_ButtonInc(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	pRect->Draw(GetButtonColor(pID, Checked), IGraphics::CORNER_R, 3.0f);
	UI()->DoLabel(pRect, pText ? pText : "+", 10.0f, TEXTALIGN_MC);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_ButtonDec(const void *pID, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	pRect->Draw(GetButtonColor(pID, Checked), IGraphics::CORNER_L, 3.0f);
	UI()->DoLabel(pRect, pText ? pText : "-", 10.0f, TEXTALIGN_MC);
	return DoButton_Editor_Common(pID, pText, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_DraggableEx(const void *pID, const char *pText, int Checked, const CUIRect *pRect, bool *pClicked, bool *pAbrupted, int Flags, const char *pToolTip, int Corners, float FontSize)
{
	pRect->Draw(GetButtonColor(pID, Checked), Corners, 3.0f);

	UI()->DoLabel(pRect, pText, FontSize, TEXTALIGN_MC);

	if(UI()->MouseInside(pRect))
	{
		if(Flags & BUTTON_CONTEXT)
			ms_pUiGotContext = pID;
	}

	UpdateTooltip(pID, pRect, pToolTip);
	return UI()->DoDraggableButtonLogic(pID, Checked, pRect, pClicked, pAbrupted);
}

void CEditor::RenderGrid(CLayerGroup *pGroup)
{
	if(!m_GridActive)
		return;

	float aGroupPoints[4];
	pGroup->Mapping(aGroupPoints);

	float w = UI()->Screen()->w;
	float h = UI()->Screen()->h;

	int LineDistance = GetLineDistance();

	int XOffset = aGroupPoints[0] / LineDistance;
	int YOffset = aGroupPoints[1] / LineDistance;
	int XGridOffset = XOffset % m_GridFactor;
	int YGridOffset = YOffset % m_GridFactor;

	Graphics()->TextureClear();
	Graphics()->LinesBegin();

	for(int i = 0; i < (int)w; i++)
	{
		if((i + YGridOffset) % m_GridFactor == 0)
			Graphics()->SetColor(1.0f, 0.3f, 0.3f, 0.3f);
		else
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.15f);

		IGraphics::CLineItem Line = IGraphics::CLineItem(LineDistance * XOffset, LineDistance * i + LineDistance * YOffset, w + aGroupPoints[2], LineDistance * i + LineDistance * YOffset);
		Graphics()->LinesDraw(&Line, 1);

		if((i + XGridOffset) % m_GridFactor == 0)
			Graphics()->SetColor(1.0f, 0.3f, 0.3f, 0.3f);
		else
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.15f);

		Line = IGraphics::CLineItem(LineDistance * i + LineDistance * XOffset, LineDistance * YOffset, LineDistance * i + LineDistance * XOffset, h + aGroupPoints[3]);
		Graphics()->LinesDraw(&Line, 1);
	}
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	Graphics()->LinesEnd();
}

void CEditor::SnapToGrid(float &x, float &y)
{
	const int GridDistance = GetLineDistance() * m_GridFactor;
	x = (int)((x + (x >= 0 ? 1.0f : -1.0f) * GridDistance / 2) / GridDistance) * GridDistance;
	y = (int)((y + (y >= 0 ? 1.0f : -1.0f) * GridDistance / 2) / GridDistance) * GridDistance;
}

void CEditor::RenderBackground(CUIRect View, IGraphics::CTextureHandle Texture, float Size, float Brightness)
{
	Graphics()->TextureSet(Texture);
	Graphics()->BlendNormal();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(Brightness, Brightness, Brightness, 1.0f);
	Graphics()->QuadsSetSubset(0, 0, View.w / Size, View.h / Size);
	IGraphics::CQuadItem QuadItem(View.x, View.y, View.w, View.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
}

int CEditor::UiDoValueSelector(void *pID, CUIRect *pRect, const char *pLabel, int Current, int Min, int Max, int Step, float Scale, const char *pToolTip, bool IsDegree, bool IsHex, int Corners, ColorRGBA *pColor, bool ShowValue)
{
	// logic
	static float s_Value;
	static CLineInputNumber s_NumberInput;
	static bool s_TextMode = false;
	static void *s_pLastTextID = pID;
	const bool Inside = UI()->MouseInside(pRect);
	const int Base = IsHex ? 16 : 10;

	if(UI()->MouseButton(1) && UI()->HotItem() == pID)
	{
		s_pLastTextID = pID;
		s_TextMode = true;
		UI()->DisableMouseLock();
		s_NumberInput.SetInteger(Current, Base);
	}

	if(UI()->CheckActiveItem(pID))
	{
		if(!UI()->MouseButton(0))
		{
			UI()->DisableMouseLock();
			UI()->SetActiveItem(nullptr);
			s_TextMode = false;
		}
	}

	if(s_TextMode && s_pLastTextID == pID)
	{
		m_pTooltip = "Type your number";

		DoEditBox(&s_NumberInput, pRect, 10.0f);

		UI()->SetActiveItem(&s_NumberInput);

		if(Input()->KeyIsPressed(KEY_RETURN) || Input()->KeyIsPressed(KEY_KP_ENTER) ||
			((UI()->MouseButton(1) || UI()->MouseButton(0)) && !Inside))
		{
			Current = clamp(s_NumberInput.GetInteger(Base), Min, Max);
			UI()->DisableMouseLock();
			UI()->SetActiveItem(nullptr);
			s_TextMode = false;
		}

		if(Input()->KeyIsPressed(KEY_ESCAPE))
		{
			UI()->DisableMouseLock();
			UI()->SetActiveItem(nullptr);
			s_TextMode = false;
		}
	}
	else
	{
		if(UI()->CheckActiveItem(pID))
		{
			if(UI()->MouseButton(0))
			{
				if(Input()->ShiftIsPressed())
					s_Value += m_MouseDeltaX * 0.05f;
				else
					s_Value += m_MouseDeltaX;

				if(absolute(s_Value) >= Scale)
				{
					int Count = (int)(s_Value / Scale);
					s_Value = std::fmod(s_Value, Scale);
					Current += Step * Count;
					Current = clamp(Current, Min, Max);

					// Constrain to discrete steps
					if(Count > 0)
						Current = Current / Step * Step;
					else
						Current = std::ceil(Current / (float)Step) * Step;
				}
			}
			if(pToolTip && !s_TextMode)
				m_pTooltip = pToolTip;
		}
		else if(UI()->HotItem() == pID)
		{
			if(UI()->MouseButton(0))
			{
				UI()->SetActiveItem(pID);
				UI()->EnableMouseLock(pID);
				s_Value = 0;
			}
			if(pToolTip && !s_TextMode)
				m_pTooltip = pToolTip;
		}

		if(Inside)
			UI()->SetHotItem(pID);

		// render
		char aBuf[128];
		if(pLabel[0] != '\0')
		{
			if(ShowValue)
				str_format(aBuf, sizeof(aBuf), "%s %d", pLabel, Current);
			else
				str_copy(aBuf, pLabel);
		}
		else if(IsDegree)
			str_format(aBuf, sizeof(aBuf), "%d°", Current);
		else if(IsHex)
			str_format(aBuf, sizeof(aBuf), "#%06X", Current);
		else
			str_format(aBuf, sizeof(aBuf), "%d", Current);
		pRect->Draw(pColor ? *pColor : GetButtonColor(pID, 0), Corners, 5.0f);
		UI()->DoLabel(pRect, aBuf, 10, TEXTALIGN_MC);
	}

	if(!s_TextMode)
		s_NumberInput.Clear();

	return Current;
}

CLayerGroup *CEditor::GetSelectedGroup() const
{
	if(m_SelectedGroup >= 0 && m_SelectedGroup < (int)m_Map.m_vpGroups.size())
		return m_Map.m_vpGroups[m_SelectedGroup];
	return nullptr;
}

CLayer *CEditor::GetSelectedLayer(int Index) const
{
	CLayerGroup *pGroup = GetSelectedGroup();
	if(!pGroup)
		return nullptr;

	if(Index < 0 || Index >= (int)m_vSelectedLayers.size())
		return nullptr;

	int LayerIndex = m_vSelectedLayers[Index];

	if(LayerIndex >= 0 && LayerIndex < (int)m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers.size())
		return pGroup->m_vpLayers[LayerIndex];
	return nullptr;
}

CLayer *CEditor::GetSelectedLayerType(int Index, int Type) const
{
	CLayer *p = GetSelectedLayer(Index);
	if(p && p->m_Type == Type)
		return p;
	return nullptr;
}

std::vector<CQuad *> CEditor::GetSelectedQuads()
{
	CLayerQuads *pQuadLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
	std::vector<CQuad *> vpQuads;
	if(!pQuadLayer)
		return vpQuads;
	vpQuads.resize(m_vSelectedQuads.size());
	for(int i = 0; i < (int)m_vSelectedQuads.size(); ++i)
		vpQuads[i] = &pQuadLayer->m_vQuads[m_vSelectedQuads[i]];
	return vpQuads;
}

CSoundSource *CEditor::GetSelectedSource()
{
	CLayerSounds *pSounds = (CLayerSounds *)GetSelectedLayerType(0, LAYERTYPE_SOUNDS);
	if(!pSounds)
		return nullptr;
	if(m_SelectedSource >= 0 && m_SelectedSource < (int)pSounds->m_vSources.size())
		return &pSounds->m_vSources[m_SelectedSource];
	return nullptr;
}

void CEditor::SelectLayer(int LayerIndex, int GroupIndex)
{
	if(GroupIndex != -1)
		m_SelectedGroup = GroupIndex;

	m_vSelectedLayers.clear();
	m_vSelectedQuads.clear();
	AddSelectedLayer(LayerIndex);
}

void CEditor::AddSelectedLayer(int LayerIndex)
{
	m_vSelectedLayers.push_back(LayerIndex);

	m_QuadKnifeActive = false;
}

void CEditor::SelectQuad(int Index)
{
	m_vSelectedQuads.clear();
	m_vSelectedQuads.push_back(Index);
}

void CEditor::DeleteSelectedQuads()
{
	CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
	if(!pLayer)
		return;

	for(int i = 0; i < (int)m_vSelectedQuads.size(); ++i)
	{
		pLayer->m_vQuads.erase(pLayer->m_vQuads.begin() + m_vSelectedQuads[i]);
		for(int j = i + 1; j < (int)m_vSelectedQuads.size(); ++j)
			if(m_vSelectedQuads[j] > m_vSelectedQuads[i])
				m_vSelectedQuads[j]--;

		m_vSelectedQuads.erase(m_vSelectedQuads.begin() + i);
		i--;
	}
}

bool CEditor::IsQuadSelected(int Index) const
{
	return FindSelectedQuadIndex(Index) >= 0;
}

int CEditor::FindSelectedQuadIndex(int Index) const
{
	for(size_t i = 0; i < m_vSelectedQuads.size(); ++i)
		if(m_vSelectedQuads[i] == Index)
			return i;
	return -1;
}

bool CEditor::CallbackOpenMap(const char *pFileName, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;
	if(pEditor->Load(pFileName, StorageType))
	{
		pEditor->m_ValidSaveFilename = StorageType == IStorage::TYPE_SAVE && pEditor->m_pFileDialogPath == pEditor->m_aFileDialogCurrentFolder;
		pEditor->m_Dialog = DIALOG_NONE;
		return true;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to load map from file '%s'.", pFileName);
		return false;
	}
}

bool CEditor::CallbackAppendMap(const char *pFileName, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;
	if(pEditor->Append(pFileName, StorageType))
	{
		pEditor->m_Dialog = DIALOG_NONE;
		return true;
	}
	else
	{
		pEditor->m_aFileName[0] = 0;
		pEditor->ShowFileDialogError("Failed to load map from file '%s'.", pFileName);
		return false;
	}
}

bool CEditor::CallbackSaveMap(const char *pFileName, int StorageType, void *pUser)
{
	CEditor *pEditor = static_cast<CEditor *>(pUser);
	char aBuf[IO_MAX_PATH_LENGTH];
	// add map extension
	if(!str_endswith(pFileName, ".map"))
	{
		str_format(aBuf, sizeof(aBuf), "%s.map", pFileName);
		pFileName = aBuf;
	}

	// Save map to specified file
	if(pEditor->Save(pFileName))
	{
		str_copy(pEditor->m_aFileName, pFileName);
		pEditor->m_ValidSaveFilename = StorageType == IStorage::TYPE_SAVE && pEditor->m_pFileDialogPath == pEditor->m_aFileDialogCurrentFolder;
		pEditor->m_Map.m_Modified = false;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to save map to file '%s'.", pFileName);
		return false;
	}

	// Also update autosave if it's older than half the configured autosave interval, so we also have periodic backups.
	const float Time = pEditor->Client()->GlobalTime();
	if(g_Config.m_EdAutosaveInterval > 0 && pEditor->m_Map.m_LastSaveTime < Time && Time - pEditor->m_Map.m_LastSaveTime > 30 * g_Config.m_EdAutosaveInterval)
	{
		if(!pEditor->PerformAutosave())
			return false;
	}

	pEditor->m_Dialog = DIALOG_NONE;
	return true;
}

bool CEditor::CallbackSaveCopyMap(const char *pFileName, int StorageType, void *pUser)
{
	CEditor *pEditor = static_cast<CEditor *>(pUser);
	char aBuf[IO_MAX_PATH_LENGTH];
	// add map extension
	if(!str_endswith(pFileName, ".map"))
	{
		str_format(aBuf, sizeof(aBuf), "%s.map", pFileName);
		pFileName = aBuf;
	}

	if(pEditor->Save(pFileName))
	{
		pEditor->m_Dialog = DIALOG_NONE;
		return true;
	}
	else
	{
		pEditor->ShowFileDialogError("Failed to save map to file '%s'.", pFileName);
		return false;
	}
}

void CEditor::DoToolbarLayers(CUIRect ToolBar)
{
	const bool ModPressed = Input()->ModifierIsPressed();
	const bool ShiftPressed = Input()->ShiftIsPressed();

	// handle shortcut for info button
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_I) && ModPressed && !ShiftPressed)
	{
		if(m_ShowTileInfo == SHOW_TILE_HEXADECIMAL)
			m_ShowTileInfo = SHOW_TILE_DECIMAL;
		else if(m_ShowTileInfo != SHOW_TILE_OFF)
			m_ShowTileInfo = SHOW_TILE_OFF;
		else
			m_ShowTileInfo = SHOW_TILE_DECIMAL;
		m_ShowEnvelopePreview = SHOWENV_NONE;
	}

	// handle shortcut for hex button
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_I) && ModPressed && ShiftPressed)
	{
		m_ShowTileInfo = m_ShowTileInfo == SHOW_TILE_HEXADECIMAL ? SHOW_TILE_OFF : SHOW_TILE_HEXADECIMAL;
		m_ShowEnvelopePreview = SHOWENV_NONE;
	}

	// handle shortcut for unused button
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_U) && ModPressed)
		m_AllowPlaceUnusedTiles = !m_AllowPlaceUnusedTiles;

	CUIRect TB_Top, TB_Bottom;
	CUIRect Button;

	ToolBar.HSplitMid(&TB_Top, &TB_Bottom, 5.0f);

	// top line buttons
	{
		// detail button
		TB_Top.VSplitLeft(40.0f, &Button, &TB_Top);
		static int s_HqButton = 0;
		if(DoButton_Editor(&s_HqButton, "HD", m_ShowDetail, &Button, 0, "[ctrl+h] Toggle High Detail") ||
			(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_H) && ModPressed))
		{
			m_ShowDetail = !m_ShowDetail;
		}

		TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

		// animation button
		TB_Top.VSplitLeft(40.0f, &Button, &TB_Top);
		static int s_AnimateButton = 0;
		if(DoButton_Editor(&s_AnimateButton, "Anim", m_Animate, &Button, 0, "[ctrl+m] Toggle animation") ||
			(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_M) && ModPressed))
		{
			m_AnimateStart = time_get();
			m_Animate = !m_Animate;
		}

		TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

		// proof button
		TB_Top.VSplitLeft(40.0f, &Button, &TB_Top);
		static int s_ProofButton = 0;
		if(DoButton_Ex(&s_ProofButton, "Proof", m_ProofBorders != PROOF_BORDER_OFF, &Button, 0, "[ctrl+p] Toggles proof borders. These borders represent what a player maximum can see.", IGraphics::CORNER_L) ||
			(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_P) && ModPressed))
		{
			m_ProofBorders = m_ProofBorders == PROOF_BORDER_OFF ? PROOF_BORDER_INGAME : PROOF_BORDER_OFF;
		}

		TB_Top.VSplitLeft(14.0f, &Button, &TB_Top);
		static int s_ProofModeButton = 0;
		if(DoButton_FontIcon(&s_ProofModeButton, FONT_ICON_CIRCLE_CHEVRON_DOWN, 0, &Button, 0, "Select proof mode.", IGraphics::CORNER_R, 8.0f))
		{
			static SPopupMenuId s_PopupProofModeId;
			UI()->DoPopupMenu(&s_PopupProofModeId, Button.x, Button.y + Button.h, 60.0f, 36.0f, this, PopupProofMode);
		}

		TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

		// zoom button
		TB_Top.VSplitLeft(40.0f, &Button, &TB_Top);
		static int s_ZoomButton = 0;
		if(DoButton_Editor(&s_ZoomButton, "Zoom", m_PreviewZoom, &Button, 0, "Toggles preview of how layers will be zoomed in-game"))
		{
			m_PreviewZoom = !m_PreviewZoom;
		}

		TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

		// grid button
		TB_Top.VSplitLeft(40.0f, &Button, &TB_Top);
		static int s_GridButton = 0;
		if(DoButton_Editor(&s_GridButton, "Grid", m_GridActive, &Button, 0, "[ctrl+g] Toggle Grid") ||
			(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_G) && ModPressed && !ShiftPressed))
		{
			m_GridActive = !m_GridActive;
		}

		TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

		// zoom group
		TB_Top.VSplitLeft(20.0f, &Button, &TB_Top);
		static int s_ZoomOutButton = 0;
		if(DoButton_FontIcon(&s_ZoomOutButton, "-", 0, &Button, 0, "[NumPad-] Zoom out", IGraphics::CORNER_L))
		{
			ChangeZoom(50.0f);
		}

		TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
		static int s_ZoomNormalButton = 0;
		if(DoButton_FontIcon(&s_ZoomNormalButton, FONT_ICON_MAGNIFYING_GLASS, 0, &Button, 0, "[NumPad*] Zoom to normal and remove editor offset", IGraphics::CORNER_NONE))
		{
			m_EditorOffsetX = 0;
			m_EditorOffsetY = 0;
			SetZoom(100.0f);
		}

		TB_Top.VSplitLeft(20.0f, &Button, &TB_Top);
		static int s_ZoomInButton = 0;
		if(DoButton_FontIcon(&s_ZoomInButton, "+", 0, &Button, 0, "[NumPad+] Zoom in", IGraphics::CORNER_R))
		{
			ChangeZoom(-50.0f);
		}

		TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

		// brush manipulation
		{
			int Enabled = m_Brush.IsEmpty() ? -1 : 0;

			// flip buttons
			TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
			static int s_FlipXButton = 0;
			if(DoButton_FontIcon(&s_FlipXButton, FONT_ICON_ARROWS_LEFT_RIGHT, Enabled, &Button, 0, "[N] Flip brush horizontal", IGraphics::CORNER_L) || (Input()->KeyPress(KEY_N) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0))
			{
				for(auto &pLayer : m_Brush.m_vpLayers)
					pLayer->BrushFlipX();
			}

			TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
			static int s_FlipyButton = 0;
			if(DoButton_FontIcon(&s_FlipyButton, FONT_ICON_ARROWS_UP_DOWN, Enabled, &Button, 0, "[M] Flip brush vertical", IGraphics::CORNER_R) || (Input()->KeyPress(KEY_M) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0))
			{
				for(auto &pLayer : m_Brush.m_vpLayers)
					pLayer->BrushFlipY();
			}
			TB_Top.VSplitLeft(5.0f, nullptr, &TB_Top);

			// rotate buttons
			TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
			static int s_RotationAmount = 90;
			bool TileLayer = false;
			// check for tile layers in brush selection
			for(auto &pLayer : m_Brush.m_vpLayers)
				if(pLayer->m_Type == LAYERTYPE_TILES)
				{
					TileLayer = true;
					s_RotationAmount = maximum(90, (s_RotationAmount / 90) * 90);
					break;
				}

			static int s_CcwButton = 0;
			if(DoButton_FontIcon(&s_CcwButton, FONT_ICON_ARROW_ROTATE_LEFT, Enabled, &Button, 0, "[R] Rotates the brush counter clockwise", IGraphics::CORNER_ALL) || (Input()->KeyPress(KEY_R) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0))
			{
				for(auto &pLayer : m_Brush.m_vpLayers)
					pLayer->BrushRotate(-s_RotationAmount / 360.0f * pi * 2);
			}

			TB_Top.VSplitLeft(30.0f, &Button, &TB_Top);
			s_RotationAmount = UiDoValueSelector(&s_RotationAmount, &Button, "", s_RotationAmount, TileLayer ? 90 : 1, 359, TileLayer ? 90 : 1, TileLayer ? 10.0f : 2.0f, "Rotation of the brush in degrees. Use left mouse button to drag and change the value. Hold shift to be more precise.", true);

			TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
			static int s_CwButton = 0;
			if(DoButton_FontIcon(&s_CwButton, FONT_ICON_ARROW_ROTATE_RIGHT, Enabled, &Button, 0, "[T] Rotates the brush clockwise", IGraphics::CORNER_ALL) || (Input()->KeyPress(KEY_T) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0))
			{
				for(auto &pLayer : m_Brush.m_vpLayers)
					pLayer->BrushRotate(s_RotationAmount / 360.0f * pi * 2);
			}

			TB_Top.VSplitLeft(5.0f, &Button, &TB_Top);
		}

		// animation speed
		if(m_Animate)
		{
			TB_Top.VSplitLeft(20.0f, &Button, &TB_Top);
			static int s_AnimSlowerButton = 0;
			if(DoButton_FontIcon(&s_AnimSlowerButton, "-", 0, &Button, 0, "Decrease animation speed", IGraphics::CORNER_L))
			{
				if(m_AnimateSpeed > 0.5f)
					m_AnimateSpeed -= 0.5f;
			}

			TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
			static int s_AnimNormalButton = 0;
			if(DoButton_FontIcon(&s_AnimNormalButton, FONT_ICON_CIRCLE_PLAY, 0, &Button, 0, "Normal animation speed", 0))
				m_AnimateSpeed = 1.0f;

			TB_Top.VSplitLeft(20.0f, &Button, &TB_Top);
			static int s_AnimFasterButton = 0;
			if(DoButton_FontIcon(&s_AnimFasterButton, "+", 0, &Button, 0, "Increase animation speed", IGraphics::CORNER_R))
				m_AnimateSpeed += 0.5f;

			TB_Top.VSplitLeft(5.0f, &Button, &TB_Top);
		}

		// grid zoom
		if(m_GridActive)
		{
			TB_Top.VSplitLeft(20.0f, &Button, &TB_Top);
			static int s_GridIncreaseButton = 0;
			if(DoButton_FontIcon(&s_GridIncreaseButton, "-", 0, &Button, 0, "Decrease grid", IGraphics::CORNER_L))
			{
				if(m_GridFactor > 1)
					m_GridFactor--;
			}

			TB_Top.VSplitLeft(25.0f, &Button, &TB_Top);
			static int s_GridNormalButton = 0;
			if(DoButton_FontIcon(&s_GridNormalButton, FONT_ICON_BORDER_ALL, 0, &Button, 0, "Normal grid", IGraphics::CORNER_NONE))
				m_GridFactor = 1;

			TB_Top.VSplitLeft(20.0f, &Button, &TB_Top);
			static int s_GridDecreaseButton = 0;
			if(DoButton_FontIcon(&s_GridDecreaseButton, "+", 0, &Button, 0, "Increase grid", IGraphics::CORNER_R))
			{
				if(m_GridFactor < 15)
					m_GridFactor++;
			}
			TB_Top.VSplitLeft(5.0f, &Button, &TB_Top);
		}
	}

	// Bottom line buttons
	{
		// refocus button
		{
			TB_Bottom.VSplitLeft(45.0f, &Button, &TB_Bottom);
			static int s_RefocusButton = 0;
			int FocusButtonChecked;
			if(m_ProofBorders == PROOF_BORDER_MENU)
			{
				if(distance(m_vMenuBackgroundPositions[m_CurrentMenuProofIndex], vec2(m_WorldOffsetX, m_WorldOffsetY)) < 0.0001f)
					FocusButtonChecked = -1;
				else
					FocusButtonChecked = 1;
			}
			else
			{
				if(m_WorldOffsetX == 0 && m_WorldOffsetY == 0)
					FocusButtonChecked = -1;
				else
					FocusButtonChecked = 1;
			}
			if(DoButton_Editor(&s_RefocusButton, "Refocus", FocusButtonChecked, &Button, 0, "[HOME] Restore map focus") || (m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_HOME)))
			{
				if(m_ProofBorders == PROOF_BORDER_MENU)
				{
					m_WorldOffsetX = m_vMenuBackgroundPositions[m_CurrentMenuProofIndex].x;
					m_WorldOffsetY = m_vMenuBackgroundPositions[m_CurrentMenuProofIndex].y;
				}
				else
				{
					m_WorldOffsetX = 0;
					m_WorldOffsetY = 0;
				}
			}
			TB_Bottom.VSplitLeft(5.0f, nullptr, &TB_Bottom);
		}

		// tile manipulation
		{
			// do tele/tune/switch/speedup button
			{
				CLayerTiles *pS = (CLayerTiles *)GetSelectedLayerType(0, LAYERTYPE_TILES);
				if(pS)
				{
					const char *pButtonName = nullptr;
					CUI::FPopupMenuFunction pfnPopupFunc = nullptr;
					int Rows = 0;
					if(pS == m_Map.m_pSwitchLayer)
					{
						pButtonName = "Switch";
						pfnPopupFunc = PopupSwitch;
						Rows = 2;
					}
					else if(pS == m_Map.m_pSpeedupLayer)
					{
						pButtonName = "Speedup";
						pfnPopupFunc = PopupSpeedup;
						Rows = 3;
					}
					else if(pS == m_Map.m_pTuneLayer)
					{
						pButtonName = "Tune";
						pfnPopupFunc = PopupTune;
						Rows = 1;
					}
					else if(pS == m_Map.m_pTeleLayer)
					{
						pButtonName = "Tele";
						pfnPopupFunc = PopupTele;
						Rows = 1;
					}

					if(pButtonName != nullptr)
					{
						static char s_aButtonTooltip[64];
						str_format(s_aButtonTooltip, sizeof(s_aButtonTooltip), "[ctrl+t] %s", pButtonName);

						TB_Bottom.VSplitLeft(60.0f, &Button, &TB_Bottom);
						static int s_ModifierButton = 0;
						if(DoButton_Ex(&s_ModifierButton, pButtonName, 0, &Button, 0, s_aButtonTooltip, IGraphics::CORNER_ALL) || (m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && ModPressed && Input()->KeyPress(KEY_T)))
						{
							static SPopupMenuId s_PopupModifierId;
							if(!UI()->IsPopupOpen(&s_PopupModifierId))
							{
								UI()->DoPopupMenu(&s_PopupModifierId, Button.x, Button.y + Button.h, 120, 10.0f + Rows * 13.0f, this, pfnPopupFunc);
							}
						}
						TB_Bottom.VSplitLeft(5.0f, nullptr, &TB_Bottom);
					}
				}
			}
		}

		// do add quad/sound button
		CLayer *pLayer = GetSelectedLayer(0);
		if(pLayer && (pLayer->m_Type == LAYERTYPE_QUADS || pLayer->m_Type == LAYERTYPE_SOUNDS))
		{
			TB_Bottom.VSplitLeft(60.0f, &Button, &TB_Bottom);

			bool Invoked = false;
			static int s_AddItemButton = 0;

			if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				Invoked = DoButton_Editor(&s_AddItemButton, "Add Quad", 0, &Button, 0, "[ctrl+q] Add a new quad") ||
					  (m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_Q) && ModPressed);
			}
			else if(pLayer->m_Type == LAYERTYPE_SOUNDS)
			{
				Invoked = DoButton_Editor(&s_AddItemButton, "Add Sound", 0, &Button, 0, "[ctrl+q] Add a new sound source") ||
					  (m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_Q) && ModPressed);
			}

			if(Invoked)
			{
				CLayerGroup *pGroup = GetSelectedGroup();

				float aMapping[4];
				pGroup->Mapping(aMapping);
				int x = aMapping[0] + (aMapping[2] - aMapping[0]) / 2;
				int y = aMapping[1] + (aMapping[3] - aMapping[1]) / 2;
				if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_Q) && ModPressed)
				{
					x += UI()->MouseWorldX() - (m_WorldOffsetX * pGroup->m_ParallaxX / 100) - pGroup->m_OffsetX;
					y += UI()->MouseWorldY() - (m_WorldOffsetY * pGroup->m_ParallaxY / 100) - pGroup->m_OffsetY;
				}

				if(pLayer->m_Type == LAYERTYPE_QUADS)
				{
					CLayerQuads *pLayerQuads = (CLayerQuads *)pLayer;

					int Width = 64;
					int Height = 64;
					if(pLayerQuads->m_Image >= 0)
					{
						Width = m_Map.m_vpImages[pLayerQuads->m_Image]->m_Width;
						Height = m_Map.m_vpImages[pLayerQuads->m_Image]->m_Height;
					}

					pLayerQuads->NewQuad(x, y, Width, Height);
				}
				else if(pLayer->m_Type == LAYERTYPE_SOUNDS)
				{
					CLayerSounds *pLayerSounds = (CLayerSounds *)pLayer;
					pLayerSounds->NewSource(x, y);
				}
			}
			TB_Bottom.VSplitLeft(5.0f, &Button, &TB_Bottom);
		}

		// Brush draw mode button
		{
			TB_Bottom.VSplitLeft(65.0f, &Button, &TB_Bottom);
			static int s_BrushDrawModeButton = 0;
			if(DoButton_Editor(&s_BrushDrawModeButton, "Destructive", m_BrushDrawDestructive, &Button, 0, "[ctrl+d] Toggle brush draw mode") ||
				(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_D) && ModPressed && !ShiftPressed))
				m_BrushDrawDestructive = !m_BrushDrawDestructive;
			TB_Bottom.VSplitLeft(5.0f, &Button, &TB_Bottom);
		}
	}
}

void CEditor::DoToolbarSounds(CUIRect ToolBar)
{
	CUIRect ToolBarTop, ToolBarBottom, Button;
	ToolBar.HSplitMid(&ToolBarTop, &ToolBarBottom, 5.0f);

	if(m_SelectedSound >= 0 && (size_t)m_SelectedSound < m_Map.m_vpSounds.size())
	{
		const CEditorSound *pSelectedSound = m_Map.m_vpSounds[m_SelectedSound];

		// play/stop button
		{
			ToolBarBottom.VSplitLeft(ToolBarBottom.h, &Button, &ToolBarBottom);
			static int s_PlayStopButton;
			if(DoButton_FontIcon(&s_PlayStopButton, Sound()->IsPlaying(pSelectedSound->m_SoundID) ? FONT_ICON_STOP : FONT_ICON_PLAY, 0, &Button, 0, "Play/stop audio preview", IGraphics::CORNER_ALL) ||
				(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_SPACE)))
			{
				if(Sound()->IsPlaying(pSelectedSound->m_SoundID))
					Sound()->Stop(pSelectedSound->m_SoundID);
				else
					Sound()->Play(CSounds::CHN_GUI, pSelectedSound->m_SoundID, 0);
			}
		}

		// duration
		{
			ToolBarBottom.VSplitLeft(5.0f, nullptr, &ToolBarBottom);
			char aDuration[32];
			char aDurationLabel[64];
			str_time_float(Sound()->GetSampleDuration(pSelectedSound->m_SoundID), TIME_HOURS, aDuration, sizeof(aDuration));
			str_format(aDurationLabel, sizeof(aDurationLabel), "Duration: %s", aDuration);
			UI()->DoLabel(&ToolBarBottom, aDurationLabel, 12.0f, TEXTALIGN_ML);
		}
	}
}

static void Rotate(const CPoint *pCenter, CPoint *pPoint, float Rotation)
{
	int x = pPoint->x - pCenter->x;
	int y = pPoint->y - pCenter->y;
	pPoint->x = (int)(x * std::cos(Rotation) - y * std::sin(Rotation) + pCenter->x);
	pPoint->y = (int)(x * std::sin(Rotation) + y * std::cos(Rotation) + pCenter->y);
}

void CEditor::DoSoundSource(CSoundSource *pSource, int Index)
{
	enum
	{
		OP_NONE = 0,
		OP_MOVE,
		OP_CONTEXT_MENU,
	};

	void *pID = &pSource->m_Position;

	static int s_Operation = OP_NONE;

	float wx = UI()->MouseWorldX();
	float wy = UI()->MouseWorldY();

	float CenterX = fx2f(pSource->m_Position.x);
	float CenterY = fx2f(pSource->m_Position.y);

	float dx = (CenterX - wx) / m_MouseWScale;
	float dy = (CenterY - wy) / m_MouseWScale;
	if(dx * dx + dy * dy < 50)
		UI()->SetHotItem(pID);

	const bool IgnoreGrid = Input()->AltIsPressed();

	if(UI()->CheckActiveItem(pID))
	{
		if(m_MouseDeltaWx * m_MouseDeltaWx + m_MouseDeltaWy * m_MouseDeltaWy > 0.0f)
		{
			if(s_Operation == OP_MOVE)
			{
				float x = wx;
				float y = wy;
				if(m_GridActive && !IgnoreGrid)
					SnapToGrid(x, y);
				pSource->m_Position.x = f2fx(x);
				pSource->m_Position.y = f2fx(y);
			}
		}

		if(s_Operation == OP_CONTEXT_MENU)
		{
			if(!UI()->MouseButton(1))
			{
				if(m_vSelectedLayers.size() == 1)
				{
					static SPopupMenuId s_PopupSourceId;
					UI()->DoPopupMenu(&s_PopupSourceId, UI()->MouseX(), UI()->MouseY(), 120, 200, this, PopupSource);
					UI()->DisableMouseLock();
				}
				s_Operation = OP_NONE;
				UI()->SetActiveItem(nullptr);
			}
		}
		else
		{
			if(!UI()->MouseButton(0))
			{
				UI()->DisableMouseLock();
				s_Operation = OP_NONE;
				UI()->SetActiveItem(nullptr);
			}
		}

		Graphics()->SetColor(1, 1, 1, 1);
	}
	else if(UI()->HotItem() == pID)
	{
		ms_pUiGotContext = pID;

		Graphics()->SetColor(1, 1, 1, 1);
		m_pTooltip = "Left mouse button to move. Hold alt to ignore grid.";

		if(UI()->MouseButton(0))
		{
			s_Operation = OP_MOVE;

			UI()->SetActiveItem(pID);
			m_SelectedSource = Index;
		}

		if(UI()->MouseButton(1))
		{
			m_SelectedSource = Index;
			s_Operation = OP_CONTEXT_MENU;
			UI()->SetActiveItem(pID);
		}
	}
	else
	{
		Graphics()->SetColor(0, 1, 0, 1);
	}

	IGraphics::CQuadItem QuadItem(CenterX, CenterY, 5.0f * m_MouseWScale, 5.0f * m_MouseWScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::DoQuad(CQuad *pQuad, int Index)
{
	enum
	{
		OP_NONE = 0,
		OP_MOVE_ALL,
		OP_MOVE_PIVOT,
		OP_ROTATE,
		OP_CONTEXT_MENU,
		OP_DELETE,
	};

	// some basic values
	void *pID = &pQuad->m_aPoints[4]; // use pivot addr as id
	static std::vector<std::vector<CPoint>> s_vvRotatePoints;
	static int s_Operation = OP_NONE;
	static float s_RotateAngle = 0;
	float wx = UI()->MouseWorldX();
	float wy = UI()->MouseWorldY();

	// get pivot
	float CenterX = fx2f(pQuad->m_aPoints[4].x);
	float CenterY = fx2f(pQuad->m_aPoints[4].y);

	float dx = (CenterX - wx) / m_MouseWScale;
	float dy = (CenterY - wy) / m_MouseWScale;
	if(dx * dx + dy * dy < 50)
		UI()->SetHotItem(pID);

	const bool IgnoreGrid = Input()->AltIsPressed();

	// draw selection background
	if(IsQuadSelected(Index))
	{
		Graphics()->SetColor(0, 0, 0, 1);
		IGraphics::CQuadItem QuadItem(CenterX, CenterY, 7.0f * m_MouseWScale, 7.0f * m_MouseWScale);
		Graphics()->QuadsDraw(&QuadItem, 1);
	}

	if(UI()->CheckActiveItem(pID))
	{
		if(m_MouseDeltaWx * m_MouseDeltaWx + m_MouseDeltaWy * m_MouseDeltaWy > 0.0f)
		{
			// check if we only should move pivot
			if(s_Operation == OP_MOVE_PIVOT)
			{
				float x = wx;
				float y = wy;
				if(m_GridActive && !IgnoreGrid)
					SnapToGrid(x, y);
				pQuad->m_aPoints[4].x = f2fx(x);
				pQuad->m_aPoints[4].y = f2fx(y);
			}
			else if(s_Operation == OP_MOVE_ALL)
			{
				// move all points including pivot
				float x = wx;
				float y = wy;
				if(m_GridActive && !IgnoreGrid)
					SnapToGrid(x, y);

				int OffsetX = f2fx(x) - pQuad->m_aPoints[4].x;
				int OffsetY = f2fx(y) - pQuad->m_aPoints[4].y;

				CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
				for(auto &Selected : m_vSelectedQuads)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
					for(auto &Point : pCurrentQuad->m_aPoints)
					{
						Point.x += OffsetX;
						Point.y += OffsetY;
					}
				}
			}
			else if(s_Operation == OP_ROTATE)
			{
				CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
				for(size_t i = 0; i < m_vSelectedQuads.size(); ++i)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[m_vSelectedQuads[i]];
					for(int v = 0; v < 4; v++)
					{
						pCurrentQuad->m_aPoints[v] = s_vvRotatePoints[i][v];
						Rotate(&pCurrentQuad->m_aPoints[4], &pCurrentQuad->m_aPoints[v], s_RotateAngle);
					}
				}
			}
		}

		s_RotateAngle += (m_MouseDeltaX)*0.002f;

		if(s_Operation == OP_CONTEXT_MENU)
		{
			if(!UI()->MouseButton(1))
			{
				if(m_vSelectedLayers.size() == 1)
				{
					m_SelectedQuadIndex = FindSelectedQuadIndex(Index);

					static SPopupMenuId s_PopupQuadId;
					UI()->DoPopupMenu(&s_PopupQuadId, UI()->MouseX(), UI()->MouseY(), 120, 198, this, PopupQuad);
					UI()->DisableMouseLock();
				}
				s_Operation = OP_NONE;
				UI()->SetActiveItem(nullptr);
			}
		}
		else if(s_Operation == OP_DELETE)
		{
			if(!UI()->MouseButton(1))
			{
				if(m_vSelectedLayers.size() == 1)
				{
					UI()->DisableMouseLock();
					m_Map.OnModify();
					DeleteSelectedQuads();
				}
				s_Operation = OP_NONE;
				UI()->SetActiveItem(nullptr);
			}
		}
		else
		{
			if(!UI()->MouseButton(0))
			{
				UI()->DisableMouseLock();
				s_Operation = OP_NONE;
				UI()->SetActiveItem(nullptr);
			}
		}

		Graphics()->SetColor(1, 1, 1, 1);
	}
	else if(UI()->HotItem() == pID)
	{
		ms_pUiGotContext = pID;

		Graphics()->SetColor(1, 1, 1, 1);
		m_pTooltip = "Left mouse button to move. Hold shift to move pivot. Hold ctrl to rotate. Hold alt to ignore grid. Hold shift and right click to delete.";

		if(UI()->MouseButton(0))
		{
			if(Input()->ShiftIsPressed())
			{
				s_Operation = OP_MOVE_PIVOT;

				SelectQuad(Index);
			}
			else if(Input()->ModifierIsPressed())
			{
				UI()->EnableMouseLock(pID);
				s_Operation = OP_ROTATE;
				s_RotateAngle = 0;

				if(!IsQuadSelected(Index))
					SelectQuad(Index);

				CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
				s_vvRotatePoints.clear();
				s_vvRotatePoints.resize(m_vSelectedQuads.size());
				for(size_t i = 0; i < m_vSelectedQuads.size(); ++i)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[m_vSelectedQuads[i]];

					s_vvRotatePoints[i].resize(4);
					s_vvRotatePoints[i][0] = pCurrentQuad->m_aPoints[0];
					s_vvRotatePoints[i][1] = pCurrentQuad->m_aPoints[1];
					s_vvRotatePoints[i][2] = pCurrentQuad->m_aPoints[2];
					s_vvRotatePoints[i][3] = pCurrentQuad->m_aPoints[3];
				}
			}
			else
			{
				s_Operation = OP_MOVE_ALL;

				if(!IsQuadSelected(Index))
					SelectQuad(Index);
			}

			UI()->SetActiveItem(pID);
		}

		if(UI()->MouseButton(1))
		{
			if(Input()->ShiftIsPressed())
			{
				s_Operation = OP_DELETE;

				if(!IsQuadSelected(Index))
					SelectQuad(Index);

				UI()->SetActiveItem(pID);
			}
			else
			{
				s_Operation = OP_CONTEXT_MENU;

				if(!IsQuadSelected(Index))
					SelectQuad(Index);

				UI()->SetActiveItem(pID);
			}
		}
	}
	else
		Graphics()->SetColor(0, 1, 0, 1);

	IGraphics::CQuadItem QuadItem(CenterX, CenterY, 5.0f * m_MouseWScale, 5.0f * m_MouseWScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::DoQuadPoint(CQuad *pQuad, int QuadIndex, int V)
{
	void *pID = &pQuad->m_aPoints[V];

	float wx = UI()->MouseWorldX();
	float wy = UI()->MouseWorldY();

	float px = fx2f(pQuad->m_aPoints[V].x);
	float py = fx2f(pQuad->m_aPoints[V].y);

	float dx = (px - wx) / m_MouseWScale;
	float dy = (py - wy) / m_MouseWScale;
	if(dx * dx + dy * dy < 50)
		UI()->SetHotItem(pID);

	// draw selection background
	if(IsQuadSelected(QuadIndex) && m_SelectedPoints & (1 << V))
	{
		Graphics()->SetColor(0, 0, 0, 1);
		IGraphics::CQuadItem QuadItem(px, py, 7.0f * m_MouseWScale, 7.0f * m_MouseWScale);
		Graphics()->QuadsDraw(&QuadItem, 1);
	}

	enum
	{
		OP_NONE = 0,
		OP_MOVEPOINT,
		OP_MOVEUV,
		OP_CONTEXT_MENU
	};

	static bool s_Moved;
	static int s_Operation = OP_NONE;

	const bool IgnoreGrid = Input()->AltIsPressed();

	if(UI()->CheckActiveItem(pID))
	{
		if(!s_Moved)
		{
			if(m_MouseDeltaWx * m_MouseDeltaWx + m_MouseDeltaWy * m_MouseDeltaWy > 0.0f)
				s_Moved = true;
		}

		if(s_Moved)
		{
			if(s_Operation == OP_MOVEPOINT)
			{
				float x = wx;
				float y = wy;
				if(m_GridActive && !IgnoreGrid)
					SnapToGrid(x, y);

				int OffsetX = f2fx(x) - pQuad->m_aPoints[V].x;
				int OffsetY = f2fx(y) - pQuad->m_aPoints[V].y;

				CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
				for(auto &Selected : m_vSelectedQuads)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
					for(int m = 0; m < 4; m++)
					{
						if(m_SelectedPoints & (1 << m))
						{
							pCurrentQuad->m_aPoints[m].x += OffsetX;
							pCurrentQuad->m_aPoints[m].y += OffsetY;
						}
					}
				}
			}
			else if(s_Operation == OP_MOVEUV)
			{
				CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
				for(auto &Selected : m_vSelectedQuads)
				{
					CQuad *pCurrentQuad = &pLayer->m_vQuads[Selected];
					for(int m = 0; m < 4; m++)
						if(m_SelectedPoints & (1 << m))
						{
							// 0,2;1,3 - line x
							// 0,1;2,3 - line y

							pCurrentQuad->m_aTexcoords[m].x += f2fx(m_MouseDeltaWx * 0.001f);
							pCurrentQuad->m_aTexcoords[(m + 2) % 4].x += f2fx(m_MouseDeltaWx * 0.001f);

							pCurrentQuad->m_aTexcoords[m].y += f2fx(m_MouseDeltaWy * 0.001f);
							pCurrentQuad->m_aTexcoords[m ^ 1].y += f2fx(m_MouseDeltaWy * 0.001f);
						}
				}
			}
		}

		if(s_Operation == OP_CONTEXT_MENU)
		{
			if(!UI()->MouseButton(1))
			{
				if(m_vSelectedLayers.size() == 1)
				{
					m_SelectedQuadPoint = V;
					m_SelectedQuadIndex = FindSelectedQuadIndex(QuadIndex);

					static SPopupMenuId s_PopupPointId;
					UI()->DoPopupMenu(&s_PopupPointId, UI()->MouseX(), UI()->MouseY(), 120, 75, this, PopupPoint);
				}
				UI()->SetActiveItem(nullptr);
			}
		}
		else
		{
			if(!UI()->MouseButton(0))
			{
				if(!s_Moved)
				{
					if(Input()->ShiftIsPressed())
						m_SelectedPoints ^= 1 << V;
					else
						m_SelectedPoints = 1 << V;
				}

				UI()->DisableMouseLock();
				UI()->SetActiveItem(nullptr);
			}
		}

		Graphics()->SetColor(1, 1, 1, 1);
	}
	else if(UI()->HotItem() == pID)
	{
		ms_pUiGotContext = pID;

		Graphics()->SetColor(1, 1, 1, 1);
		m_pTooltip = "Left mouse button to move. Hold shift to move the texture. Hold alt to ignore grid.";

		if(UI()->MouseButton(0))
		{
			UI()->SetActiveItem(pID);
			s_Moved = false;
			if(Input()->ShiftIsPressed())
			{
				s_Operation = OP_MOVEUV;
				UI()->EnableMouseLock(pID);
			}
			else
				s_Operation = OP_MOVEPOINT;

			if(!(m_SelectedPoints & (1 << V)))
			{
				if(Input()->ShiftIsPressed())
					m_SelectedPoints |= 1 << V;
				else
					m_SelectedPoints = 1 << V;
				s_Moved = true;
			}

			if(!IsQuadSelected(QuadIndex))
				SelectQuad(QuadIndex);
		}
		else if(UI()->MouseButton(1))
		{
			s_Operation = OP_CONTEXT_MENU;

			if(!IsQuadSelected(QuadIndex))
				SelectQuad(QuadIndex);

			UI()->SetActiveItem(pID);
			if(!(m_SelectedPoints & (1 << V)))
			{
				if(Input()->ShiftIsPressed())
					m_SelectedPoints |= 1 << V;
				else
					m_SelectedPoints = 1 << V;
				s_Moved = true;
			}
		}
	}
	else
		Graphics()->SetColor(1, 0, 0, 1);

	IGraphics::CQuadItem QuadItem(px, py, 5.0f * m_MouseWScale, 5.0f * m_MouseWScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

float CEditor::TriangleArea(vec2 A, vec2 B, vec2 C)
{
	return absolute(((B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y)) * 0.5f);
}

bool CEditor::IsInTriangle(vec2 Point, vec2 A, vec2 B, vec2 C)
{
	// Normalize to increase precision
	vec2 Min(minimum(A.x, B.x, C.x), minimum(A.y, B.y, C.y));
	vec2 Max(maximum(A.x, B.x, C.x), maximum(A.y, B.y, C.y));
	vec2 Size(Max.x - Min.x, Max.y - Min.y);

	if(Size.x < 0.0000001f || Size.y < 0.0000001f)
		return false;

	vec2 Normal(1.f / Size.x, 1.f / Size.y);

	A = (A - Min) * Normal;
	B = (B - Min) * Normal;
	C = (C - Min) * Normal;
	Point = (Point - Min) * Normal;

	float Area = TriangleArea(A, B, C);
	return Area > 0.f && absolute(TriangleArea(Point, A, B) + TriangleArea(Point, B, C) + TriangleArea(Point, C, A) - Area) < 0.000001f;
}

void CEditor::DoQuadKnife(int QuadIndex)
{
	CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
	CQuad *pQuad = &pLayer->m_vQuads[QuadIndex];

	const bool IgnoreGrid = Input()->AltIsPressed();
	float SnapRadius = 4.f * m_MouseWScale;

	vec2 Mouse = vec2(UI()->MouseWorldX(), UI()->MouseWorldY());
	vec2 Point = Mouse;

	vec2 v[4] = {
		vec2(fx2f(pQuad->m_aPoints[0].x), fx2f(pQuad->m_aPoints[0].y)),
		vec2(fx2f(pQuad->m_aPoints[1].x), fx2f(pQuad->m_aPoints[1].y)),
		vec2(fx2f(pQuad->m_aPoints[3].x), fx2f(pQuad->m_aPoints[3].y)),
		vec2(fx2f(pQuad->m_aPoints[2].x), fx2f(pQuad->m_aPoints[2].y))};

	m_pTooltip = "Left click inside the quad to select an area to slice. Hold alt to ignore grid. Right click to leave knife mode";

	if(UI()->MouseButtonClicked(1))
	{
		m_QuadKnifeActive = false;
		return;
	}

	// Handle snapping
	if(m_GridActive && !IgnoreGrid)
	{
		float CellSize = (float)GetLineDistance();
		vec2 OnGrid = vec2(std::round(Mouse.x / CellSize) * CellSize, std::round(Mouse.y / CellSize) * CellSize);

		if(IsInTriangle(OnGrid, v[0], v[1], v[2]) || IsInTriangle(OnGrid, v[0], v[3], v[2]))
			Point = OnGrid;
		else
		{
			float MinDistance = -1.f;

			for(int i = 0; i < 4; i++)
			{
				int j = (i + 1) % 4;
				vec2 Min(minimum(v[i].x, v[j].x), minimum(v[i].y, v[j].y));
				vec2 Max(maximum(v[i].x, v[j].x), maximum(v[i].y, v[j].y));

				if(in_range(OnGrid.y, Min.y, Max.y) && Max.y - Min.y > 0.0000001f)
				{
					vec2 OnEdge(v[i].x + (OnGrid.y - v[i].y) / (v[j].y - v[i].y) * (v[j].x - v[i].x), OnGrid.y);
					float Distance = absolute(OnGrid.x - OnEdge.x);

					if(Distance < CellSize && (Distance < MinDistance || MinDistance < 0.f))
					{
						MinDistance = Distance;
						Point = OnEdge;
					}
				}

				if(in_range(OnGrid.x, Min.x, Max.x) && Max.x - Min.x > 0.0000001f)
				{
					vec2 OnEdge(OnGrid.x, v[i].y + (OnGrid.x - v[i].x) / (v[j].x - v[i].x) * (v[j].y - v[i].y));
					float Distance = absolute(OnGrid.y - OnEdge.y);

					if(Distance < CellSize && (Distance < MinDistance || MinDistance < 0.f))
					{
						MinDistance = Distance;
						Point = OnEdge;
					}
				}
			}
		}
	}
	else
	{
		float MinDistance = -1.f;

		// Try snapping to corners
		for(const auto &x : v)
		{
			float Distance = distance(Mouse, x);

			if(Distance <= SnapRadius && (Distance < MinDistance || MinDistance < 0.f))
			{
				MinDistance = Distance;
				Point = x;
			}
		}

		if(MinDistance < 0.f)
		{
			// Try snapping to edges
			for(int i = 0; i < 4; i++)
			{
				int j = (i + 1) % 4;
				vec2 s(v[j] - v[i]);

				float t = ((Mouse.x - v[i].x) * s.x + (Mouse.y - v[i].y) * s.y) / (s.x * s.x + s.y * s.y);

				if(in_range(t, 0.f, 1.f))
				{
					vec2 OnEdge = vec2((v[i].x + t * s.x), (v[i].y + t * s.y));
					float Distance = distance(Mouse, OnEdge);

					if(Distance <= SnapRadius && (Distance < MinDistance || MinDistance < 0.f))
					{
						MinDistance = Distance;
						Point = OnEdge;
					}
				}
			}
		}
	}

	bool ValidPosition = IsInTriangle(Point, v[0], v[1], v[2]) || IsInTriangle(Point, v[0], v[3], v[2]);

	if(UI()->MouseButtonClicked(0) && ValidPosition)
	{
		m_aQuadKnifePoints[m_QuadKnifeCount] = Point;
		m_QuadKnifeCount++;
	}

	if(m_QuadKnifeCount == 4)
	{
		if(IsInTriangle(m_aQuadKnifePoints[3], m_aQuadKnifePoints[0], m_aQuadKnifePoints[1], m_aQuadKnifePoints[2]) ||
			IsInTriangle(m_aQuadKnifePoints[1], m_aQuadKnifePoints[0], m_aQuadKnifePoints[2], m_aQuadKnifePoints[3]))
		{
			// Fix concave order
			std::swap(m_aQuadKnifePoints[0], m_aQuadKnifePoints[3]);
			std::swap(m_aQuadKnifePoints[1], m_aQuadKnifePoints[2]);
		}

		std::swap(m_aQuadKnifePoints[2], m_aQuadKnifePoints[3]);

		CQuad *pResult = pLayer->NewQuad(64, 64, 64, 64);
		pQuad = &pLayer->m_vQuads[QuadIndex];

		for(int i = 0; i < 4; i++)
		{
			int t = IsInTriangle(m_aQuadKnifePoints[i], v[0], v[3], v[2]) ? 2 : 1;

			vec2 A = vec2(fx2f(pQuad->m_aPoints[0].x), fx2f(pQuad->m_aPoints[0].y));
			vec2 B = vec2(fx2f(pQuad->m_aPoints[3].x), fx2f(pQuad->m_aPoints[3].y));
			vec2 C = vec2(fx2f(pQuad->m_aPoints[t].x), fx2f(pQuad->m_aPoints[t].y));

			float TriArea = TriangleArea(A, B, C);
			float WeightA = TriangleArea(m_aQuadKnifePoints[i], B, C) / TriArea;
			float WeightB = TriangleArea(m_aQuadKnifePoints[i], C, A) / TriArea;
			float WeightC = TriangleArea(m_aQuadKnifePoints[i], A, B) / TriArea;

			pResult->m_aColors[i].r = (int)std::round(pQuad->m_aColors[0].r * WeightA + pQuad->m_aColors[3].r * WeightB + pQuad->m_aColors[t].r * WeightC);
			pResult->m_aColors[i].g = (int)std::round(pQuad->m_aColors[0].g * WeightA + pQuad->m_aColors[3].g * WeightB + pQuad->m_aColors[t].g * WeightC);
			pResult->m_aColors[i].b = (int)std::round(pQuad->m_aColors[0].b * WeightA + pQuad->m_aColors[3].b * WeightB + pQuad->m_aColors[t].b * WeightC);
			pResult->m_aColors[i].a = (int)std::round(pQuad->m_aColors[0].a * WeightA + pQuad->m_aColors[3].a * WeightB + pQuad->m_aColors[t].a * WeightC);

			pResult->m_aTexcoords[i].x = (int)std::round(pQuad->m_aTexcoords[0].x * WeightA + pQuad->m_aTexcoords[3].x * WeightB + pQuad->m_aTexcoords[t].x * WeightC);
			pResult->m_aTexcoords[i].y = (int)std::round(pQuad->m_aTexcoords[0].y * WeightA + pQuad->m_aTexcoords[3].y * WeightB + pQuad->m_aTexcoords[t].y * WeightC);

			pResult->m_aPoints[i].x = f2fx(m_aQuadKnifePoints[i].x);
			pResult->m_aPoints[i].y = f2fx(m_aQuadKnifePoints[i].y);
		}

		pResult->m_aPoints[4].x = ((pResult->m_aPoints[0].x + pResult->m_aPoints[3].x) / 2 + (pResult->m_aPoints[1].x + pResult->m_aPoints[2].x) / 2) / 2;
		pResult->m_aPoints[4].y = ((pResult->m_aPoints[0].y + pResult->m_aPoints[3].y) / 2 + (pResult->m_aPoints[1].y + pResult->m_aPoints[2].y) / 2) / 2;

		m_QuadKnifeCount = 0;
	}

	// Render
	Graphics()->TextureClear();
	Graphics()->LinesBegin();

	IGraphics::CLineItem aEdges[4] = {
		IGraphics::CLineItem(v[0].x, v[0].y, v[1].x, v[1].y),
		IGraphics::CLineItem(v[1].x, v[1].y, v[2].x, v[2].y),
		IGraphics::CLineItem(v[2].x, v[2].y, v[3].x, v[3].y),
		IGraphics::CLineItem(v[3].x, v[3].y, v[0].x, v[0].y)};

	Graphics()->SetColor(1.f, 0.5f, 0.f, 1.f);
	Graphics()->LinesDraw(aEdges, 4);

	IGraphics::CLineItem aLines[4];
	int LineCount = maximum(m_QuadKnifeCount - 1, 0);

	for(int i = 0; i < LineCount; i++)
		aLines[i] = IGraphics::CLineItem(m_aQuadKnifePoints[i].x, m_aQuadKnifePoints[i].y, m_aQuadKnifePoints[i + 1].x, m_aQuadKnifePoints[i + 1].y);

	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
	Graphics()->LinesDraw(aLines, LineCount);

	if(ValidPosition)
	{
		if(m_QuadKnifeCount > 0)
		{
			IGraphics::CLineItem LineCurrent(Point.x, Point.y, m_aQuadKnifePoints[m_QuadKnifeCount - 1].x, m_aQuadKnifePoints[m_QuadKnifeCount - 1].y);
			Graphics()->LinesDraw(&LineCurrent, 1);
		}

		if(m_QuadKnifeCount == 3)
		{
			IGraphics::CLineItem LineClose(Point.x, Point.y, m_aQuadKnifePoints[0].x, m_aQuadKnifePoints[0].y);
			Graphics()->LinesDraw(&LineClose, 1);
		}
	}

	Graphics()->LinesEnd();
	Graphics()->QuadsBegin();

	IGraphics::CQuadItem aMarkers[4];

	for(int i = 0; i < m_QuadKnifeCount; i++)
		aMarkers[i] = IGraphics::CQuadItem(m_aQuadKnifePoints[i].x, m_aQuadKnifePoints[i].y, 5.f * m_MouseWScale, 5.f * m_MouseWScale);

	Graphics()->SetColor(0.f, 0.f, 1.f, 1.f);
	Graphics()->QuadsDraw(aMarkers, m_QuadKnifeCount);

	if(ValidPosition)
	{
		IGraphics::CQuadItem MarkerCurrent(Point.x, Point.y, 5.f * m_MouseWScale, 5.f * m_MouseWScale);
		Graphics()->QuadsDraw(&MarkerCurrent, 1);
	}

	Graphics()->QuadsEnd();
}

void CEditor::DoQuadEnvelopes(const std::vector<CQuad> &vQuads, IGraphics::CTextureHandle Texture)
{
	size_t Num = vQuads.size();
	CEnvelope **apEnvelope = new CEnvelope *[Num];
	mem_zero(apEnvelope, sizeof(CEnvelope *) * Num); // NOLINT(bugprone-sizeof-expression)
	for(size_t i = 0; i < Num; i++)
	{
		if((m_ShowEnvelopePreview == SHOWENV_SELECTED && vQuads[i].m_PosEnv == m_SelectedEnvelope) || m_ShowEnvelopePreview == SHOWENV_ALL)
			if(vQuads[i].m_PosEnv >= 0 && vQuads[i].m_PosEnv < (int)m_Map.m_vpEnvelopes.size())
				apEnvelope[i] = m_Map.m_vpEnvelopes[vQuads[i].m_PosEnv];
	}

	//Draw Lines
	Graphics()->TextureClear();
	Graphics()->LinesBegin();
	Graphics()->SetColor(80.0f / 255, 150.0f / 255, 230.f / 255, 0.5f);
	for(size_t j = 0; j < Num; j++)
	{
		if(!apEnvelope[j])
			continue;

		//QuadParams
		const CPoint *pPoints = vQuads[j].m_aPoints;
		for(size_t i = 0; i < apEnvelope[j]->m_vPoints.size() - 1; i++)
		{
			float OffsetX = fx2f(apEnvelope[j]->m_vPoints[i].m_aValues[0]);
			float OffsetY = fx2f(apEnvelope[j]->m_vPoints[i].m_aValues[1]);
			vec2 Pos0 = vec2(fx2f(pPoints[4].x) + OffsetX, fx2f(pPoints[4].y) + OffsetY);

			OffsetX = fx2f(apEnvelope[j]->m_vPoints[i + 1].m_aValues[0]);
			OffsetY = fx2f(apEnvelope[j]->m_vPoints[i + 1].m_aValues[1]);
			vec2 Pos1 = vec2(fx2f(pPoints[4].x) + OffsetX, fx2f(pPoints[4].y) + OffsetY);

			IGraphics::CLineItem Line = IGraphics::CLineItem(Pos0.x, Pos0.y, Pos1.x, Pos1.y);
			Graphics()->LinesDraw(&Line, 1);
		}
	}
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	Graphics()->LinesEnd();

	//Draw Quads
	Graphics()->TextureSet(Texture);
	Graphics()->QuadsBegin();

	for(size_t j = 0; j < Num; j++)
	{
		if(!apEnvelope[j])
			continue;

		//QuadParams
		const CPoint *pPoints = vQuads[j].m_aPoints;

		for(size_t i = 0; i < apEnvelope[j]->m_vPoints.size(); i++)
		{
			//Calc Env Position
			float OffsetX = fx2f(apEnvelope[j]->m_vPoints[i].m_aValues[0]);
			float OffsetY = fx2f(apEnvelope[j]->m_vPoints[i].m_aValues[1]);
			float Rot = fx2f(apEnvelope[j]->m_vPoints[i].m_aValues[2]) / 360.0f * pi * 2;

			//Set Colours
			float Alpha = (m_SelectedQuadEnvelope == vQuads[j].m_PosEnv && m_SelectedEnvelopePoint == (int)i) ? 0.65f : 0.35f;
			IGraphics::CColorVertex aArray[4] = {
				IGraphics::CColorVertex(0, vQuads[j].m_aColors[0].r, vQuads[j].m_aColors[0].g, vQuads[j].m_aColors[0].b, Alpha),
				IGraphics::CColorVertex(1, vQuads[j].m_aColors[1].r, vQuads[j].m_aColors[1].g, vQuads[j].m_aColors[1].b, Alpha),
				IGraphics::CColorVertex(2, vQuads[j].m_aColors[2].r, vQuads[j].m_aColors[2].g, vQuads[j].m_aColors[2].b, Alpha),
				IGraphics::CColorVertex(3, vQuads[j].m_aColors[3].r, vQuads[j].m_aColors[3].g, vQuads[j].m_aColors[3].b, Alpha)};
			Graphics()->SetColorVertex(aArray, 4);

			//Rotation
			if(Rot != 0)
			{
				static CPoint aRotated[4];
				aRotated[0] = vQuads[j].m_aPoints[0];
				aRotated[1] = vQuads[j].m_aPoints[1];
				aRotated[2] = vQuads[j].m_aPoints[2];
				aRotated[3] = vQuads[j].m_aPoints[3];
				pPoints = aRotated;

				Rotate(&vQuads[j].m_aPoints[4], &aRotated[0], Rot);
				Rotate(&vQuads[j].m_aPoints[4], &aRotated[1], Rot);
				Rotate(&vQuads[j].m_aPoints[4], &aRotated[2], Rot);
				Rotate(&vQuads[j].m_aPoints[4], &aRotated[3], Rot);
			}

			//Set Texture Coords
			Graphics()->QuadsSetSubsetFree(
				fx2f(vQuads[j].m_aTexcoords[0].x), fx2f(vQuads[j].m_aTexcoords[0].y),
				fx2f(vQuads[j].m_aTexcoords[1].x), fx2f(vQuads[j].m_aTexcoords[1].y),
				fx2f(vQuads[j].m_aTexcoords[2].x), fx2f(vQuads[j].m_aTexcoords[2].y),
				fx2f(vQuads[j].m_aTexcoords[3].x), fx2f(vQuads[j].m_aTexcoords[3].y));

			//Set Quad Coords & Draw
			IGraphics::CFreeformItem Freeform(
				fx2f(pPoints[0].x) + OffsetX, fx2f(pPoints[0].y) + OffsetY,
				fx2f(pPoints[1].x) + OffsetX, fx2f(pPoints[1].y) + OffsetY,
				fx2f(pPoints[2].x) + OffsetX, fx2f(pPoints[2].y) + OffsetY,
				fx2f(pPoints[3].x) + OffsetX, fx2f(pPoints[3].y) + OffsetY);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}
	}
	Graphics()->QuadsEnd();
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();

	// Draw QuadPoints
	for(size_t j = 0; j < Num; j++)
	{
		if(!apEnvelope[j])
			continue;

		for(size_t i = 0; i < apEnvelope[j]->m_vPoints.size(); i++)
			DoQuadEnvPoint(&vQuads[j], j, i);
	}
	Graphics()->QuadsEnd();
	delete[] apEnvelope;
}

void CEditor::DoQuadEnvPoint(const CQuad *pQuad, int QIndex, int PIndex)
{
	enum
	{
		OP_NONE = 0,
		OP_MOVE,
		OP_ROTATE,
	};

	// some basic values
	static float s_LastWx = 0;
	static float s_LastWy = 0;
	static int s_Operation = OP_NONE;
	float wx = UI()->MouseWorldX();
	float wy = UI()->MouseWorldY();
	CEnvelope *pEnvelope = m_Map.m_vpEnvelopes[pQuad->m_PosEnv];
	void *pID = &pEnvelope->m_vPoints[PIndex];
	static int s_CurQIndex = -1;

	// get pivot
	float CenterX = fx2f(pQuad->m_aPoints[4].x) + fx2f(pEnvelope->m_vPoints[PIndex].m_aValues[0]);
	float CenterY = fx2f(pQuad->m_aPoints[4].y) + fx2f(pEnvelope->m_vPoints[PIndex].m_aValues[1]);

	float dx = (CenterX - wx) / m_MouseWScale;
	float dy = (CenterY - wy) / m_MouseWScale;
	if(dx * dx + dy * dy < 50.0f && UI()->CheckActiveItem(nullptr))
	{
		UI()->SetHotItem(pID);
		s_CurQIndex = QIndex;
	}

	const bool IgnoreGrid = Input()->AltIsPressed();

	if(UI()->CheckActiveItem(pID) && s_CurQIndex == QIndex)
	{
		if(s_Operation == OP_MOVE)
		{
			if(m_GridActive && !IgnoreGrid)
			{
				float x = wx;
				float y = wy;
				SnapToGrid(x, y);
				pEnvelope->m_vPoints[PIndex].m_aValues[0] = f2fx(x) - pQuad->m_aPoints[4].x;
				pEnvelope->m_vPoints[PIndex].m_aValues[1] = f2fx(y) - pQuad->m_aPoints[4].y;
			}
			else
			{
				pEnvelope->m_vPoints[PIndex].m_aValues[0] += f2fx(wx - s_LastWx);
				pEnvelope->m_vPoints[PIndex].m_aValues[1] += f2fx(wy - s_LastWy);
			}
		}
		else if(s_Operation == OP_ROTATE)
			pEnvelope->m_vPoints[PIndex].m_aValues[2] += 10 * m_MouseDeltaX;

		s_LastWx = wx;
		s_LastWy = wy;

		if(!UI()->MouseButton(0))
		{
			UI()->DisableMouseLock();
			s_Operation = OP_NONE;
			UI()->SetActiveItem(nullptr);
		}

		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	else if(UI()->HotItem() == pID && s_CurQIndex == QIndex)
	{
		ms_pUiGotContext = pID;

		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		m_pTooltip = "Left mouse button to move. Hold ctrl to rotate. Hold alt to ignore grid.";

		if(UI()->MouseButton(0))
		{
			if(Input()->ModifierIsPressed())
			{
				UI()->EnableMouseLock(pID);
				s_Operation = OP_ROTATE;

				SelectQuad(QIndex);
			}
			else
			{
				s_Operation = OP_MOVE;

				SelectQuad(QIndex);
			}

			m_SelectedEnvelopePoint = PIndex;
			m_SelectedQuadEnvelope = pQuad->m_PosEnv;

			UI()->SetActiveItem(pID);

			s_LastWx = wx;
			s_LastWy = wy;
		}
		else
		{
			m_SelectedEnvelopePoint = -1;
			m_SelectedQuadEnvelope = -1;
		}
	}
	else
		Graphics()->SetColor(0.0f, 1.0f, 0.0f, 1.0f);

	IGraphics::CQuadItem QuadItem(CenterX, CenterY, 5.0f * m_MouseWScale, 5.0f * m_MouseWScale);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CEditor::DoMapEditor(CUIRect View)
{
	// render all good stuff
	if(!m_ShowPicker)
	{
		if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->ShiftIsPressed() && !Input()->ModifierIsPressed() && Input()->KeyPress(KEY_G))
		{
			const bool AnyHidden =
				!m_Map.m_pGameLayer->m_Visible ||
				(m_Map.m_pFrontLayer && !m_Map.m_pFrontLayer->m_Visible) ||
				(m_Map.m_pTeleLayer && !m_Map.m_pTeleLayer->m_Visible) ||
				(m_Map.m_pSpeedupLayer && !m_Map.m_pSpeedupLayer->m_Visible) ||
				(m_Map.m_pTuneLayer && !m_Map.m_pTuneLayer->m_Visible) ||
				(m_Map.m_pSwitchLayer && !m_Map.m_pSwitchLayer->m_Visible);
			m_Map.m_pGameLayer->m_Visible = AnyHidden;
			if(m_Map.m_pFrontLayer)
				m_Map.m_pFrontLayer->m_Visible = AnyHidden;
			if(m_Map.m_pTeleLayer)
				m_Map.m_pTeleLayer->m_Visible = AnyHidden;
			if(m_Map.m_pSpeedupLayer)
				m_Map.m_pSpeedupLayer->m_Visible = AnyHidden;
			if(m_Map.m_pTuneLayer)
				m_Map.m_pTuneLayer->m_Visible = AnyHidden;
			if(m_Map.m_pSwitchLayer)
				m_Map.m_pSwitchLayer->m_Visible = AnyHidden;
		}

		for(auto &pGroup : m_Map.m_vpGroups)
		{
			if(pGroup->m_Visible)
				pGroup->Render();
		}

		// render the game, tele, speedup, front, tune and switch above everything else
		if(m_Map.m_pGameGroup->m_Visible)
		{
			m_Map.m_pGameGroup->MapScreen();
			for(auto &pLayer : m_Map.m_pGameGroup->m_vpLayers)
			{
				if(pLayer->m_Visible && pLayer->IsEntitiesLayer())
					pLayer->Render();
			}
		}

		CLayerTiles *pT = static_cast<CLayerTiles *>(GetSelectedLayerType(0, LAYERTYPE_TILES));
		if(m_ShowTileInfo != SHOW_TILE_OFF && pT && pT->m_Visible && m_Zoom <= 300.0f)
		{
			GetSelectedGroup()->MapScreen();
			pT->ShowInfo();
		}
	}
	else
	{
		// fix aspect ratio of the image in the picker
		float Max = minimum(View.w, View.h);
		View.w = View.h = Max;
	}

	static void *s_pEditorID = (void *)&s_pEditorID;
	const bool Inside = !m_GuiActive || UI()->MouseInside(&View);

	// fetch mouse position
	float wx = UI()->MouseWorldX();
	float wy = UI()->MouseWorldY();
	float mx = UI()->MouseX();
	float my = UI()->MouseY();

	static float s_StartWx = 0;
	static float s_StartWy = 0;

	enum
	{
		OP_NONE = 0,
		OP_BRUSH_GRAB,
		OP_BRUSH_DRAW,
		OP_BRUSH_PAINT,
		OP_PAN_WORLD,
		OP_PAN_EDITOR,
	};

	// remap the screen so it can display the whole tileset
	if(m_ShowPicker)
	{
		CUIRect Screen = *UI()->Screen();
		float Size = 32.0f * 16.0f;
		float w = Size * (Screen.w / View.w);
		float h = Size * (Screen.h / View.h);
		float x = -(View.x / Screen.w) * w;
		float y = -(View.y / Screen.h) * h;
		wx = x + w * mx / Screen.w;
		wy = y + h * my / Screen.h;
		CLayerTiles *pTileLayer = (CLayerTiles *)GetSelectedLayerType(0, LAYERTYPE_TILES);
		if(pTileLayer)
		{
			Graphics()->MapScreen(x, y, x + w, y + h);
			m_TilesetPicker.m_Image = pTileLayer->m_Image;
			m_TilesetPicker.m_Texture = pTileLayer->m_Texture;
			if(m_BrushColorEnabled)
			{
				m_TilesetPicker.m_Color = pTileLayer->m_Color;
				m_TilesetPicker.m_Color.a = 255;
			}
			else
			{
				m_TilesetPicker.m_Color = {255, 255, 255, 255};
			}

			m_TilesetPicker.m_Game = pTileLayer->m_Game;
			m_TilesetPicker.m_Tele = pTileLayer->m_Tele;
			m_TilesetPicker.m_Speedup = pTileLayer->m_Speedup;
			m_TilesetPicker.m_Front = pTileLayer->m_Front;
			m_TilesetPicker.m_Switch = pTileLayer->m_Switch;
			m_TilesetPicker.m_Tune = pTileLayer->m_Tune;

			m_TilesetPicker.Render(true);

			if(m_ShowTileInfo != SHOW_TILE_OFF)
				m_TilesetPicker.ShowInfo();
		}
		else
		{
			CLayerQuads *pQuadLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
			if(pQuadLayer)
			{
				m_QuadsetPicker.m_Image = pQuadLayer->m_Image;
				m_QuadsetPicker.m_vQuads[0].m_aPoints[0].x = f2fx(View.x);
				m_QuadsetPicker.m_vQuads[0].m_aPoints[0].y = f2fx(View.y);
				m_QuadsetPicker.m_vQuads[0].m_aPoints[1].x = f2fx((View.x + View.w));
				m_QuadsetPicker.m_vQuads[0].m_aPoints[1].y = f2fx(View.y);
				m_QuadsetPicker.m_vQuads[0].m_aPoints[2].x = f2fx(View.x);
				m_QuadsetPicker.m_vQuads[0].m_aPoints[2].y = f2fx((View.y + View.h));
				m_QuadsetPicker.m_vQuads[0].m_aPoints[3].x = f2fx((View.x + View.w));
				m_QuadsetPicker.m_vQuads[0].m_aPoints[3].y = f2fx((View.y + View.h));
				m_QuadsetPicker.m_vQuads[0].m_aPoints[4].x = f2fx((View.x + View.w / 2));
				m_QuadsetPicker.m_vQuads[0].m_aPoints[4].y = f2fx((View.y + View.h / 2));
				m_QuadsetPicker.Render();
			}
		}
	}

	static int s_Operation = OP_NONE;

	// draw layer borders
	CLayer *apEditLayers[128];
	size_t NumEditLayers = 0;

	if(m_ShowPicker && GetSelectedLayer(0) && GetSelectedLayer(0)->m_Type == LAYERTYPE_TILES)
	{
		apEditLayers[0] = &m_TilesetPicker;
		NumEditLayers++;
	}
	else if(m_ShowPicker)
	{
		apEditLayers[0] = &m_QuadsetPicker;
		NumEditLayers++;
	}
	else
	{
		// pick a type of layers to edit, preferring Tiles layers.
		int EditingType = -1;
		for(size_t i = 0; i < m_vSelectedLayers.size(); i++)
		{
			CLayer *pLayer = GetSelectedLayer(i);
			if(pLayer && (EditingType == -1 || pLayer->m_Type == LAYERTYPE_TILES))
			{
				EditingType = pLayer->m_Type;
				if(EditingType == LAYERTYPE_TILES)
					break;
			}
		}
		for(size_t i = 0; i < m_vSelectedLayers.size() && NumEditLayers < 128; i++)
		{
			apEditLayers[NumEditLayers] = GetSelectedLayerType(i, EditingType);
			if(apEditLayers[NumEditLayers])
			{
				NumEditLayers++;
			}
		}

		CLayerGroup *pGroup = GetSelectedGroup();
		if(pGroup)
		{
			pGroup->MapScreen();

			RenderGrid(pGroup);

			for(size_t i = 0; i < NumEditLayers; i++)
			{
				if(apEditLayers[i]->m_Type != LAYERTYPE_TILES)
					continue;

				float w, h;
				apEditLayers[i]->GetSize(&w, &h);

				IGraphics::CLineItem Array[4] = {
					IGraphics::CLineItem(0, 0, w, 0),
					IGraphics::CLineItem(w, 0, w, h),
					IGraphics::CLineItem(w, h, 0, h),
					IGraphics::CLineItem(0, h, 0, 0)};
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				Graphics()->LinesDraw(Array, 4);
				Graphics()->LinesEnd();
			}
		}
	}

	if(Inside)
	{
		UI()->SetHotItem(s_pEditorID);

		// do global operations like pan and zoom
		if(UI()->CheckActiveItem(nullptr) && (UI()->MouseButton(0) || UI()->MouseButton(2)))
		{
			s_StartWx = wx;
			s_StartWy = wy;

			if(Input()->ModifierIsPressed() || UI()->MouseButton(2))
			{
				if(Input()->ShiftIsPressed())
					s_Operation = OP_PAN_EDITOR;
				else
					s_Operation = OP_PAN_WORLD;
				UI()->SetActiveItem(s_pEditorID);
			}
			else
				s_Operation = OP_NONE;
		}

		// brush editing
		if(UI()->HotItem() == s_pEditorID)
		{
			int Layer = NUM_LAYERS;
			if(m_ShowPicker)
			{
				CLayer *pLayer = GetSelectedLayer(0);
				if(pLayer == m_Map.m_pGameLayer)
					Layer = LAYER_GAME;
				else if(pLayer == m_Map.m_pFrontLayer)
					Layer = LAYER_FRONT;
				else if(pLayer == m_Map.m_pSwitchLayer)
					Layer = LAYER_SWITCH;
				else if(pLayer == m_Map.m_pTeleLayer)
					Layer = LAYER_TELE;
				else if(pLayer == m_Map.m_pSpeedupLayer)
					Layer = LAYER_SPEEDUP;
				else if(pLayer == m_Map.m_pTuneLayer)
					Layer = LAYER_TUNE;
			}
			if(m_ShowPicker && Layer != NUM_LAYERS)
			{
				if(m_SelectEntitiesImage == "DDNet")
					m_pTooltip = Explain(EXPLANATION_DDNET, (int)wx / 32 + (int)wy / 32 * 16, Layer);
				else if(m_SelectEntitiesImage == "FNG")
					m_pTooltip = Explain(EXPLANATION_FNG, (int)wx / 32 + (int)wy / 32 * 16, Layer);
				else if(m_SelectEntitiesImage == "Vanilla")
					m_pTooltip = Explain(EXPLANATION_VANILLA, (int)wx / 32 + (int)wy / 32 * 16, Layer);
			}
			else if(m_Brush.IsEmpty())
				m_pTooltip = "Use left mouse button to drag and create a brush. Hold shift to select multiple quads. Use ctrl+right mouse to select layer.";
			else
				m_pTooltip = "Use left mouse button to paint with the brush. Right button clears the brush.";

			if(UI()->CheckActiveItem(s_pEditorID))
			{
				CUIRect r;
				r.x = s_StartWx;
				r.y = s_StartWy;
				r.w = wx - s_StartWx;
				r.h = wy - s_StartWy;
				if(r.w < 0)
				{
					r.x += r.w;
					r.w = -r.w;
				}

				if(r.h < 0)
				{
					r.y += r.h;
					r.h = -r.h;
				}

				if(s_Operation == OP_BRUSH_DRAW)
				{
					if(!m_Brush.IsEmpty())
					{
						// draw with brush
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k % m_Brush.m_vpLayers.size();
							if(apEditLayers[k]->m_Type == m_Brush.m_vpLayers[BrushIndex]->m_Type)
							{
								if(apEditLayers[k]->m_Type == LAYERTYPE_TILES)
								{
									CLayerTiles *pLayer = (CLayerTiles *)apEditLayers[k];
									CLayerTiles *pBrushLayer = (CLayerTiles *)m_Brush.m_vpLayers[BrushIndex];

									if(pLayer->m_Tele <= pBrushLayer->m_Tele && pLayer->m_Speedup <= pBrushLayer->m_Speedup && pLayer->m_Front <= pBrushLayer->m_Front && pLayer->m_Game <= pBrushLayer->m_Game && pLayer->m_Switch <= pBrushLayer->m_Switch && pLayer->m_Tune <= pBrushLayer->m_Tune)
										pLayer->BrushDraw(pBrushLayer, wx, wy);
								}
								else
								{
									apEditLayers[k]->BrushDraw(m_Brush.m_vpLayers[BrushIndex], wx, wy);
								}
							}
						}
					}
				}
				else if(s_Operation == OP_BRUSH_GRAB)
				{
					if(!UI()->MouseButton(0))
					{
						if(Input()->ShiftIsPressed())
						{
							CLayerQuads *pQuadLayer = (CLayerQuads *)GetSelectedLayerType(0, LAYERTYPE_QUADS);
							if(pQuadLayer)
							{
								for(size_t i = 0; i < pQuadLayer->m_vQuads.size(); i++)
								{
									CQuad *pQuad = &pQuadLayer->m_vQuads[i];
									float px = fx2f(pQuad->m_aPoints[4].x);
									float py = fx2f(pQuad->m_aPoints[4].y);

									if(px > r.x && px < r.x + r.w && py > r.y && py < r.y + r.h)
										if(!IsQuadSelected(i))
											m_vSelectedQuads.push_back(i);
								}
							}
						}
						else
						{
							// grab brush
							char aBuf[256];
							str_format(aBuf, sizeof(aBuf), "grabbing %f %f %f %f", r.x, r.y, r.w, r.h);
							Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "editor", aBuf);

							// TODO: do all layers
							int Grabs = 0;
							for(size_t k = 0; k < NumEditLayers; k++)
								Grabs += apEditLayers[k]->BrushGrab(&m_Brush, r);
							if(Grabs == 0)
								m_Brush.Clear();

							for(auto &pLayer : m_Brush.m_vpLayers)
								pLayer->m_BrushRefCount = 1;

							m_vSelectedQuads.clear();
							m_SelectedPoints = 0;
						}
					}
					else
					{
						for(size_t k = 0; k < NumEditLayers; k++)
							apEditLayers[k]->BrushSelecting(r);
						UI()->MapScreen();
					}
				}
				else if(s_Operation == OP_BRUSH_PAINT)
				{
					if(!UI()->MouseButton(0))
					{
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(m_Brush.m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;
							CLayer *pBrush = m_Brush.IsEmpty() ? nullptr : m_Brush.m_vpLayers[BrushIndex];
							apEditLayers[k]->FillSelection(m_Brush.IsEmpty(), pBrush, r);
						}
					}
					else
					{
						for(size_t k = 0; k < NumEditLayers; k++)
							apEditLayers[k]->BrushSelecting(r);
						UI()->MapScreen();
					}
				}
			}
			else
			{
				if(UI()->MouseButton(1))
				{
					for(auto &pLayer : m_Brush.m_vpLayers)
					{
						if(pLayer->m_BrushRefCount == 1)
							delete pLayer;
					}
					m_Brush.Clear();
				}

				if(UI()->MouseButton(0) && s_Operation == OP_NONE && !m_QuadKnifeActive)
				{
					UI()->SetActiveItem(s_pEditorID);

					if(m_Brush.IsEmpty())
						s_Operation = OP_BRUSH_GRAB;
					else
					{
						s_Operation = OP_BRUSH_DRAW;
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(m_Brush.m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;
							if(apEditLayers[k]->m_Type == m_Brush.m_vpLayers[BrushIndex]->m_Type)
								apEditLayers[k]->BrushPlace(m_Brush.m_vpLayers[BrushIndex], wx, wy);
						}
					}

					CLayerTiles *pLayer = (CLayerTiles *)GetSelectedLayerType(0, LAYERTYPE_TILES);
					if(Input()->ShiftIsPressed() && pLayer)
						s_Operation = OP_BRUSH_PAINT;
				}

				if(!m_Brush.IsEmpty())
				{
					m_Brush.m_OffsetX = -(int)wx;
					m_Brush.m_OffsetY = -(int)wy;
					for(const auto &pLayer : m_Brush.m_vpLayers)
					{
						if(pLayer->m_Type == LAYERTYPE_TILES)
						{
							m_Brush.m_OffsetX = -(int)(wx / 32.0f) * 32;
							m_Brush.m_OffsetY = -(int)(wy / 32.0f) * 32;
							break;
						}
					}

					CLayerGroup *pGroup = GetSelectedGroup();
					if(!m_ShowPicker && pGroup)
					{
						m_Brush.m_OffsetX += pGroup->m_OffsetX;
						m_Brush.m_OffsetY += pGroup->m_OffsetY;
						m_Brush.m_ParallaxX = pGroup->m_ParallaxX;
						m_Brush.m_ParallaxY = pGroup->m_ParallaxY;
						m_Brush.m_ParallaxZoom = pGroup->m_ParallaxZoom;
						m_Brush.Render();
						float w, h;
						m_Brush.GetSize(&w, &h);

						IGraphics::CLineItem Array[4] = {
							IGraphics::CLineItem(0, 0, w, 0),
							IGraphics::CLineItem(w, 0, w, h),
							IGraphics::CLineItem(w, h, 0, h),
							IGraphics::CLineItem(0, h, 0, 0)};
						Graphics()->TextureClear();
						Graphics()->LinesBegin();
						Graphics()->LinesDraw(Array, 4);
						Graphics()->LinesEnd();
					}
				}
			}
		}

		// quad & sound editing
		{
			if(!m_ShowPicker && m_Brush.IsEmpty())
			{
				// fetch layers
				CLayerGroup *pGroup = GetSelectedGroup();
				if(pGroup)
					pGroup->MapScreen();

				for(size_t k = 0; k < NumEditLayers; k++)
				{
					if(apEditLayers[k]->m_Type == LAYERTYPE_QUADS)
					{
						CLayerQuads *pLayer = (CLayerQuads *)apEditLayers[k];

						if(m_ShowEnvelopePreview == SHOWENV_NONE)
							m_ShowEnvelopePreview = SHOWENV_ALL;

						if(m_QuadKnifeActive)
							DoQuadKnife(m_vSelectedQuads[m_SelectedQuadIndex]);
						else
						{
							Graphics()->TextureClear();
							Graphics()->QuadsBegin();
							for(size_t i = 0; i < pLayer->m_vQuads.size(); i++)
							{
								for(int v = 0; v < 4; v++)
									DoQuadPoint(&pLayer->m_vQuads[i], i, v);

								DoQuad(&pLayer->m_vQuads[i], i);
							}
							Graphics()->QuadsEnd();
						}
					}

					if(apEditLayers[k]->m_Type == LAYERTYPE_SOUNDS)
					{
						CLayerSounds *pLayer = (CLayerSounds *)apEditLayers[k];

						Graphics()->TextureClear();
						Graphics()->QuadsBegin();
						for(size_t i = 0; i < pLayer->m_vSources.size(); i++)
						{
							DoSoundSource(&pLayer->m_vSources[i], i);
						}
						Graphics()->QuadsEnd();
					}
				}

				UI()->MapScreen();
			}
		}

		// menu proof selection
		if(m_ProofBorders == PROOF_BORDER_MENU && !m_ShowPicker)
		{
			ResetMenuBackgroundPositions();
			for(int i = 0; i < (int)m_vMenuBackgroundPositions.size(); i++)
			{
				vec2 Pos = m_vMenuBackgroundPositions[i];
				Pos += vec2(m_WorldOffsetX, m_WorldOffsetY) - m_vMenuBackgroundPositions[m_CurrentMenuProofIndex];
				Pos.y -= 3.0f;

				vec2 MousePos(m_MouseWorldNoParaX, m_MouseWorldNoParaY);
				if(distance(Pos, MousePos) <= 20.0f)
				{
					UI()->SetHotItem(&m_vMenuBackgroundPositions[i]);

					if(i != m_CurrentMenuProofIndex && UI()->CheckActiveItem(&m_vMenuBackgroundPositions[i]))
					{
						if(!UI()->MouseButton(0))
						{
							m_CurrentMenuProofIndex = i;
							m_WorldOffsetX = m_vMenuBackgroundPositions[i].x;
							m_WorldOffsetY = m_vMenuBackgroundPositions[i].y;
							UI()->SetActiveItem(nullptr);
						}
					}
					else if(UI()->HotItem() == &m_vMenuBackgroundPositions[i])
					{
						char aTooltipPrefix[32] = "Switch proof position to";
						if(i == m_CurrentMenuProofIndex)
							str_copy(aTooltipPrefix, "Current proof position at");

						char aNumBuf[8];
						if(i < (TILE_TIME_CHECKPOINT_LAST - TILE_TIME_CHECKPOINT_FIRST))
							str_format(aNumBuf, sizeof(aNumBuf), "#%d", i + 1);
						else
							aNumBuf[0] = '\0';

						char aTooltipPositions[128];
						str_format(aTooltipPositions, sizeof(aTooltipPositions), "%s %s", m_vpMenuBackgroundPositionNames[i], aNumBuf);

						for(int k : m_vMenuBackgroundCollisions.at(i))
						{
							if(k == m_CurrentMenuProofIndex)
								str_copy(aTooltipPrefix, "Current proof position at");

							Pos = m_vMenuBackgroundPositions[k];
							Pos += vec2(m_WorldOffsetX, m_WorldOffsetY) - m_vMenuBackgroundPositions[m_CurrentMenuProofIndex];
							Pos.y -= 3.0f;

							MousePos = vec2(m_MouseWorldNoParaX, m_MouseWorldNoParaY);
							if(distance(Pos, MousePos) > 20.0f)
								continue;

							if(i < (TILE_TIME_CHECKPOINT_LAST - TILE_TIME_CHECKPOINT_FIRST))
								str_format(aNumBuf, sizeof(aNumBuf), "#%d", k + 1);
							else
								aNumBuf[0] = '\0';

							char aTooltipPositionsCopy[128];
							str_copy(aTooltipPositionsCopy, aTooltipPositions);
							str_format(aTooltipPositions, sizeof(aTooltipPositions), "%s, %s %s", aTooltipPositionsCopy, m_vpMenuBackgroundPositionNames[k], aNumBuf);
						}
						str_format(m_aMenuBackgroundTooltip, sizeof(m_aMenuBackgroundTooltip), "%s %s", aTooltipPrefix, aTooltipPositions);

						m_pTooltip = m_aMenuBackgroundTooltip;
						if(UI()->MouseButton(0))
							UI()->SetActiveItem(&m_vMenuBackgroundPositions[i]);
					}
					break;
				}
			}
		}

		// do panning
		if(UI()->CheckActiveItem(s_pEditorID))
		{
			if(s_Operation == OP_PAN_WORLD)
			{
				m_WorldOffsetX -= m_MouseDeltaX * m_MouseWScale;
				m_WorldOffsetY -= m_MouseDeltaY * m_MouseWScale;
			}
			else if(s_Operation == OP_PAN_EDITOR)
			{
				m_EditorOffsetX -= m_MouseDeltaX * m_MouseWScale;
				m_EditorOffsetY -= m_MouseDeltaY * m_MouseWScale;
			}

			// release mouse
			if(!UI()->MouseButton(0))
			{
				s_Operation = OP_NONE;
				UI()->SetActiveItem(nullptr);
			}
		}
		if(!Input()->ShiftIsPressed() && !Input()->ModifierIsPressed() && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0)
		{
			float PanSpeed = 64.0f;
			if(Input()->KeyPress(KEY_A))
				m_WorldOffsetX -= PanSpeed * m_MouseWScale;
			else if(Input()->KeyPress(KEY_D))
				m_WorldOffsetX += PanSpeed * m_MouseWScale;
			if(Input()->KeyPress(KEY_W))
				m_WorldOffsetY -= PanSpeed * m_MouseWScale;
			else if(Input()->KeyPress(KEY_S))
				m_WorldOffsetY += PanSpeed * m_MouseWScale;
		}
	}
	else if(UI()->CheckActiveItem(s_pEditorID))
	{
		// release mouse
		if(!UI()->MouseButton(0))
		{
			s_Operation = OP_NONE;
			UI()->SetActiveItem(nullptr);
		}
	}

	if(!m_ShowPicker && GetSelectedGroup() && GetSelectedGroup()->m_UseClipping)
	{
		CLayerGroup *pGameGroup = m_Map.m_pGameGroup;
		pGameGroup->MapScreen();

		Graphics()->TextureClear();
		Graphics()->LinesBegin();

		CUIRect r;
		r.x = GetSelectedGroup()->m_ClipX;
		r.y = GetSelectedGroup()->m_ClipY;
		r.w = GetSelectedGroup()->m_ClipW;
		r.h = GetSelectedGroup()->m_ClipH;

		IGraphics::CLineItem Array[4] = {
			IGraphics::CLineItem(r.x, r.y, r.x + r.w, r.y),
			IGraphics::CLineItem(r.x + r.w, r.y, r.x + r.w, r.y + r.h),
			IGraphics::CLineItem(r.x + r.w, r.y + r.h, r.x, r.y + r.h),
			IGraphics::CLineItem(r.x, r.y + r.h, r.x, r.y)};
		Graphics()->SetColor(1, 0, 0, 1);
		Graphics()->LinesDraw(Array, 4);

		Graphics()->LinesEnd();
	}

	// render screen sizes
	if(m_ProofBorders != PROOF_BORDER_OFF && !m_ShowPicker)
	{
		CLayerGroup *pGameGroup = m_Map.m_pGameGroup;
		pGameGroup->MapScreen();

		Graphics()->TextureClear();
		Graphics()->LinesBegin();

		// possible screen sizes (white border)
		float aLastPoints[4];
		float Start = 1.0f; //9.0f/16.0f;
		float End = 16.0f / 9.0f;
		const int NumSteps = 20;
		for(int i = 0; i <= NumSteps; i++)
		{
			float aPoints[4];
			float Aspect = Start + (End - Start) * (i / (float)NumSteps);

			float Zoom = (m_ProofBorders == PROOF_BORDER_MENU) ? 0.7f : 1.0f;
			RenderTools()->MapScreenToWorld(
				m_WorldOffsetX, m_WorldOffsetY,
				100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Aspect, Zoom, aPoints);

			if(i == 0)
			{
				IGraphics::CLineItem Array[2] = {
					IGraphics::CLineItem(aPoints[0], aPoints[1], aPoints[2], aPoints[1]),
					IGraphics::CLineItem(aPoints[0], aPoints[3], aPoints[2], aPoints[3])};
				Graphics()->LinesDraw(Array, 2);
			}

			if(i != 0)
			{
				IGraphics::CLineItem Array[4] = {
					IGraphics::CLineItem(aPoints[0], aPoints[1], aLastPoints[0], aLastPoints[1]),
					IGraphics::CLineItem(aPoints[2], aPoints[1], aLastPoints[2], aLastPoints[1]),
					IGraphics::CLineItem(aPoints[0], aPoints[3], aLastPoints[0], aLastPoints[3]),
					IGraphics::CLineItem(aPoints[2], aPoints[3], aLastPoints[2], aLastPoints[3])};
				Graphics()->LinesDraw(Array, 4);
			}

			if(i == NumSteps)
			{
				IGraphics::CLineItem Array[2] = {
					IGraphics::CLineItem(aPoints[0], aPoints[1], aPoints[0], aPoints[3]),
					IGraphics::CLineItem(aPoints[2], aPoints[1], aPoints[2], aPoints[3])};
				Graphics()->LinesDraw(Array, 2);
			}

			mem_copy(aLastPoints, aPoints, sizeof(aPoints));
		}

		// two screen sizes (green and red border)
		{
			Graphics()->SetColor(1, 0, 0, 1);
			for(int i = 0; i < 2; i++)
			{
				float aPoints[4];
				const float aAspects[] = {4.0f / 3.0f, 16.0f / 10.0f, 5.0f / 4.0f, 16.0f / 9.0f};
				float Aspect = aAspects[i];

				float Zoom = (m_ProofBorders == PROOF_BORDER_MENU) ? 0.7f : 1.0f;
				RenderTools()->MapScreenToWorld(
					m_WorldOffsetX, m_WorldOffsetY,
					100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Aspect, Zoom, aPoints);

				CUIRect r;
				r.x = aPoints[0];
				r.y = aPoints[1];
				r.w = aPoints[2] - aPoints[0];
				r.h = aPoints[3] - aPoints[1];

				IGraphics::CLineItem Array[4] = {
					IGraphics::CLineItem(r.x, r.y, r.x + r.w, r.y),
					IGraphics::CLineItem(r.x + r.w, r.y, r.x + r.w, r.y + r.h),
					IGraphics::CLineItem(r.x + r.w, r.y + r.h, r.x, r.y + r.h),
					IGraphics::CLineItem(r.x, r.y + r.h, r.x, r.y)};
				Graphics()->LinesDraw(Array, 4);
				Graphics()->SetColor(0, 1, 0, 1);
			}
		}
		Graphics()->LinesEnd();

		// tee position (blue circle) and other screen positions
		{
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(0, 0, 1, 0.3f);
			Graphics()->DrawCircle(m_WorldOffsetX, m_WorldOffsetY - 3.0f, 20.0f, 32);

			if(m_ProofBorders == PROOF_BORDER_MENU)
			{
				Graphics()->SetColor(0, 1, 0, 0.3f);

				std::set<int> indices;
				for(int i = 0; i < (int)m_vMenuBackgroundPositions.size(); i++)
					indices.insert(i);

				while(!indices.empty())
				{
					int i = *indices.begin();
					indices.erase(i);
					for(int k : m_vMenuBackgroundCollisions.at(i))
						indices.erase(k);

					vec2 Pos = m_vMenuBackgroundPositions[i];
					Pos += vec2(m_WorldOffsetX, m_WorldOffsetY) - m_vMenuBackgroundPositions[m_CurrentMenuProofIndex];

					if(distance(Pos, vec2(m_WorldOffsetX, m_WorldOffsetY)) < 0.001f)
						continue;

					Graphics()->DrawCircle(Pos.x, Pos.y - 3.0f, 20.0f, 32);
				}
			}

			Graphics()->QuadsEnd();
		}
	}

	if(!m_ShowPicker && m_ShowTileInfo != SHOW_TILE_OFF && m_ShowEnvelopePreview != SHOWENV_NONE && GetSelectedLayer(0) && GetSelectedLayer(0)->m_Type == LAYERTYPE_QUADS)
	{
		GetSelectedGroup()->MapScreen();

		CLayerQuads *pLayer = (CLayerQuads *)GetSelectedLayer(0);
		IGraphics::CTextureHandle Texture;
		if(pLayer->m_Image >= 0 && pLayer->m_Image < (int)m_Map.m_vpImages.size())
			Texture = m_Map.m_vpImages[pLayer->m_Image]->m_Texture;

		DoQuadEnvelopes(pLayer->m_vQuads, Texture);
		m_ShowEnvelopePreview = SHOWENV_NONE;
	}

	UI()->MapScreen();
}

float CEditor::ScaleFontSize(char *pText, int TextSize, float FontSize, int Width)
{
	while(TextRender()->TextWidth(FontSize, pText, -1, -1.0f) > Width)
	{
		if(FontSize > 6.0f)
		{
			FontSize--;
		}
		else
		{
			pText[str_length(pText) - 4] = '\0';
			str_append(pText, "…", TextSize);
		}
	}
	return FontSize;
}

int CEditor::DoProperties(CUIRect *pToolBox, CProperty *pProps, int *pIDs, int *pNewVal, ColorRGBA Color)
{
	int Change = -1;

	for(int i = 0; pProps[i].m_pName; i++)
	{
		CUIRect Slot;
		pToolBox->HSplitTop(13.0f, &Slot, pToolBox);
		CUIRect Label, Shifter;
		Slot.VSplitMid(&Label, &Shifter);
		Shifter.HMargin(1.0f, &Shifter);
		UI()->DoLabel(&Label, pProps[i].m_pName, 10.0f, TEXTALIGN_ML);

		if(pProps[i].m_Type == PROPTYPE_INT_STEP)
		{
			CUIRect Inc, Dec;
			char aBuf[64];

			Shifter.VSplitRight(10.0f, &Shifter, &Inc);
			Shifter.VSplitLeft(10.0f, &Dec, &Shifter);
			str_format(aBuf, sizeof(aBuf), "%d", pProps[i].m_Value);
			int NewValue = UiDoValueSelector((char *)&pIDs[i], &Shifter, "", pProps[i].m_Value, pProps[i].m_Min, pProps[i].m_Max, 1, 1.0f, "Use left mouse button to drag and change the value. Hold shift to be more precise. Rightclick to edit as text.", false, false, 0, &Color);
			if(NewValue != pProps[i].m_Value)
			{
				*pNewVal = NewValue;
				Change = i;
			}
			if(DoButton_ButtonDec((char *)&pIDs[i] + 1, nullptr, 0, &Dec, 0, "Decrease"))
			{
				*pNewVal = pProps[i].m_Value - 1;
				Change = i;
			}
			if(DoButton_ButtonInc(((char *)&pIDs[i]) + 2, nullptr, 0, &Inc, 0, "Increase"))
			{
				*pNewVal = pProps[i].m_Value + 1;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_BOOL)
		{
			CUIRect No, Yes;
			Shifter.VSplitMid(&No, &Yes);
			if(DoButton_ButtonDec(&pIDs[i], "No", !pProps[i].m_Value, &No, 0, ""))
			{
				*pNewVal = 0;
				Change = i;
			}
			if(DoButton_ButtonInc(((char *)&pIDs[i]) + 1, "Yes", pProps[i].m_Value, &Yes, 0, ""))
			{
				*pNewVal = 1;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_INT_SCROLL)
		{
			int NewValue = UiDoValueSelector(&pIDs[i], &Shifter, "", pProps[i].m_Value, pProps[i].m_Min, pProps[i].m_Max, 1, 1.0f, "Use left mouse button to drag and change the value. Hold shift to be more precise. Rightclick to edit as text.");
			if(NewValue != pProps[i].m_Value)
			{
				*pNewVal = NewValue;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_ANGLE_SCROLL)
		{
			CUIRect Inc, Dec;
			Shifter.VSplitRight(10.0f, &Shifter, &Inc);
			Shifter.VSplitLeft(10.0f, &Dec, &Shifter);
			const bool Shift = Input()->ShiftIsPressed();
			int Step = Shift ? 1 : 45;
			int Value = pProps[i].m_Value;

			int NewValue = UiDoValueSelector(&pIDs[i], &Shifter, "", Value, pProps[i].m_Min, pProps[i].m_Max, Shift ? 1 : 45, Shift ? 1.0f : 10.0f, "Use left mouse button to drag and change the value. Hold shift to be more precise. Rightclick to edit as text.", false, false, 0);
			if(DoButton_ButtonDec(&pIDs[i] + 1, nullptr, 0, &Dec, 0, "Decrease"))
			{
				NewValue = (std::ceil((pProps[i].m_Value / (float)Step)) - 1) * Step;
				if(NewValue < 0)
					NewValue += 360;
			}
			if(DoButton_ButtonInc(&pIDs[i] + 2, nullptr, 0, &Inc, 0, "Increase"))
				NewValue = (pProps[i].m_Value + Step) / Step * Step;

			if(NewValue != pProps[i].m_Value)
			{
				*pNewVal = NewValue % 360;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_COLOR)
		{
			const ColorRGBA ColorPick = ColorRGBA(
				((pProps[i].m_Value >> 24) & 0xff) / 255.0f,
				((pProps[i].m_Value >> 16) & 0xff) / 255.0f,
				((pProps[i].m_Value >> 8) & 0xff) / 255.0f,
				(pProps[i].m_Value & 0xff) / 255.0f);

			CUIRect ColorRect;
			Shifter.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f * UI()->ButtonColorMul(&pIDs[i])), IGraphics::CORNER_ALL, 5.0f);
			Shifter.Margin(1.0f, &ColorRect);
			ColorRect.Draw(ColorPick, IGraphics::CORNER_ALL, ColorRect.h / 2.0f);

			static CUI::SColorPickerPopupContext s_ColorPickerPopupContext;
			const int ButtonResult = DoButton_Editor_Common(&pIDs[i], nullptr, 0, &Shifter, 0, "Click to show the color picker. Shift+rightclick to copy color to clipboard. Shift+leftclick to paste color from clipboard.");
			if(Input()->ShiftIsPressed())
			{
				if(ButtonResult == 1)
				{
					const char *pClipboard = Input()->GetClipboardText();
					if(*pClipboard == '#' || *pClipboard == '$') // ignore leading # (web color format) and $ (console color format)
						++pClipboard;
					if(str_isallnum_hex(pClipboard))
					{
						std::optional<ColorRGBA> ParsedColor = color_parse<ColorRGBA>(pClipboard);
						if(ParsedColor)
						{
							*pNewVal = ParsedColor.value().PackAlphaLast();
							Change = i;
						}
					}
				}
				else if(ButtonResult == 2)
				{
					char aClipboard[9];
					str_format(aClipboard, sizeof(aClipboard), "%08X", ColorPick.PackAlphaLast());
					Input()->SetClipboardText(aClipboard);
				}
			}
			else if(ButtonResult > 0)
			{
				s_ColorPickerPopupContext.m_HsvaColor = color_cast<ColorHSVA>(ColorPick);
				s_ColorPickerPopupContext.m_Alpha = true;
				UI()->ShowPopupColorPicker(UI()->MouseX(), UI()->MouseY(), &s_ColorPickerPopupContext);
			}
			else if(UI()->IsPopupOpen(&s_ColorPickerPopupContext))
			{
				ColorRGBA c = color_cast<ColorRGBA>(s_ColorPickerPopupContext.m_HsvaColor);
				const int NewColor = ((int)(c.r * 255.0f) & 0xff) << 24 | ((int)(c.g * 255.0f) & 0xff) << 16 | ((int)(c.b * 255.0f) & 0xff) << 8 | ((int)(c.a * 255.0f) & 0xff);
				if(NewColor != pProps[i].m_Value)
				{
					*pNewVal = NewColor;
					Change = i;
				}
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_IMAGE)
		{
			char aBuf[64];
			if(pProps[i].m_Value < 0)
				str_copy(aBuf, "None");
			else
				str_copy(aBuf, m_Map.m_vpImages[pProps[i].m_Value]->m_aName);

			float FontSize = ScaleFontSize(aBuf, sizeof(aBuf), 10.0f, Shifter.w);
			if(DoButton_Ex(&pIDs[i], aBuf, 0, &Shifter, 0, nullptr, IGraphics::CORNER_ALL, FontSize))
				PopupSelectImageInvoke(pProps[i].m_Value, UI()->MouseX(), UI()->MouseY());

			int r = PopupSelectImageResult();
			if(r >= -1)
			{
				*pNewVal = r;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_SHIFT)
		{
			CUIRect Left, Right, Up, Down;
			Shifter.VSplitMid(&Left, &Up, 2.0f);
			Left.VSplitLeft(10.0f, &Left, &Shifter);
			Shifter.VSplitRight(10.0f, &Shifter, &Right);
			Shifter.Draw(ColorRGBA(1, 1, 1, 0.5f), 0, 0.0f);
			UI()->DoLabel(&Shifter, "X", 10.0f, TEXTALIGN_MC);
			Up.VSplitLeft(10.0f, &Up, &Shifter);
			Shifter.VSplitRight(10.0f, &Shifter, &Down);
			Shifter.Draw(ColorRGBA(1, 1, 1, 0.5f), 0, 0.0f);
			UI()->DoLabel(&Shifter, "Y", 10.0f, TEXTALIGN_MC);
			if(DoButton_ButtonDec(&pIDs[i], "-", 0, &Left, 0, "Left"))
			{
				*pNewVal = DIRECTION_LEFT;
				Change = i;
			}
			if(DoButton_ButtonInc(((char *)&pIDs[i]) + 3, "+", 0, &Right, 0, "Right"))
			{
				*pNewVal = DIRECTION_RIGHT;
				Change = i;
			}
			if(DoButton_ButtonDec(((char *)&pIDs[i]) + 1, "-", 0, &Up, 0, "Up"))
			{
				*pNewVal = DIRECTION_UP;
				Change = i;
			}
			if(DoButton_ButtonInc(((char *)&pIDs[i]) + 2, "+", 0, &Down, 0, "Down"))
			{
				*pNewVal = DIRECTION_DOWN;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_SOUND)
		{
			char aBuf[64];
			if(pProps[i].m_Value < 0)
				str_copy(aBuf, "None");
			else
				str_copy(aBuf, m_Map.m_vpSounds[pProps[i].m_Value]->m_aName);

			float FontSize = ScaleFontSize(aBuf, sizeof(aBuf), 10.0f, Shifter.w);
			if(DoButton_Ex(&pIDs[i], aBuf, 0, &Shifter, 0, nullptr, IGraphics::CORNER_ALL, FontSize))
				PopupSelectSoundInvoke(pProps[i].m_Value, UI()->MouseX(), UI()->MouseY());

			int r = PopupSelectSoundResult();
			if(r >= -1)
			{
				*pNewVal = r;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_AUTOMAPPER)
		{
			char aBuf[64];
			if(pProps[i].m_Value < 0 || pProps[i].m_Min < 0 || pProps[i].m_Min >= (int)m_Map.m_vpImages.size())
				str_copy(aBuf, "None");
			else
				str_copy(aBuf, m_Map.m_vpImages[pProps[i].m_Min]->m_AutoMapper.GetConfigName(pProps[i].m_Value));

			float FontSize = ScaleFontSize(aBuf, sizeof(aBuf), 10.0f, Shifter.w);
			if(DoButton_Ex(&pIDs[i], aBuf, 0, &Shifter, 0, nullptr, IGraphics::CORNER_ALL, FontSize))
				PopupSelectConfigAutoMapInvoke(pProps[i].m_Value, UI()->MouseX(), UI()->MouseY());

			int r = PopupSelectConfigAutoMapResult();
			if(r >= -1)
			{
				*pNewVal = r;
				Change = i;
			}
		}
		else if(pProps[i].m_Type == PROPTYPE_ENVELOPE)
		{
			CUIRect Inc, Dec;
			char aBuf[8];
			int CurValue = pProps[i].m_Value;

			Shifter.VSplitRight(10.0f, &Shifter, &Inc);
			Shifter.VSplitLeft(10.0f, &Dec, &Shifter);

			if(CurValue <= 0)
				str_copy(aBuf, "None:");
			else if(m_Map.m_vpEnvelopes[CurValue - 1]->m_aName[0])
			{
				str_format(aBuf, sizeof(aBuf), "%s:", m_Map.m_vpEnvelopes[CurValue - 1]->m_aName);
				if(!str_endswith(aBuf, ":"))
				{
					aBuf[sizeof(aBuf) - 2] = ':';
					aBuf[sizeof(aBuf) - 1] = '\0';
				}
			}
			else
				aBuf[0] = '\0';

			int NewVal = UiDoValueSelector((char *)&pIDs[i], &Shifter, aBuf, CurValue, 0, m_Map.m_vpEnvelopes.size(), 1, 1.0f, "Set Envelope", false, false, IGraphics::CORNER_NONE);
			if(NewVal != CurValue)
			{
				*pNewVal = NewVal;
				Change = i;
			}

			if(DoButton_ButtonDec((char *)&pIDs[i] + 1, nullptr, 0, &Dec, 0, "Previous Envelope"))
			{
				*pNewVal = pProps[i].m_Value - 1;
				Change = i;
			}
			if(DoButton_ButtonInc(((char *)&pIDs[i]) + 2, nullptr, 0, &Inc, 0, "Next Envelope"))
			{
				*pNewVal = pProps[i].m_Value + 1;
				Change = i;
			}
		}
	}

	return Change;
}

void CEditor::RenderLayers(CUIRect LayersBox)
{
	const float RowHeight = 12.0f;
	char aBuf[64];

	CUIRect UnscrolledLayersBox = LayersBox;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollbarWidth = 10.0f;
	ScrollParams.m_ScrollbarMargin = 3.0f;
	ScrollParams.m_ScrollUnit = RowHeight * 5.0f;
	s_ScrollRegion.Begin(&LayersBox, &ScrollOffset, &ScrollParams);
	LayersBox.y += ScrollOffset.y;

	enum
	{
		OP_NONE = 0,
		OP_CLICK,
		OP_LAYER_DRAG,
		OP_GROUP_DRAG
	};
	static int s_Operation = OP_NONE;
	static const void *s_pDraggedButton = 0;
	static float s_InitialMouseY = 0;
	static float s_InitialCutHeight = 0;
	int GroupAfterDraggedLayer = -1;
	int LayerAfterDraggedLayer = -1;
	bool DraggedPositionFound = false;
	bool MoveLayers = false;
	bool MoveGroup = false;
	bool StartDragLayer = false;
	bool StartDragGroup = false;
	std::vector<int> vButtonsPerGroup;

	vButtonsPerGroup.reserve(m_Map.m_vpGroups.size());
	for(CLayerGroup *pGroup : m_Map.m_vpGroups)
	{
		vButtonsPerGroup.push_back(pGroup->m_vpLayers.size() + 1);
	}

	if(!UI()->CheckActiveItem(s_pDraggedButton))
		s_Operation = OP_NONE;

	if(s_Operation == OP_LAYER_DRAG || s_Operation == OP_GROUP_DRAG)
	{
		float MinDraggableValue = UnscrolledLayersBox.y;
		float MaxDraggableValue = MinDraggableValue;
		for(int NumButtons : vButtonsPerGroup)
		{
			MaxDraggableValue += NumButtons * (RowHeight + 2.0f) + 5.0f;
		}
		MaxDraggableValue += ScrollOffset.y;

		if(s_Operation == OP_GROUP_DRAG)
		{
			MaxDraggableValue -= vButtonsPerGroup[m_SelectedGroup] * (RowHeight + 2.0f) + 5.0f;
		}
		else if(s_Operation == OP_LAYER_DRAG)
		{
			MinDraggableValue += RowHeight + 2.0f;
			MaxDraggableValue -= m_vSelectedLayers.size() * (RowHeight + 2.0f) + 5.0f;
		}

		UnscrolledLayersBox.HSplitTop(s_InitialCutHeight, nullptr, &UnscrolledLayersBox);
		UnscrolledLayersBox.y -= s_InitialMouseY - UI()->MouseY();

		UnscrolledLayersBox.y = clamp(UnscrolledLayersBox.y, MinDraggableValue, MaxDraggableValue);

		UnscrolledLayersBox.w = LayersBox.w;
	}

	static bool s_ScrollToSelectionNext = false;
	const bool ScrollToSelection = SelectLayerByTile() || s_ScrollToSelectionNext;
	s_ScrollToSelectionNext = false;

	// render layers
	for(int g = 0; g < (int)m_Map.m_vpGroups.size(); g++)
	{
		if(s_Operation == OP_LAYER_DRAG && g > 0 && !DraggedPositionFound && UI()->MouseY() < LayersBox.y + RowHeight / 2)
		{
			DraggedPositionFound = true;
			GroupAfterDraggedLayer = g;

			LayerAfterDraggedLayer = m_Map.m_vpGroups[g - 1]->m_vpLayers.size();

			CUIRect Slot;
			LayersBox.HSplitTop(m_vSelectedLayers.size() * (RowHeight + 2.0f), &Slot, &LayersBox);
			s_ScrollRegion.AddRect(Slot);
		}

		CUIRect Slot, VisibleToggle;
		if(s_Operation == OP_GROUP_DRAG)
		{
			if(g == m_SelectedGroup)
			{
				UnscrolledLayersBox.HSplitTop(RowHeight, &Slot, &UnscrolledLayersBox);
				UnscrolledLayersBox.HSplitTop(2.0f, nullptr, &UnscrolledLayersBox);
			}
			else if(!DraggedPositionFound && UI()->MouseY() < LayersBox.y + RowHeight * vButtonsPerGroup[g] / 2 + 3.0f)
			{
				DraggedPositionFound = true;
				GroupAfterDraggedLayer = g;

				CUIRect TmpSlot;
				if(m_Map.m_vpGroups[m_SelectedGroup]->m_Collapse)
					LayersBox.HSplitTop(RowHeight + 7.0f, &TmpSlot, &LayersBox);
				else
					LayersBox.HSplitTop(vButtonsPerGroup[m_SelectedGroup] * (RowHeight + 2.0f) + 5.0f, &TmpSlot, &LayersBox);
				s_ScrollRegion.AddRect(TmpSlot, false);
			}
		}
		if(s_Operation != OP_GROUP_DRAG || g != m_SelectedGroup)
		{
			LayersBox.HSplitTop(RowHeight, &Slot, &LayersBox);

			CUIRect TmpRect;
			LayersBox.HSplitTop(2.0f, &TmpRect, &LayersBox);
			s_ScrollRegion.AddRect(TmpRect);
		}

		if(s_ScrollRegion.AddRect(Slot))
		{
			Slot.VSplitLeft(15.0f, &VisibleToggle, &Slot);
			if(DoButton_FontIcon(&m_Map.m_vpGroups[g]->m_Visible, m_Map.m_vpGroups[g]->m_Visible ? FONT_ICON_EYE : FONT_ICON_EYE_SLASH, m_Map.m_vpGroups[g]->m_Collapse ? 1 : 0, &VisibleToggle, 0, "Toggle group visibility", IGraphics::CORNER_L, 8.0f))
				m_Map.m_vpGroups[g]->m_Visible = !m_Map.m_vpGroups[g]->m_Visible;

			str_format(aBuf, sizeof(aBuf), "#%d %s", g, m_Map.m_vpGroups[g]->m_aName);
			float FontSize = 10.0f;
			while(TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f) > Slot.w && FontSize >= 7.0f)
				FontSize--;

			bool Clicked;
			bool Abrupted;
			if(int Result = DoButton_DraggableEx(&m_Map.m_vpGroups[g], aBuf, g == m_SelectedGroup, &Slot, &Clicked, &Abrupted,
				   BUTTON_CONTEXT, m_Map.m_vpGroups[g]->m_Collapse ? "Select group. Shift click to select all layers. Double click to expand." : "Select group. Shift click to select all layers. Double click to collapse.", IGraphics::CORNER_R, FontSize))
			{
				if(s_Operation == OP_NONE)
				{
					s_InitialMouseY = UI()->MouseY();
					s_InitialCutHeight = s_InitialMouseY - UnscrolledLayersBox.y;
					s_Operation = OP_CLICK;

					if(g != m_SelectedGroup)
						SelectLayer(0, g);
				}

				if(Abrupted)
					s_Operation = OP_NONE;

				if(s_Operation == OP_CLICK)
				{
					if(absolute(UI()->MouseY() - s_InitialMouseY) > 5)
						StartDragGroup = true;

					s_pDraggedButton = &m_Map.m_vpGroups[g];
				}

				if(s_Operation == OP_CLICK && Clicked)
				{
					if(g != m_SelectedGroup)
						SelectLayer(0, g);

					if(Input()->ShiftIsPressed() && m_SelectedGroup == g)
					{
						m_vSelectedLayers.clear();
						for(size_t i = 0; i < m_Map.m_vpGroups[g]->m_vpLayers.size(); i++)
						{
							AddSelectedLayer(i);
						}
					}

					if(Result == 2)
					{
						static SPopupMenuId s_PopupGroupId;
						UI()->DoPopupMenu(&s_PopupGroupId, UI()->MouseX(), UI()->MouseY(), 145, 256, this, PopupGroup);
					}

					if(!m_Map.m_vpGroups[g]->m_vpLayers.empty() && Input()->MouseDoubleClick())
						m_Map.m_vpGroups[g]->m_Collapse ^= 1;

					s_Operation = OP_NONE;
				}

				if(s_Operation == OP_GROUP_DRAG && Clicked)
					MoveGroup = true;
			}
			else if(s_pDraggedButton == &m_Map.m_vpGroups[g])
			{
				s_Operation = OP_NONE;
			}
		}

		for(int i = 0; i < (int)m_Map.m_vpGroups[g]->m_vpLayers.size(); i++)
		{
			if(m_Map.m_vpGroups[g]->m_Collapse)
				continue;

			bool IsLayerSelected = false;
			if(m_SelectedGroup == g)
			{
				for(const auto &Selected : m_vSelectedLayers)
				{
					if(Selected == i)
					{
						IsLayerSelected = true;
						break;
					}
				}
			}

			if(s_Operation == OP_GROUP_DRAG && g == m_SelectedGroup)
			{
				UnscrolledLayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &UnscrolledLayersBox);
			}
			else if(s_Operation == OP_LAYER_DRAG)
			{
				if(IsLayerSelected)
				{
					UnscrolledLayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &UnscrolledLayersBox);
				}
				else
				{
					if(!DraggedPositionFound && UI()->MouseY() < LayersBox.y + RowHeight / 2)
					{
						DraggedPositionFound = true;
						GroupAfterDraggedLayer = g + 1;
						LayerAfterDraggedLayer = i;
						for(size_t j = 0; j < m_vSelectedLayers.size(); j++)
						{
							LayersBox.HSplitTop(RowHeight + 2.0f, nullptr, &LayersBox);
							s_ScrollRegion.AddRect(Slot);
						}
					}
					LayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &LayersBox);
					if(!s_ScrollRegion.AddRect(Slot, ScrollToSelection && IsLayerSelected))
						continue;
				}
			}
			else
			{
				LayersBox.HSplitTop(RowHeight + 2.0f, &Slot, &LayersBox);
				if(!s_ScrollRegion.AddRect(Slot, ScrollToSelection && IsLayerSelected))
					continue;
			}

			Slot.HSplitTop(RowHeight, &Slot, nullptr);

			CUIRect Button;
			Slot.VSplitLeft(12.0f, nullptr, &Slot);
			Slot.VSplitLeft(15.0f, &VisibleToggle, &Button);

			if(DoButton_FontIcon(&m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Visible, m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Visible ? FONT_ICON_EYE : FONT_ICON_EYE_SLASH, 0, &VisibleToggle, 0, "Toggle layer visibility", IGraphics::CORNER_L, 8.0f))
				m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Visible = !m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Visible;

			if(m_Map.m_vpGroups[g]->m_vpLayers[i]->m_aName[0])
				str_copy(aBuf, m_Map.m_vpGroups[g]->m_vpLayers[i]->m_aName);
			else
			{
				if(m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Type == LAYERTYPE_TILES)
				{
					CLayerTiles *pTiles = (CLayerTiles *)m_Map.m_vpGroups[g]->m_vpLayers[i];
					str_copy(aBuf, pTiles->m_Image >= 0 ? m_Map.m_vpImages[pTiles->m_Image]->m_aName : "Tiles");
				}
				else if(m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Type == LAYERTYPE_QUADS)
				{
					CLayerQuads *pQuads = (CLayerQuads *)m_Map.m_vpGroups[g]->m_vpLayers[i];
					str_copy(aBuf, pQuads->m_Image >= 0 ? m_Map.m_vpImages[pQuads->m_Image]->m_aName : "Quads");
				}
				else if(m_Map.m_vpGroups[g]->m_vpLayers[i]->m_Type == LAYERTYPE_SOUNDS)
				{
					CLayerSounds *pSounds = (CLayerSounds *)m_Map.m_vpGroups[g]->m_vpLayers[i];
					str_copy(aBuf, pSounds->m_Sound >= 0 ? m_Map.m_vpSounds[pSounds->m_Sound]->m_aName : "Sounds");
				}
				if(str_length(aBuf) > 11)
					str_format(aBuf, sizeof(aBuf), "%.8s…", aBuf);
			}

			float FontSize = 10.0f;
			while(TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f) > Button.w && FontSize >= 7.0f)
				FontSize--;

			int Checked = IsLayerSelected ? 1 : 0;
			if(m_Map.m_vpGroups[g]->m_vpLayers[i]->IsEntitiesLayer())
			{
				Checked += 6;
			}

			bool Clicked;
			bool Abrupted;
			if(int Result = DoButton_DraggableEx(m_Map.m_vpGroups[g]->m_vpLayers[i], aBuf, Checked, &Button, &Clicked, &Abrupted,
				   BUTTON_CONTEXT, "Select layer. Shift click to select multiple.", IGraphics::CORNER_R, FontSize))
			{
				if(s_Operation == OP_NONE)
				{
					s_InitialMouseY = UI()->MouseY();
					s_InitialCutHeight = s_InitialMouseY - UnscrolledLayersBox.y;
					s_Operation = OP_CLICK;

					if(!Input()->ShiftIsPressed() && !IsLayerSelected)
					{
						SelectLayer(i, g);
					}
				}

				if(Abrupted)
					s_Operation = OP_NONE;

				if(s_Operation == OP_CLICK)
				{
					if(absolute(UI()->MouseY() - s_InitialMouseY) > 5)
					{
						bool EntitiesLayerSelected = false;
						for(int k : m_vSelectedLayers)
						{
							if(m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers[k]->IsEntitiesLayer())
								EntitiesLayerSelected = true;
						}

						if(!EntitiesLayerSelected)
							StartDragLayer = true;
					}

					s_pDraggedButton = m_Map.m_vpGroups[g]->m_vpLayers[i];
				}

				if(s_Operation == OP_CLICK && Clicked)
				{
					static SLayerPopupContext s_LayerPopupContext = {};
					s_LayerPopupContext.m_pEditor = this;
					if(Result == 1)
					{
						if(Input()->ShiftIsPressed() && m_SelectedGroup == g)
						{
							auto Position = std::find(m_vSelectedLayers.begin(), m_vSelectedLayers.end(), i);
							if(Position != m_vSelectedLayers.end())
								m_vSelectedLayers.erase(Position);
							else
								AddSelectedLayer(i);
						}
						else if(!Input()->ShiftIsPressed())
						{
							SelectLayer(i, g);
						}
					}
					else if(Result == 2)
					{
						if(!IsLayerSelected)
						{
							SelectLayer(i, g);
						}

						if(m_vSelectedLayers.size() > 1)
						{
							bool AllTile = true;
							for(size_t j = 0; AllTile && j < m_vSelectedLayers.size(); j++)
							{
								if(m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers[m_vSelectedLayers[j]]->m_Type == LAYERTYPE_TILES)
									s_LayerPopupContext.m_vpLayers.push_back((CLayerTiles *)m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers[m_vSelectedLayers[j]]);
								else
									AllTile = false;
							}

							// Don't allow editing if all selected layers are tile layers
							if(!AllTile)
								s_LayerPopupContext.m_vpLayers.clear();
						}
						else
							s_LayerPopupContext.m_vpLayers.clear();

						UI()->DoPopupMenu(&s_LayerPopupContext, UI()->MouseX(), UI()->MouseY(), 120, 270, &s_LayerPopupContext, PopupLayer);
					}

					s_Operation = OP_NONE;
				}

				if(s_Operation == OP_LAYER_DRAG && Clicked)
				{
					MoveLayers = true;
				}
			}
			else if(s_pDraggedButton == m_Map.m_vpGroups[g]->m_vpLayers[i])
			{
				s_Operation = OP_NONE;
			}
		}

		if(s_Operation != OP_GROUP_DRAG || g != m_SelectedGroup)
		{
			LayersBox.HSplitTop(5.0f, &Slot, &LayersBox);
			s_ScrollRegion.AddRect(Slot);
		}
	}

	if(!DraggedPositionFound && s_Operation == OP_LAYER_DRAG)
	{
		GroupAfterDraggedLayer = m_Map.m_vpGroups.size();
		LayerAfterDraggedLayer = m_Map.m_vpGroups[GroupAfterDraggedLayer - 1]->m_vpLayers.size();

		CUIRect TmpSlot;
		LayersBox.HSplitTop(m_vSelectedLayers.size() * (RowHeight + 2.0f), &TmpSlot, &LayersBox);
		s_ScrollRegion.AddRect(TmpSlot);
	}

	if(!DraggedPositionFound && s_Operation == OP_GROUP_DRAG)
	{
		GroupAfterDraggedLayer = m_Map.m_vpGroups.size();

		CUIRect TmpSlot;
		if(m_Map.m_vpGroups[m_SelectedGroup]->m_Collapse)
			LayersBox.HSplitTop(RowHeight + 7.0f, &TmpSlot, &LayersBox);
		else
			LayersBox.HSplitTop(vButtonsPerGroup[m_SelectedGroup] * (RowHeight + 2.0f) + 5.0f, &TmpSlot, &LayersBox);
		s_ScrollRegion.AddRect(TmpSlot, false);
	}

	if(MoveLayers && 1 <= GroupAfterDraggedLayer && GroupAfterDraggedLayer <= (int)m_Map.m_vpGroups.size())
	{
		std::vector<CLayer *> &vpNewGroupLayers = m_Map.m_vpGroups[GroupAfterDraggedLayer - 1]->m_vpLayers;
		if(0 <= LayerAfterDraggedLayer && LayerAfterDraggedLayer <= (int)vpNewGroupLayers.size())
		{
			std::vector<CLayer *> vpSelectedLayers;
			std::vector<CLayer *> &vpSelectedGroupLayers = m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers;
			CLayer *pNextLayer = nullptr;
			if(LayerAfterDraggedLayer < (int)vpNewGroupLayers.size())
				pNextLayer = vpNewGroupLayers[LayerAfterDraggedLayer];

			std::sort(m_vSelectedLayers.begin(), m_vSelectedLayers.end(), std::greater<>());
			for(int k : m_vSelectedLayers)
			{
				vpSelectedLayers.insert(vpSelectedLayers.begin(), vpSelectedGroupLayers[k]);
			}
			for(int k : m_vSelectedLayers)
			{
				vpSelectedGroupLayers.erase(vpSelectedGroupLayers.begin() + k);
			}

			auto InsertPosition = std::find(vpNewGroupLayers.begin(), vpNewGroupLayers.end(), pNextLayer);
			vpNewGroupLayers.insert(InsertPosition, vpSelectedLayers.begin(), vpSelectedLayers.end());

			m_SelectedGroup = GroupAfterDraggedLayer - 1;
			m_vSelectedLayers.clear();
			m_vSelectedQuads.clear();
			m_Map.OnModify();
		}
	}

	if(MoveGroup && 0 <= GroupAfterDraggedLayer && GroupAfterDraggedLayer <= (int)m_Map.m_vpGroups.size())
	{
		CLayerGroup *pSelectedGroup = m_Map.m_vpGroups[m_SelectedGroup];
		CLayerGroup *pNextGroup = nullptr;
		if(GroupAfterDraggedLayer < (int)m_Map.m_vpGroups.size())
			pNextGroup = m_Map.m_vpGroups[GroupAfterDraggedLayer];

		m_Map.m_vpGroups.erase(m_Map.m_vpGroups.begin() + m_SelectedGroup);

		auto InsertPosition = std::find(m_Map.m_vpGroups.begin(), m_Map.m_vpGroups.end(), pNextGroup);
		m_Map.m_vpGroups.insert(InsertPosition, pSelectedGroup);

		m_SelectedGroup = InsertPosition - m_Map.m_vpGroups.begin();
		m_Map.OnModify();
	}

	if(MoveLayers || MoveGroup)
		s_Operation = OP_NONE;
	if(StartDragLayer)
		s_Operation = OP_LAYER_DRAG;
	if(StartDragGroup)
		s_Operation = OP_GROUP_DRAG;

	if(s_Operation == OP_LAYER_DRAG || s_Operation == OP_GROUP_DRAG)
	{
		s_ScrollRegion.DoEdgeScrolling();
		UI()->SetActiveItem(s_pDraggedButton);
	}

	if(Input()->KeyPress(KEY_DOWN) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && s_Operation == OP_NONE)
	{
		if(Input()->ShiftIsPressed())
		{
			if(m_vSelectedLayers[m_vSelectedLayers.size() - 1] < (int)m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers.size() - 1)
				AddSelectedLayer(m_vSelectedLayers[m_vSelectedLayers.size() - 1] + 1);
		}
		else
		{
			int CurrentLayer = 0;
			for(const auto &Selected : m_vSelectedLayers)
				CurrentLayer = maximum(Selected, CurrentLayer);
			SelectLayer(CurrentLayer);

			if(m_vSelectedLayers[0] < (int)m_Map.m_vpGroups[m_SelectedGroup]->m_vpLayers.size() - 1)
			{
				SelectLayer(m_vSelectedLayers[0] + 1);
			}
			else
			{
				for(size_t Group = m_SelectedGroup + 1; Group < m_Map.m_vpGroups.size(); Group++)
				{
					if(!m_Map.m_vpGroups[Group]->m_vpLayers.empty())
					{
						SelectLayer(0, Group);
						break;
					}
				}
			}
		}
		s_ScrollToSelectionNext = true;
	}
	if(Input()->KeyPress(KEY_UP) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && s_Operation == OP_NONE)
	{
		if(Input()->ShiftIsPressed())
		{
			if(m_vSelectedLayers[m_vSelectedLayers.size() - 1] > 0)
				AddSelectedLayer(m_vSelectedLayers[m_vSelectedLayers.size() - 1] - 1);
		}
		else
		{
			int CurrentLayer = std::numeric_limits<int>::max();
			for(const auto &Selected : m_vSelectedLayers)
				CurrentLayer = minimum(Selected, CurrentLayer);
			SelectLayer(CurrentLayer);

			if(m_vSelectedLayers[0] > 0)
			{
				SelectLayer(m_vSelectedLayers[0] - 1);
			}
			else
			{
				for(int Group = m_SelectedGroup - 1; Group >= 0; Group--)
				{
					if(!m_Map.m_vpGroups[Group]->m_vpLayers.empty())
					{
						SelectLayer(m_Map.m_vpGroups[Group]->m_vpLayers.size() - 1, Group);
						break;
					}
				}
			}
		}
		s_ScrollToSelectionNext = true;
	}

	CUIRect AddGroupButton;
	LayersBox.HSplitTop(RowHeight + 1.0f, &AddGroupButton, &LayersBox);
	if(s_ScrollRegion.AddRect(AddGroupButton))
	{
		AddGroupButton.HSplitTop(RowHeight, &AddGroupButton, 0);
		static int s_AddGroupButton = 0;
		if(DoButton_Editor(&s_AddGroupButton, "Add group", 0, &AddGroupButton, IGraphics::CORNER_R, "Adds a new group"))
		{
			m_Map.NewGroup();
			m_SelectedGroup = m_Map.m_vpGroups.size() - 1;
		}
	}

	s_ScrollRegion.End();
}

bool CEditor::SelectLayerByTile()
{
	// ctrl+rightclick a map index to select the layer that has a tile there
	static bool s_CtrlClick = false;
	static int s_Selected = 0;
	int MatchedGroup = -1;
	int MatchedLayer = -1;
	int Matches = 0;
	bool IsFound = false;
	if(UI()->MouseButton(1) && Input()->ModifierIsPressed())
	{
		if(s_CtrlClick)
			return false;
		s_CtrlClick = true;
		for(size_t g = 0; g < m_Map.m_vpGroups.size(); g++)
		{
			for(size_t l = 0; l < m_Map.m_vpGroups[g]->m_vpLayers.size(); l++)
			{
				if(IsFound)
					continue;
				if(m_Map.m_vpGroups[g]->m_vpLayers[l]->m_Type != LAYERTYPE_TILES)
					continue;

				CLayerTiles *pTiles = (CLayerTiles *)m_Map.m_vpGroups[g]->m_vpLayers[l];
				int x = (int)UI()->MouseWorldX() / 32 + m_Map.m_vpGroups[g]->m_OffsetX;
				int y = (int)UI()->MouseWorldY() / 32 + m_Map.m_vpGroups[g]->m_OffsetY;
				if(x < 0 || x >= pTiles->m_Width)
					continue;
				if(y < 0 || y >= pTiles->m_Height)
					continue;
				CTile Tile = pTiles->GetTile(x, y);
				if(Tile.m_Index)
				{
					if(MatchedGroup == -1)
					{
						MatchedGroup = g;
						MatchedLayer = l;
					}
					if(++Matches > s_Selected)
					{
						s_Selected++;
						MatchedGroup = g;
						MatchedLayer = l;
						IsFound = true;
					}
				}
			}
		}
		if(MatchedGroup != -1 && MatchedLayer != -1)
		{
			if(!IsFound)
				s_Selected = 1;
			SelectLayer(MatchedLayer, MatchedGroup);
			return true;
		}
	}
	else
		s_CtrlClick = false;
	return false;
}

bool CEditor::ReplaceImage(const char *pFileName, int StorageType, bool CheckDuplicate)
{
	// check if we have that image already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFileName, aBuf, sizeof(aBuf));
	if(CheckDuplicate)
	{
		for(const auto &pImage : m_Map.m_vpImages)
		{
			if(!str_comp(pImage->m_aName, aBuf))
			{
				ShowFileDialogError("Image named '%s' was already added.", pImage->m_aName);
				return false;
			}
		}
	}

	CEditorImage ImgInfo(this);
	if(!Graphics()->LoadPNG(&ImgInfo, pFileName, StorageType))
	{
		ShowFileDialogError("Failed to load image from file '%s'.", pFileName);
		return false;
	}

	CEditorImage *pImg = m_Map.m_vpImages[m_SelectedImage];
	Graphics()->UnloadTexture(&(pImg->m_Texture));
	free(pImg->m_pData);
	pImg->m_pData = nullptr;
	*pImg = ImgInfo;
	str_copy(pImg->m_aName, aBuf);
	pImg->m_External = IsVanillaImage(pImg->m_aName);

	if(!pImg->m_External && g_Config.m_ClEditorDilate == 1 && pImg->m_Format == CImageInfo::FORMAT_RGBA)
	{
		int ColorChannelCount = 0;
		if(ImgInfo.m_Format == CImageInfo::FORMAT_RGBA)
			ColorChannelCount = 4;

		DilateImage((unsigned char *)ImgInfo.m_pData, ImgInfo.m_Width, ImgInfo.m_Height, ColorChannelCount);
	}

	pImg->m_AutoMapper.Load(pImg->m_aName);
	int TextureLoadFlag = Graphics()->HasTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
	if(ImgInfo.m_Width % 16 != 0 || ImgInfo.m_Height % 16 != 0)
		TextureLoadFlag = 0;
	pImg->m_Texture = Graphics()->LoadTextureRaw(ImgInfo.m_Width, ImgInfo.m_Height, ImgInfo.m_Format, ImgInfo.m_pData, CImageInfo::FORMAT_AUTO, TextureLoadFlag, pFileName);
	ImgInfo.m_pData = nullptr;
	SortImages();
	for(size_t i = 0; i < m_Map.m_vpImages.size(); ++i)
	{
		if(!str_comp(m_Map.m_vpImages[i]->m_aName, pImg->m_aName))
			m_SelectedImage = i;
	}
	m_Dialog = DIALOG_NONE;
	return true;
}

bool CEditor::ReplaceImageCallback(const char *pFileName, int StorageType, void *pUser)
{
	return static_cast<CEditor *>(pUser)->ReplaceImage(pFileName, StorageType, true);
}

bool CEditor::AddImage(const char *pFileName, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;

	// check if we have that image already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFileName, aBuf, sizeof(aBuf));
	for(const auto &pImage : pEditor->m_Map.m_vpImages)
	{
		if(!str_comp(pImage->m_aName, aBuf))
		{
			pEditor->ShowFileDialogError("Image named '%s' was already added.", pImage->m_aName);
			return false;
		}
	}

	if(pEditor->m_Map.m_vpImages.size() >= 64) // hard limit for teeworlds
	{
		pEditor->m_PopupEventType = POPEVENT_IMAGE_MAX;
		pEditor->m_PopupEventActivated = true;
		return false;
	}

	CEditorImage ImgInfo(pEditor);
	if(!pEditor->Graphics()->LoadPNG(&ImgInfo, pFileName, StorageType))
	{
		pEditor->ShowFileDialogError("Failed to load image from file '%s'.", pFileName);
		return false;
	}

	CEditorImage *pImg = new CEditorImage(pEditor);
	*pImg = ImgInfo;
	pImg->m_External = IsVanillaImage(aBuf);

	if(!pImg->m_External && g_Config.m_ClEditorDilate == 1 && pImg->m_Format == CImageInfo::FORMAT_RGBA)
	{
		int ColorChannelCount = 0;
		if(ImgInfo.m_Format == CImageInfo::FORMAT_RGBA)
			ColorChannelCount = 4;

		DilateImage((unsigned char *)ImgInfo.m_pData, ImgInfo.m_Width, ImgInfo.m_Height, ColorChannelCount);
	}

	int TextureLoadFlag = pEditor->Graphics()->HasTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
	if(ImgInfo.m_Width % 16 != 0 || ImgInfo.m_Height % 16 != 0)
		TextureLoadFlag = 0;
	pImg->m_Texture = pEditor->Graphics()->LoadTextureRaw(ImgInfo.m_Width, ImgInfo.m_Height, ImgInfo.m_Format, ImgInfo.m_pData, CImageInfo::FORMAT_AUTO, TextureLoadFlag, pFileName);
	ImgInfo.m_pData = nullptr;
	str_copy(pImg->m_aName, aBuf);
	pImg->m_AutoMapper.Load(pImg->m_aName);
	pEditor->m_Map.m_vpImages.push_back(pImg);
	pEditor->SortImages();
	if(pEditor->m_SelectedImage >= 0 && (size_t)pEditor->m_SelectedImage < pEditor->m_Map.m_vpImages.size())
	{
		for(int i = 0; i <= pEditor->m_SelectedImage; ++i)
			if(!str_comp(pEditor->m_Map.m_vpImages[i]->m_aName, aBuf))
			{
				pEditor->m_SelectedImage++;
				break;
			}
	}
	pEditor->m_Dialog = DIALOG_NONE;
	return true;
}

bool CEditor::AddSound(const char *pFileName, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;

	// check if we have that sound already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFileName, aBuf, sizeof(aBuf));
	for(const auto &pSound : pEditor->m_Map.m_vpSounds)
	{
		if(!str_comp(pSound->m_aName, aBuf))
		{
			pEditor->ShowFileDialogError("Sound named '%s' was already added.", pSound->m_aName);
			return false;
		}
	}

	// load external
	void *pData;
	unsigned DataSize;
	if(!pEditor->Storage()->ReadFile(pFileName, StorageType, &pData, &DataSize))
	{
		pEditor->ShowFileDialogError("Failed to open sound file '%s'.", pFileName);
		return false;
	}

	// load sound
	const int SoundId = pEditor->Sound()->LoadOpusFromMem(pData, DataSize, true);
	if(SoundId == -1)
	{
		free(pData);
		pEditor->ShowFileDialogError("Failed to load sound from file '%s'.", pFileName);
		return false;
	}

	// add sound
	CEditorSound *pSound = new CEditorSound(pEditor);
	pSound->m_SoundID = SoundId;
	pSound->m_DataSize = DataSize;
	pSound->m_pData = pData;
	str_copy(pSound->m_aName, aBuf);
	pEditor->m_Map.m_vpSounds.push_back(pSound);

	if(pEditor->m_SelectedSound >= 0 && (size_t)pEditor->m_SelectedSound < pEditor->m_Map.m_vpSounds.size())
	{
		for(int i = 0; i <= pEditor->m_SelectedSound; ++i)
			if(!str_comp(pEditor->m_Map.m_vpSounds[i]->m_aName, aBuf))
			{
				pEditor->m_SelectedSound++;
				break;
			}
	}

	pEditor->m_Dialog = DIALOG_NONE;
	return true;
}

bool CEditor::ReplaceSound(const char *pFileName, int StorageType, bool CheckDuplicate)
{
	// check if we have that sound already
	char aBuf[128];
	IStorage::StripPathAndExtension(pFileName, aBuf, sizeof(aBuf));
	if(CheckDuplicate)
	{
		for(const auto &pSound : m_Map.m_vpSounds)
		{
			if(!str_comp(pSound->m_aName, aBuf))
			{
				ShowFileDialogError("Sound named '%s' was already added.", pSound->m_aName);
				return false;
			}
		}
	}

	// load external
	void *pData;
	unsigned DataSize;
	if(!Storage()->ReadFile(pFileName, StorageType, &pData, &DataSize))
	{
		ShowFileDialogError("Failed to open sound file '%s'.", pFileName);
		return false;
	}

	// load sound
	const int SoundId = Sound()->LoadOpusFromMem(pData, DataSize, true);
	if(SoundId == -1)
	{
		free(pData);
		ShowFileDialogError("Failed to load sound from file '%s'.", pFileName);
		return false;
	}

	CEditorSound *pSound = m_Map.m_vpSounds[m_SelectedSound];

	// unload sample
	Sound()->UnloadSample(pSound->m_SoundID);
	free(pSound->m_pData);

	// replace sound
	str_copy(pSound->m_aName, aBuf);
	pSound->m_SoundID = SoundId;
	pSound->m_pData = pData;
	pSound->m_DataSize = DataSize;

	m_Dialog = DIALOG_NONE;
	return true;
}

bool CEditor::ReplaceSoundCallback(const char *pFileName, int StorageType, void *pUser)
{
	return static_cast<CEditor *>(pUser)->ReplaceSound(pFileName, StorageType, true);
}

void CEditor::SelectGameLayer()
{
	for(size_t g = 0; g < m_Map.m_vpGroups.size(); g++)
	{
		for(size_t i = 0; i < m_Map.m_vpGroups[g]->m_vpLayers.size(); i++)
		{
			if(m_Map.m_vpGroups[g]->m_vpLayers[i] == m_Map.m_pGameLayer)
			{
				SelectLayer(i, g);
				return;
			}
		}
	}
}

static bool ImageNameLess(const CEditorImage *const &a, const CEditorImage *const &b)
{
	return str_comp(a->m_aName, b->m_aName) < 0;
}

static int *gs_pSortedIndex = nullptr;
static void ModifySortedIndex(int *pIndex)
{
	if(*pIndex >= 0)
		*pIndex = gs_pSortedIndex[*pIndex];
}

void CEditor::SortImages()
{
	if(!std::is_sorted(m_Map.m_vpImages.begin(), m_Map.m_vpImages.end(), ImageNameLess))
	{
		std::vector<CEditorImage *> vpTemp = m_Map.m_vpImages;
		gs_pSortedIndex = new int[vpTemp.size()];

		std::sort(m_Map.m_vpImages.begin(), m_Map.m_vpImages.end(), ImageNameLess);
		for(size_t OldIndex = 0; OldIndex < vpTemp.size(); OldIndex++)
		{
			for(size_t NewIndex = 0; NewIndex < m_Map.m_vpImages.size(); NewIndex++)
			{
				if(vpTemp[OldIndex] == m_Map.m_vpImages[NewIndex])
				{
					gs_pSortedIndex[OldIndex] = NewIndex;
					break;
				}
			}
		}
		m_Map.ModifyImageIndex(ModifySortedIndex);

		delete[] gs_pSortedIndex;
		gs_pSortedIndex = nullptr;
	}
}

void CEditor::RenderImagesList(CUIRect ToolBox)
{
	const float RowHeight = 12.0f;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollbarWidth = 10.0f;
	ScrollParams.m_ScrollbarMargin = 3.0f;
	ScrollParams.m_ScrollUnit = RowHeight * 5;
	s_ScrollRegion.Begin(&ToolBox, &ScrollOffset, &ScrollParams);
	ToolBox.y += ScrollOffset.y;

	bool ScrollToSelection = false;
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && !m_Map.m_vpImages.empty())
	{
		if(Input()->KeyPress(KEY_DOWN))
		{
			int OldImage = m_SelectedImage;
			m_SelectedImage = clamp(m_SelectedImage, 0, (int)m_Map.m_vpImages.size() - 1);
			for(size_t i = m_SelectedImage + 1; i < m_Map.m_vpImages.size(); i++)
			{
				if(m_Map.m_vpImages[i]->m_External == m_Map.m_vpImages[m_SelectedImage]->m_External)
				{
					m_SelectedImage = i;
					break;
				}
			}
			if(m_SelectedImage == OldImage && !m_Map.m_vpImages[m_SelectedImage]->m_External)
			{
				for(size_t i = 0; i < m_Map.m_vpImages.size(); i++)
				{
					if(m_Map.m_vpImages[i]->m_External)
					{
						m_SelectedImage = i;
						break;
					}
				}
			}
			ScrollToSelection = OldImage != m_SelectedImage;
		}
		else if(Input()->KeyPress(KEY_UP))
		{
			int OldImage = m_SelectedImage;
			m_SelectedImage = clamp(m_SelectedImage, 0, (int)m_Map.m_vpImages.size() - 1);
			for(int i = m_SelectedImage - 1; i >= 0; i--)
			{
				if(m_Map.m_vpImages[i]->m_External == m_Map.m_vpImages[m_SelectedImage]->m_External)
				{
					m_SelectedImage = i;
					break;
				}
			}
			if(m_SelectedImage == OldImage && m_Map.m_vpImages[m_SelectedImage]->m_External)
			{
				for(int i = (int)m_Map.m_vpImages.size() - 1; i >= 0; i--)
				{
					if(!m_Map.m_vpImages[i]->m_External)
					{
						m_SelectedImage = i;
						break;
					}
				}
			}
			ScrollToSelection = OldImage != m_SelectedImage;
		}
	}

	for(int e = 0; e < 2; e++) // two passes, first embedded, then external
	{
		CUIRect Slot;
		ToolBox.HSplitTop(RowHeight + 3.0f, &Slot, &ToolBox);
		if(s_ScrollRegion.AddRect(Slot))
			UI()->DoLabel(&Slot, e == 0 ? "Embedded" : "External", 12.0f, TEXTALIGN_MC);

		for(int i = 0; i < (int)m_Map.m_vpImages.size(); i++)
		{
			if((e && !m_Map.m_vpImages[i]->m_External) ||
				(!e && m_Map.m_vpImages[i]->m_External))
			{
				continue;
			}

			ToolBox.HSplitTop(RowHeight + 2.0f, &Slot, &ToolBox);
			int Selected = m_SelectedImage == i;
			if(!s_ScrollRegion.AddRect(Slot, Selected && ScrollToSelection))
				continue;
			Slot.HSplitTop(RowHeight, &Slot, nullptr);

			const bool ImageUsed = std::any_of(m_Map.m_vpGroups.cbegin(), m_Map.m_vpGroups.cend(), [i](const auto &pGroup) {
				return std::any_of(pGroup->m_vpLayers.cbegin(), pGroup->m_vpLayers.cend(), [i](const auto &pLayer) {
					if(pLayer->m_Type == LAYERTYPE_QUADS)
						return static_cast<CLayerQuads *>(pLayer)->m_Image == i;
					else if(pLayer->m_Type == LAYERTYPE_TILES)
						return static_cast<CLayerTiles *>(pLayer)->m_Image == i;
					return false;
				});
			});

			if(!ImageUsed)
				Selected += 2; // Image is unused

			if(Selected < 2 && e == 1)
			{
				if(!IsVanillaImage(m_Map.m_vpImages[i]->m_aName))
				{
					Selected += 4; // Image should be embedded
				}
			}

			float FontSize = 10.0f;
			while(TextRender()->TextWidth(FontSize, m_Map.m_vpImages[i]->m_aName, -1, -1.0f) > Slot.w)
				FontSize--;

			if(int Result = DoButton_Ex(&m_Map.m_vpImages[i], m_Map.m_vpImages[i]->m_aName, Selected, &Slot,
				   BUTTON_CONTEXT, "Select image.", IGraphics::CORNER_ALL, FontSize))
			{
				m_SelectedImage = i;

				if(Result == 2)
				{
					const CEditorImage *pImg = m_Map.m_vpImages[m_SelectedImage];
					const int Height = pImg->m_External || IsVanillaImage(pImg->m_aName) ? 73 : 56;
					static SPopupMenuId s_PopupImageId;
					UI()->DoPopupMenu(&s_PopupImageId, UI()->MouseX(), UI()->MouseY(), 120, Height, this, PopupImage);
				}
			}
		}

		// separator
		ToolBox.HSplitTop(5.0f, &Slot, &ToolBox);
		if(s_ScrollRegion.AddRect(Slot))
		{
			IGraphics::CLineItem LineItem(Slot.x, Slot.y + Slot.h / 2, Slot.x + Slot.w, Slot.y + Slot.h / 2);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->LinesDraw(&LineItem, 1);
			Graphics()->LinesEnd();
		}
	}

	// new image
	static int s_AddImageButton = 0;
	CUIRect AddImageButton;
	ToolBox.HSplitTop(5.0f + RowHeight + 1.0f, &AddImageButton, &ToolBox);
	if(s_ScrollRegion.AddRect(AddImageButton))
	{
		AddImageButton.HSplitTop(5.0f, nullptr, &AddImageButton);
		AddImageButton.HSplitTop(RowHeight, &AddImageButton, nullptr);
		if(DoButton_Editor(&s_AddImageButton, "Add", 0, &AddImageButton, 0, "Load a new image to use in the map"))
			InvokeFileDialog(IStorage::TYPE_ALL, FILETYPE_IMG, "Add Image", "Add", "mapres", "", AddImage, this);
	}
	s_ScrollRegion.End();
}

void CEditor::RenderSelectedImage(CUIRect View)
{
	if(m_SelectedImage < 0 || (size_t)m_SelectedImage >= m_Map.m_vpImages.size())
		return;

	View.Margin(10.0f, &View);
	if(View.h < View.w)
		View.w = View.h;
	else
		View.h = View.w;
	float Max = maximum<float>(m_Map.m_vpImages[m_SelectedImage]->m_Width, m_Map.m_vpImages[m_SelectedImage]->m_Height);
	View.w *= m_Map.m_vpImages[m_SelectedImage]->m_Width / Max;
	View.h *= m_Map.m_vpImages[m_SelectedImage]->m_Height / Max;
	Graphics()->TextureSet(m_Map.m_vpImages[m_SelectedImage]->m_Texture);
	Graphics()->BlendNormal();
	Graphics()->WrapClamp();
	Graphics()->QuadsBegin();
	IGraphics::CQuadItem QuadItem(View.x, View.y, View.w, View.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
}

void CEditor::RenderSounds(CUIRect ToolBox)
{
	const float RowHeight = 12.0f;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollbarWidth = 10.0f;
	ScrollParams.m_ScrollbarMargin = 3.0f;
	ScrollParams.m_ScrollUnit = RowHeight * 5;
	s_ScrollRegion.Begin(&ToolBox, &ScrollOffset, &ScrollParams);
	ToolBox.y += ScrollOffset.y;

	bool ScrollToSelection = false;
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && !m_Map.m_vpSounds.empty())
	{
		if(Input()->KeyPress(KEY_DOWN))
		{
			m_SelectedSound = (m_SelectedSound + 1) % m_Map.m_vpSounds.size();
			ScrollToSelection = true;
		}
		else if(Input()->KeyPress(KEY_UP))
		{
			m_SelectedSound = (m_SelectedSound + m_Map.m_vpSounds.size() - 1) % m_Map.m_vpSounds.size();
			ScrollToSelection = true;
		}
	}

	CUIRect Slot;
	ToolBox.HSplitTop(RowHeight + 3.0f, &Slot, &ToolBox);
	if(s_ScrollRegion.AddRect(Slot))
		UI()->DoLabel(&Slot, "Embedded", 12.0f, TEXTALIGN_MC);

	for(int i = 0; i < (int)m_Map.m_vpSounds.size(); i++)
	{
		ToolBox.HSplitTop(RowHeight + 2.0f, &Slot, &ToolBox);
		int Selected = m_SelectedSound == i;
		if(!s_ScrollRegion.AddRect(Slot, Selected && ScrollToSelection))
			continue;
		Slot.HSplitTop(RowHeight, &Slot, nullptr);

		const bool SoundUsed = std::any_of(m_Map.m_vpGroups.cbegin(), m_Map.m_vpGroups.cend(), [i](const auto &pGroup) {
			return std::any_of(pGroup->m_vpLayers.cbegin(), pGroup->m_vpLayers.cend(), [i](const auto &pLayer) {
				if(pLayer->m_Type == LAYERTYPE_SOUNDS)
					return static_cast<CLayerSounds *>(pLayer)->m_Sound == i;
				return false;
			});
		});

		if(!SoundUsed)
			Selected += 2; // Sound is unused

		float FontSize = 10.0f;
		while(TextRender()->TextWidth(FontSize, m_Map.m_vpSounds[i]->m_aName, -1, -1.0f) > Slot.w)
			FontSize--;

		if(int Result = DoButton_Ex(&m_Map.m_vpSounds[i], m_Map.m_vpSounds[i]->m_aName, Selected, &Slot,
			   BUTTON_CONTEXT, "Select sound.", IGraphics::CORNER_ALL, FontSize))
		{
			m_SelectedSound = i;

			if(Result == 2)
			{
				static SPopupMenuId s_PopupSoundId;
				UI()->DoPopupMenu(&s_PopupSoundId, UI()->MouseX(), UI()->MouseY(), 120, 56, this, PopupSound);
			}
		}
	}

	// separator
	ToolBox.HSplitTop(5.0f, &Slot, &ToolBox);
	if(s_ScrollRegion.AddRect(Slot))
	{
		IGraphics::CLineItem LineItem(Slot.x, Slot.y + Slot.h / 2, Slot.x + Slot.w, Slot.y + Slot.h / 2);
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		Graphics()->LinesDraw(&LineItem, 1);
		Graphics()->LinesEnd();
	}

	// new sound
	static int s_AddSoundButton = 0;
	CUIRect AddSoundButton;
	ToolBox.HSplitTop(5.0f + RowHeight + 1.0f, &AddSoundButton, &ToolBox);
	if(s_ScrollRegion.AddRect(AddSoundButton))
	{
		AddSoundButton.HSplitTop(5.0f, nullptr, &AddSoundButton);
		AddSoundButton.HSplitTop(RowHeight, &AddSoundButton, nullptr);
		if(DoButton_Editor(&s_AddSoundButton, "Add", 0, &AddSoundButton, 0, "Load a new sound to use in the map"))
			InvokeFileDialog(IStorage::TYPE_ALL, FILETYPE_SOUND, "Add Sound", "Add", "mapres", "", AddSound, this);
	}
	s_ScrollRegion.End();
}

static int EditorListdirCallback(const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser)
{
	CEditor *pEditor = (CEditor *)pUser;
	if((pInfo->m_pName[0] == '.' && (pInfo->m_pName[1] == 0 ||
						(pInfo->m_pName[1] == '.' && pInfo->m_pName[2] == 0 && (!str_comp(pEditor->m_pFileDialogPath, "maps") || !str_comp(pEditor->m_pFileDialogPath, "mapres"))))) ||
		(!IsDir && ((pEditor->m_FileDialogFileType == CEditor::FILETYPE_MAP && !str_endswith(pInfo->m_pName, ".map")) ||
				   (pEditor->m_FileDialogFileType == CEditor::FILETYPE_IMG && !str_endswith(pInfo->m_pName, ".png")) ||
				   (pEditor->m_FileDialogFileType == CEditor::FILETYPE_SOUND && !str_endswith(pInfo->m_pName, ".opus")))))
		return 0;

	CEditor::CFilelistItem Item;
	str_copy(Item.m_aFilename, pInfo->m_pName);
	if(IsDir)
		str_format(Item.m_aName, sizeof(Item.m_aName), "%s/", pInfo->m_pName);
	else
	{
		int LenEnding = pEditor->m_FileDialogFileType == CEditor::FILETYPE_SOUND ? 5 : 4;
		str_truncate(Item.m_aName, sizeof(Item.m_aName), pInfo->m_pName, str_length(pInfo->m_pName) - LenEnding);
	}
	Item.m_IsDir = IsDir != 0;
	Item.m_IsLink = false;
	Item.m_StorageType = StorageType;
	Item.m_TimeModified = pInfo->m_TimeModified;
	pEditor->m_vCompleteFileList.push_back(Item);

	return 0;
}

void CEditor::SortFilteredFileList()
{
	if(m_SortByFilename == 1)
	{
		std::sort(m_vpFilteredFileList.begin(), m_vpFilteredFileList.end(), CEditor::CompareFilenameAscending);
	}
	else
	{
		std::sort(m_vpFilteredFileList.begin(), m_vpFilteredFileList.end(), CEditor::CompareFilenameDescending);
	}

	if(m_SortByTimeModified == 1)
	{
		std::stable_sort(m_vpFilteredFileList.begin(), m_vpFilteredFileList.end(), CEditor::CompareTimeModifiedAscending);
	}
	else if(m_SortByTimeModified == -1)
	{
		std::stable_sort(m_vpFilteredFileList.begin(), m_vpFilteredFileList.end(), CEditor::CompareTimeModifiedDescending);
	}
}

void CEditor::RenderFileDialog()
{
	// GUI coordsys
	UI()->MapScreen();
	CUIRect View = *UI()->Screen();
	CUIRect Preview = {0.0f, 0.0f, 0.0f, 0.0f};
	float Width = View.w, Height = View.h;

	View.Draw(ColorRGBA(0, 0, 0, 0.25f), 0, 0);
	View.VMargin(150.0f, &View);
	View.HMargin(50.0f, &View);
	View.Draw(ColorRGBA(0, 0, 0, 0.75f), IGraphics::CORNER_ALL, 5.0f);
	View.Margin(10.0f, &View);

	CUIRect Title, FileBox, FileBoxLabel, ButtonBar, PathBox;
	View.HSplitTop(18.0f, &Title, &View);
	View.HSplitTop(5.0f, nullptr, &View); // some spacing
	View.HSplitBottom(14.0f, &View, &ButtonBar);
	View.HSplitBottom(10.0f, &View, nullptr); // some spacing
	View.HSplitBottom(14.0f, &View, &PathBox);
	View.HSplitBottom(5.0f, &View, nullptr); // some spacing
	View.HSplitBottom(14.0f, &View, &FileBox);
	FileBox.VSplitLeft(55.0f, &FileBoxLabel, &FileBox);
	View.HSplitBottom(10.0f, &View, nullptr); // some spacing
	if(m_FileDialogFileType == CEditor::FILETYPE_IMG || m_FileDialogFileType == CEditor::FILETYPE_SOUND)
		View.VSplitMid(&View, &Preview);

	// title
	CUIRect ButtonTimeModified, ButtonFileName;
	Title.VSplitRight(10.0f, &Title, nullptr);
	Title.VSplitRight(90.0f, &Title, &ButtonTimeModified);
	Title.VSplitRight(10.0f, &Title, nullptr);
	Title.VSplitRight(90.0f, &Title, &ButtonFileName);
	Title.VSplitRight(10.0f, &Title, nullptr);

	const char *aSortIndicator[3] = {"▼", "", "▲"};

	static int s_ButtonTimeModified = 0;
	char aBufLabelButtonTimeModified[64];
	str_format(aBufLabelButtonTimeModified, sizeof(aBufLabelButtonTimeModified), "Time modified %s", aSortIndicator[m_SortByTimeModified + 1]);
	if(DoButton_Editor(&s_ButtonTimeModified, aBufLabelButtonTimeModified, 0, &ButtonTimeModified, 0, "Sort by time modified"))
	{
		if(m_SortByTimeModified == 1)
		{
			m_SortByTimeModified = -1;
		}
		else if(m_SortByTimeModified == -1)
		{
			m_SortByTimeModified = 0;
		}
		else
		{
			m_SortByTimeModified = 1;
		}

		RefreshFilteredFileList();
	}

	static int s_ButtonFileName = 0;
	char aBufLabelButtonFilename[64];
	str_format(aBufLabelButtonFilename, sizeof(aBufLabelButtonFilename), "Filename %s", aSortIndicator[m_SortByFilename + 1]);
	if(DoButton_Editor(&s_ButtonFileName, aBufLabelButtonFilename, 0, &ButtonFileName, 0, "Sort by file name"))
	{
		if(m_SortByFilename == 1)
		{
			m_SortByFilename = -1;
			m_SortByTimeModified = 0;
		}
		else
		{
			m_SortByFilename = 1;
			m_SortByTimeModified = 0;
		}

		RefreshFilteredFileList();
	}

	Title.Draw(ColorRGBA(1, 1, 1, 0.25f), IGraphics::CORNER_ALL, 4.0f);
	Title.VMargin(10.0f, &Title);
	UI()->DoLabel(&Title, m_pFileDialogTitle, 12.0f, TEXTALIGN_ML);

	// pathbox
	char aPath[IO_MAX_PATH_LENGTH], aBuf[128 + IO_MAX_PATH_LENGTH];
	if(m_FilesSelectedIndex != -1)
		Storage()->GetCompletePath(m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType, m_pFileDialogPath, aPath, sizeof(aPath));
	else
		aPath[0] = 0;
	str_format(aBuf, sizeof(aBuf), "Current path: %s", aPath);
	UI()->DoLabel(&PathBox, aBuf, 10.0f, TEXTALIGN_ML);

	const auto &&UpdateFileNameInput = [this]() {
		if(m_FilesSelectedIndex >= 0 && !m_vpFilteredFileList[m_FilesSelectedIndex]->m_IsDir)
		{
			char aNameWithoutExt[IO_MAX_PATH_LENGTH];
			fs_split_file_extension(m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename, aNameWithoutExt, sizeof(aNameWithoutExt));
			m_FileDialogFileNameInput.Set(aNameWithoutExt);
		}
		else
			m_FileDialogFileNameInput.Clear();
	};

	// filebox
	static CListBox s_ListBox;
	s_ListBox.SetActive(!UI()->IsPopupOpen());

	if(m_FileDialogStorageType == IStorage::TYPE_SAVE)
	{
		UI()->DoLabel(&FileBoxLabel, "Filename:", 10.0f, TEXTALIGN_ML);
		if(DoEditBox(&m_FileDialogFileNameInput, &FileBox, 10.0f))
		{
			// remove '/' and '\'
			for(int i = 0; m_FileDialogFileNameInput.GetString()[i]; ++i)
			{
				if(m_FileDialogFileNameInput.GetString()[i] == '/' || m_FileDialogFileNameInput.GetString()[i] == '\\')
				{
					m_FileDialogFileNameInput.SetRange(m_FileDialogFileNameInput.GetString() + i + 1, i, m_FileDialogFileNameInput.GetLength());
					--i;
				}
			}
			m_FilesSelectedIndex = -1;
			m_aFilesSelectedName[0] = '\0';
			// find first valid entry, if it exists
			for(size_t i = 0; i < m_vpFilteredFileList.size(); i++)
			{
				if(str_comp_nocase(m_vpFilteredFileList[i]->m_aName, m_FileDialogFileNameInput.GetString()) == 0)
				{
					m_FilesSelectedIndex = i;
					str_copy(m_aFilesSelectedName, m_vpFilteredFileList[i]->m_aName);
					break;
				}
			}
			if(m_FilesSelectedIndex >= 0)
				s_ListBox.ScrollToSelected();
		}

		if(m_FileDialogOpening)
			UI()->SetActiveItem(&m_FileDialogFileNameInput);
	}
	else
	{
		// render search bar
		UI()->DoLabel(&FileBoxLabel, "Search:", 10.0f, TEXTALIGN_ML);
		if(m_FileDialogOpening)
			UI()->SetActiveItem(&m_FileDialogFilterInput);
		if(UI()->DoClearableEditBox(&m_FileDialogFilterInput, &FileBox, 10.0f))
		{
			RefreshFilteredFileList();
			if(m_vpFilteredFileList.empty())
			{
				m_FilesSelectedIndex = -1;
			}
			else if(m_FilesSelectedIndex == -1 || (!m_FileDialogFilterInput.IsEmpty() && !str_find_nocase(m_vpFilteredFileList[m_FilesSelectedIndex]->m_aName, m_FileDialogFilterInput.GetString())))
			{
				// we need to refresh selection
				m_FilesSelectedIndex = -1;
				for(size_t i = 0; i < m_vpFilteredFileList.size(); i++)
				{
					if(str_find_nocase(m_vpFilteredFileList[i]->m_aName, m_FileDialogFilterInput.GetString()))
					{
						m_FilesSelectedIndex = i;
						break;
					}
				}
				if(m_FilesSelectedIndex == -1)
				{
					// select first item
					m_FilesSelectedIndex = 0;
				}
			}
			if(m_FilesSelectedIndex >= 0)
				str_copy(m_aFilesSelectedName, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aName);
			else
				m_aFilesSelectedName[0] = '\0';
			UpdateFileNameInput();
			s_ListBox.ScrollToSelected();
			m_FilePreviewState = PREVIEW_UNLOADED;
		}
	}

	m_FileDialogOpening = false;

	if(m_FilesSelectedIndex > -1)
	{
		if(m_FilePreviewState == PREVIEW_UNLOADED)
		{
			if(m_FileDialogFileType == CEditor::FILETYPE_IMG && str_endswith(m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename, ".png"))
			{
				char aBuffer[IO_MAX_PATH_LENGTH];
				str_format(aBuffer, sizeof(aBuffer), "%s/%s", m_pFileDialogPath, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename);
				if(Graphics()->LoadPNG(&m_FilePreviewImageInfo, aBuffer, m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType))
				{
					Graphics()->UnloadTexture(&m_FilePreviewImage);
					m_FilePreviewImage = Graphics()->LoadTextureRaw(m_FilePreviewImageInfo.m_Width, m_FilePreviewImageInfo.m_Height, m_FilePreviewImageInfo.m_Format, m_FilePreviewImageInfo.m_pData, m_FilePreviewImageInfo.m_Format, 0);
					Graphics()->FreePNG(&m_FilePreviewImageInfo);
					m_FilePreviewState = PREVIEW_LOADED;
				}
				else
				{
					m_FilePreviewState = PREVIEW_ERROR;
				}
			}
			else if(m_FileDialogFileType == CEditor::FILETYPE_SOUND && str_endswith(m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename, ".opus"))
			{
				char aBuffer[IO_MAX_PATH_LENGTH];
				str_format(aBuffer, sizeof(aBuffer), "%s/%s", m_pFileDialogPath, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename);
				Sound()->UnloadSample(m_FilePreviewSound);
				m_FilePreviewSound = Sound()->LoadOpus(aBuffer, m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType);
				m_FilePreviewState = m_FilePreviewSound == -1 ? PREVIEW_ERROR : PREVIEW_LOADED;
			}
		}

		if(m_FileDialogFileType == CEditor::FILETYPE_IMG)
		{
			Preview.Margin(10.0f, &Preview);
			if(m_FilePreviewState == PREVIEW_LOADED)
			{
				int w = m_FilePreviewImageInfo.m_Width;
				int h = m_FilePreviewImageInfo.m_Height;
				if(m_FilePreviewImageInfo.m_Width > Preview.w)
				{
					h = m_FilePreviewImageInfo.m_Height * Preview.w / m_FilePreviewImageInfo.m_Width;
					w = Preview.w;
				}
				if(h > Preview.h)
				{
					w = w * Preview.h / h;
					h = Preview.h;
				}

				Graphics()->TextureSet(m_FilePreviewImage);
				Graphics()->BlendNormal();
				Graphics()->QuadsBegin();
				IGraphics::CQuadItem QuadItem(Preview.x, Preview.y, w, h);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
				Graphics()->QuadsEnd();
			}
			else if(m_FilePreviewState == PREVIEW_ERROR)
			{
				SLabelProperties Props;
				Props.m_MaxWidth = Preview.w;
				UI()->DoLabel(&Preview, "Failed to load the image (check the local console for details).", 12.0f, TEXTALIGN_TL, Props);
			}
		}
		else if(m_FileDialogFileType == CEditor::FILETYPE_SOUND)
		{
			Preview.Margin(10.0f, &Preview);
			if(m_FilePreviewState == PREVIEW_LOADED)
			{
				CUIRect Button;
				Preview.HSplitTop(20.0f, &Preview, nullptr);
				Preview.VSplitLeft(Preview.h, &Button, &Preview);
				Preview.VSplitLeft(Preview.h / 4.0f, nullptr, &Preview);

				static int s_PlayStopButton;
				if(DoButton_FontIcon(&s_PlayStopButton, Sound()->IsPlaying(m_FilePreviewSound) ? FONT_ICON_STOP : FONT_ICON_PLAY, 0, &Button, 0, "Play/stop audio preview", IGraphics::CORNER_ALL))
				{
					if(Sound()->IsPlaying(m_FilePreviewSound))
						Sound()->Stop(m_FilePreviewSound);
					else
						Sound()->Play(CSounds::CHN_GUI, m_FilePreviewSound, 0);
				}

				char aDuration[32];
				char aDurationLabel[64];
				str_time_float(Sound()->GetSampleDuration(m_FilePreviewSound), TIME_HOURS, aDuration, sizeof(aDuration));
				str_format(aDurationLabel, sizeof(aDurationLabel), "Duration: %s", aDuration);
				UI()->DoLabel(&Preview, aDurationLabel, 12.0f, TEXTALIGN_ML);
			}
			else if(m_FilePreviewState == PREVIEW_ERROR)
			{
				SLabelProperties Props;
				Props.m_MaxWidth = Preview.w;
				UI()->DoLabel(&Preview, "Failed to load the sound (check the local console for details). Make sure you enabled sounds in the settings.", 12.0f, TEXTALIGN_TL, Props);
			}
		}
	}

	s_ListBox.DoStart(15.0f, m_vpFilteredFileList.size(), 1, 5, m_FilesSelectedIndex, &View, false);

	for(size_t i = 0; i < m_vpFilteredFileList.size(); i++)
	{
		const CListboxItem Item = s_ListBox.DoNextItem(m_vpFilteredFileList[i], m_FilesSelectedIndex >= 0 && (size_t)m_FilesSelectedIndex == i);
		if(!Item.m_Visible)
			continue;

		CUIRect Button, FileIcon, TimeModified;
		Item.m_Rect.VSplitLeft(Item.m_Rect.h, &FileIcon, &Button);
		Button.VSplitLeft(5.0f, nullptr, &Button);
		Button.VSplitRight(100.0f, &Button, &TimeModified);

		const char *pIconType;
		if(!m_vpFilteredFileList[i]->m_IsDir)
		{
			switch(m_FileDialogFileType)
			{
			case FILETYPE_MAP:
				pIconType = FONT_ICON_MAP;
				break;
			case FILETYPE_IMG:
				pIconType = FONT_ICON_IMAGE;
				break;
			case FILETYPE_SOUND:
				pIconType = FONT_ICON_MUSIC;
				break;
			default:
				pIconType = FONT_ICON_FILE;
			}
		}
		else
		{
			if(str_comp(m_vpFilteredFileList[i]->m_aFilename, "..") == 0)
				pIconType = FONT_ICON_FOLDER_TREE;
			else
				pIconType = FONT_ICON_FOLDER;
		}

		TextRender()->SetCurFont(TextRender()->GetFont(TEXT_FONT_ICON_FONT));
		UI()->DoLabel(&FileIcon, pIconType, 12.0f, TEXTALIGN_ML);
		TextRender()->SetCurFont(nullptr);

		UI()->DoLabel(&Button, m_vpFilteredFileList[i]->m_aName, 10.0f, TEXTALIGN_ML);

		if(!m_vpFilteredFileList[i]->m_IsLink && str_comp(m_vpFilteredFileList[i]->m_aFilename, "..") != 0)
		{
			char aBufTimeModified[64];
			str_timestamp_ex(m_vpFilteredFileList[i]->m_TimeModified, aBufTimeModified, sizeof(aBufTimeModified), "%d.%m.%Y %H:%M");
			UI()->DoLabel(&TimeModified, aBufTimeModified, 10.0f, TEXTALIGN_MR);
		}
	}

	const int NewSelection = s_ListBox.DoEnd();
	if(NewSelection != m_FilesSelectedIndex)
	{
		m_FilesSelectedIndex = NewSelection;
		str_copy(m_aFilesSelectedName, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aName);
		const bool WasChanged = m_FileDialogFileNameInput.WasChanged();
		UpdateFileNameInput();
		if(!WasChanged) // ensure that changed flag is not set if it wasn't previously set, as this would reset the selection after DoEditBox is called
			m_FileDialogFileNameInput.WasChanged(); // this clears the changed flag
		m_FilePreviewState = PREVIEW_UNLOADED;
	}

	const float ButtonSpacing = ButtonBar.w > 600.0f ? 40.0f : 10.0f;

	// the buttons
	static int s_OkButton = 0;
	static int s_CancelButton = 0;
	static int s_RefreshButton = 0;
	static int s_ShowDirectoryButton = 0;
	static int s_DeleteButton = 0;
	static int s_NewFolderButton = 0;

	CUIRect Button;
	ButtonBar.VSplitRight(50.0f, &ButtonBar, &Button);
	bool IsDir = m_FilesSelectedIndex >= 0 && m_vpFilteredFileList[m_FilesSelectedIndex]->m_IsDir;
	if(DoButton_Editor(&s_OkButton, IsDir ? "Open" : m_pFileDialogButtonText, 0, &Button, 0, nullptr) || s_ListBox.WasItemActivated())
	{
		if(IsDir) // folder
		{
			m_FileDialogFilterInput.Clear();
			if(str_comp(m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename, "..") == 0) // parent folder
			{
				if(fs_parent_dir(m_pFileDialogPath))
					m_pFileDialogPath = m_aFileDialogCurrentFolder; // leave the link
			}
			else // sub folder
			{
				if(m_vpFilteredFileList[m_FilesSelectedIndex]->m_IsLink)
				{
					m_pFileDialogPath = m_aFileDialogCurrentLink; // follow the link
					str_copy(m_aFileDialogCurrentLink, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename);
				}
				else
				{
					char aTemp[IO_MAX_PATH_LENGTH];
					str_copy(aTemp, m_pFileDialogPath);
					str_format(m_pFileDialogPath, IO_MAX_PATH_LENGTH, "%s/%s", aTemp, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename);
				}
			}
			FilelistPopulate(!str_comp(m_pFileDialogPath, "maps") || !str_comp(m_pFileDialogPath, "mapres") ? m_FileDialogStorageType :
															  m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType);
			UpdateFileNameInput();
		}
		else // file
		{
			str_format(m_aFileSaveName, sizeof(m_aFileSaveName), "%s/%s", m_pFileDialogPath, m_FileDialogFileNameInput.GetString());
			if(!str_endswith(m_aFileSaveName, FILETYPE_EXTENSIONS[m_FileDialogFileType]))
				str_append(m_aFileSaveName, FILETYPE_EXTENSIONS[m_FileDialogFileType]);
			if(!str_comp(m_pFileDialogButtonText, "Save"))
			{
				if(Storage()->FileExists(m_aFileSaveName, IStorage::TYPE_SAVE))
				{
					m_PopupEventType = m_pfnFileDialogFunc == &CallbackSaveCopyMap ? POPEVENT_SAVE_COPY : POPEVENT_SAVE;
					m_PopupEventActivated = true;
				}
				else if(m_pfnFileDialogFunc)
					m_pfnFileDialogFunc(m_aFileSaveName, m_FilesSelectedIndex >= 0 ? m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType : m_FileDialogStorageType, m_pFileDialogUser);
			}
			else if(m_pfnFileDialogFunc)
				m_pfnFileDialogFunc(m_aFileSaveName, m_FilesSelectedIndex >= 0 ? m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType : m_FileDialogStorageType, m_pFileDialogUser);
		}
	}

	ButtonBar.VSplitRight(ButtonSpacing, &ButtonBar, nullptr);
	ButtonBar.VSplitRight(50.0f, &ButtonBar, &Button);
	if(DoButton_Editor(&s_CancelButton, "Cancel", 0, &Button, 0, nullptr) || (s_ListBox.Active() && UI()->ConsumeHotkey(CUI::HOTKEY_ESCAPE)))
		m_Dialog = DIALOG_NONE;

	ButtonBar.VSplitRight(ButtonSpacing, &ButtonBar, nullptr);
	ButtonBar.VSplitRight(50.0f, &ButtonBar, &Button);
	if(DoButton_Editor(&s_RefreshButton, "Refresh", 0, &Button, 0, nullptr) || (s_ListBox.Active() && (Input()->KeyIsPressed(KEY_F5) || (Input()->ModifierIsPressed() && Input()->KeyIsPressed(KEY_R)))))
		FilelistPopulate(m_FileDialogLastPopulatedStorageType, true);

	ButtonBar.VSplitRight(ButtonSpacing, &ButtonBar, nullptr);
	ButtonBar.VSplitRight(90.0f, &ButtonBar, &Button);
	if(DoButton_Editor(&s_ShowDirectoryButton, "Show directory", 0, &Button, 0, "Open the current directory in the file browser"))
	{
		if(!open_file(aPath))
		{
			ShowFileDialogError("Failed to open the directory '%s'.", aPath);
		}
	}

	ButtonBar.VSplitRight(ButtonSpacing, &ButtonBar, nullptr);
	ButtonBar.VSplitRight(50.0f, &ButtonBar, &Button);
	static CUI::SConfirmPopupContext s_ConfirmDeletePopupContext;
	if(m_FilesSelectedIndex >= 0 && m_vpFilteredFileList[m_FilesSelectedIndex]->m_StorageType == IStorage::TYPE_SAVE && !m_vpFilteredFileList[m_FilesSelectedIndex]->m_IsLink && str_comp(m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename, "..") != 0)
	{
		if(DoButton_Editor(&s_DeleteButton, "Delete", 0, &Button, 0, nullptr) || (s_ListBox.Active() && UI()->ConsumeHotkey(CUI::HOTKEY_DELETE)))
		{
			s_ConfirmDeletePopupContext.Reset();
			s_ConfirmDeletePopupContext.YesNoButtons();
			str_format(s_ConfirmDeletePopupContext.m_aMessage, sizeof(s_ConfirmDeletePopupContext.m_aMessage), "Are you sure that you want to delete the %s '%s/%s'?", IsDir ? "folder" : "file", m_pFileDialogPath, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename);
			UI()->ShowPopupConfirm(UI()->MouseX(), UI()->MouseY(), &s_ConfirmDeletePopupContext);
		}
		if(s_ConfirmDeletePopupContext.m_Result == CUI::SConfirmPopupContext::CONFIRMED)
		{
			char aDeleteFilePath[IO_MAX_PATH_LENGTH];
			str_format(aDeleteFilePath, sizeof(aDeleteFilePath), "%s/%s", m_pFileDialogPath, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aFilename);
			if(IsDir)
			{
				if(Storage()->RemoveFolder(aDeleteFilePath, IStorage::TYPE_SAVE))
					FilelistPopulate(m_FileDialogLastPopulatedStorageType, true);
				else
					ShowFileDialogError("Failed to delete folder '%s'. Make sure it's empty first.", aDeleteFilePath);
			}
			else
			{
				if(Storage()->RemoveFile(aDeleteFilePath, IStorage::TYPE_SAVE))
					FilelistPopulate(m_FileDialogLastPopulatedStorageType, true);
				else
					ShowFileDialogError("Failed to delete file '%s'.", aDeleteFilePath);
			}
		}
		if(s_ConfirmDeletePopupContext.m_Result != CUI::SConfirmPopupContext::UNSET)
			s_ConfirmDeletePopupContext.Reset();
	}
	else
		s_ConfirmDeletePopupContext.Reset();

	if(m_FileDialogStorageType == IStorage::TYPE_SAVE)
	{
		ButtonBar.VSplitLeft(70.0f, &Button, &ButtonBar);
		if(DoButton_Editor(&s_NewFolderButton, "New folder", 0, &Button, 0, nullptr))
		{
			m_FileDialogNewFolderNameInput.Clear();
			static SPopupMenuId s_PopupNewFolderId;
			constexpr float PopupWidth = 400.0f;
			constexpr float PopupHeight = 110.0f;
			UI()->DoPopupMenu(&s_PopupNewFolderId, Width / 2.0f - PopupWidth / 2.0f, Height / 2.0f - PopupHeight / 2.0f, PopupWidth, PopupHeight, this, PopupNewFolder);
			UI()->SetActiveItem(&m_FileDialogNewFolderNameInput);
		}
	}
}

void CEditor::RefreshFilteredFileList()
{
	m_vpFilteredFileList.clear();
	for(const CFilelistItem &Item : m_vCompleteFileList)
	{
		if(m_FileDialogFilterInput.IsEmpty() || str_find_nocase(Item.m_aName, m_FileDialogFilterInput.GetString()))
		{
			m_vpFilteredFileList.push_back(&Item);
		}
	}
	SortFilteredFileList();
	if(!m_vpFilteredFileList.empty())
	{
		if(m_aFilesSelectedName[0])
		{
			for(size_t i = 0; i < m_vpFilteredFileList.size(); i++)
			{
				if(m_aFilesSelectedName[0] && str_comp(m_vpFilteredFileList[i]->m_aName, m_aFilesSelectedName) == 0)
				{
					m_FilesSelectedIndex = i;
					break;
				}
			}
		}
		m_FilesSelectedIndex = clamp<int>(m_FilesSelectedIndex, 0, m_vpFilteredFileList.size() - 1);
		str_copy(m_aFilesSelectedName, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aName);
	}
	else
	{
		m_FilesSelectedIndex = -1;
		m_aFilesSelectedName[0] = '\0';
	}
}

void CEditor::FilelistPopulate(int StorageType, bool KeepSelection)
{
	m_FileDialogLastPopulatedStorageType = StorageType;
	m_vCompleteFileList.clear();
	if(m_FileDialogStorageType != IStorage::TYPE_SAVE && !str_comp(m_pFileDialogPath, "maps"))
	{
		CFilelistItem Item;
		str_copy(Item.m_aFilename, "downloadedmaps");
		str_copy(Item.m_aName, "downloadedmaps/");
		Item.m_IsDir = true;
		Item.m_IsLink = true;
		Item.m_StorageType = IStorage::TYPE_SAVE;
		Item.m_TimeModified = 0;
		m_vCompleteFileList.push_back(Item);
	}
	Storage()->ListDirectoryInfo(StorageType, m_pFileDialogPath, EditorListdirCallback, this);
	RefreshFilteredFileList();
	if(!KeepSelection)
	{
		m_FilesSelectedIndex = m_vpFilteredFileList.empty() ? -1 : 0;
		if(m_FilesSelectedIndex >= 0)
			str_copy(m_aFilesSelectedName, m_vpFilteredFileList[m_FilesSelectedIndex]->m_aName);
		else
			m_aFilesSelectedName[0] = '\0';
	}
	m_FilePreviewState = PREVIEW_UNLOADED;
}

void CEditor::InvokeFileDialog(int StorageType, int FileType, const char *pTitle, const char *pButtonText,
	const char *pBasePath, const char *pDefaultName,
	bool (*pfnFunc)(const char *pFileName, int StorageType, void *pUser), void *pUser)
{
	UI()->ClosePopupMenus();
	m_FileDialogStorageType = StorageType;
	m_pFileDialogTitle = pTitle;
	m_pFileDialogButtonText = pButtonText;
	m_pfnFileDialogFunc = pfnFunc;
	m_pFileDialogUser = pUser;
	m_FileDialogFileNameInput.Clear();
	m_FileDialogFilterInput.Clear();
	m_aFileDialogCurrentFolder[0] = 0;
	m_aFileDialogCurrentLink[0] = 0;
	m_pFileDialogPath = m_aFileDialogCurrentFolder;
	m_FileDialogFileType = FileType;
	m_FilePreviewState = PREVIEW_UNLOADED;
	m_FileDialogOpening = true;

	if(pDefaultName)
		m_FileDialogFileNameInput.Set(pDefaultName);
	if(pBasePath)
		str_copy(m_aFileDialogCurrentFolder, pBasePath);

	FilelistPopulate(m_FileDialogStorageType);

	m_FileDialogOpening = true;
	m_Dialog = DIALOG_FILE;
}

void CEditor::ShowFileDialogError(const char *pFormat, ...)
{
	static CUI::SMessagePopupContext s_MessagePopupContext;
	s_MessagePopupContext.ErrorColor();
	va_list VarArgs;
	va_start(VarArgs, pFormat);
	str_format_v(s_MessagePopupContext.m_aMessage, sizeof(s_MessagePopupContext.m_aMessage), pFormat, VarArgs);
	va_end(VarArgs);
	UI()->ShowPopupMessage(UI()->MouseX(), UI()->MouseY(), &s_MessagePopupContext);
}

void CEditor::RenderModebar(CUIRect View)
{
	CUIRect Button;

	// mode buttons
	{
		View.VSplitLeft(65.0f, &Button, &View);
		Button.HSplitTop(30.0f, nullptr, &Button);
		static int s_Button = 0;
		const char *pButName = "";

		if(m_Mode == MODE_LAYERS)
			pButName = "Layers";
		else if(m_Mode == MODE_IMAGES)
			pButName = "Images";
		else if(m_Mode == MODE_SOUNDS)
			pButName = "Sounds";

		int MouseButton = DoButton_Tab(&s_Button, pButName, 0, &Button, 0, "Switch between images, sounds and layers management.");
		if(MouseButton == 2 || (Input()->KeyPress(KEY_LEFT) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0))
		{
			if(m_Mode == MODE_LAYERS)
				m_Mode = MODE_SOUNDS;
			else if(m_Mode == MODE_IMAGES)
				m_Mode = MODE_LAYERS;
			else
				m_Mode = MODE_IMAGES;
		}
		else if(MouseButton == 1 || (Input()->KeyPress(KEY_RIGHT) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0))
		{
			if(m_Mode == MODE_LAYERS)
				m_Mode = MODE_IMAGES;
			else if(m_Mode == MODE_IMAGES)
				m_Mode = MODE_SOUNDS;
			else
				m_Mode = MODE_LAYERS;
		}
	}

	View.VSplitLeft(5.0f, nullptr, &View);
}

void CEditor::RenderStatusbar(CUIRect View)
{
	CUIRect Button;
	View.VSplitRight(60.0f, &View, &Button);
	static int s_EnvelopeButton = 0;
	if(DoButton_Editor(&s_EnvelopeButton, "Envelopes", m_ShowEnvelopeEditor, &Button, 0, "Toggles the envelope editor."))
	{
		m_ShowEnvelopeEditor ^= 1;
		m_ShowServerSettingsEditor = false;
	}

	View.VSplitRight(100.0f, &View, &Button);
	Button.VSplitRight(10.0f, &Button, nullptr);
	static int s_SettingsButton = 0;
	if(DoButton_Editor(&s_SettingsButton, "Server settings", m_ShowServerSettingsEditor, &Button, 0, "Toggles the server settings editor."))
	{
		m_ShowEnvelopeEditor = false;
		m_ShowServerSettingsEditor ^= 1;
	}

	if(m_pTooltip)
	{
		char aBuf[512];
		if(ms_pUiGotContext && ms_pUiGotContext == UI()->HotItem())
			str_format(aBuf, sizeof(aBuf), "%s Right click for context menu.", m_pTooltip);
		else
			str_copy(aBuf, m_pTooltip);

		float FontSize = ScaleFontSize(aBuf, sizeof(aBuf), 10.0f, View.w);
		SLabelProperties Props;
		Props.m_MaxWidth = View.w;
		UI()->DoLabel(&View, aBuf, FontSize, TEXTALIGN_ML, Props);
	}
}

bool CEditor::IsEnvelopeUsed(int EnvelopeIndex) const
{
	for(const auto &pGroup : m_Map.m_vpGroups)
	{
		for(const auto &pLayer : pGroup->m_vpLayers)
		{
			if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				CLayerQuads *pLayerQuads = (CLayerQuads *)pLayer;
				for(const auto &Quad : pLayerQuads->m_vQuads)
				{
					if(Quad.m_PosEnv == EnvelopeIndex || Quad.m_ColorEnv == EnvelopeIndex)
					{
						return true;
					}
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_SOUNDS)
			{
				CLayerSounds *pLayerSounds = (CLayerSounds *)pLayer;
				for(const auto &Source : pLayerSounds->m_vSources)
				{
					if(Source.m_PosEnv == EnvelopeIndex || Source.m_SoundEnv == EnvelopeIndex)
					{
						return true;
					}
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_TILES)
			{
				CLayerTiles *pLayerTiles = (CLayerTiles *)pLayer;
				if(pLayerTiles->m_ColorEnv == EnvelopeIndex)
					return true;
			}
		}
	}
	return false;
}

void CEditor::RemoveUnusedEnvelopes()
{
	for(size_t Envelope = 0; Envelope < m_Map.m_vpEnvelopes.size();)
	{
		if(IsEnvelopeUsed(Envelope))
		{
			++Envelope;
		}
		else
		{
			m_Map.DeleteEnvelope(Envelope);
		}
	}
}

void CEditor::RenderEnvelopeEditor(CUIRect View)
{
	RenderExtraEditorDragBar(View, &m_EnvelopeEditorSplit);

	if(m_SelectedEnvelope < 0)
		m_SelectedEnvelope = 0;
	if(m_SelectedEnvelope >= (int)m_Map.m_vpEnvelopes.size())
		m_SelectedEnvelope = m_Map.m_vpEnvelopes.size() - 1;

	CEnvelope *pEnvelope = nullptr;
	if(m_SelectedEnvelope >= 0 && m_SelectedEnvelope < (int)m_Map.m_vpEnvelopes.size())
		pEnvelope = m_Map.m_vpEnvelopes[m_SelectedEnvelope];

	CUIRect ToolBar, CurveBar, ColorBar;
	View.HSplitTop(15.0f, &ToolBar, &View);
	View.HSplitTop(15.0f, &CurveBar, &View);
	ToolBar.Margin(2.0f, &ToolBar);
	CurveBar.Margin(2.0f, &CurveBar);

	bool CurrentEnvelopeSwitched = false;

	// do the toolbar
	{
		CUIRect Button;
		CEnvelope *pNewEnv = nullptr;

		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_NewSoundButton = 0;
		if(DoButton_Editor(&s_NewSoundButton, "Sound+", 0, &Button, 0, "Creates a new sound envelope"))
		{
			m_Map.OnModify();
			pNewEnv = m_Map.NewEnvelope(1);
		}

		ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_New4dButton = 0;
		if(DoButton_Editor(&s_New4dButton, "Color+", 0, &Button, 0, "Creates a new color envelope"))
		{
			m_Map.OnModify();
			pNewEnv = m_Map.NewEnvelope(4);
		}

		ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_New2dButton = 0;
		if(DoButton_Editor(&s_New2dButton, "Pos.+", 0, &Button, 0, "Creates a new position envelope"))
		{
			m_Map.OnModify();
			pNewEnv = m_Map.NewEnvelope(3);
		}

		if(m_SelectedEnvelope >= 0)
		{
			// Delete button
			ToolBar.VSplitRight(10.0f, &ToolBar, nullptr);
			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			static int s_DeleteButton = 0;
			if(DoButton_Editor(&s_DeleteButton, "✗", 0, &Button, 0, "Delete this envelope"))
			{
				m_Map.DeleteEnvelope(m_SelectedEnvelope);
				if(m_SelectedEnvelope >= (int)m_Map.m_vpEnvelopes.size())
					m_SelectedEnvelope = m_Map.m_vpEnvelopes.size() - 1;
				pEnvelope = m_SelectedEnvelope >= 0 ? m_Map.m_vpEnvelopes[m_SelectedEnvelope] : nullptr;
			}

			// Move right button
			ToolBar.VSplitRight(5.0f, &ToolBar, nullptr);
			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			static int s_MoveRightButton = 0;
			if(DoButton_Ex(&s_MoveRightButton, "→", 0, &Button, 0, "Move this envelope to the right", IGraphics::CORNER_R))
			{
				m_Map.SwapEnvelopes(m_SelectedEnvelope, m_SelectedEnvelope + 1);
				m_SelectedEnvelope = clamp<int>(m_SelectedEnvelope + 1, 0, m_Map.m_vpEnvelopes.size() - 1);
				pEnvelope = m_Map.m_vpEnvelopes[m_SelectedEnvelope];
			}

			// Move left button
			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			static int s_MoveLeftButton = 0;
			if(DoButton_Ex(&s_MoveLeftButton, "←", 0, &Button, 0, "Move this envelope to the left", IGraphics::CORNER_L))
			{
				m_Map.SwapEnvelopes(m_SelectedEnvelope - 1, m_SelectedEnvelope);
				m_SelectedEnvelope = clamp<int>(m_SelectedEnvelope - 1, 0, m_Map.m_vpEnvelopes.size() - 1);
				pEnvelope = m_Map.m_vpEnvelopes[m_SelectedEnvelope];
			}

			// Margin on the right side
			ToolBar.VSplitRight(7.0f, &ToolBar, nullptr);
		}

		if(pNewEnv) // add the default points
		{
			if(pNewEnv->m_Channels == 4)
			{
				pNewEnv->AddPoint(0, f2fx(1.0f), f2fx(1.0f), f2fx(1.0f), f2fx(1.0f));
				pNewEnv->AddPoint(1000, f2fx(1.0f), f2fx(1.0f), f2fx(1.0f), f2fx(1.0f));
			}
			else
			{
				pNewEnv->AddPoint(0, 0);
				pNewEnv->AddPoint(1000, 0);
			}
		}

		CUIRect Shifter, Inc, Dec;
		ToolBar.VSplitLeft(60.0f, &Shifter, &ToolBar);
		Shifter.VSplitRight(15.0f, &Shifter, &Inc);
		Shifter.VSplitLeft(15.0f, &Dec, &Shifter);
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "%d/%d", m_SelectedEnvelope + 1, (int)m_Map.m_vpEnvelopes.size());

		ColorRGBA EnvColor = ColorRGBA(1, 1, 1, 0.5f);
		if(!m_Map.m_vpEnvelopes.empty())
		{
			EnvColor = IsEnvelopeUsed(m_SelectedEnvelope) ?
					   ColorRGBA(1, 0.7f, 0.7f, 0.5f) :
					   ColorRGBA(0.7f, 1, 0.7f, 0.5f);
		}

		static int s_EnvelopeSelector = 0;
		int NewValue = UiDoValueSelector(&s_EnvelopeSelector, &Shifter, aBuf, m_SelectedEnvelope + 1, 1, m_Map.m_vpEnvelopes.size(), 1, 1.0f, "Select Envelope", false, false, IGraphics::CORNER_NONE, &EnvColor, false);
		if(NewValue - 1 != m_SelectedEnvelope)
		{
			m_SelectedEnvelope = NewValue - 1;
			CurrentEnvelopeSwitched = true;
		}

		static int s_PrevButton = 0;
		if(DoButton_ButtonDec(&s_PrevButton, nullptr, 0, &Dec, 0, "Previous Envelope"))
		{
			m_SelectedEnvelope--;
			if(m_SelectedEnvelope < 0)
				m_SelectedEnvelope = m_Map.m_vpEnvelopes.size() - 1;
			CurrentEnvelopeSwitched = true;
		}

		static int s_NextButton = 0;
		if(DoButton_ButtonInc(&s_NextButton, nullptr, 0, &Inc, 0, "Next Envelope"))
		{
			m_SelectedEnvelope++;
			if(m_SelectedEnvelope >= (int)m_Map.m_vpEnvelopes.size())
				m_SelectedEnvelope = 0;
			CurrentEnvelopeSwitched = true;
		}

		if(pEnvelope)
		{
			ToolBar.VSplitLeft(15.0f, nullptr, &ToolBar);
			ToolBar.VSplitLeft(40.0f, &Button, &ToolBar);
			UI()->DoLabel(&Button, "Name:", 10.0f, TEXTALIGN_MR);

			ToolBar.VSplitLeft(3.0f, nullptr, &ToolBar);
			ToolBar.VSplitLeft(ToolBar.w > ToolBar.h * 40 ? 80.0f : 60.0f, &Button, &ToolBar);

			static CLineInput s_NameInput;
			s_NameInput.SetBuffer(pEnvelope->m_aName, sizeof(pEnvelope->m_aName));
			if(DoEditBox(&s_NameInput, &Button, 10.0f, IGraphics::CORNER_ALL, "The name of the selected envelope"))
			{
				m_Map.OnModify();
			}
		}
	}

	bool ShowColorBar = false;
	if(pEnvelope && pEnvelope->m_Channels == 4)
	{
		ShowColorBar = true;
		View.HSplitTop(20.0f, &ColorBar, &View);
		ColorBar.Margin(2.0f, &ColorBar);
		RenderBackground(ColorBar, m_CheckerTexture, 16.0f, 1.0f);
	}

	RenderBackground(View, m_CheckerTexture, 32.0f, 0.1f);

	if(pEnvelope)
	{
		static std::vector<int> s_vSelection;
		static int s_EnvelopeEditorID = 0;
		static int s_ActiveChannels = 0xf;

		ColorRGBA aColors[] = {ColorRGBA(1, 0.2f, 0.2f), ColorRGBA(0.2f, 1, 0.2f), ColorRGBA(0.2f, 0.2f, 1), ColorRGBA(1, 1, 0.2f)};

		CUIRect Button;

		ToolBar.VSplitLeft(15.0f, &Button, &ToolBar);

		static const char *s_aapNames[4][CEnvPoint::MAX_CHANNELS] = {
			{"V", "", "", ""},
			{"", "", "", ""},
			{"X", "Y", "R", ""},
			{"R", "G", "B", "A"},
		};

		static const char *s_aapDescriptions[4][CEnvPoint::MAX_CHANNELS] = {
			{"Volume of the envelope", "", "", ""},
			{"", "", "", ""},
			{"X-axis of the envelope", "Y-axis of the envelope", "Rotation of the envelope", ""},
			{"Red value of the envelope", "Green value of the envelope", "Blue value of the envelope", "Alpha value of the envelope"},
		};

		static int s_aChannelButtons[CEnvPoint::MAX_CHANNELS] = {0};
		int Bit = 1;

		for(int i = 0; i < CEnvPoint::MAX_CHANNELS; i++, Bit <<= 1)
		{
			ToolBar.VSplitLeft(15.0f, &Button, &ToolBar);
			if(i < pEnvelope->m_Channels)
			{
				int Corners = IGraphics::CORNER_NONE;
				if(pEnvelope->m_Channels == 1)
					Corners = IGraphics::CORNER_ALL;
				else if(i == 0)
					Corners = IGraphics::CORNER_L;
				else if(i == pEnvelope->m_Channels - 1)
					Corners = IGraphics::CORNER_R;

				if(DoButton_Env(&s_aChannelButtons[i], s_aapNames[pEnvelope->m_Channels - 1][i], s_ActiveChannels & Bit, &Button, s_aapDescriptions[pEnvelope->m_Channels - 1][i], aColors[i], Corners))
					s_ActiveChannels ^= Bit;
			}
		}

		// sync checkbox
		ToolBar.VSplitLeft(15.0f, nullptr, &ToolBar);
		ToolBar.VSplitLeft(12.0f, &Button, &ToolBar);
		static int s_SyncButton;
		if(DoButton_Editor(&s_SyncButton, pEnvelope->m_Synchronized ? "X" : "", 0, &Button, 0, "Synchronize envelope animation to game time (restarts when you touch the start line)"))
			pEnvelope->m_Synchronized = !pEnvelope->m_Synchronized;

		ToolBar.VSplitLeft(4.0f, nullptr, &ToolBar);
		ToolBar.VSplitLeft(40.0f, &Button, &ToolBar);
		UI()->DoLabel(&Button, "Sync.", 10.0f, TEXTALIGN_ML);

		float EndTime = pEnvelope->EndTime();
		if(EndTime < 1)
			EndTime = 1;

		pEnvelope->FindTopBottom(s_ActiveChannels);
		float Top = pEnvelope->m_Top;
		float Bottom = pEnvelope->m_Bottom;

		if(Top < 1)
			Top = 1;
		if(Bottom >= 0)
			Bottom = 0;

		float TimeScale = EndTime / View.w;
		float ValueScale = (Top - Bottom) / View.h;

		if(UI()->MouseInside(&View))
			UI()->SetHotItem(&s_EnvelopeEditorID);

		if(UI()->HotItem() == &s_EnvelopeEditorID)
		{
			// do stuff
			if(UI()->MouseButtonClicked(1))
			{
				// add point
				int Time = (int)(((UI()->MouseX() - View.x) * TimeScale) * 1000.0f);
				ColorRGBA Channels;
				pEnvelope->Eval(Time / 1000.0f, Channels);
				pEnvelope->AddPoint(Time,
					f2fx(Channels.r), f2fx(Channels.g),
					f2fx(Channels.b), f2fx(Channels.a));
				m_Map.OnModify();
			}

			m_ShowEnvelopePreview = SHOWENV_SELECTED;
			m_pTooltip = "Press right mouse button to create a new point";
		}

		// render lines
		{
			UI()->ClipEnable(&View);
			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			for(int c = 0; c < pEnvelope->m_Channels; c++)
			{
				if(s_ActiveChannels & (1 << c))
					Graphics()->SetColor(aColors[c].r, aColors[c].g, aColors[c].b, 1);
				else
					Graphics()->SetColor(aColors[c].r * 0.5f, aColors[c].g * 0.5f, aColors[c].b * 0.5f, 1);

				float PrevX = 0;
				ColorRGBA Channels;
				pEnvelope->Eval(0.000001f, Channels);
				float PrevValue = Channels[c];

				int Steps = (int)((View.w / UI()->Screen()->w) * Graphics()->ScreenWidth());
				for(int i = 1; i <= Steps; i++)
				{
					float a = i / (float)Steps;
					pEnvelope->Eval(a * EndTime, Channels);
					float v = Channels[c];
					v = (v - Bottom) / (Top - Bottom);

					IGraphics::CLineItem LineItem(View.x + PrevX * View.w, View.y + View.h - PrevValue * View.h, View.x + a * View.w, View.y + View.h - v * View.h);
					Graphics()->LinesDraw(&LineItem, 1);
					PrevX = a;
					PrevValue = v;
				}
			}
			Graphics()->LinesEnd();
			UI()->ClipDisable();
		}

		// render curve options
		{
			for(int i = 0; i < (int)pEnvelope->m_vPoints.size() - 1; i++)
			{
				float t0 = pEnvelope->m_vPoints[i].m_Time / 1000.0f / EndTime;
				float t1 = pEnvelope->m_vPoints[i + 1].m_Time / 1000.0f / EndTime;

				CUIRect v;
				v.x = CurveBar.x + (t0 + (t1 - t0) * 0.5f) * CurveBar.w;
				v.y = CurveBar.y;
				v.h = CurveBar.h;
				v.w = CurveBar.h;
				v.x -= v.w / 2;
				void *pID = &pEnvelope->m_vPoints[i].m_Curvetype;
				const char *apTypeName[] = {
					"N", "L", "S", "F", "M"};
				const char *pTypeName = "Invalid";
				if(0 <= pEnvelope->m_vPoints[i].m_Curvetype && pEnvelope->m_vPoints[i].m_Curvetype < (int)std::size(apTypeName))
					pTypeName = apTypeName[pEnvelope->m_vPoints[i].m_Curvetype];
				if(DoButton_Editor(pID, pTypeName, 0, &v, 0, "Switch curve type"))
					pEnvelope->m_vPoints[i].m_Curvetype = (pEnvelope->m_vPoints[i].m_Curvetype + 1) % NUM_CURVETYPES;
			}
		}

		// render colorbar
		if(ShowColorBar)
		{
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			for(int i = 0; i < (int)pEnvelope->m_vPoints.size() - 1; i++)
			{
				float r0 = fx2f(pEnvelope->m_vPoints[i].m_aValues[0]);
				float g0 = fx2f(pEnvelope->m_vPoints[i].m_aValues[1]);
				float b0 = fx2f(pEnvelope->m_vPoints[i].m_aValues[2]);
				float a0 = fx2f(pEnvelope->m_vPoints[i].m_aValues[3]);
				float r1 = fx2f(pEnvelope->m_vPoints[i + 1].m_aValues[0]);
				float g1 = fx2f(pEnvelope->m_vPoints[i + 1].m_aValues[1]);
				float b1 = fx2f(pEnvelope->m_vPoints[i + 1].m_aValues[2]);
				float a1 = fx2f(pEnvelope->m_vPoints[i + 1].m_aValues[3]);

				IGraphics::CColorVertex Array[4] = {IGraphics::CColorVertex(0, r0, g0, b0, a0),
					IGraphics::CColorVertex(1, r1, g1, b1, a1),
					IGraphics::CColorVertex(2, r1, g1, b1, a1),
					IGraphics::CColorVertex(3, r0, g0, b0, a0)};
				Graphics()->SetColorVertex(Array, 4);

				float x0 = pEnvelope->m_vPoints[i].m_Time / 1000.0f / EndTime;
				float x1 = pEnvelope->m_vPoints[i + 1].m_Time / 1000.0f / EndTime;

				IGraphics::CQuadItem QuadItem(ColorBar.x + x0 * ColorBar.w, ColorBar.y, (x1 - x0) * ColorBar.w, ColorBar.h);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
			}
			Graphics()->QuadsEnd();
		}

		// render handles

		// keep track of last Env
		static void *s_pID = nullptr;

		static CLineInputNumber s_CurValueInput;
		static CLineInputNumber s_CurTimeInput;

		if(CurrentEnvelopeSwitched)
		{
			s_pID = nullptr;

			// update displayed text
			s_CurValueInput.SetFloat(0.0f);
			s_CurTimeInput.SetFloat(0.0f);
		}

		{
			int CurrentValue = 0, CurrentTime = 0;

			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			for(int c = 0; c < pEnvelope->m_Channels; c++)
			{
				if(!(s_ActiveChannels & (1 << c)))
					continue;

				for(size_t i = 0; i < pEnvelope->m_vPoints.size(); i++)
				{
					float x0 = pEnvelope->m_vPoints[i].m_Time / 1000.0f / EndTime;
					float y0 = (fx2f(pEnvelope->m_vPoints[i].m_aValues[c]) - Bottom) / (Top - Bottom);
					CUIRect Final;
					Final.x = View.x + x0 * View.w;
					Final.y = View.y + View.h - y0 * View.h;
					Final.x -= 2.0f;
					Final.y -= 2.0f;
					Final.w = 4.0f;
					Final.h = 4.0f;

					void *pID = &pEnvelope->m_vPoints[i].m_aValues[c];

					if(UI()->MouseInside(&Final))
						UI()->SetHotItem(pID);

					float ColorMod = 1.0f;

					if(UI()->CheckActiveItem(pID))
					{
						if(!UI()->MouseButton(0))
						{
							m_SelectedQuadEnvelope = -1;
							m_SelectedEnvelopePoint = -1;

							UI()->SetActiveItem(nullptr);
						}
						else
						{
							if(Input()->ShiftIsPressed())
							{
								if(i != 0)
								{
									if(Input()->ModifierIsPressed())
										pEnvelope->m_vPoints[i].m_Time += (int)((m_MouseDeltaX));
									else
										pEnvelope->m_vPoints[i].m_Time += (int)((m_MouseDeltaX * TimeScale) * 1000.0f);
									if(pEnvelope->m_vPoints[i].m_Time < pEnvelope->m_vPoints[i - 1].m_Time)
										pEnvelope->m_vPoints[i].m_Time = pEnvelope->m_vPoints[i - 1].m_Time + 1;
									if(i + 1 != pEnvelope->m_vPoints.size() && pEnvelope->m_vPoints[i].m_Time > pEnvelope->m_vPoints[i + 1].m_Time)
										pEnvelope->m_vPoints[i].m_Time = pEnvelope->m_vPoints[i + 1].m_Time - 1;
								}
							}
							else
							{
								if(Input()->ModifierIsPressed())
									pEnvelope->m_vPoints[i].m_aValues[c] -= f2fx(m_MouseDeltaY * 0.001f);
								else
									pEnvelope->m_vPoints[i].m_aValues[c] -= f2fx(m_MouseDeltaY * ValueScale);
							}

							m_SelectedQuadEnvelope = m_SelectedEnvelope;
							m_ShowEnvelopePreview = SHOWENV_SELECTED;
							m_SelectedEnvelopePoint = i;
							m_Map.OnModify();
						}

						ColorMod = 100.0f;
						Graphics()->SetColor(1, 1, 1, 1);
					}
					else if(UI()->HotItem() == pID)
					{
						if(UI()->MouseButton(0))
						{
							s_vSelection.clear();
							s_vSelection.push_back(i);
							UI()->SetActiveItem(pID);
							// track it
							s_pID = pID;
						}

						// remove point
						if(UI()->MouseButtonClicked(1))
						{
							if(s_pID == pID)
							{
								s_pID = nullptr;

								// update displayed text
								s_CurValueInput.SetFloat(0.0f);
								s_CurTimeInput.SetFloat(0.0f);
							}

							pEnvelope->m_vPoints.erase(pEnvelope->m_vPoints.begin() + i);
							m_Map.OnModify();
						}

						m_ShowEnvelopePreview = SHOWENV_SELECTED;
						ColorMod = 100.0f;
						Graphics()->SetColor(1, 0.75f, 0.75f, 1);
						m_pTooltip = "Left mouse to drag. Hold ctrl to be more precise. Hold shift to alter time point as well. Right click to delete.";
					}

					if(pID == s_pID && (Input()->KeyIsPressed(KEY_RETURN) || Input()->KeyIsPressed(KEY_KP_ENTER)))
					{
						if(i != 0)
						{
							pEnvelope->m_vPoints[i].m_Time = s_CurTimeInput.GetFloat() * 1000.0f;

							if(pEnvelope->m_vPoints[i].m_Time < pEnvelope->m_vPoints[i - 1].m_Time)
								pEnvelope->m_vPoints[i].m_Time = pEnvelope->m_vPoints[i - 1].m_Time + 1;
							if(i + 1 != pEnvelope->m_vPoints.size() && pEnvelope->m_vPoints[i].m_Time > pEnvelope->m_vPoints[i + 1].m_Time)
								pEnvelope->m_vPoints[i].m_Time = pEnvelope->m_vPoints[i + 1].m_Time - 1;
						}
						else
							pEnvelope->m_vPoints[i].m_Time = 0.0f;

						s_CurTimeInput.SetFloat(pEnvelope->m_vPoints[i].m_Time / 1000.0f);

						pEnvelope->m_vPoints[i].m_aValues[c] = f2fx(s_CurValueInput.GetFloat());
						s_CurValueInput.SetFloat(fx2f(pEnvelope->m_vPoints[i].m_aValues[c]));
					}

					if(UI()->CheckActiveItem(pID))
					{
						CurrentTime = pEnvelope->m_vPoints[i].m_Time;
						CurrentValue = pEnvelope->m_vPoints[i].m_aValues[c];

						// update displayed text
						s_CurValueInput.SetFloat(fx2f(CurrentValue));
						s_CurTimeInput.SetFloat(CurrentTime / 1000.0f);
					}

					if(m_SelectedQuadEnvelope == m_SelectedEnvelope && m_SelectedEnvelopePoint == (int)i)
						Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
					else
						Graphics()->SetColor(aColors[c].r * ColorMod, aColors[c].g * ColorMod, aColors[c].b * ColorMod, 1.0f);
					IGraphics::CQuadItem QuadItem(Final.x, Final.y, Final.w, Final.h);
					Graphics()->QuadsDrawTL(&QuadItem, 1);
				}
			}
			Graphics()->QuadsEnd();

			CUIRect ToolBar1;
			CUIRect ToolBar2;
			ToolBar.VSplitMid(&ToolBar1, &ToolBar2);
			ToolBar1.VSplitRight(3.0f, &ToolBar1, nullptr);
			ToolBar2.VSplitRight(3.0f, &ToolBar2, nullptr);
			if(ToolBar.w > ToolBar.h * 21)
			{
				CUIRect Label1;
				CUIRect Label2;

				ToolBar1.VSplitMid(&Label1, &ToolBar1);
				ToolBar2.VSplitMid(&Label2, &ToolBar2);
				Label1.VSplitRight(3.0f, &Label1, nullptr);
				Label2.VSplitRight(3.0f, &Label2, nullptr);

				UI()->DoLabel(&Label1, "Value:", 10.0f, TEXTALIGN_MR);
				UI()->DoLabel(&Label2, "Time (in s):", 10.0f, TEXTALIGN_MR);
			}

			DoEditBox(&s_CurValueInput, &ToolBar1, 10.0f, IGraphics::CORNER_ALL, "The value of the selected envelope point");
			DoEditBox(&s_CurTimeInput, &ToolBar2, 10.0f, IGraphics::CORNER_ALL, "The time of the selected envelope point");
		}
	}
}

void CEditor::RenderServerSettingsEditor(CUIRect View, bool ShowServerSettingsEditorLast)
{
	RenderExtraEditorDragBar(View, &m_ServerSettingsEditorSplit);

	static int s_CommandSelectedIndex = -1;

	CUIRect ToolBar;
	View.HSplitTop(20.0f, &ToolBar, &View);
	ToolBar.Margin(2.0f, &ToolBar);

	// do the toolbar
	{
		CUIRect Button;

		// command line
		ToolBar.VSplitLeft(5.0f, nullptr, &Button);
		UI()->DoLabel(&Button, "Command:", 12.0f, TEXTALIGN_ML);

		Button.VSplitLeft(70.0f, nullptr, &Button);
		Button.VSplitLeft(180.0f, &Button, nullptr);
		if(!ShowServerSettingsEditorLast) // Just activated
			UI()->SetActiveItem(&m_SettingsCommandInput);
		DoClearableEditBox(&m_SettingsCommandInput, &Button, 12.0f);

		// buttons
		ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
		static int s_AddButton = 0;
		if(DoButton_Editor(&s_AddButton, "Add", 0, &Button, 0, "Add a command to command list.") || ((Input()->KeyPress(KEY_RETURN) || Input()->KeyPress(KEY_KP_ENTER)) && UI()->LastActiveItem() == &m_SettingsCommandInput && m_Dialog == DIALOG_NONE))
		{
			if(!m_SettingsCommandInput.IsEmpty() && str_find(m_SettingsCommandInput.GetString(), " "))
			{
				bool Found = false;
				for(const auto &Setting : m_Map.m_vSettings)
					if(!str_comp(Setting.m_aCommand, m_SettingsCommandInput.GetString()))
					{
						Found = true;
						break;
					}

				if(!Found)
				{
					CEditorMap::CSetting Setting;
					str_copy(Setting.m_aCommand, m_SettingsCommandInput.GetString());
					m_Map.m_vSettings.push_back(Setting);
					s_CommandSelectedIndex = m_Map.m_vSettings.size() - 1;
				}
			}
			UI()->SetActiveItem(&m_SettingsCommandInput);
		}

		if(!m_Map.m_vSettings.empty() && s_CommandSelectedIndex >= 0 && (size_t)s_CommandSelectedIndex < m_Map.m_vSettings.size())
		{
			ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
			Button.VSplitRight(5.0f, &Button, nullptr);
			static int s_ModButton = 0;
			if(DoButton_Editor(&s_ModButton, "Mod", 0, &Button, 0, "Modify a command from the command list.") || (Input()->KeyPress(KEY_M) && UI()->LastActiveItem() != &m_SettingsCommandInput && m_Dialog == DIALOG_NONE))
			{
				if(!m_SettingsCommandInput.IsEmpty() && str_comp(m_Map.m_vSettings[s_CommandSelectedIndex].m_aCommand, m_SettingsCommandInput.GetString()) != 0 && str_find(m_SettingsCommandInput.GetString(), " "))
				{
					bool Found = false;
					int i;
					for(i = 0; i < (int)m_Map.m_vSettings.size(); i++)
						if(i != s_CommandSelectedIndex && !str_comp(m_Map.m_vSettings[i].m_aCommand, m_SettingsCommandInput.GetString()))
						{
							Found = true;
							break;
						}
					if(Found)
					{
						m_Map.m_vSettings.erase(m_Map.m_vSettings.begin() + s_CommandSelectedIndex);
						s_CommandSelectedIndex = i > s_CommandSelectedIndex ? i - 1 : i;
					}
					else
					{
						str_copy(m_Map.m_vSettings[s_CommandSelectedIndex].m_aCommand, m_SettingsCommandInput.GetString());
					}
				}
				UI()->SetActiveItem(&m_SettingsCommandInput);
			}

			ToolBar.VSplitRight(50.0f, &ToolBar, &Button);
			Button.VSplitRight(5.0f, &Button, nullptr);
			static int s_DelButton = 0;
			if(DoButton_Editor(&s_DelButton, "Del", 0, &Button, 0, "Delete a command from the command list.") || (Input()->KeyPress(KEY_DELETE) && UI()->LastActiveItem() != &m_SettingsCommandInput && m_Dialog == DIALOG_NONE))
			{
				m_Map.m_vSettings.erase(m_Map.m_vSettings.begin() + s_CommandSelectedIndex);
				if(s_CommandSelectedIndex >= (int)m_Map.m_vSettings.size())
					s_CommandSelectedIndex = m_Map.m_vSettings.size() - 1;
				if(s_CommandSelectedIndex >= 0)
					m_SettingsCommandInput.Set(m_Map.m_vSettings[s_CommandSelectedIndex].m_aCommand);
				UI()->SetActiveItem(&m_SettingsCommandInput);
			}

			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			Button.VSplitRight(5.0f, &Button, nullptr);
			static int s_DownButton = 0;
			if(s_CommandSelectedIndex < (int)m_Map.m_vSettings.size() - 1 && DoButton_Editor(&s_DownButton, "▼", 0, &Button, 0, "Move command down"))
			{
				std::swap(m_Map.m_vSettings[s_CommandSelectedIndex], m_Map.m_vSettings[s_CommandSelectedIndex + 1]);
				s_CommandSelectedIndex++;
			}

			ToolBar.VSplitRight(25.0f, &ToolBar, &Button);
			Button.VSplitRight(5.0f, &Button, nullptr);
			static int s_UpButton = 0;
			if(s_CommandSelectedIndex > 0 && DoButton_Editor(&s_UpButton, "▲", 0, &Button, 0, "Move command up"))
			{
				std::swap(m_Map.m_vSettings[s_CommandSelectedIndex], m_Map.m_vSettings[s_CommandSelectedIndex - 1]);
				s_CommandSelectedIndex--;
			}
		}
	}

	View.HSplitTop(2.0f, nullptr, &View);
	RenderBackground(View, m_CheckerTexture, 32.0f, 0.1f);

	CUIRect ListBox;
	View.Margin(1.0f, &ListBox);

	const float ButtonHeight = 15.0f;
	const float ButtonMargin = 2.0f;

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = (ButtonHeight + ButtonMargin) * 5.0f;
	s_ScrollRegion.Begin(&ListBox, &ScrollOffset, &ScrollParams);
	ListBox.y += ScrollOffset.y;

	for(size_t i = 0; i < m_Map.m_vSettings.size(); i++)
	{
		CUIRect Button;
		ListBox.HSplitTop(ButtonHeight, &Button, &ListBox);
		ListBox.HSplitTop(ButtonMargin, nullptr, &ListBox);
		Button.VSplitLeft(5.0f, nullptr, &Button);
		if(s_ScrollRegion.AddRect(Button))
		{
			if(DoButton_MenuItem(&m_Map.m_vSettings[i], m_Map.m_vSettings[i].m_aCommand, s_CommandSelectedIndex >= 0 && (size_t)s_CommandSelectedIndex == i, &Button, 0, nullptr))
			{
				s_CommandSelectedIndex = i;
				m_SettingsCommandInput.Set(m_Map.m_vSettings[i].m_aCommand);
				UI()->SetActiveItem(&m_SettingsCommandInput);
			}
		}
	}

	s_ScrollRegion.End();
}

void CEditor::RenderExtraEditorDragBar(CUIRect View, float *pSplit)
{
	const CUIRect DragBar = {
		View.x,
		View.y - 2.0f, // use margin
		View.w,
		22.0f,
	};

	enum EDragOperation
	{
		OP_NONE,
		OP_DRAGGING,
		OP_CLICKED
	};
	static EDragOperation s_Operation = OP_NONE;
	static float s_InitialMouseY = 0.0f;
	static float s_InitialMouseOffsetY = 0.0f;

	bool Clicked;
	bool Abrupted;
	if(int Result = DoButton_DraggableEx(&s_Operation, "", 8, &DragBar, &Clicked, &Abrupted, 0, "Change the size of the editor by dragging."))
	{
		if(s_Operation == OP_NONE && Result == 1)
		{
			s_InitialMouseY = UI()->MouseY();
			s_InitialMouseOffsetY = UI()->MouseY() - DragBar.y;
			s_Operation = OP_CLICKED;
		}

		if(Clicked || Abrupted)
			s_Operation = OP_NONE;

		if(s_Operation == OP_CLICKED && absolute(UI()->MouseY() - s_InitialMouseY) > 5.0f)
			s_Operation = OP_DRAGGING;

		if(s_Operation == OP_DRAGGING)
			*pSplit = clamp(s_InitialMouseOffsetY + View.y + View.h - UI()->MouseY(), 100.0f, 400.0f);
	}
}

void CEditor::RenderMenubar(CUIRect MenuBar)
{
	SPopupMenuProperties PopupProperties;
	PopupProperties.m_Corners = IGraphics::CORNER_R | IGraphics::CORNER_B;

	CUIRect FileButton;
	static int s_FileButton = 0;
	MenuBar.VSplitLeft(60.0f, &FileButton, &MenuBar);
	if(DoButton_Menu(&s_FileButton, "File", 0, &FileButton, 0, nullptr))
	{
		static SPopupMenuId s_PopupMenuFileId;
		UI()->DoPopupMenu(&s_PopupMenuFileId, FileButton.x, FileButton.y + FileButton.h - 1.0f, 120.0f, 174.0f, this, PopupMenuFile, PopupProperties);
	}

	MenuBar.VSplitLeft(5.0f, nullptr, &MenuBar);

	CUIRect ToolsButton;
	static int s_ToolsButton = 0;
	MenuBar.VSplitLeft(60.0f, &ToolsButton, &MenuBar);
	if(DoButton_Menu(&s_ToolsButton, "Tools", 0, &ToolsButton, 0, nullptr))
	{
		static SPopupMenuId s_PopupMenuToolsId;
		UI()->DoPopupMenu(&s_PopupMenuToolsId, ToolsButton.x, ToolsButton.y + ToolsButton.h - 1.0f, 200.0f, 50.0f, this, PopupMenuTools, PopupProperties);
	}

	MenuBar.VSplitLeft(5.0f, nullptr, &MenuBar);

	CUIRect SettingsButton;
	static int s_SettingsButton = 0;
	MenuBar.VSplitLeft(60.0f, &SettingsButton, &MenuBar);
	if(DoButton_Menu(&s_SettingsButton, "Settings", 0, &SettingsButton, 0, nullptr))
	{
		static SPopupMenuId s_PopupMenuEntitiesId;
		UI()->DoPopupMenu(&s_PopupMenuEntitiesId, SettingsButton.x, SettingsButton.y + SettingsButton.h - 1.0f, 200.0f, 64.0f, this, PopupMenuSettings, PopupProperties);
	}

	CUIRect ChangedIndicator, Info, Close;
	MenuBar.VSplitLeft(5.0f, nullptr, &MenuBar);
	MenuBar.VSplitLeft(MenuBar.h, &ChangedIndicator, &MenuBar);
	MenuBar.VSplitRight(20.0f, &MenuBar, &Close);
	Close.VSplitLeft(5.0f, nullptr, &Close);
	MenuBar.VSplitLeft(MenuBar.w * 0.6f, &MenuBar, &Info);

	if(m_Map.m_Modified)
	{
		TextRender()->SetCurFont(TextRender()->GetFont(TEXT_FONT_ICON_FONT));
		TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);
		UI()->DoLabel(&ChangedIndicator, FONT_ICON_CIRCLE, 8.0f, TEXTALIGN_MC);
		TextRender()->SetRenderFlags(0);
		TextRender()->SetCurFont(nullptr);
		static int s_ChangedIndicator;
		DoButton_Editor_Common(&s_ChangedIndicator, "", 0, &ChangedIndicator, 0, "This map has unsaved changes"); // just for the tooltip, result unused
	}

	char aBuf[IO_MAX_PATH_LENGTH + 32];
	str_format(aBuf, sizeof(aBuf), "File: %s", m_aFileName);
	UI()->DoLabel(&MenuBar, aBuf, 10.0f, TEXTALIGN_ML);

	char aTimeStr[6];
	str_timestamp_format(aTimeStr, sizeof(aTimeStr), "%H:%M");

	str_format(aBuf, sizeof(aBuf), "X: %.1f, Y: %.1f, Z: %.1f, A: %.1f, G: %i  %s", UI()->MouseWorldX() / 32.0f, UI()->MouseWorldY() / 32.0f, m_Zoom, m_AnimateSpeed, m_GridFactor, aTimeStr);
	UI()->DoLabel(&Info, aBuf, 10.0f, TEXTALIGN_MR);

	static int s_CloseButton = 0;
	if(DoButton_Editor(&s_CloseButton, "×", 0, &Close, 0, "Exits from the editor") || (m_Dialog == DIALOG_NONE && !UI()->IsPopupOpen() && !m_PopupEventActivated && Input()->KeyPress(KEY_ESCAPE)))
		g_Config.m_ClEditor = 0;
}

void CEditor::Render()
{
	// basic start
	Graphics()->Clear(0.0f, 0.0f, 0.0f);
	CUIRect View = *UI()->Screen();
	UI()->MapScreen();

	float Width = View.w;
	float Height = View.h;

	// reset tip
	m_pTooltip = nullptr;

	if(m_EditBoxActive)
		--m_EditBoxActive;

	// render checker
	RenderBackground(View, m_CheckerTexture, 32.0f, 1.0f);

	CUIRect MenuBar, CModeBar, ToolBar, StatusBar, ExtraEditor, ToolBox;
	m_ShowPicker = Input()->KeyIsPressed(KEY_SPACE) && m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && UI()->LastActiveItem() != &m_SettingsCommandInput && m_vSelectedLayers.size() == 1;

	if(m_GuiActive)
	{
		View.HSplitTop(16.0f, &MenuBar, &View);
		View.HSplitTop(53.0f, &ToolBar, &View);
		View.VSplitLeft(100.0f, &ToolBox, &View);
		View.HSplitBottom(16.0f, &View, &StatusBar);

		if(m_ShowEnvelopeEditor && !m_ShowPicker)
			View.HSplitBottom(m_EnvelopeEditorSplit, &View, &ExtraEditor);

		if(m_ShowServerSettingsEditor && !m_ShowPicker)
			View.HSplitBottom(m_ServerSettingsEditorSplit, &View, &ExtraEditor);
	}
	else
	{
		// hack to get keyboard inputs from toolbar even when GUI is not active
		ToolBar.x = -100;
		ToolBar.y = -100;
		ToolBar.w = 50;
		ToolBar.h = 50;
	}

	//	a little hack for now
	if(m_Mode == MODE_LAYERS)
		DoMapEditor(View);

	// do zooming
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0)
	{
		if(Input()->KeyPress(KEY_KP_MINUS))
			ChangeZoom(50.0f);
		if(Input()->KeyPress(KEY_KP_PLUS))
			ChangeZoom(-50.0f);
		if(Input()->KeyPress(KEY_KP_MULTIPLY))
		{
			m_EditorOffsetX = 0;
			m_EditorOffsetY = 0;
			SetZoom(100.0f);
		}
	}

	for(int i = KEY_1; i <= KEY_0; i++)
	{
		if(m_Dialog != DIALOG_NONE || m_EditBoxActive != 0)
			break;

		if(Input()->KeyPress(i))
		{
			int Slot = i - KEY_1;
			if(Input()->ModifierIsPressed() && !m_Brush.IsEmpty())
			{
				dbg_msg("editor", "saving current brush to %d", Slot);
				if(m_apSavedBrushes[Slot])
				{
					CLayerGroup *pPrev = m_apSavedBrushes[Slot];
					for(auto &pLayer : pPrev->m_vpLayers)
					{
						if(pLayer->m_BrushRefCount == 1)
							delete pLayer;
						else
							pLayer->m_BrushRefCount--;
					}
				}
				delete m_apSavedBrushes[Slot];
				m_apSavedBrushes[Slot] = new CLayerGroup(m_Brush);

				for(auto &pLayer : m_apSavedBrushes[Slot]->m_vpLayers)
					pLayer->m_BrushRefCount++;
			}
			else if(m_apSavedBrushes[Slot])
			{
				dbg_msg("editor", "loading brush from slot %d", Slot);

				CLayerGroup *pNew = m_apSavedBrushes[Slot];
				for(auto &pLayer : pNew->m_vpLayers)
					pLayer->m_BrushRefCount++;

				m_Brush = *pNew;
			}
		}
	}

	float Brightness = 0.25f;

	if(m_GuiActive)
	{
		RenderBackground(MenuBar, m_BackgroundTexture, 128.0f, Brightness * 0);
		MenuBar.Margin(2.0f, &MenuBar);

		RenderBackground(ToolBox, m_BackgroundTexture, 128.0f, Brightness);
		ToolBox.Margin(2.0f, &ToolBox);

		RenderBackground(ToolBar, m_BackgroundTexture, 128.0f, Brightness);
		ToolBar.Margin(2.0f, &ToolBar);
		ToolBar.VSplitLeft(100.0f, &CModeBar, &ToolBar);

		RenderBackground(StatusBar, m_BackgroundTexture, 128.0f, Brightness);
		StatusBar.Margin(2.0f, &StatusBar);
	}

	// show mentions
	if(m_GuiActive && m_Mentions)
	{
		char aBuf[64];
		if(m_Mentions == 1)
		{
			str_copy(aBuf, Localize("1 new mention"));
		}
		else if(m_Mentions <= 9)
		{
			str_format(aBuf, sizeof(aBuf), Localize("%d new mentions"), m_Mentions);
		}
		else
		{
			str_copy(aBuf, Localize("9+ new mentions"));
		}

		TextRender()->TextColor(1.0f, 0.0f, 0.0f, 1.0f);
		TextRender()->Text(5.0f, 27.0f, 10.0f, aBuf, -1.0f);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// do the toolbar
	if(m_Mode == MODE_LAYERS)
		DoToolbarLayers(ToolBar);
	else if(m_Mode == MODE_SOUNDS)
		DoToolbarSounds(ToolBar);

	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0)
	{
		const bool ModPressed = Input()->ModifierIsPressed();
		const bool ShiftPressed = Input()->ShiftIsPressed();
		const bool AltPressed = Input()->AltIsPressed();
		// ctrl+n to create new map
		if(Input()->KeyPress(KEY_N) && ModPressed)
		{
			if(HasUnsavedData())
			{
				if(!m_PopupEventWasActivated)
				{
					m_PopupEventType = POPEVENT_NEW;
					m_PopupEventActivated = true;
				}
			}
			else
			{
				Reset();
				m_aFileName[0] = 0;
			}
		}
		// ctrl+a to append map
		if(Input()->KeyPress(KEY_A) && ModPressed)
		{
			InvokeFileDialog(IStorage::TYPE_ALL, FILETYPE_MAP, "Append map", "Append", "maps", "", CallbackAppendMap, this);
		}
		// ctrl+o or ctrl+l to open
		if((Input()->KeyPress(KEY_O) || Input()->KeyPress(KEY_L)) && ModPressed)
		{
			if(ShiftPressed)
			{
				if(HasUnsavedData())
				{
					if(!m_PopupEventWasActivated)
					{
						m_PopupEventType = POPEVENT_LOADCURRENT;
						m_PopupEventActivated = true;
					}
				}
				else
				{
					LoadCurrentMap();
				}
			}
			else
			{
				if(HasUnsavedData())
				{
					if(!m_PopupEventWasActivated)
					{
						m_PopupEventType = POPEVENT_LOAD;
						m_PopupEventActivated = true;
					}
				}
				else
				{
					InvokeFileDialog(IStorage::TYPE_ALL, FILETYPE_MAP, "Load map", "Load", "maps", "", CallbackOpenMap, this);
				}
			}
		}

		// ctrl+shift+alt+s to save copy
		if(Input()->KeyPress(KEY_S) && ModPressed && ShiftPressed && AltPressed)
			InvokeFileDialog(IStorage::TYPE_SAVE, FILETYPE_MAP, "Save map", "Save", "maps", "", CallbackSaveCopyMap, this);
		// ctrl+shift+s to save as
		else if(Input()->KeyPress(KEY_S) && ModPressed && ShiftPressed)
			InvokeFileDialog(IStorage::TYPE_SAVE, FILETYPE_MAP, "Save map", "Save", "maps", "", CallbackSaveMap, this);
		// ctrl+s to save
		else if(Input()->KeyPress(KEY_S) && ModPressed)
		{
			if(m_aFileName[0] && m_ValidSaveFilename)
			{
				if(!m_PopupEventWasActivated)
				{
					str_copy(m_aFileSaveName, m_aFileName);
					CallbackSaveMap(m_aFileSaveName, IStorage::TYPE_SAVE, this);
				}
			}
			else
				InvokeFileDialog(IStorage::TYPE_SAVE, FILETYPE_MAP, "Save map", "Save", "maps", "", CallbackSaveMap, this);
		}
	}

	if(m_GuiActive)
	{
		if(!m_ShowPicker)
		{
			if(m_ShowEnvelopeEditor || m_ShowServerSettingsEditor)
			{
				RenderBackground(ExtraEditor, m_BackgroundTexture, 128.0f, Brightness);
				ExtraEditor.Margin(2.0f, &ExtraEditor);
			}
		}

		if(m_Mode == MODE_LAYERS)
			RenderLayers(ToolBox);
		else if(m_Mode == MODE_IMAGES)
		{
			RenderImagesList(ToolBox);
			RenderSelectedImage(View);
		}
		else if(m_Mode == MODE_SOUNDS)
			RenderSounds(ToolBox);
	}

	UI()->MapScreen();

	if(m_GuiActive)
	{
		RenderMenubar(MenuBar);

		RenderModebar(CModeBar);
		if(!m_ShowPicker)
		{
			if(m_ShowEnvelopeEditor)
				RenderEnvelopeEditor(ExtraEditor);
			static bool s_ShowServerSettingsEditorLast = false;
			if(m_ShowServerSettingsEditor)
			{
				RenderServerSettingsEditor(ExtraEditor, s_ShowServerSettingsEditorLast);
			}
			s_ShowServerSettingsEditorLast = m_ShowServerSettingsEditor;
		}
	}

	if(m_Dialog == DIALOG_FILE)
	{
		static int s_NullUiTarget = 0;
		UI()->SetHotItem(&s_NullUiTarget);
		RenderFileDialog();
	}

	if(m_PopupEventActivated)
	{
		static SPopupMenuId s_PopupEventId;
		constexpr float PopupWidth = 400.0f;
		constexpr float PopupHeight = 150.0f;
		UI()->DoPopupMenu(&s_PopupEventId, Width / 2.0f - PopupWidth / 2.0f, Height / 2.0f - PopupHeight / 2.0f, PopupWidth, PopupHeight, this, PopupEvent);
		m_PopupEventActivated = false;
		m_PopupEventWasActivated = true;
	}

	if(m_Dialog == DIALOG_NONE && !UI()->IsPopupHovered() && (!m_GuiActive || UI()->MouseInside(&View)))
	{
		if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
			ChangeZoom(20.0f);
		if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
			ChangeZoom(-20.0f);
	}

	UpdateZoom();

	// Popup menus must be rendered before the statusbar, because UI elements in
	// popup menus can set tooltips, which are rendered in the status bar.
	UI()->RenderPopupMenus();

	if(m_GuiActive)
		RenderStatusbar(StatusBar);

	RenderPressedKeys(View);
	RenderSavingIndicator(View);
	RenderMousePointer();
}

void CEditor::RenderPressedKeys(CUIRect View)
{
	if(!g_Config.m_EdShowkeys)
		return;

	UI()->MapScreen();
	CTextCursor Cursor;
	TextRender()->SetCursor(&Cursor, View.x + 10, View.y + View.h - 24 - 10, 24.0f, TEXTFLAG_RENDER);

	int NKeys = 0;
	for(int i = 0; i < KEY_LAST; i++)
	{
		if(Input()->KeyIsPressed(i))
		{
			if(NKeys)
				TextRender()->TextEx(&Cursor, " + ", -1);
			TextRender()->TextEx(&Cursor, Input()->KeyName(i), -1);
			NKeys++;
		}
	}
}

void CEditor::RenderSavingIndicator(CUIRect View)
{
	if(m_lpWriterFinishJobs.empty())
		return;

	UI()->MapScreen();
	CUIRect Label;
	View.Margin(20.0f, &Label);
	UI()->DoLabel(&Label, "Saving…", 24.0f, TEXTALIGN_BR);
}

void CEditor::RenderMousePointer()
{
	if(!m_ShowMousePointer)
		return;

	Graphics()->WrapClamp();
	Graphics()->TextureSet(m_CursorTexture);
	Graphics()->QuadsBegin();
	if(ms_pUiGotContext == UI()->HotItem())
		Graphics()->SetColor(1, 0, 0, 1);
	IGraphics::CQuadItem QuadItem(UI()->MouseX(), UI()->MouseY(), 16.0f, 16.0f);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
}

void CEditor::Reset(bool CreateDefault)
{
	m_Map.Clean();

	mem_zero(m_apSavedBrushes, sizeof m_apSavedBrushes);

	// create default layers
	if(CreateDefault)
	{
		m_EditorWasUsedBefore = true;
		m_Map.CreateDefault(GetEntitiesTexture());
	}

	SelectGameLayer();
	m_vSelectedQuads.clear();
	m_SelectedPoints = 0;
	m_SelectedEnvelope = 0;
	m_SelectedImage = 0;
	m_SelectedSound = 0;
	m_SelectedSource = -1;

	m_WorldOffsetX = 0;
	m_WorldOffsetY = 0;
	m_EditorOffsetX = 0.0f;
	m_EditorOffsetY = 0.0f;

	m_Zoom = 200.0f;
	m_Zooming = false;
	m_WorldZoom = 1.0f;

	m_MouseDeltaX = 0;
	m_MouseDeltaY = 0;
	m_MouseDeltaWx = 0;
	m_MouseDeltaWy = 0;

	m_Map.m_Modified = false;
	m_Map.m_ModifiedAuto = false;
	m_Map.m_LastModifiedTime = -1.0f;
	m_Map.m_LastSaveTime = Client()->GlobalTime();
	m_Map.m_LastAutosaveUpdateTime = -1.0f;

	m_ShowEnvelopePreview = SHOWENV_NONE;
	m_ShiftBy = 1;
}

int CEditor::GetLineDistance() const
{
	if(m_Zoom <= 100.0f)
		return 16;
	else if(m_Zoom <= 250.0f)
		return 32;
	else if(m_Zoom <= 450.0f)
		return 64;
	else if(m_Zoom <= 850.0f)
		return 128;
	else if(m_Zoom <= 1550.0f)
		return 256;
	else
		return 512;
}

void CEditor::SetZoom(float Target)
{
	Target = clamp(Target, MinZoomLevel(), MaxZoomLevel());

	const float Now = Client()->GlobalTime();
	float Current = m_Zoom;
	float Derivative = 0.0f;
	if(m_Zooming)
	{
		const float Progress = ZoomProgress(Now);
		Current = m_ZoomSmoothing.Evaluate(Progress);
		Derivative = m_ZoomSmoothing.Derivative(Progress);
	}

	m_ZoomSmoothingTarget = Target;
	m_ZoomSmoothing = CCubicBezier::With(Current, Derivative, 0.0f, m_ZoomSmoothingTarget);
	m_ZoomSmoothingStart = Now;
	m_ZoomSmoothingEnd = Now + g_Config.m_EdSmoothZoomTime / 1000.0f;

	m_Zooming = true;
}

void CEditor::ChangeZoom(float Amount)
{
	const float CurrentTarget = m_Zooming ? m_ZoomSmoothingTarget : m_Zoom;
	SetZoom(CurrentTarget + Amount);
}

void CEditor::ZoomMouseTarget(float ZoomFactor)
{
	// zoom to the current mouse position
	// get absolute mouse position
	float aPoints[4];
	RenderTools()->MapScreenToWorld(
		m_WorldOffsetX, m_WorldOffsetY,
		100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Graphics()->ScreenAspect(), m_WorldZoom, aPoints);

	float WorldWidth = aPoints[2] - aPoints[0];
	float WorldHeight = aPoints[3] - aPoints[1];

	float Mwx = aPoints[0] + WorldWidth * (UI()->MouseX() / UI()->Screen()->w);
	float Mwy = aPoints[1] + WorldHeight * (UI()->MouseY() / UI()->Screen()->h);

	// adjust camera
	m_WorldOffsetX += (Mwx - m_WorldOffsetX) * (1.0f - ZoomFactor);
	m_WorldOffsetY += (Mwy - m_WorldOffsetY) * (1.0f - ZoomFactor);
}

void CEditor::UpdateZoom()
{
	if(m_Zooming)
	{
		const float Time = Client()->GlobalTime();
		const float OldLevel = m_Zoom;
		if(Time >= m_ZoomSmoothingEnd)
		{
			m_Zoom = m_ZoomSmoothingTarget;
			m_Zooming = false;
		}
		else
		{
			m_Zoom = m_ZoomSmoothing.Evaluate(ZoomProgress(Time));
			if((OldLevel < m_ZoomSmoothingTarget && m_Zoom > m_ZoomSmoothingTarget) || (OldLevel > m_ZoomSmoothingTarget && m_Zoom < m_ZoomSmoothingTarget))
			{
				m_Zoom = m_ZoomSmoothingTarget;
				m_Zooming = false;
			}
		}
		m_Zoom = clamp(m_Zoom, MinZoomLevel(), MaxZoomLevel());
		if(g_Config.m_EdZoomTarget)
			ZoomMouseTarget(m_Zoom / OldLevel);
	}

	m_WorldZoom = m_Zoom / 100.0f;
}

float CEditor::MinZoomLevel() const
{
	return 10.0f;
}

float CEditor::MaxZoomLevel() const
{
	return g_Config.m_EdLimitMaxZoomLevel ? 2000.0f : std::numeric_limits<float>::max();
}

float CEditor::ZoomProgress(float CurrentTime) const
{
	return (CurrentTime - m_ZoomSmoothingStart) / (m_ZoomSmoothingEnd - m_ZoomSmoothingStart);
}

void CEditor::Goto(float X, float Y)
{
	m_WorldOffsetX = X * 32;
	m_WorldOffsetY = Y * 32;
}

void CEditorMap::OnModify()
{
	m_Modified = true;
	m_ModifiedAuto = true;
	m_LastModifiedTime = m_pEditor->Client()->GlobalTime();
}

void CEditorMap::DeleteEnvelope(int Index)
{
	if(Index < 0 || Index >= (int)m_vpEnvelopes.size())
		return;

	OnModify();

	VisitEnvelopeReferences([Index](int &ElementIndex) {
		if(ElementIndex == Index)
			ElementIndex = -1;
		else if(ElementIndex > Index)
			ElementIndex--;
	});

	m_vpEnvelopes.erase(m_vpEnvelopes.begin() + Index);
}

void CEditorMap::SwapEnvelopes(int Index0, int Index1)
{
	if(Index0 < 0 || Index0 >= (int)m_vpEnvelopes.size())
		return;
	if(Index1 < 0 || Index1 >= (int)m_vpEnvelopes.size())
		return;
	if(Index0 == Index1)
		return;

	OnModify();

	VisitEnvelopeReferences([Index0, Index1](int &ElementIndex) {
		if(ElementIndex == Index0)
			ElementIndex = Index1;
		else if(ElementIndex == Index1)
			ElementIndex = Index0;
	});

	std::swap(m_vpEnvelopes[Index0], m_vpEnvelopes[Index1]);
}

template<typename F>
void CEditorMap::VisitEnvelopeReferences(F &&Visitor)
{
	for(auto &pGroup : m_vpGroups)
	{
		for(auto &pLayer : pGroup->m_vpLayers)
		{
			if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				CLayerQuads *pLayerQuads = static_cast<CLayerQuads *>(pLayer);
				for(auto &Quad : pLayerQuads->m_vQuads)
				{
					Visitor(Quad.m_PosEnv);
					Visitor(Quad.m_ColorEnv);
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_TILES)
			{
				CLayerTiles *pLayerTiles = static_cast<CLayerTiles *>(pLayer);
				Visitor(pLayerTiles->m_ColorEnv);
			}
			else if(pLayer->m_Type == LAYERTYPE_SOUNDS)
			{
				CLayerSounds *pLayerSounds = static_cast<CLayerSounds *>(pLayer);
				for(auto &Source : pLayerSounds->m_vSources)
				{
					Visitor(Source.m_PosEnv);
					Visitor(Source.m_SoundEnv);
				}
			}
		}
	}
}

void CEditorMap::MakeGameLayer(CLayer *pLayer)
{
	m_pGameLayer = (CLayerGame *)pLayer;
	m_pGameLayer->m_pEditor = m_pEditor;
	m_pGameLayer->m_Texture = m_pEditor->GetEntitiesTexture();
}

void CEditorMap::MakeGameGroup(CLayerGroup *pGroup)
{
	m_pGameGroup = pGroup;
	m_pGameGroup->m_GameGroup = true;
	str_copy(m_pGameGroup->m_aName, "Game");
}

void CEditorMap::Clean()
{
	for(auto &pGroup : m_vpGroups)
	{
		DeleteAll(pGroup->m_vpLayers);
	}
	DeleteAll(m_vpGroups);
	DeleteAll(m_vpEnvelopes);
	DeleteAll(m_vpImages);
	DeleteAll(m_vpSounds);

	m_MapInfo.Reset();
	m_MapInfoTmp.Reset();

	m_vSettings.clear();

	m_pGameLayer = nullptr;
	m_pGameGroup = nullptr;

	m_Modified = false;
	m_ModifiedAuto = false;

	m_pTeleLayer = nullptr;
	m_pSpeedupLayer = nullptr;
	m_pFrontLayer = nullptr;
	m_pSwitchLayer = nullptr;
	m_pTuneLayer = nullptr;
}

void CEditorMap::CreateDefault(IGraphics::CTextureHandle EntitiesTexture)
{
	// add background
	CLayerGroup *pGroup = NewGroup();
	pGroup->m_ParallaxX = 0;
	pGroup->m_ParallaxY = 0;
	pGroup->m_CustomParallaxZoom = 0;
	pGroup->m_ParallaxZoom = 0;
	CLayerQuads *pLayer = new CLayerQuads;
	pLayer->m_pEditor = m_pEditor;
	CQuad *pQuad = pLayer->NewQuad(0, 0, 1600, 1200);
	pQuad->m_aColors[0].r = pQuad->m_aColors[1].r = 94;
	pQuad->m_aColors[0].g = pQuad->m_aColors[1].g = 132;
	pQuad->m_aColors[0].b = pQuad->m_aColors[1].b = 174;
	pQuad->m_aColors[2].r = pQuad->m_aColors[3].r = 204;
	pQuad->m_aColors[2].g = pQuad->m_aColors[3].g = 232;
	pQuad->m_aColors[2].b = pQuad->m_aColors[3].b = 255;
	pGroup->AddLayer(pLayer);

	// add game layer and reset front, tele, speedup, tune and switch layer pointers
	MakeGameGroup(NewGroup());
	MakeGameLayer(new CLayerGame(50, 50));
	m_pGameGroup->AddLayer(m_pGameLayer);

	m_pFrontLayer = nullptr;
	m_pTeleLayer = nullptr;
	m_pSpeedupLayer = nullptr;
	m_pSwitchLayer = nullptr;
	m_pTuneLayer = nullptr;
}

int CEditor::GetTextureUsageFlag()
{
	return Graphics()->HasTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
}

IGraphics::CTextureHandle CEditor::GetFrontTexture()
{
	int TextureLoadFlag = GetTextureUsageFlag();

	if(!m_FrontTexture.IsValid())
		m_FrontTexture = Graphics()->LoadTexture("editor/front.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, TextureLoadFlag);
	return m_FrontTexture;
}

IGraphics::CTextureHandle CEditor::GetTeleTexture()
{
	int TextureLoadFlag = GetTextureUsageFlag();
	if(!m_TeleTexture.IsValid())
		m_TeleTexture = Graphics()->LoadTexture("editor/tele.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, TextureLoadFlag);
	return m_TeleTexture;
}

IGraphics::CTextureHandle CEditor::GetSpeedupTexture()
{
	int TextureLoadFlag = GetTextureUsageFlag();
	if(!m_SpeedupTexture.IsValid())
		m_SpeedupTexture = Graphics()->LoadTexture("editor/speedup.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, TextureLoadFlag);
	return m_SpeedupTexture;
}

IGraphics::CTextureHandle CEditor::GetSwitchTexture()
{
	int TextureLoadFlag = GetTextureUsageFlag();
	if(!m_SwitchTexture.IsValid())
		m_SwitchTexture = Graphics()->LoadTexture("editor/switch.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, TextureLoadFlag);
	return m_SwitchTexture;
}

IGraphics::CTextureHandle CEditor::GetTuneTexture()
{
	int TextureLoadFlag = GetTextureUsageFlag();
	if(!m_TuneTexture.IsValid())
		m_TuneTexture = Graphics()->LoadTexture("editor/tune.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, TextureLoadFlag);
	return m_TuneTexture;
}

IGraphics::CTextureHandle CEditor::GetEntitiesTexture()
{
	int TextureLoadFlag = GetTextureUsageFlag();
	if(!m_EntitiesTexture.IsValid())
		m_EntitiesTexture = Graphics()->LoadTexture("editor/entities/DDNet.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, TextureLoadFlag);
	return m_EntitiesTexture;
}

void CEditor::Init()
{
	m_pInput = Kernel()->RequestInterface<IInput>();
	m_pClient = Kernel()->RequestInterface<IClient>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pGraphics = Kernel()->RequestInterface<IGraphics>();
	m_pTextRender = Kernel()->RequestInterface<ITextRender>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pSound = Kernel()->RequestInterface<ISound>();
	m_UI.Init(Kernel());
	m_UI.SetPopupMenuClosedCallback([this]() {
		m_PopupEventWasActivated = false;
	});
	m_RenderTools.Init(m_pGraphics, m_pTextRender);
	m_Map.m_pEditor = this;

	m_CheckerTexture = Graphics()->LoadTexture("editor/checker.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, 0);
	m_BackgroundTexture = Graphics()->LoadTexture("editor/background.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, 0);
	m_CursorTexture = Graphics()->LoadTexture("editor/cursor.png", IStorage::TYPE_ALL, CImageInfo::FORMAT_AUTO, 0);

	m_TilesetPicker.m_pEditor = this;
	m_TilesetPicker.MakePalette();
	m_TilesetPicker.m_Readonly = true;

	m_QuadsetPicker.m_pEditor = this;
	m_QuadsetPicker.NewQuad(0, 0, 64, 64);
	m_QuadsetPicker.m_Readonly = true;

	m_Brush.m_pMap = &m_Map;

	Reset(false);

	ResetMenuBackgroundPositions();
	m_vpMenuBackgroundPositionNames.resize(CMenuBackground::NUM_POS);
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_START] = "start";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_INTERNET] = "browser(internet)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_LAN] = "browser(lan)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_DEMOS] = "demos";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_NEWS] = "news";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_FAVORITES] = "favorites";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_LANGUAGE] = "settings(language)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_GENERAL] = "settings(general)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_PLAYER] = "settings(player)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_TEE] = "settings(tee)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_APPEARANCE] = "settings(appearance)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_CONTROLS] = "settings(controls)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_GRAPHICS] = "settings(graphics)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_SOUND] = "settings(sound)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_DDNET] = "settings(ddnet)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_ASSETS] = "settings(assets)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_CUSTOM0] = "custom(ddnet)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_CUSTOM1] = "custom(kog)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_CUSTOM2] = "custom(3)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_BROWSER_CUSTOM3] = "custom(4)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_RESERVED0] = "reserved settings(1)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_SETTINGS_RESERVED1] = "reserved settings(2)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_RESERVED0] = "reserved(1)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_RESERVED1] = "reserved(2)";
	m_vpMenuBackgroundPositionNames[CMenuBackground::POS_RESERVED2] = "reserved(3)";
}

void CEditor::PlaceBorderTiles()
{
	CLayerTiles *pT = (CLayerTiles *)GetSelectedLayerType(0, LAYERTYPE_TILES);

	for(int i = 0; i < pT->m_Width * 2; ++i)
		pT->m_pTiles[i].m_Index = 1;

	for(int i = 0; i < pT->m_Width * pT->m_Height; ++i)
	{
		if(i % pT->m_Width < 2 || i % pT->m_Width > pT->m_Width - 3)
			pT->m_pTiles[i].m_Index = 1;
	}

	for(int i = (pT->m_Width * (pT->m_Height - 2)); i < pT->m_Width * pT->m_Height; ++i)
		pT->m_pTiles[i].m_Index = 1;
}

void CEditor::HandleCursorMovement()
{
	static float s_MouseX = 0.0f;
	static float s_MouseY = 0.0f;

	float MouseRelX = 0.0f, MouseRelY = 0.0f;
	IInput::ECursorType CursorType = Input()->CursorRelative(&MouseRelX, &MouseRelY);
	if(CursorType != IInput::CURSOR_NONE)
		UI()->ConvertMouseMove(&MouseRelX, &MouseRelY, CursorType);

	m_MouseDeltaX += MouseRelX;
	m_MouseDeltaY += MouseRelY;

	if(!UI()->CheckMouseLock())
	{
		s_MouseX = clamp<float>(s_MouseX + MouseRelX, 0.0f, Graphics()->WindowWidth());
		s_MouseY = clamp<float>(s_MouseY + MouseRelY, 0.0f, Graphics()->WindowHeight());
	}

	// update positions for ui, but only update ui when rendering
	m_MouseX = UI()->Screen()->w * ((float)s_MouseX / Graphics()->WindowWidth());
	m_MouseY = UI()->Screen()->h * ((float)s_MouseY / Graphics()->WindowHeight());

	// fix correct world x and y
	CLayerGroup *pGroup = GetSelectedGroup();
	if(pGroup)
	{
		float aPoints[4];
		pGroup->Mapping(aPoints);

		float WorldWidth = aPoints[2] - aPoints[0];
		float WorldHeight = aPoints[3] - aPoints[1];

		m_MouseWScale = WorldWidth / Graphics()->WindowWidth();

		m_MouseWorldX = aPoints[0] + WorldWidth * (s_MouseX / Graphics()->WindowWidth());
		m_MouseWorldY = aPoints[1] + WorldHeight * (s_MouseY / Graphics()->WindowHeight());
		m_MouseDeltaWx = m_MouseDeltaX * (WorldWidth / Graphics()->WindowWidth());
		m_MouseDeltaWy = m_MouseDeltaY * (WorldHeight / Graphics()->WindowHeight());
	}
	else
	{
		m_MouseWorldX = 0.0f;
		m_MouseWorldY = 0.0f;
	}

	for(CLayerGroup *pGameGroup : m_Map.m_vpGroups)
	{
		if(!pGameGroup->m_GameGroup)
			continue;

		float aPoints[4];
		pGameGroup->Mapping(aPoints);

		float WorldWidth = aPoints[2] - aPoints[0];
		float WorldHeight = aPoints[3] - aPoints[1];

		m_MouseWorldNoParaX = aPoints[0] + WorldWidth * (s_MouseX / Graphics()->WindowWidth());
		m_MouseWorldNoParaY = aPoints[1] + WorldHeight * (s_MouseY / Graphics()->WindowHeight());
	}
}

void CEditor::HandleAutosave()
{
	const float Time = Client()->GlobalTime();
	const float LastAutosaveUpdateTime = m_Map.m_LastAutosaveUpdateTime;
	m_Map.m_LastAutosaveUpdateTime = Time;

	if(g_Config.m_EdAutosaveInterval == 0)
		return; // autosave disabled
	if(!m_Map.m_ModifiedAuto || m_Map.m_LastModifiedTime < 0.0f)
		return; // no unsaved changes

	// Add time to autosave timer if the editor was disabled for more than 10 seconds,
	// to prevent autosave from immediately activating when the editor is activated
	// after being deactivated for some time.
	if(LastAutosaveUpdateTime >= 0.0f && Time - LastAutosaveUpdateTime > 10.0f)
	{
		m_Map.m_LastSaveTime += Time - LastAutosaveUpdateTime;
	}

	// Check if autosave timer has expired.
	if(m_Map.m_LastSaveTime >= Time || Time - m_Map.m_LastSaveTime < 60 * g_Config.m_EdAutosaveInterval)
		return;

	// Wait for 5 seconds of no modification before saving, to prevent autosave
	// from immediately activating when a map is first modified or while user is
	// modifying the map, but don't delay the autosave for more than 1 minute.
	if(Time - m_Map.m_LastModifiedTime < 5.0f && Time - m_Map.m_LastSaveTime < 60 * (g_Config.m_EdAutosaveInterval + 1))
		return;

	PerformAutosave();
}

bool CEditor::PerformAutosave()
{
	char aDate[20];
	char aAutosavePath[IO_MAX_PATH_LENGTH];
	str_timestamp(aDate, sizeof(aDate));
	char aFileNameNoExt[IO_MAX_PATH_LENGTH];
	if(m_aFileName[0] == '\0')
	{
		str_copy(aFileNameNoExt, "unnamed");
	}
	else
	{
		const char *pFileName = fs_filename(m_aFileName);
		str_truncate(aFileNameNoExt, sizeof(aFileNameNoExt), pFileName, str_length(pFileName) - str_length(".map"));
	}
	str_format(aAutosavePath, sizeof(aAutosavePath), "maps/auto/%s_%s.map", aFileNameNoExt, aDate);

	m_Map.m_LastSaveTime = Client()->GlobalTime();
	if(Save(aAutosavePath))
	{
		m_Map.m_ModifiedAuto = false;
		// Clean up autosaves
		if(g_Config.m_EdAutosaveMax)
		{
			CFileCollection AutosavedMaps;
			AutosavedMaps.Init(Storage(), "maps/auto", aFileNameNoExt, ".map", g_Config.m_EdAutosaveMax);
		}
		return true;
	}
	else
	{
		ShowFileDialogError("Failed to automatically save map to file '%s'.", aAutosavePath);
		return false;
	}
}

void CEditor::HandleWriterFinishJobs()
{
	if(m_lpWriterFinishJobs.empty())
		return;

	std::shared_ptr<CDataFileWriterFinishJob> pJob = m_lpWriterFinishJobs.front();
	if(pJob->Status() != IJob::STATE_DONE)
		return;

	char aBuf[IO_MAX_PATH_LENGTH + 32];
	str_format(aBuf, sizeof(aBuf), "saving '%s' done", pJob->GetFileName());
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "editor", aBuf);

	// send rcon.. if we can
	if(Client()->RconAuthed())
	{
		CServerInfo CurrentServerInfo;
		Client()->GetServerInfo(&CurrentServerInfo);
		NETADDR ServerAddr = Client()->ServerAddress();
		const unsigned char aIpv4Localhost[16] = {127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		const unsigned char aIpv6Localhost[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

		// and if we're on localhost
		if(!mem_comp(ServerAddr.ip, aIpv4Localhost, sizeof(aIpv4Localhost)) || !mem_comp(ServerAddr.ip, aIpv6Localhost, sizeof(aIpv6Localhost)))
		{
			char aMapName[128];
			IStorage::StripPathAndExtension(pJob->GetFileName(), aMapName, sizeof(aMapName));
			if(!str_comp(aMapName, CurrentServerInfo.m_aMap))
				Client()->Rcon("reload");
		}
	}

	m_lpWriterFinishJobs.pop_front();
}

void CEditor::OnUpdate()
{
	CUIElementBase::Init(UI()); // update static pointer because game and editor use separate UI

	if(!m_EditorWasUsedBefore)
	{
		m_EditorWasUsedBefore = true;
		Reset();
	}

	HandleCursorMovement();
	HandleAutosave();
	HandleWriterFinishJobs();
}

void CEditor::OnRender()
{
	UI()->ResetMouseSlow();

	// toggle gui
	if(m_Dialog == DIALOG_NONE && m_EditBoxActive == 0 && Input()->KeyPress(KEY_TAB))
		m_GuiActive = !m_GuiActive;

	if(Input()->KeyPress(KEY_F10))
		m_ShowMousePointer = false;

	if(m_Animate)
		m_AnimateTime = (time_get() - m_AnimateStart) / (float)time_freq();
	else
		m_AnimateTime = 0;

	ms_pUiGotContext = nullptr;
	UI()->StartCheck();

	for(size_t i = 0; i < Input()->NumEvents(); i++)
		UI()->OnInput(Input()->GetEvent(i));

	UI()->Update(m_MouseX, m_MouseY, m_MouseDeltaX, m_MouseDeltaY, m_MouseWorldX, m_MouseWorldY);

	Render();

	m_MouseDeltaX = 0.0f;
	m_MouseDeltaY = 0.0f;
	m_MouseDeltaWx = 0.0f;
	m_MouseDeltaWy = 0.0f;

	if(Input()->KeyPress(KEY_F10))
	{
		Graphics()->TakeScreenshot(nullptr);
		m_ShowMousePointer = true;
	}

	UI()->FinishCheck();
	UI()->ClearHotkeys();
	Input()->Clear();

	CLineInput::RenderCandidates();
}

void CEditor::LoadCurrentMap()
{
	Load(m_pClient->GetCurrentMapPath(), IStorage::TYPE_ALL);
	m_ValidSaveFilename = true;

	CGameClient *pGameClient = (CGameClient *)Kernel()->RequestInterface<IGameClient>();
	vec2 Center = pGameClient->m_Camera.m_Center;

	m_WorldOffsetX = Center.x;
	m_WorldOffsetY = Center.y;
}

IEditor *CreateEditor() { return new CEditor; }

void CEditor::ResetMenuBackgroundPositions()
{
	std::array<vec2, CMenuBackground::NUM_POS> aBackgroundPositions = GenerateMenuBackgroundPositions();
	m_vMenuBackgroundPositions.assign(aBackgroundPositions.begin(), aBackgroundPositions.end());

	CLayerGame *pLayer = m_Map.m_pGameLayer;
	if(pLayer)
	{
		for(int y = 0; y < pLayer->m_Height; ++y)
		{
			for(int x = 0; x < pLayer->m_Width; ++x)
			{
				CTile Tile = pLayer->GetTile(x, y);
				if(Tile.m_Index >= TILE_TIME_CHECKPOINT_FIRST && Tile.m_Index <= TILE_TIME_CHECKPOINT_LAST)
				{
					int ArrayIndex = clamp<int>((Tile.m_Index - TILE_TIME_CHECKPOINT_FIRST), 0, CMenuBackground::NUM_POS);
					m_vMenuBackgroundPositions[ArrayIndex] = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				}

				x += Tile.m_Skip;
			}
		}
	}

	m_vMenuBackgroundCollisions.clear();
	m_vMenuBackgroundCollisions.resize(m_vMenuBackgroundPositions.size());
	for(size_t i = 0; i < m_vMenuBackgroundPositions.size(); i++)
	{
		for(size_t j = i + 1; j < m_vMenuBackgroundPositions.size(); j++)
		{
			if(i != j && distance(m_vMenuBackgroundPositions[i], m_vMenuBackgroundPositions[j]) < 0.001f)
				m_vMenuBackgroundCollisions.at(i).push_back(j);
		}
	}
}

// DDRace

void CEditorMap::MakeTeleLayer(CLayer *pLayer)
{
	m_pTeleLayer = (CLayerTele *)pLayer;
	m_pTeleLayer->m_pEditor = m_pEditor;
	m_pTeleLayer->m_Texture = m_pEditor->GetTeleTexture();
}

void CEditorMap::MakeSpeedupLayer(CLayer *pLayer)
{
	m_pSpeedupLayer = (CLayerSpeedup *)pLayer;
	m_pSpeedupLayer->m_pEditor = m_pEditor;
	m_pSpeedupLayer->m_Texture = m_pEditor->GetSpeedupTexture();
}

void CEditorMap::MakeFrontLayer(CLayer *pLayer)
{
	m_pFrontLayer = (CLayerFront *)pLayer;
	m_pFrontLayer->m_pEditor = m_pEditor;
	m_pFrontLayer->m_Texture = m_pEditor->GetFrontTexture();
}

void CEditorMap::MakeSwitchLayer(CLayer *pLayer)
{
	m_pSwitchLayer = (CLayerSwitch *)pLayer;
	m_pSwitchLayer->m_pEditor = m_pEditor;
	m_pSwitchLayer->m_Texture = m_pEditor->GetSwitchTexture();
}

void CEditorMap::MakeTuneLayer(CLayer *pLayer)
{
	m_pTuneLayer = (CLayerTune *)pLayer;
	m_pTuneLayer->m_pEditor = m_pEditor;
	m_pTuneLayer->m_Texture = m_pEditor->GetTuneTexture();
}
