/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "chat.h"

#include <base/io.h>
#include <base/time.h>

#include <engine/editor.h>
#include <engine/external/regex.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/csv.h>
#include <engine/shared/http.h>
#include <engine/shared/json.h>
#include <engine/textrender.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/client/animstate.h>
#include <game/client/components/censor.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/skins.h>
#include <game/client/components/sounds.h>
#include <game/client/components/tclient/colored_parts.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/localization.h>

#include <cctype>
#include <string>

#if defined(CONF_FAMILY_WINDOWS)
#define IStorage IWindowsStorage
#include <objidl.h>
#include <wincodec.h>
#undef IStorage
#pragma comment(lib, "windowscodecs.lib")
#endif

char CChat::ms_aDisplayText[MAX_LINE_LENGTH] = "";

CChat::CLine::CLine()
{
	m_TextContainerIndex.Reset();
	m_QuadContainerIndex = -1;
}

void CChat::CLine::Reset(CChat &This)
{
	This.TextRender()->DeleteTextContainer(m_TextContainerIndex);
	This.Graphics()->DeleteQuadContainer(m_QuadContainerIndex);
	m_Initialized = false;
	m_Time = 0;
	m_aText[0] = '\0';
	m_aName[0] = '\0';
	m_Friend = false;
	m_TimesRepeated = 0;
	m_pManagedTeeRenderInfo = nullptr;
	m_pTranslateResponse = nullptr;
	if(m_pGifRequest)
		m_pGifRequest->Abort();
	m_pGifRequest.reset();
	for(auto &Frame : m_vGifFrames)
		This.Graphics()->UnloadTexture(&Frame.m_Texture);
	m_vGifFrames.clear();
	m_aGifUrl[0] = '\0';
	m_GifWidth = 0;
	m_GifHeight = 0;
	m_GifStartTime = 0;
	m_GifResolveDepth = 0;
	m_GifFailed = false;
}

CChat::CChat()
{
	m_Mode = MODE_NONE;

	m_Input.SetCalculateOffsetCallback([this]() { return m_IsInputCensored; });
	m_Input.SetDisplayTextCallback([this](char *pStr, size_t NumChars) {
		m_IsInputCensored = false;
		if(
			g_Config.m_ClStreamerMode &&
			(str_startswith(pStr, "/login ") ||
				str_startswith(pStr, "/register ") ||
				str_startswith(pStr, "/code ") ||
				str_startswith(pStr, "/timeout ") ||
				str_startswith(pStr, "/save ") ||
				str_startswith(pStr, "/load ")))
		{
			bool Censor = false;
			const size_t NumLetters = minimum(NumChars, sizeof(ms_aDisplayText) - 1);
			for(size_t i = 0; i < NumLetters; ++i)
			{
				if(Censor)
					ms_aDisplayText[i] = '*';
				else
					ms_aDisplayText[i] = pStr[i];
				if(pStr[i] == ' ')
				{
					Censor = true;
					m_IsInputCensored = true;
				}
			}
			ms_aDisplayText[NumLetters] = '\0';
			return ms_aDisplayText;
		}
		return pStr;
	});
}

void CChat::RegisterCommand(const char *pName, const char *pParams, const char *pHelpText)
{
	// Don't allow duplicate commands.
	for(const auto &Command : m_vServerCommands)
		if(str_comp(Command.m_aName, pName) == 0)
			return;

	m_vServerCommands.emplace_back(pName, pParams, pHelpText);
	m_ServerCommandsNeedSorting = true;
}

void CChat::UnregisterCommand(const char *pName)
{
	m_vServerCommands.erase(std::remove_if(m_vServerCommands.begin(), m_vServerCommands.end(), [pName](const CCommand &Command) { return str_comp(Command.m_aName, pName) == 0; }), m_vServerCommands.end());
}

void CChat::RebuildChat()
{
	for(auto &Line : m_aLines)
	{
		if(!Line.m_Initialized)
			continue;
		TextRender()->DeleteTextContainer(Line.m_TextContainerIndex);
		Graphics()->DeleteQuadContainer(Line.m_QuadContainerIndex);
		// recalculate sizes
		Line.m_aYOffset[0] = -1.0f;
		Line.m_aYOffset[1] = -1.0f;
	}
}

void CChat::ClearLines()
{
	for(auto &Line : m_aLines)
		Line.Reset(*this);
	m_PrevScoreBoardShowed = false;
	m_PrevShowChat = false;
}

void CChat::OnWindowResize()
{
	RebuildChat();
}

void CChat::Reset()
{
	ClearLines();

	m_Show = false;
	m_CompletionUsed = false;
	m_CompletionChosen = -1;
	m_aCompletionBuffer[0] = 0;
	m_PlaceholderOffset = 0;
	m_PlaceholderLength = 0;
	m_pHistoryEntry = nullptr;
	m_PendingChatCounter = 0;
	m_LastChatSend = 0;
	m_IgnoreTagSafeStart = 0;
	m_IgnoreTagPendingCount = 0;
	m_aIgnoreTagLastLine[0] = '\0';
	m_CurrentLine = 0;
	m_IsInputCensored = false;
	m_EditingNewLine = true;
	m_ServerSupportsCommandInfo = false;
	m_ServerCommandsNeedSorting = false;
	m_aCurrentInputText[0] = '\0';
	DisableMode();
	m_vServerCommands.clear();

	for(int64_t &LastSoundPlayed : m_aLastSoundPlayed)
		LastSoundPlayed = 0;
}

void CChat::OnRelease()
{
	m_Show = false;
}

void CChat::OnStateChange(int NewState, int OldState)
{
	if(OldState <= IClient::STATE_CONNECTING)
		Reset();
}

void CChat::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->SendChat(0, pResult->GetString(0));
}

void CChat::ConSayTeam(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->SendChat(1, pResult->GetString(0));
}

void CChat::ConChat(IConsole::IResult *pResult, void *pUserData)
{
	const char *pMode = pResult->GetString(0);
	if(str_comp(pMode, "all") == 0)
		((CChat *)pUserData)->EnableMode(0);
	else if(str_comp(pMode, "team") == 0)
		((CChat *)pUserData)->EnableMode(1);
	else
		((CChat *)pUserData)->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "console", "expected all or team as mode");

	if(pResult->GetString(1)[0] || g_Config.m_ClChatReset)
		((CChat *)pUserData)->m_Input.Set(pResult->GetString(1));
}

void CChat::ConShowChat(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->m_Show = pResult->GetInteger(0) != 0;
}

void CChat::ConEcho(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->Echo(pResult->GetString(0));
}

void CChat::ConClearChat(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->ClearLines();
}

void CChat::ConchainChatOld(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	((CChat *)pUserData)->RebuildChat();
}

void CChat::ConchainChatFontSize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	CChat *pChat = (CChat *)pUserData;
	pChat->EnsureCoherentWidth();
	pChat->RebuildChat();
}

void CChat::ConchainChatWidth(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	CChat *pChat = (CChat *)pUserData;
	pChat->EnsureCoherentFontSize();
	pChat->RebuildChat();
}

void CChat::Echo(const char *pString)
{
	AddLine(CLIENT_MSG, 0, pString);
}

bool CChat::HandleSkinCommand(const char *pText)
{
	static constexpr const char *pCommand = ".skin";
	const int CommandLength = str_length(pCommand);
	if(str_comp_nocase_num(pText, pCommand, CommandLength) != 0 ||
		(pText[CommandLength] != '\0' && pText[CommandLength] != ' '))
		return false;

	const char *pName = pText + CommandLength;
	while(*pName == ' ')
		pName++;
	char aName[MAX_NAME_LENGTH];
	str_copy(aName, pName);
	int Length = str_length(aName);
	while(Length > 0 && aName[Length - 1] == ' ')
		aName[--Length] = '\0';
	if(Length >= 2 && aName[0] == '"' && aName[Length - 1] == '"')
	{
		mem_move(aName, aName + 1, Length - 2);
		aName[Length - 2] = '\0';
	}
	if(aName[0] == '\0')
	{
		Echo("Использование: .skin \"player\"");
		return true;
	}

	int ClientId = -1;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GameClient()->m_aClients[i].m_Active && str_comp_nocase(GameClient()->m_aClients[i].m_aName, aName) == 0)
		{
			ClientId = i;
			break;
		}
	}
	if(ClientId < 0)
	{
		char aMessage[128];
		str_format(aMessage, sizeof(aMessage), "Игрок %s не найден.", aName);
		Echo(aMessage);
		return true;
	}

	const CGameClient::CClientData &ClientData = GameClient()->m_aClients[ClientId];
	if(g_Config.m_ClDummy)
	{
		str_copy(g_Config.m_ClDummySkin, ClientData.m_aSkinName);
		g_Config.m_ClDummyUseCustomColor = ClientData.m_UseCustomColor;
		g_Config.m_ClDummyColorBody = ClientData.m_ColorBody;
		g_Config.m_ClDummyColorFeet = ClientData.m_ColorFeet;
		GameClient()->SendDummyInfo(false);
	}
	else
	{
		str_copy(g_Config.m_ClPlayerSkin, ClientData.m_aSkinName);
		g_Config.m_ClPlayerUseCustomColor = ClientData.m_UseCustomColor;
		g_Config.m_ClPlayerColorBody = ClientData.m_ColorBody;
		g_Config.m_ClPlayerColorFeet = ClientData.m_ColorFeet;
		GameClient()->SendInfo(false);
	}

	char aMessage[192];
	str_format(aMessage, sizeof(aMessage), "Скин игрока %s скопирован: %s.", ClientData.m_aName, ClientData.m_aSkinName);
	Echo(aMessage);
	return true;
}

bool CChat::HandleHourCommand(const char *pText)
{
	const char *pCommand = nullptr;
	if(str_comp_nocase_num(pText, ".hours", 6) == 0 && (pText[6] == '\0' || pText[6] == ' '))
		pCommand = ".hours";
	else if(str_comp_nocase_num(pText, ".hour", 5) == 0 && (pText[5] == '\0' || pText[5] == ' '))
		pCommand = ".hour";
	if(!pCommand)
		return false;

	const char *pName = pText + str_length(pCommand);
	while(*pName == ' ')
		pName++;
	char aName[MAX_NAME_LENGTH];
	str_copy(aName, pName);
	int Length = str_length(aName);
	while(Length > 0 && aName[Length - 1] == ' ')
		aName[--Length] = '\0';
	if(Length >= 2 && aName[0] == '"' && aName[Length - 1] == '"')
	{
		mem_move(aName, aName + 1, Length - 2);
		aName[Length - 2] = '\0';
	}
	if(aName[0] == '\0')
	{
		Echo("Использование: .hours \"player\"");
		return true;
	}
	if(m_pHourRequest && m_pHourRequest->State() != EHttpState::DONE && m_pHourRequest->State() != EHttpState::ERROR && m_pHourRequest->State() != EHttpState::ABORTED)
	{
		Echo("Запрос часов уже выполняется...");
		return true;
	}

	char aEscapedName[MAX_NAME_LENGTH * 3];
	char aUrl[512];
	EscapeUrl(aEscapedName, aName);
	str_format(aUrl, sizeof(aUrl), "https://ddstats.tw/player/json?player=%s", aEscapedName);
	m_pHourRequest = HttpGet(aUrl);
	m_pHourRequest->Timeout(CTimeout{4000, 12000, 500, 2});
	Http()->Run(m_pHourRequest);
	str_copy(m_aHourPlayer, aName);
	char aStatus[128];
	str_format(aStatus, sizeof(aStatus), "Ищу часы игрока %s...", aName);
	Echo(aStatus);
	return true;
}

void CChat::UpdateHourRequest()
{
	if(!m_pHourRequest || (m_pHourRequest->State() != EHttpState::DONE && m_pHourRequest->State() != EHttpState::ERROR && m_pHourRequest->State() != EHttpState::ABORTED))
		return;

	char aMessage[256];
	if(m_pHourRequest->State() != EHttpState::DONE)
		str_format(aMessage, sizeof(aMessage), "Не удалось получить часы игрока %s: сервис DDStats не отвечает.", m_aHourPlayer);
	else
	{
		json_value *pJson = m_pHourRequest->ResultJson();
		const json_value *pActivity = pJson ? json_object_get(pJson, "general_activity") : nullptr;
		const json_value *pSeconds = pActivity ? json_object_get(pActivity, "total_seconds_played") : nullptr;
		double Seconds = -1.0;
		if(pSeconds && pSeconds->type == json_integer)
			Seconds = (double)pSeconds->u.integer;
		else if(pSeconds && pSeconds->type == json_double)
			Seconds = pSeconds->u.dbl;
		if(Seconds >= 0.0)
			str_format(aMessage, sizeof(aMessage), "%s: %.1f часов в игре.", m_aHourPlayer, Seconds / 3600.0);
		else
			str_format(aMessage, sizeof(aMessage), "Игрок %s не найден или часы отсутствуют.", m_aHourPlayer);
		if(pJson)
			json_value_free(pJson);
	}
	Echo(aMessage);
	m_pHourRequest.reset();
	m_aHourPlayer[0] = '\0';
}

static bool IsDirectGifUrl(const char *pUrl)
{
	const char *pGif = str_find_nocase(pUrl, ".gif");
	return pGif && (pGif[4] == '\0' || pGif[4] == '?' || pGif[4] == '#');
}

static bool IsSupportedGifPage(const char *pUrl)
{
	return str_find_nocase(pUrl, "tenor.com/") ||
	       str_find_nocase(pUrl, "giphy.com/") ||
	       str_find_nocase(pUrl, "imgur.com/");
}

static void ReplaceAll(std::string &Text, const char *pFrom, const char *pTo)
{
	const size_t FromLength = str_length(pFrom);
	const size_t ToLength = str_length(pTo);
	for(size_t Position = 0; (Position = Text.find(pFrom, Position)) != std::string::npos; Position += ToLength)
		Text.replace(Position, FromLength, pTo);
}

static bool ExtractGifUrlFromHtml(const unsigned char *pData, size_t DataSize, char *pUrl, size_t UrlSize)
{
	if(!pData || DataSize == 0 || DataSize > 4 * 1024 * 1024)
		return false;
	std::string Html((const char *)pData, DataSize);

	auto ExtractAttribute = [&](size_t TagStart, size_t TagEnd, const char *pAttribute, std::string &Value) {
		size_t At = Html.find(pAttribute, TagStart);
		if(At == std::string::npos || At >= TagEnd)
			return false;
		At += str_length(pAttribute);
		while(At < TagEnd && std::isspace((unsigned char)Html[At]))
			At++;
		if(At >= TagEnd || Html[At] != '=')
			return false;
		At++;
		while(At < TagEnd && std::isspace((unsigned char)Html[At]))
			At++;
		if(At >= TagEnd || (Html[At] != '"' && Html[At] != '\''))
			return false;
		const char Quote = Html[At++];
		const size_t End = Html.find(Quote, At);
		if(End == std::string::npos || End > TagEnd)
			return false;
		Value = Html.substr(At, End - At);
		return true;
	};

	std::string Candidate;
	size_t Search = 0;
	while((Search = Html.find("<meta", Search)) != std::string::npos)
	{
		const size_t End = Html.find('>', Search);
		if(End == std::string::npos)
			break;
		const std::string Tag = Html.substr(Search, End - Search);
		if((Tag.find("og:image") != std::string::npos || Tag.find("twitter:image") != std::string::npos) &&
			ExtractAttribute(Search, End, "content", Candidate))
			break;
		Search = End + 1;
	}

	if(Candidate.empty())
	{
		for(const char *pKey : {"\"contentUrl\"", "\"url\""})
		{
			size_t At = Html.find(pKey);
			if(At == std::string::npos)
				continue;
			At = Html.find(':', At + str_length(pKey));
			At = At == std::string::npos ? At : Html.find('"', At + 1);
			if(At == std::string::npos)
				continue;
			const size_t End = Html.find('"', At + 1);
			if(End != std::string::npos)
			{
				Candidate = Html.substr(At + 1, End - At - 1);
				break;
			}
		}
	}

	ReplaceAll(Candidate, "&amp;", "&");
	ReplaceAll(Candidate, "\\/", "/");
	ReplaceAll(Candidate, "\\u0026", "&");
	if(Candidate.size() >= UrlSize || (!str_startswith(Candidate.c_str(), "https://") && !str_startswith(Candidate.c_str(), "http://")) || !IsDirectGifUrl(Candidate.c_str()))
		return false;
	str_copy(pUrl, Candidate.c_str(), UrlSize);
	return true;
}

void CChat::RequestGifUrl(CLine &Line, const char *pUrl)
{
	str_copy(Line.m_aGifUrl, pUrl);
	auto pGet = HttpGet(Line.m_aGifUrl);
	pGet->Timeout(CTimeout{3000, 12000, 500, 2});
	pGet->MaxResponseSize(16 * 1024 * 1024);
	Line.m_pGifRequest = std::move(pGet);
	Http()->Run(Line.m_pGifRequest);
}

void CChat::StartGifPreview(CLine &Line)
{
	if(!g_Config.m_TcChatGifPreview)
		return;

	const char *pUrl = str_find_nocase(Line.m_aText, "https://");
	if(!pUrl)
		pUrl = str_find_nocase(Line.m_aText, "http://");
	if(!pUrl)
		return;

	const char *pEnd = pUrl;
	while(*pEnd && !str_utf8_isspace((unsigned char)*pEnd) && *pEnd != '"' && *pEnd != '<' && *pEnd != '>')
		pEnd++;
	while(pEnd > pUrl && (pEnd[-1] == '.' || pEnd[-1] == ',' || pEnd[-1] == ')' || pEnd[-1] == ']'))
		pEnd--;
	const int UrlLength = minimum((int)sizeof(Line.m_aGifUrl) - 1, (int)(pEnd - pUrl));
	if(UrlLength <= 0)
		return;
	char aUrl[256];
	str_copy(aUrl, pUrl, UrlLength + 1);
	if(str_find_nocase(aUrl, "imgur.com/") && !str_find_nocase(aUrl, "i.imgur.com/"))
	{
		const char *pId = str_find_nocase(aUrl, "imgur.com/") + str_length("imgur.com/");
		if(!str_find(pId, "/"))
		{
			char aId[64];
			str_copy(aId, pId);
			char *pSuffix = (char *)str_find_nocase(aId, ".gifv");
			if(pSuffix)
				*pSuffix = '\0';
			pSuffix = (char *)str_find(aId, "?");
			if(pSuffix)
				*pSuffix = '\0';
			pSuffix = (char *)str_find(aId, "#");
			if(pSuffix)
				*pSuffix = '\0';
			if(aId[0] != '\0')
				str_format(aUrl, sizeof(aUrl), "https://i.imgur.com/%s.gif", aId);
		}
	}
	if(!IsDirectGifUrl(aUrl) && !IsSupportedGifPage(aUrl))
	{
		return;
	}

	RequestGifUrl(Line, aUrl);
}

bool CChat::DecodeGifPreview(CLine &Line, const unsigned char *pData, size_t DataSize)
{
#if !defined(CONF_FAMILY_WINDOWS)
	(void)Line;
	(void)pData;
	(void)DataSize;
	return false;
#else
	if(!pData || DataSize < 6 || DataSize > 16 * 1024 * 1024 ||
		(mem_comp(pData, "GIF87a", 6) != 0 && mem_comp(pData, "GIF89a", 6) != 0))
		return false;

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IWICImagingFactory *pFactory = nullptr;
	IWICBitmapDecoder *pDecoder = nullptr;
	IStream *pStream = nullptr;
	HGLOBAL Memory = GlobalAlloc(GMEM_MOVEABLE, DataSize);
	if(!Memory)
		return false;
	void *pMemory = GlobalLock(Memory);
	if(!pMemory)
	{
		GlobalFree(Memory);
		return false;
	}
	mem_copy(pMemory, pData, DataSize);
	GlobalUnlock(Memory);
	if(FAILED(CreateStreamOnHGlobal(Memory, TRUE, &pStream)))
	{
		GlobalFree(Memory);
		return false;
	}

	bool Success = false;
	if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))) &&
		SUCCEEDED(pFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder)))
	{
		UINT FrameCount = 0;
		pDecoder->GetFrameCount(&FrameCount);
		FrameCount = minimum<UINT>(FrameCount, 96);
		size_t TotalBytes = 0;
		for(UINT i = 0; i < FrameCount; ++i)
		{
			IWICBitmapFrameDecode *pFrame = nullptr;
			IWICBitmapScaler *pScaler = nullptr;
			IWICFormatConverter *pConverter = nullptr;
			if(FAILED(pDecoder->GetFrame(i, &pFrame)))
				break;

			UINT SourceWidth = 0, SourceHeight = 0;
			pFrame->GetSize(&SourceWidth, &SourceHeight);
			if(SourceWidth == 0 || SourceHeight == 0)
			{
				pFrame->Release();
				break;
			}
			const double Scale = minimum(1.0, minimum(420.0 / SourceWidth, 300.0 / SourceHeight));
			const UINT Width = maximum<UINT>(1, round_to_int(SourceWidth * Scale));
			const UINT Height = maximum<UINT>(1, round_to_int(SourceHeight * Scale));
			const size_t FrameBytes = (size_t)Width * Height * 4;
			if(FrameBytes == 0 || TotalBytes + FrameBytes > 32 * 1024 * 1024)
			{
				pFrame->Release();
				break;
			}

			IWICBitmapSource *pSource = pFrame;
			if((Width != SourceWidth || Height != SourceHeight) &&
				SUCCEEDED(pFactory->CreateBitmapScaler(&pScaler)) &&
				SUCCEEDED(pScaler->Initialize(pFrame, Width, Height, WICBitmapInterpolationModeFant)))
				pSource = pScaler;

			if(SUCCEEDED(pFactory->CreateFormatConverter(&pConverter)) &&
				SUCCEEDED(pConverter->Initialize(pSource, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			{
				CImageInfo Image;
				Image.m_Width = Width;
				Image.m_Height = Height;
				Image.m_Format = CImageInfo::FORMAT_RGBA;
				Image.m_pData = (uint8_t *)malloc(FrameBytes);
				if(Image.m_pData && SUCCEEDED(pConverter->CopyPixels(nullptr, Width * 4, FrameBytes, Image.m_pData)))
				{
					CLine::SGifFrame GifFrame;
					IWICMetadataQueryReader *pMetadata = nullptr;
					if(SUCCEEDED(pFrame->GetMetadataQueryReader(&pMetadata)))
					{
						PROPVARIANT Delay;
						PropVariantInit(&Delay);
						if(SUCCEEDED(pMetadata->GetMetadataByName(L"/grctlext/Delay", &Delay)) && Delay.vt == VT_UI2)
							GifFrame.m_DurationMs = std::clamp((int)Delay.uiVal * 10, 20, 10000);
						PropVariantClear(&Delay);
						pMetadata->Release();
					}
					GifFrame.m_Texture = Graphics()->LoadTextureRawMove(Image, 0, Line.m_aGifUrl);
					if(GifFrame.m_Texture.IsValid())
					{
						Line.m_vGifFrames.push_back(GifFrame);
						Line.m_GifWidth = Width;
						Line.m_GifHeight = Height;
						TotalBytes += FrameBytes;
					}
				}
				else
					Image.Free();
			}
			if(pConverter)
				pConverter->Release();
			if(pScaler)
				pScaler->Release();
			pFrame->Release();
		}
		Success = !Line.m_vGifFrames.empty();
	}
	if(pDecoder)
		pDecoder->Release();
	if(pFactory)
		pFactory->Release();
	pStream->Release();
	if(Success)
	{
		Line.m_GifStartTime = time_get();
		Line.m_aYOffset[0] = -1.0f;
		Line.m_aYOffset[1] = -1.0f;
	}
	return Success;
#endif
}

void CChat::UpdateGifPreviews()
{
	for(CLine &Line : m_aLines)
	{
		if(!Line.m_Initialized || !Line.m_pGifRequest || !Line.m_pGifRequest->Done())
			continue;
		const std::shared_ptr<CHttpRequest> pCompletedRequest = Line.m_pGifRequest;
		Line.m_pGifRequest.reset();
		if(pCompletedRequest->State() == EHttpState::DONE && pCompletedRequest->StatusCode() >= 200 && pCompletedRequest->StatusCode() < 300)
		{
			unsigned char *pResult = nullptr;
			size_t ResultSize = 0;
			pCompletedRequest->Result(&pResult, &ResultSize);
			const bool IsGifData = ResultSize >= 6 &&
					       (mem_comp(pResult, "GIF87a", 6) == 0 || mem_comp(pResult, "GIF89a", 6) == 0);
			if(IsGifData)
				Line.m_GifFailed = !DecodeGifPreview(Line, pResult, ResultSize);
			else if(Line.m_GifResolveDepth < 2)
			{
				char aResolvedUrl[256];
				if(ExtractGifUrlFromHtml(pResult, ResultSize, aResolvedUrl, sizeof(aResolvedUrl)))
				{
					Line.m_GifResolveDepth++;
					RequestGifUrl(Line, aResolvedUrl);
					continue;
				}
				Line.m_GifFailed = true;
			}
			else
				Line.m_GifFailed = true;
		}
		else
			Line.m_GifFailed = true;
	}
}

IGraphics::CTextureHandle CChat::GifFrameTexture(const CLine &Line) const
{
	if(Line.m_vGifFrames.empty())
		return IGraphics::CTextureHandle();
	int64_t TotalMs = 0;
	for(const auto &Frame : Line.m_vGifFrames)
		TotalMs += Frame.m_DurationMs;
	if(TotalMs <= 0)
		return Line.m_vGifFrames.front().m_Texture;
	int64_t Offset = ((time_get() - Line.m_GifStartTime) * 1000 / time_freq()) % TotalMs;
	for(const auto &Frame : Line.m_vGifFrames)
	{
		if(Offset < Frame.m_DurationMs)
			return Frame.m_Texture;
		Offset -= Frame.m_DurationMs;
	}
	return Line.m_vGifFrames.front().m_Texture;
}

void CChat::OnConsoleInit()
{
	Console()->Register("say", "r[message]", CFGFLAG_CLIENT, ConSay, this, "Say in chat");
	Console()->Register("say_team", "r[message]", CFGFLAG_CLIENT, ConSayTeam, this, "Say in team chat");
	Console()->Register("chat", "s['team'|'all'] ?r[message]", CFGFLAG_CLIENT, ConChat, this, "Enable chat with all/team mode");
	Console()->Register("+show_chat", "", CFGFLAG_CLIENT, ConShowChat, this, "Show chat");
	Console()->Register("echo", "r[message]", CFGFLAG_CLIENT | CFGFLAG_STORE, ConEcho, this, "Echo the text in chat window");
	Console()->Register("clear_chat", "", CFGFLAG_CLIENT | CFGFLAG_STORE, ConClearChat, this, "Clear chat messages");
}

void CChat::OnInit()
{
	Reset();
	Console()->Chain("cl_chat_old", ConchainChatOld, this);
	Console()->Chain("cl_chat_size", ConchainChatFontSize, this);
	Console()->Chain("cl_chat_width", ConchainChatWidth, this);
}

bool CChat::OnInput(const IInput::CEvent &Event)
{
	if(m_Mode == MODE_NONE)
		return false;

	if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		DisableMode();
		GameClient()->OnRelease();
		if(g_Config.m_ClChatReset)
		{
			m_Input.Clear();
			m_pHistoryEntry = nullptr;
		}
	}
	else if(Event.m_Flags & IInput::FLAG_PRESS && (Event.m_Key == KEY_RETURN || Event.m_Key == KEY_KP_ENTER))
	{
		if(m_ServerCommandsNeedSorting)
		{
			std::sort(m_vServerCommands.begin(), m_vServerCommands.end());
			m_ServerCommandsNeedSorting = false;
		}

		if(HandleSkinCommand(m_Input.GetString()) || HandleHourCommand(m_Input.GetString()))
			; // Local command, do not send it to the server.
		else if(GameClient()->m_BindChat.ChatDoBinds(m_Input.GetString()))
			; // Do nothing as bindchat was executed
		else if(GameClient()->m_TClient.ChatDoSpecId(m_Input.GetString()))
			; // Do nothing as specid was executed
		else
			SendChatQueued(m_Input.GetString());
		m_pHistoryEntry = nullptr;
		DisableMode();
		GameClient()->OnRelease();
		m_Input.Clear();
	}
	if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_TAB)
	{
		const bool ShiftPressed = Input()->ShiftIsPressed();
		const char *pInputText = m_Input.GetString();
		const char *pLocalArgument = nullptr;
		for(const char *pCommand : {".skin", ".hours", ".hour"})
		{
			const int CommandLength = str_length(pCommand);
			if(str_comp_nocase_num(pInputText, pCommand, CommandLength) == 0 && pInputText[CommandLength] == ' ')
			{
				pLocalArgument = pInputText + CommandLength;
				while(*pLocalArgument == ' ')
					pLocalArgument++;
				break;
			}
		}
		const bool LocalPlayerCommand = pLocalArgument != nullptr;

		// fill the completion buffer
		if(!m_CompletionUsed)
		{
			if(LocalPlayerCommand)
			{
				const char *pArgumentEnd = pLocalArgument + str_length(pLocalArgument);
				m_PlaceholderOffset = pLocalArgument - pInputText;
				m_PlaceholderLength = pArgumentEnd - pLocalArgument;
				const char *pCompletionStart = pLocalArgument;
				if(*pCompletionStart == '"')
					pCompletionStart++;
				const char *pCompletionEnd = pInputText + m_Input.GetCursorOffset();
				if(pCompletionEnd > pArgumentEnd)
					pCompletionEnd = pArgumentEnd;
				if(pCompletionEnd > pCompletionStart && pCompletionEnd[-1] == '"')
					pCompletionEnd--;
				const int CompletionLength = maximum(0, (int)(pCompletionEnd - pCompletionStart));
				str_truncate(m_aCompletionBuffer, sizeof(m_aCompletionBuffer), pCompletionStart, CompletionLength);
			}
			else
			{
				const char *pCursor = pInputText + m_Input.GetCursorOffset();
				for(size_t Count = 0; Count < m_Input.GetCursorOffset() && *(pCursor - 1) != ' '; --pCursor, ++Count)
					;
				m_PlaceholderOffset = pCursor - pInputText;

				for(m_PlaceholderLength = 0; *pCursor && *pCursor != ' '; ++pCursor)
					++m_PlaceholderLength;

				str_truncate(m_aCompletionBuffer, sizeof(m_aCompletionBuffer), pInputText + m_PlaceholderOffset, m_PlaceholderLength);
			}
		}

		if(!m_CompletionUsed && m_aCompletionBuffer[0] != '/')
		{
			// Create the completion list of player names through which the player can iterate
			const char *PlayerName, *FoundInput;
			m_PlayerCompletionListLength = 0;
			for(auto &PlayerInfo : GameClient()->m_Snap.m_apInfoByName)
			{
				if(PlayerInfo)
				{
					PlayerName = GameClient()->m_aClients[PlayerInfo->m_ClientId].m_aName;
					FoundInput = str_utf8_find_nocase(PlayerName, m_aCompletionBuffer);
					if(FoundInput != nullptr)
					{
						m_aPlayerCompletionList[m_PlayerCompletionListLength].m_ClientId = PlayerInfo->m_ClientId;
						// The score for suggesting a player name is determined by the distance of the search input to the beginning of the player name
						m_aPlayerCompletionList[m_PlayerCompletionListLength].m_Score = (int)(FoundInput - PlayerName);
						m_PlayerCompletionListLength++;
					}
				}
			}
			std::stable_sort(m_aPlayerCompletionList, m_aPlayerCompletionList + m_PlayerCompletionListLength,
				[](const CRateablePlayer &Player1, const CRateablePlayer &Player2) -> bool {
					return Player1.m_Score < Player2.m_Score;
				});
		}

		if(GameClient()->m_BindChat.ChatDoAutocomplete(ShiftPressed))
		{
		}
		else if(m_aCompletionBuffer[0] == '.' && m_PlaceholderOffset == 0)
		{
			const char *apLocalCommands[] = {".skin", ".hour", ".hours"};
			const char *apMatches[3];
			int NumMatches = 0;
			for(const char *pCommand : apLocalCommands)
			{
				if(str_startswith_nocase(pCommand, m_aCompletionBuffer))
					apMatches[NumMatches++] = pCommand;
			}
			if(NumMatches > 0)
			{
				if(ShiftPressed && m_CompletionUsed)
					m_CompletionChosen--;
				else if(!ShiftPressed)
					m_CompletionChosen++;
				m_CompletionChosen = (m_CompletionChosen + NumMatches) % NumMatches;
				m_CompletionUsed = true;

				char aBuf[MAX_LINE_LENGTH];
				str_format(aBuf, sizeof(aBuf), "%s %s", apMatches[m_CompletionChosen], pInputText + m_PlaceholderOffset + m_PlaceholderLength);
				m_PlaceholderLength = str_length(apMatches[m_CompletionChosen]) + 1;
				m_Input.Set(aBuf);
				m_Input.SetCursorOffset(m_PlaceholderLength);
			}
		}
		else if(m_aCompletionBuffer[0] == '/' && !m_vServerCommands.empty())
		{
			CCommand *pCompletionCommand = nullptr;

			const size_t NumCommands = m_vServerCommands.size();

			if(ShiftPressed && m_CompletionUsed)
				m_CompletionChosen--;
			else if(!ShiftPressed)
				m_CompletionChosen++;
			m_CompletionChosen = (m_CompletionChosen + 2 * NumCommands) % (2 * NumCommands);

			m_CompletionUsed = true;

			const char *pCommandStart = m_aCompletionBuffer + 1;
			for(size_t i = 0; i < 2 * NumCommands; ++i)
			{
				int SearchType;
				int Index;

				if(ShiftPressed)
				{
					SearchType = ((m_CompletionChosen - i + 2 * NumCommands) % (2 * NumCommands)) / NumCommands;
					Index = (m_CompletionChosen - i + NumCommands) % NumCommands;
				}
				else
				{
					SearchType = ((m_CompletionChosen + i) % (2 * NumCommands)) / NumCommands;
					Index = (m_CompletionChosen + i) % NumCommands;
				}

				auto &Command = m_vServerCommands[Index];

				if(str_startswith_nocase(Command.m_aName, pCommandStart))
				{
					pCompletionCommand = &Command;
					m_CompletionChosen = Index + SearchType * NumCommands;
					break;
				}
			}

			// insert the command
			if(pCompletionCommand)
			{
				char aBuf[MAX_LINE_LENGTH];
				// add part before the name
				str_truncate(aBuf, sizeof(aBuf), m_Input.GetString(), m_PlaceholderOffset);

				// add the command
				str_append(aBuf, "/");
				str_append(aBuf, pCompletionCommand->m_aName);

				// add separator
				const char *pSeparator = pCompletionCommand->m_aParams[0] == '\0' ? "" : " ";
				str_append(aBuf, pSeparator);

				// add part after the name
				str_append(aBuf, m_Input.GetString() + m_PlaceholderOffset + m_PlaceholderLength);

				m_PlaceholderLength = str_length(pSeparator) + str_length(pCompletionCommand->m_aName) + 1;
				m_Input.Set(aBuf);
				m_Input.SetCursorOffset(m_PlaceholderOffset + m_PlaceholderLength);
			}
		}
		else
		{
			// find next possible name
			const char *pCompletionString = nullptr;
			if(m_PlayerCompletionListLength > 0)
			{
				// We do this in a loop, if a player left the game during the repeated pressing of Tab, they are skipped
				CGameClient::CClientData *pCompletionClientData;
				for(int i = 0; i < m_PlayerCompletionListLength; ++i)
				{
					if(ShiftPressed && m_CompletionUsed)
					{
						m_CompletionChosen--;
					}
					else if(!ShiftPressed)
					{
						m_CompletionChosen++;
					}
					if(m_CompletionChosen < 0)
					{
						m_CompletionChosen += m_PlayerCompletionListLength;
					}
					m_CompletionChosen %= m_PlayerCompletionListLength;
					m_CompletionUsed = true;

					pCompletionClientData = &GameClient()->m_aClients[m_aPlayerCompletionList[m_CompletionChosen].m_ClientId];
					if(!pCompletionClientData->m_Active)
					{
						continue;
					}

					pCompletionString = pCompletionClientData->m_aName;
					break;
				}
			}

			// insert the name
			if(pCompletionString)
			{
				char aBuf[MAX_LINE_LENGTH];
				// add part before the name
				str_truncate(aBuf, sizeof(aBuf), m_Input.GetString(), m_PlaceholderOffset);

				// quote the name
				char aQuoted[128];
				if(LocalPlayerCommand || ((m_Input.GetString()[0] == '/' || GameClient()->m_BindChat.CheckBindChat(m_Input.GetString())) && (str_find(pCompletionString, " ") || str_find(pCompletionString, "\""))))
				{
					// escape the name
					str_copy(aQuoted, "\"");
					char *pDst = aQuoted + str_length(aQuoted);
					str_escape(&pDst, pCompletionString, aQuoted + sizeof(aQuoted));
					str_append(aQuoted, "\"");

					pCompletionString = aQuoted;
				}

				// add the name
				str_append(aBuf, pCompletionString);

				// add separator
				const char *pSeparator = "";
				if(!LocalPlayerCommand && *(m_Input.GetString() + m_PlaceholderOffset + m_PlaceholderLength) != ' ')
					pSeparator = m_PlaceholderOffset == 0 ? ": " : " ";
				else if(m_PlaceholderOffset == 0)
					pSeparator = ":";
				if(*pSeparator)
					str_append(aBuf, pSeparator);

				// add part after the name
				str_append(aBuf, m_Input.GetString() + m_PlaceholderOffset + m_PlaceholderLength);

				m_PlaceholderLength = str_length(pSeparator) + str_length(pCompletionString);
				m_Input.Set(aBuf);
				m_Input.SetCursorOffset(m_PlaceholderOffset + m_PlaceholderLength);
			}
		}
	}
	else
	{
		// reset name completion process
		if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key != KEY_TAB && Event.m_Key != KEY_LSHIFT && Event.m_Key != KEY_RSHIFT)
		{
			m_CompletionChosen = -1;
			m_CompletionUsed = false;
		}

		m_Input.ProcessInput(Event);
	}

	if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_UP)
	{
		if(m_EditingNewLine)
		{
			str_copy(m_aCurrentInputText, m_Input.GetString());
			m_EditingNewLine = false;
		}

		if(m_pHistoryEntry)
		{
			CHistoryEntry *pTest = m_History.Prev(m_pHistoryEntry);

			if(pTest)
				m_pHistoryEntry = pTest;
		}
		else
			m_pHistoryEntry = m_History.Last();

		if(m_pHistoryEntry)
			m_Input.Set(m_pHistoryEntry->m_aText);
	}
	else if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_DOWN)
	{
		if(m_pHistoryEntry)
			m_pHistoryEntry = m_History.Next(m_pHistoryEntry);

		if(m_pHistoryEntry)
		{
			m_Input.Set(m_pHistoryEntry->m_aText);
		}
		else if(!m_EditingNewLine)
		{
			m_Input.Set(m_aCurrentInputText);
			m_EditingNewLine = true;
		}
	}

	return true;
}

void CChat::EnableMode(int Team)
{
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return;

	if(m_Mode == MODE_NONE)
	{
		if(Team)
			m_Mode = MODE_TEAM;
		else
			m_Mode = MODE_ALL;

		Input()->Clear();
		m_CompletionChosen = -1;
		m_CompletionUsed = false;
		m_Input.Activate(EInputPriority::CHAT);
	}
}

void CChat::DisableMode()
{
	if(m_Mode != MODE_NONE)
	{
		m_Mode = MODE_NONE;
		m_Input.Deactivate();
	}
}

void CChat::OnMessage(int MsgType, void *pRawMsg)
{
	if(GameClient()->m_SuppressEvents)
		return;

	if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

		auto &Re = GameClient()->m_TClient.m_RegexChatIgnore;
		if(Re.error().empty() && Re.test(pMsg->m_pMessage))
			return;

		/*
		if(g_Config.m_ClCensorChat)
		{
			char aMessage[MAX_LINE_LENGTH];
			str_copy(aMessage, pMsg->m_pMessage);
			GameClient()->m_Censor.CensorMessage(aMessage);
			AddLine(pMsg->m_ClientId, pMsg->m_Team, aMessage);
		}
		else
			AddLine(pMsg->m_ClientId, pMsg->m_Team, pMsg->m_pMessage);
		*/

		AddLine(pMsg->m_ClientId, pMsg->m_Team, pMsg->m_pMessage);

		if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
			pMsg->m_ClientId == SERVER_MSG)
		{
			StoreSave(pMsg->m_pMessage);
		}
	}
	else if(MsgType == NETMSGTYPE_SV_COMMANDINFO)
	{
		CNetMsg_Sv_CommandInfo *pMsg = (CNetMsg_Sv_CommandInfo *)pRawMsg;
		if(!m_ServerSupportsCommandInfo)
		{
			m_vServerCommands.clear();
			m_ServerSupportsCommandInfo = true;
		}
		RegisterCommand(pMsg->m_pName, pMsg->m_pArgsFormat, pMsg->m_pHelpText);
	}
	else if(MsgType == NETMSGTYPE_SV_COMMANDINFOREMOVE)
	{
		CNetMsg_Sv_CommandInfoRemove *pMsg = (CNetMsg_Sv_CommandInfoRemove *)pRawMsg;
		UnregisterCommand(pMsg->m_pName);
	}
}

bool CChat::LineShouldHighlight(const char *pLine, const char *pName)
{
	const char *pHit = str_utf8_find_nocase(pLine, pName);

	while(pHit)
	{
		int Length = str_length(pName);

		if(Length > 0 && (pLine == pHit || pHit[-1] == ' ') && (pHit[Length] == 0 || pHit[Length] == ' ' || pHit[Length] == '.' || pHit[Length] == '!' || pHit[Length] == ',' || pHit[Length] == '?' || pHit[Length] == ':'))
			return true;

		pHit = str_utf8_find_nocase(pHit + 1, pName);
	}

	return false;
}

bool CChat::IsIgnoreTagActive() const
{
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || !GameClient()->m_Snap.m_pLocalCharacter)
		return false;
	if(Client()->State() != IClient::STATE_ONLINE || !GameClient()->m_GameInfo.m_Race)
		return false;
	return GameClient()->LastRaceTick() >= 0;
}

bool CChat::IsFocusModeActive() const
{
	return g_Config.m_TcFocusMode && IsIgnoreTagActive();
}

bool CChat::IsIgnoreTagSafe()
{
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || !GameClient()->m_Snap.m_pLocalCharacter)
		return false;

	CCharacter *pLocalChar = GameClient()->m_PredictedWorld.GetCharacterById(LocalId);
	if(pLocalChar)
	{
		const CCharacterCore *pCore = pLocalChar->Core();
		if(pCore->m_FreezeEnd != 0 || pCore->m_IsInFreeze || pCore->m_DeepFrozen || pCore->m_LiveFrozen)
			return false;
		return pLocalChar->IsGrounded();
	}

	const vec2 Pos = GameClient()->m_LocalCharacterPos;
	if(GameClient()->m_aClients[LocalId].m_FreezeEnd != 0 || GameClient()->m_aClients[LocalId].m_DeepFrozen)
		return false;
	return Collision()->CheckPoint(Pos.x + 14.0f, Pos.y + 21.0f) || Collision()->CheckPoint(Pos.x - 14.0f, Pos.y + 21.0f);
}

void CChat::UpdateIgnoreTag()
{
	if(!g_Config.m_TcIgnoreTag || m_IgnoreTagPendingCount <= 0)
	{
		m_IgnoreTagSafeStart = 0;
		return;
	}

	const int64_t Now = time();
	if(!IsIgnoreTagActive())
	{
		char aBuf[sizeof(m_aIgnoreTagLastLine) + 64];
		if(m_IgnoreTagPendingCount > 1)
			str_format(aBuf, sizeof(aBuf), "Ignored tags x%d, last: %s", m_IgnoreTagPendingCount, m_aIgnoreTagLastLine);
		else
			str_format(aBuf, sizeof(aBuf), "Ignored tag: %s", m_aIgnoreTagLastLine);
		m_IgnoreTagPendingCount = 0;
		m_aIgnoreTagLastLine[0] = '\0';
		m_IgnoreTagSafeStart = 0;
		AddLine(CLIENT_MSG, 0, aBuf);
	}
	else if(IsIgnoreTagSafe())
	{
		if(m_IgnoreTagSafeStart == 0)
			m_IgnoreTagSafeStart = Now;
		if(Now - m_IgnoreTagSafeStart >= time_freq() * 3)
		{
			char aBuf[sizeof(m_aIgnoreTagLastLine) + 64];
			if(m_IgnoreTagPendingCount > 1)
				str_format(aBuf, sizeof(aBuf), "Ignored tags x%d, last: %s", m_IgnoreTagPendingCount, m_aIgnoreTagLastLine);
			else
				str_format(aBuf, sizeof(aBuf), "Ignored tag: %s", m_aIgnoreTagLastLine);
			m_IgnoreTagPendingCount = 0;
			m_aIgnoreTagLastLine[0] = '\0';
			m_IgnoreTagSafeStart = 0;
			AddLine(CLIENT_MSG, 0, aBuf);
		}
	}
	else
	{
		m_IgnoreTagSafeStart = 0;
	}
}

static constexpr const char *SAVES_HEADER[] = {
	"Time",
	"Player",
	"Map",
	"Code",
};

// TODO: remove this in a few releases (in 2027 or later)
//       it got deprecated by CGameClient::StoreSave
void CChat::StoreSave(const char *pText)
{
	const char *pStart = str_find(pText, "Team successfully saved by ");
	const char *pMid = str_find(pText, ". Use '/load ");
	const char *pOn = str_find(pText, "' on ");
	const char *pEnd = str_find(pText, pOn ? " to continue" : "' to continue");

	if(!pStart || !pMid || !pEnd || pMid < pStart || pEnd < pMid || (pOn && (pOn < pMid || pEnd < pOn)))
		return;

	char aName[16];
	str_truncate(aName, sizeof(aName), pStart + 27, pMid - pStart - 27);

	char aSaveCode[64];

	str_truncate(aSaveCode, sizeof(aSaveCode), pMid + 13, (pOn ? pOn : pEnd) - pMid - 13);

	char aTimestamp[20];
	str_timestamp_format(aTimestamp, sizeof(aTimestamp), TimestampFormat::SPACE);

	const bool SavesFileExists = Storage()->FileExists(SAVES_FILE, IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(SAVES_FILE, IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
		return;

	const char *apColumns[4] = {
		aTimestamp,
		aName,
		GameClient()->Map()->BaseName(),
		aSaveCode,
	};

	if(!SavesFileExists)
	{
		CsvWrite(File, 4, SAVES_HEADER);
	}
	CsvWrite(File, 4, apColumns);
	io_close(File);
}

void CChat::AddLine(int ClientId, int Team, const char *pLine)
{
	if(ClientId == SERVER_MSG && str_startswith(pLine, "__cloude_dev "))
	{
		int DevClientId = -1;
		int Enabled = 0;
		if(sscanf(pLine, "__cloude_dev %d %d", &DevClientId, &Enabled) == 2 && in_range(DevClientId, MAX_CLIENTS - 1))
			GameClient()->m_aClients[DevClientId].m_CloudeDevBadge = Enabled != 0;
		return;
	}

	// Detect team invites, e.g. "'nameless tee' invited you to team 5. Use /team 5 to join."
	// and offer an accept/ignore prompt on the media island.
	if(ClientId == SERVER_MSG)
	{
		const char *pInvited = str_find_nocase(pLine, "invited you to team ");
		if(pInvited)
		{
			int InviteTeam = -1;
			if(sscanf(pInvited, "invited you to team %d", &InviteTeam) == 1 && InviteTeam >= 0)
			{
				char aInviter[32] = "";
				if(pLine[0] == '\'')
				{
					const char *pEndQuote = str_find(pLine + 1, "'");
					if(pEndQuote && pEndQuote > pLine + 1)
					{
						const int NameLen = minimum((int)sizeof(aInviter) - 1, (int)(pEndQuote - (pLine + 1)));
						str_copy(aInviter, pLine + 1, NameLen + 1);
					}
				}
				GameClient()->m_MediaIsland.NotifyTeamInvite(InviteTeam, aInviter);
			}
		}
	}

	if(*pLine == 0 ||
		(ClientId == SERVER_MSG && !g_Config.m_ClShowChatSystem) ||
		(ClientId >= 0 && (GameClient()->m_aClients[ClientId].m_aName[0] == '\0' || // unknown client
					  GameClient()->m_aClients[ClientId].m_ChatIgnore ||
					  (GameClient()->m_Snap.m_LocalClientId != ClientId && g_Config.m_ClShowChatFriends && !GameClient()->m_aClients[ClientId].m_Friend) ||
					  (GameClient()->m_Snap.m_LocalClientId != ClientId && g_Config.m_ClShowChatTeamMembersOnly && GameClient()->IsOtherTeam(ClientId) && GameClient()->m_Teams.Team(GameClient()->m_Snap.m_LocalClientId) != TEAM_FLOCK) ||
					  (GameClient()->m_Snap.m_LocalClientId != ClientId && GameClient()->m_aClients[ClientId].m_Foe))))
		return;

	// TClient
	if(ClientId == CLIENT_MSG && !g_Config.m_TcShowChatClient)
		return;

	// trim right and set maximum length to 256 utf8-characters
	int Length = 0;
	const char *pStr = pLine;
	const char *pEnd = nullptr;
	while(*pStr)
	{
		const char *pStrOld = pStr;
		int Code = str_utf8_decode(&pStr);

		// check if unicode is not empty
		if(!str_utf8_isspace(Code))
		{
			pEnd = nullptr;
		}
		else if(pEnd == nullptr)
			pEnd = pStrOld;

		if(++Length >= MAX_LINE_LENGTH)
		{
			*(const_cast<char *>(pStr)) = '\0';
			break;
		}
	}
	if(pEnd != nullptr)
		*(const_cast<char *>(pEnd)) = '\0';

	if(*pLine == 0)
		return;

	bool Highlighted = false;

	auto &&FChatMsgCheckAndPrint = [this](const CLine &Line) {
		char aBuf[1024];
		str_format(aBuf, sizeof(aBuf), "%s%s%s", Line.m_aName, Line.m_ClientId >= 0 ? ": " : "", Line.m_aText);

		ColorRGBA ChatLogColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
		if(Line.m_Highlighted)
		{
			ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageHighlightColor));
		}
		else
		{
			if(Line.m_Friend && g_Config.m_ClMessageFriend)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageFriendColor));
			else if(Line.m_Team)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageTeamColor));
			else if(Line.m_ClientId == SERVER_MSG)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
			else if(Line.m_ClientId == CLIENT_MSG)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageClientColor));
			else // regular message
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageColor));
		}

		const char *pFrom;
		if(Line.m_Whisper)
			pFrom = "chat/whisper";
		else if(Line.m_Team)
			pFrom = "chat/team";
		else if(Line.m_ClientId == SERVER_MSG)
			pFrom = "chat/server";
		else if(Line.m_ClientId == CLIENT_MSG)
			pFrom = "chat/client";
		else
			pFrom = "chat/all";

		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, pFrom, aBuf, ChatLogColor);
	};

	// Custom color for new line
	std::optional<ColorRGBA> CustomColor = std::nullopt;
	if(ClientId == CLIENT_MSG)
		CustomColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageClientColor));

	CLine &PreviousLine = m_aLines[m_CurrentLine];

	// Team Number:
	// 0 = global; 1 = team; 2 = sending whisper; 3 = receiving whisper

	// If it's a client message, m_aText will have ": " prepended so we have to work around it.
	if(PreviousLine.m_Initialized &&
		PreviousLine.m_TeamNumber == Team &&
		PreviousLine.m_ClientId == ClientId &&
		str_comp(PreviousLine.m_aText, pLine) == 0 &&
		PreviousLine.m_CustomColor == CustomColor)
	{
		PreviousLine.m_TimesRepeated++;
		TextRender()->DeleteTextContainer(PreviousLine.m_TextContainerIndex);
		Graphics()->DeleteQuadContainer(PreviousLine.m_QuadContainerIndex);
		PreviousLine.m_Time = time();
		PreviousLine.m_aYOffset[0] = -1.0f;
		PreviousLine.m_aYOffset[1] = -1.0f;

		FChatMsgCheckAndPrint(PreviousLine);
		return;
	}

	m_CurrentLine = (m_CurrentLine + 1) % MAX_LINES;

	CLine &CurrentLine = m_aLines[m_CurrentLine];
	CurrentLine.Reset(*this);
	CurrentLine.m_Initialized = true;
	CurrentLine.m_Time = time();
	CurrentLine.m_aYOffset[0] = -1.0f;
	CurrentLine.m_aYOffset[1] = -1.0f;
	CurrentLine.m_ClientId = ClientId;
	CurrentLine.m_TeamNumber = Team;
	CurrentLine.m_Team = Team == 1;
	CurrentLine.m_Whisper = Team >= 2;
	CurrentLine.m_NameColor = -2;
	CurrentLine.m_CustomColor = CustomColor;

	// check for highlighted name
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(ClientId >= 0 && ClientId != GameClient()->m_aLocalIds[0] && ClientId != GameClient()->m_aLocalIds[1])
		{
			for(int LocalId : GameClient()->m_aLocalIds)
			{
				Highlighted |= LocalId >= 0 && LineShouldHighlight(pLine, GameClient()->m_aClients[LocalId].m_aName);
			}
		}
	}
	else
	{
		// on demo playback use local id from snap directly,
		// since m_aLocalIds isn't valid there
		Highlighted |= GameClient()->m_Snap.m_LocalClientId >= 0 && LineShouldHighlight(pLine, GameClient()->m_aClients[GameClient()->m_Snap.m_LocalClientId].m_aName);
	}
	CurrentLine.m_Highlighted = Highlighted;

	const bool FocusSuppress = IsFocusModeActive() && Team != 1;
	if(FocusSuppress)
	{
		CurrentLine.m_Highlighted = false;
		Highlighted = false;
	}

	if(g_Config.m_TcIgnoreTag && Highlighted && ClientId >= 0 && Team != TEAM_WHISPER_RECV && IsIgnoreTagActive() && !IsIgnoreTagSafe())
	{
		const char *pAuthor = GameClient()->m_aClients[ClientId].m_aName;
		if(pAuthor[0] != '\0')
			str_format(m_aIgnoreTagLastLine, sizeof(m_aIgnoreTagLastLine), "%s: %s", pAuthor, pLine);
		else
			str_copy(m_aIgnoreTagLastLine, pLine);
		m_IgnoreTagPendingCount++;
		m_IgnoreTagSafeStart = 0;
		CurrentLine.Reset(*this);
		m_CurrentLine = (m_CurrentLine + MAX_LINES - 1) % MAX_LINES;
		return;
	}

	str_copy(CurrentLine.m_aText, pLine);
	StartGifPreview(CurrentLine);

	if(CurrentLine.m_ClientId == SERVER_MSG)
	{
		str_copy(CurrentLine.m_aName, "*** ");
	}
	else if(CurrentLine.m_ClientId == CLIENT_MSG)
	{
		str_copy(CurrentLine.m_aName, "— ");
	}
	else
	{
		const auto &LineAuthor = GameClient()->m_aClients[CurrentLine.m_ClientId];

		if(LineAuthor.m_Active)
		{
			if(LineAuthor.m_Team == TEAM_SPECTATORS)
				CurrentLine.m_NameColor = TEAM_SPECTATORS;

			if(GameClient()->IsTeamPlay())
			{
				if(LineAuthor.m_Team == TEAM_RED)
					CurrentLine.m_NameColor = TEAM_RED;
				else if(LineAuthor.m_Team == TEAM_BLUE)
					CurrentLine.m_NameColor = TEAM_BLUE;
			}
		}

		if(Team == TEAM_WHISPER_SEND)
		{
			str_copy(CurrentLine.m_aName, "→");
			if(LineAuthor.m_Active)
			{
				str_append(CurrentLine.m_aName, " ");
				str_append(CurrentLine.m_aName, LineAuthor.m_aName);
			}
			CurrentLine.m_NameColor = TEAM_BLUE;
			CurrentLine.m_Highlighted = false;
			Highlighted = false;
		}
		else if(Team == TEAM_WHISPER_RECV)
		{
			str_copy(CurrentLine.m_aName, "←");
			if(LineAuthor.m_Active)
			{
				str_append(CurrentLine.m_aName, " ");
				str_append(CurrentLine.m_aName, LineAuthor.m_aName);
			}
			CurrentLine.m_NameColor = TEAM_RED;
			CurrentLine.m_Highlighted = true;
			Highlighted = true;
		}
		else
		{
			str_copy(CurrentLine.m_aName, LineAuthor.m_aName);
		}

		if(LineAuthor.m_Active)
		{
			CurrentLine.m_Friend = LineAuthor.m_Friend;
			CurrentLine.m_pManagedTeeRenderInfo = GameClient()->CreateManagedTeeRenderInfo(LineAuthor);
		}
	}

	FChatMsgCheckAndPrint(CurrentLine);

	// play sound
	int64_t Now = time();
	if(FocusSuppress)
	{
		// Focus Mode keeps the chat quiet except for the current DDRace team.
	}
	else if(ClientId == SERVER_MSG)
	{
		if(Now - m_aLastSoundPlayed[CHAT_SERVER] >= time_freq() * 3 / 10)
		{
			if(g_Config.m_SndServerMessage)
			{
				GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_SERVER, 1.0f);
				m_aLastSoundPlayed[CHAT_SERVER] = Now;
			}
		}
	}
	else if(ClientId == CLIENT_MSG)
	{
		// No sound yet
	}
	else if(Highlighted && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(Now - m_aLastSoundPlayed[CHAT_HIGHLIGHT] >= time_freq() * 3 / 10)
		{
			char aBuf[1024];
			str_format(aBuf, sizeof(aBuf), "%s: %s", CurrentLine.m_aName, CurrentLine.m_aText);
			Client()->Notify("DDNet Chat", aBuf);
			if(g_Config.m_SndHighlight)
			{
				GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_HIGHLIGHT, 1.0f);
				m_aLastSoundPlayed[CHAT_HIGHLIGHT] = Now;
			}

			if(g_Config.m_ClEditor)
			{
				GameClient()->Editor()->UpdateMentions();
			}
		}
	}
	else if(Team != TEAM_WHISPER_SEND)
	{
		if(Now - m_aLastSoundPlayed[CHAT_CLIENT] >= time_freq() * 3 / 10)
		{
			bool PlaySound = CurrentLine.m_Team ? g_Config.m_SndTeamChat : g_Config.m_SndChat;
#if defined(CONF_VIDEORECORDER)
			if(IVideo::Current())
			{
				PlaySound &= (bool)g_Config.m_ClVideoShowChat;
			}
#endif
			if(PlaySound)
			{
				GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_CLIENT, 1.0f);
				m_aLastSoundPlayed[CHAT_CLIENT] = Now;
			}
		}
	}

	// TClient
	GameClient()->m_Translate.AutoTranslate(CurrentLine);
}

void CChat::OnPrepareLines(float y)
{
	float x = 5.0f;
	float FontSize = this->FontSize();

	const bool IsScoreBoardOpen = GameClient()->m_Scoreboard.IsActive() && (Graphics()->ScreenAspect() > 1.7f); // only assume scoreboard when screen ratio is widescreen(something around 16:9)
	const bool ShowLargeArea = m_Show || (m_Mode != MODE_NONE && g_Config.m_ClShowChat == 1) || g_Config.m_ClShowChat == 2;
	const bool ForceRecreate = IsScoreBoardOpen != m_PrevScoreBoardShowed || ShowLargeArea != m_PrevShowChat;
	m_PrevScoreBoardShowed = IsScoreBoardOpen;
	m_PrevShowChat = ShowLargeArea;

	const int TeeSize = MessageTeeSize();
	float RealMsgPaddingX = MessagePaddingX();
	float RealMsgPaddingY = MessagePaddingY();
	float RealMsgPaddingTee = TeeSize + MESSAGE_TEE_PADDING_RIGHT;

	if(g_Config.m_ClChatOld)
	{
		RealMsgPaddingX = 0;
		RealMsgPaddingY = 0;
		RealMsgPaddingTee = 0;
	}

	int64_t Now = time();
	float LineWidth = (IsScoreBoardOpen ? maximum(85.0f, (FontSize * 85.0f / 6.0f)) : g_Config.m_ClChatWidth) - (RealMsgPaddingX * 1.5f) - RealMsgPaddingTee;

	float HeightLimit = IsScoreBoardOpen ? 180.0f : (m_PrevShowChat ? 50.0f : 200.0f);
	float Begin = x;
	float TextBegin = Begin + RealMsgPaddingX / 2.0f;
	int OffsetType = IsScoreBoardOpen ? 1 : 0;

	for(int i = 0; i < MAX_LINES; i++)
	{
		CLine &Line = m_aLines[((m_CurrentLine - i) + MAX_LINES) % MAX_LINES];
		if(!Line.m_Initialized)
			break;
		if(Now > Line.m_Time + 16 * time_freq() && !m_PrevShowChat)
			break;

		if(Line.m_TextContainerIndex.Valid() && !ForceRecreate)
			continue;

		TextRender()->DeleteTextContainer(Line.m_TextContainerIndex);
		Graphics()->DeleteQuadContainer(Line.m_QuadContainerIndex);

		char aClientId[16] = "";
		if(g_Config.m_ClShowIds && Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
		{
			GameClient()->FormatClientId(Line.m_ClientId, aClientId, EClientIdFormat::INDENT_AUTO);
		}

		char aCount[12];
		if(Line.m_ClientId < 0)
			str_format(aCount, sizeof(aCount), "[%d] ", Line.m_TimesRepeated + 1);
		else
			str_format(aCount, sizeof(aCount), " [%d]", Line.m_TimesRepeated + 1);

		const char *pText = Line.m_aText;
		if(Config()->m_ClStreamerMode && Line.m_ClientId == SERVER_MSG)
		{
			if(str_startswith(Line.m_aText, "Team save in progress. You'll be able to load with '/load ") && str_endswith(Line.m_aText, "'"))
			{
				pText = "Team save in progress. You'll be able to load with '/load *** *** ***'";
			}
			else if(str_startswith(Line.m_aText, "Team save in progress. You'll be able to load with '/load") && str_endswith(Line.m_aText, "if it fails"))
			{
				pText = "Team save in progress. You'll be able to load with '/load *** *** ***' if save is successful or with '/load *** *** ***' if it fails";
			}
			else if(str_startswith(Line.m_aText, "Team successfully saved by ") && str_endswith(Line.m_aText, " to continue"))
			{
				pText = "Team successfully saved by ***. Use '/load *** *** ***' to continue";
			}
		}

		const CColoredParts ColoredParts(pText, Line.m_ClientId == CLIENT_MSG);
		if(!ColoredParts.Colors().empty() && ColoredParts.Colors()[0].m_Index == 0)
			Line.m_CustomColor = ColoredParts.Colors()[0].m_Color;
		pText = ColoredParts.Text();

		const char *pTranslatedError = nullptr;
		const char *pTranslatedText = nullptr;
		const char *pTranslatedLanguage = nullptr;
		if(Line.m_pTranslateResponse != nullptr && Line.m_pTranslateResponse->m_Text[0])
		{
			// If hidden and there is translated text
			if(pText != Line.m_aText)
			{
				pTranslatedError = TCLocalize("Translated text hidden due to streamer mode");
			}
			else if(Line.m_pTranslateResponse->m_Error)
			{
				pTranslatedError = Line.m_pTranslateResponse->m_Text;
			}
			else
			{
				pTranslatedText = Line.m_pTranslateResponse->m_Text;
				if(Line.m_pTranslateResponse->m_Language[0] != '\0')
					pTranslatedLanguage = Line.m_pTranslateResponse->m_Language;
			}
		}

		// get the y offset (calculate it if we haven't done that yet)
		if(Line.m_aYOffset[OffsetType] < 0.0f)
		{
			CTextCursor MeasureCursor;
			MeasureCursor.SetPosition(vec2(TextBegin, 0.0f));
			MeasureCursor.m_FontSize = FontSize;
			MeasureCursor.m_Flags = 0;
			MeasureCursor.m_LineWidth = LineWidth;

			if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
			{
				MeasureCursor.m_X += RealMsgPaddingTee;

				if(Line.m_Friend && g_Config.m_ClMessageFriend)
				{
					TextRender()->TextEx(&MeasureCursor, "♥ ");
				}
			}

			TextRender()->TextEx(&MeasureCursor, aClientId);
			TextRender()->TextEx(&MeasureCursor, Line.m_aName);
			if(Line.m_TimesRepeated > 0)
				TextRender()->TextEx(&MeasureCursor, aCount);

			if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
			{
				TextRender()->TextEx(&MeasureCursor, ": ");
			}

			CTextCursor AppendCursor = MeasureCursor;
			AppendCursor.m_LongestLineWidth = 0.0f;
			if(!IsScoreBoardOpen && !g_Config.m_ClChatOld)
			{
				AppendCursor.m_StartX = MeasureCursor.m_X;
				AppendCursor.m_LineWidth -= MeasureCursor.m_LongestLineWidth;
			}

			if(pTranslatedText)
			{
				TextRender()->TextEx(&AppendCursor, pTranslatedText);
				if(pTranslatedLanguage)
				{
					TextRender()->TextEx(&AppendCursor, " [");
					TextRender()->TextEx(&AppendCursor, pTranslatedLanguage);
					TextRender()->TextEx(&AppendCursor, "]");
				}
				TextRender()->TextEx(&AppendCursor, "\n");
				AppendCursor.m_FontSize *= 0.8f;
				TextRender()->TextEx(&AppendCursor, pText);
				AppendCursor.m_FontSize /= 0.8f;
			}
			else if(pTranslatedError)
			{
				TextRender()->TextEx(&AppendCursor, pText);
				TextRender()->TextEx(&AppendCursor, "\n");
				AppendCursor.m_FontSize *= 0.8f;
				TextRender()->TextEx(&AppendCursor, pTranslatedError);
				AppendCursor.m_FontSize /= 0.8f;
			}
			else
			{
				TextRender()->TextEx(&AppendCursor, pText);
			}
			Line.m_aYOffset[OffsetType] = AppendCursor.Height() + RealMsgPaddingY;
			if(g_Config.m_TcChatGifPreview && !Line.m_vGifFrames.empty() && Line.m_GifWidth > 0 && Line.m_GifHeight > 0)
			{
				const float GifScale = minimum(LineWidth / (float)Line.m_GifWidth, 58.0f / (float)Line.m_GifHeight);
				Line.m_aYOffset[OffsetType] += maximum(1.0f, Line.m_GifHeight * GifScale) + FontSize * 0.4f;
			}
		}

		y -= Line.m_aYOffset[OffsetType];

		// cut off if msgs waste too much space
		if(y < HeightLimit)
			break;

		// the position the text was created
		Line.m_TextYOffset = y + RealMsgPaddingY / 2.0f;

		int CurRenderFlags = TextRender()->GetRenderFlags();
		TextRender()->SetRenderFlags(CurRenderFlags | ETextRenderFlags::TEXT_RENDER_FLAG_NO_AUTOMATIC_QUAD_UPLOAD);

		// reset the cursor
		CTextCursor LineCursor;
		LineCursor.SetPosition(vec2(TextBegin, Line.m_TextYOffset));
		LineCursor.m_FontSize = FontSize;
		LineCursor.m_LineWidth = LineWidth;

		// Message is from valid player
		if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
		{
			LineCursor.m_X += RealMsgPaddingTee;

			if(Line.m_Friend && g_Config.m_ClMessageFriend)
			{
				TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageFriendColor)).WithAlpha(1.0f));
				TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &LineCursor, "♥ ");
			}
		}

		// render name
		ColorRGBA NameColor;
		if(Line.m_CustomColor)
			NameColor = *Line.m_CustomColor;
		else if(Line.m_ClientId == SERVER_MSG)
			NameColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
		else if(Line.m_ClientId == CLIENT_MSG)
			NameColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageClientColor));
		else if(Line.m_ClientId >= 0 && g_Config.m_TcWarList && g_Config.m_TcWarListChat && GameClient()->m_WarList.GetAnyWar(Line.m_ClientId)) // TClient
			NameColor = GameClient()->m_WarList.GetPriorityColor(Line.m_ClientId);
		else if(Line.m_Team)
			NameColor = CalculateNameColor(ColorHSLA(g_Config.m_ClMessageTeamColor));
		else if(Line.m_NameColor == TEAM_RED)
			NameColor = ColorRGBA(1.0f, 0.5f, 0.5f, 1.0f);
		else if(Line.m_NameColor == TEAM_BLUE)
			NameColor = ColorRGBA(0.7f, 0.7f, 1.0f, 1.0f);
		else if(Line.m_NameColor == TEAM_SPECTATORS)
			NameColor = ColorRGBA(0.75f, 0.5f, 0.75f, 1.0f);
		else if(Line.m_ClientId >= 0 && g_Config.m_ClChatTeamColors && GameClient()->m_Teams.Team(Line.m_ClientId))
			NameColor = GameClient()->GetDDTeamColor(GameClient()->m_Teams.Team(Line.m_ClientId), 0.75f);
		else
			NameColor = ColorRGBA(0.8f, 0.8f, 0.8f, 1.0f);

		TextRender()->TextColor(NameColor);
		TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &LineCursor, aClientId);
		TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &LineCursor, Line.m_aName);

		if(Line.m_TimesRepeated > 0)
		{
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.3f);
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &LineCursor, aCount);
		}

		if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
		{
			TextRender()->TextColor(NameColor);
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &LineCursor, ": ");
		}

		ColorRGBA Color;
		if(Line.m_CustomColor)
			Color = *Line.m_CustomColor;
		else if(Line.m_ClientId == SERVER_MSG)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
		else if(Line.m_ClientId == CLIENT_MSG)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageClientColor));
		else if(Line.m_Highlighted)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageHighlightColor));
		else if(Line.m_Team)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageTeamColor));
		else // regular message
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageColor));
		TextRender()->TextColor(Color);

		CTextCursor AppendCursor = LineCursor;
		AppendCursor.m_LongestLineWidth = 0.0f;
		if(!IsScoreBoardOpen && !g_Config.m_ClChatOld)
		{
			AppendCursor.m_StartX = LineCursor.m_X;
			AppendCursor.m_LineWidth -= LineCursor.m_LongestLineWidth;
		}

		if(pTranslatedText)
		{
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, pTranslatedText);
			if(pTranslatedLanguage)
			{
				ColorRGBA ColorLang = Color;
				ColorLang.r *= 0.8f;
				ColorLang.g *= 0.8f;
				ColorLang.b *= 0.8f;
				TextRender()->TextColor(ColorLang);
				TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, " [");
				TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, pTranslatedLanguage);
				TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, "]");
			}
			ColorRGBA ColorSub = Color;
			ColorSub.r *= 0.7f;
			ColorSub.g *= 0.7f;
			ColorSub.b *= 0.7f;
			TextRender()->TextColor(ColorSub);
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, "\n");
			AppendCursor.m_FontSize *= 0.8f;
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, pText);
			AppendCursor.m_FontSize /= 0.8f;
			TextRender()->TextColor(Color);
		}
		else if(pTranslatedError)
		{
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, pText);
			ColorRGBA ColorSub = Color;
			ColorSub.r = 0.7f;
			ColorSub.g = 0.6f;
			ColorSub.b = 0.6f;
			TextRender()->TextColor(ColorSub);
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, "\n");
			AppendCursor.m_FontSize *= 0.8f;
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, pTranslatedError);
			AppendCursor.m_FontSize /= 0.8f;
			TextRender()->TextColor(Color);
		}
		else
		{
			ColoredParts.AddSplitsToCursor(AppendCursor);
			TextRender()->CreateOrAppendTextContainer(Line.m_TextContainerIndex, &AppendCursor, pText);
			AppendCursor.m_vColorSplits.clear();
		}
		if(!g_Config.m_ClChatOld && (Line.m_aText[0] != '\0' || Line.m_aName[0] != '\0'))
		{
			float FullWidth = RealMsgPaddingX * 1.5f;
			if(!IsScoreBoardOpen && !g_Config.m_ClChatOld)
			{
				FullWidth += LineCursor.m_LongestLineWidth + AppendCursor.m_LongestLineWidth;
			}
			else
			{
				FullWidth += maximum(LineCursor.m_LongestLineWidth, AppendCursor.m_LongestLineWidth);
			}
			if(g_Config.m_TcChatGifPreview && !Line.m_vGifFrames.empty() && Line.m_GifWidth > 0 && Line.m_GifHeight > 0)
			{
				const float GifScale = minimum(LineWidth / (float)Line.m_GifWidth, 58.0f / (float)Line.m_GifHeight);
				FullWidth = maximum(FullWidth, RealMsgPaddingX * 1.5f + Line.m_GifWidth * GifScale);
			}
			Graphics()->SetColor(1, 1, 1, 1);
			Line.m_QuadContainerIndex = Graphics()->CreateRectQuadContainer(Begin, y, FullWidth, Line.m_aYOffset[OffsetType], MessageRounding(), IGraphics::CORNER_ALL);
		}

		TextRender()->SetRenderFlags(CurRenderFlags);
		if(Line.m_TextContainerIndex.Valid())
			TextRender()->UploadTextContainer(Line.m_TextContainerIndex);
	}

	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CChat::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	UpdateIgnoreTag();
	UpdateHourRequest();
	UpdateGifPreviews();

	// send pending chat messages
	if(m_PendingChatCounter > 0 && m_LastChatSend + time_freq() < time())
	{
		CHistoryEntry *pEntry = m_History.Last();
		for(int i = m_PendingChatCounter - 1; pEntry; --i, pEntry = m_History.Prev(pEntry))
		{
			if(i == 0)
			{
				SendChat(pEntry->m_Team, pEntry->m_aText);
				break;
			}
		}
		--m_PendingChatCounter;
	}

	const float Height = 300.0f;
	const float Width = Height * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, Width, Height);

	float x = 5.0f;

	// TClient
	float y = 300.0f - (20.0f * FontSize() / 6.0f + (g_Config.m_TcStatusBar ? g_Config.m_TcStatusBarHeight : 0.0f));
	// float y = 300.0f - 20.0f * FontSize() / 6.0f;

	float ScaledFontSize = FontSize() * (8.0f / 6.0f);
	if(m_Mode != MODE_NONE)
	{
		// render chat input
		CTextCursor InputCursor;
		InputCursor.SetPosition(vec2(x, y));
		InputCursor.m_FontSize = ScaledFontSize;
		InputCursor.m_LineWidth = Width - 190.0f;

		// TClient
		InputCursor.m_LineWidth = std::max(Width - 190.0f, 190.0f);

		if(m_Mode == MODE_ALL)
			TextRender()->TextEx(&InputCursor, Localize("All"));
		else if(m_Mode == MODE_TEAM)
			TextRender()->TextEx(&InputCursor, Localize("Team"));
		else
			TextRender()->TextEx(&InputCursor, Localize("Chat"));

		TextRender()->TextEx(&InputCursor, ": ");

		const float MessageMaxWidth = InputCursor.m_LineWidth - (InputCursor.m_X - InputCursor.m_StartX);
		const CUIRect ClippingRect = {InputCursor.m_X, InputCursor.m_Y, MessageMaxWidth, 2.25f * InputCursor.m_FontSize};
		const float XScale = Graphics()->ScreenWidth() / Width;
		const float YScale = Graphics()->ScreenHeight() / Height;
		Graphics()->ClipEnable((int)(ClippingRect.x * XScale), (int)(ClippingRect.y * YScale), (int)(ClippingRect.w * XScale), (int)(ClippingRect.h * YScale));

		float ScrollOffset = m_Input.GetScrollOffset();
		float ScrollOffsetChange = m_Input.GetScrollOffsetChange();

		m_Input.Activate(EInputPriority::CHAT); // Ensure that the input is active
		const CUIRect InputCursorRect = {InputCursor.m_X, InputCursor.m_Y - ScrollOffset, 0.0f, 0.0f};
		const bool WasChanged = m_Input.WasChanged();
		const bool WasCursorChanged = m_Input.WasCursorChanged();
		const bool Changed = WasChanged || WasCursorChanged;
		const STextBoundingBox BoundingBox = m_Input.Render(&InputCursorRect, InputCursor.m_FontSize, TEXTALIGN_TL, Changed, MessageMaxWidth, 0.0f);

		Graphics()->ClipDisable();

		// Scroll up or down to keep the caret inside the clipping rect
		const float CaretPositionY = m_Input.GetCaretPosition().y - ScrollOffsetChange;
		if(CaretPositionY < ClippingRect.y)
			ScrollOffsetChange -= ClippingRect.y - CaretPositionY;
		else if(CaretPositionY + InputCursor.m_FontSize > ClippingRect.y + ClippingRect.h)
			ScrollOffsetChange += CaretPositionY + InputCursor.m_FontSize - (ClippingRect.y + ClippingRect.h);

		Ui()->DoSmoothScrollLogic(&ScrollOffset, &ScrollOffsetChange, ClippingRect.h, BoundingBox.m_H);

		m_Input.SetScrollOffset(ScrollOffset);
		m_Input.SetScrollOffsetChange(ScrollOffsetChange);

		// Autocompletion hint
		if(m_Input.GetString()[0] == '/' && m_Input.GetString()[1] != '\0' && !m_vServerCommands.empty())
		{
			for(const auto &Command : m_vServerCommands)
			{
				if(str_startswith_nocase(Command.m_aName, m_Input.GetString() + 1))
				{
					InputCursor.m_X = InputCursor.m_X + TextRender()->TextWidth(InputCursor.m_FontSize, m_Input.GetString(), -1, InputCursor.m_LineWidth);
					InputCursor.m_Y = m_Input.GetCaretPosition().y;
					TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.5f);
					TextRender()->TextEx(&InputCursor, Command.m_aName + str_length(m_Input.GetString() + 1));
					TextRender()->TextColor(TextRender()->DefaultTextColor());
					break;
				}
			}
		}
	}

#if defined(CONF_VIDEORECORDER)
	if(!((g_Config.m_ClShowChat && !IVideo::Current()) || (g_Config.m_ClVideoShowChat && IVideo::Current())))
#else
	if(!g_Config.m_ClShowChat)
#endif
		return;

	y -= ScaledFontSize;

	OnPrepareLines(y);

	bool IsScoreBoardOpen = GameClient()->m_Scoreboard.IsActive() && (Graphics()->ScreenAspect() > 1.7f); // only assume scoreboard when screen ratio is widescreen(something around 16:9)

	int64_t Now = time();
	float HeightLimit = IsScoreBoardOpen ? 180.0f : (m_PrevShowChat ? 50.0f : 200.0f);
	int OffsetType = IsScoreBoardOpen ? 1 : 0;

	float RealMsgPaddingX = MessagePaddingX();
	float RealMsgPaddingY = MessagePaddingY();

	if(g_Config.m_ClChatOld)
	{
		RealMsgPaddingX = 0;
		RealMsgPaddingY = 0;
	}

	for(int i = 0; i < MAX_LINES; i++)
	{
		CLine &Line = m_aLines[((m_CurrentLine - i) + MAX_LINES) % MAX_LINES];
		if(!Line.m_Initialized)
			break;
		if(Now > Line.m_Time + 16 * time_freq() && !m_PrevShowChat)
			break;
		if(IsFocusModeActive() && !Line.m_Team)
			continue;

		y -= Line.m_aYOffset[OffsetType];

		// cut off if msgs waste too much space
		if(y < HeightLimit)
			break;

		float Blend = Now > Line.m_Time + 14 * time_freq() && !m_PrevShowChat ? 1.0f - (Now - Line.m_Time - 14 * time_freq()) / (2.0f * time_freq()) : 1.0f;

		// Draw backgrounds for messages in one batch
		if(!g_Config.m_ClChatOld)
		{
			Graphics()->TextureClear();
			if(Line.m_QuadContainerIndex != -1)
			{
				Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClChatBackgroundColor, true)).WithMultipliedAlpha(Blend));
				Graphics()->RenderQuadContainerEx(Line.m_QuadContainerIndex, 0, -1, 0, ((y + RealMsgPaddingY / 2.0f) - Line.m_TextYOffset));
			}
		}

		if(Line.m_TextContainerIndex.Valid())
		{
			if(!g_Config.m_ClChatOld && Line.m_pManagedTeeRenderInfo != nullptr)
			{
				CTeeRenderInfo &TeeRenderInfo = Line.m_pManagedTeeRenderInfo->TeeRenderInfo();
				const int TeeSize = MessageTeeSize();
				TeeRenderInfo.m_Size = TeeSize;

				float RowHeight = FontSize() + RealMsgPaddingY;
				float OffsetTeeY = TeeSize / 2.0f;
				float FullHeightMinusTee = RowHeight - TeeSize;

				const CAnimState *pIdleState = CAnimState::GetIdle();
				vec2 OffsetToMid;
				CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeRenderInfo, OffsetToMid);
				vec2 TeeRenderPos(x + (RealMsgPaddingX + TeeSize) / 2.0f, y + OffsetTeeY + FullHeightMinusTee / 2.0f + OffsetToMid.y);
				RenderTools()->RenderTee(pIdleState, &TeeRenderInfo, EMOTE_NORMAL, vec2(1, 0.1f), TeeRenderPos, Blend);
			}

			const ColorRGBA TextColor = TextRender()->DefaultTextColor().WithMultipliedAlpha(Blend);
			const ColorRGBA TextOutlineColor = TextRender()->DefaultTextOutlineColor().WithMultipliedAlpha(Blend);
			TextRender()->RenderTextContainer(Line.m_TextContainerIndex, TextColor, TextOutlineColor, 0, (y + RealMsgPaddingY / 2.0f) - Line.m_TextYOffset);
		}

		if(g_Config.m_TcChatGifPreview && !Line.m_vGifFrames.empty() && Line.m_GifWidth > 0 && Line.m_GifHeight > 0)
		{
			const float AvailableWidth = (IsScoreBoardOpen ? maximum(85.0f, FontSize() * 85.0f / 6.0f) : (float)g_Config.m_ClChatWidth) - RealMsgPaddingX * 1.5f;
			const float GifScale = minimum(AvailableWidth / (float)Line.m_GifWidth, 58.0f / (float)Line.m_GifHeight);
			const float GifW = maximum(1.0f, Line.m_GifWidth * GifScale);
			const float GifH = maximum(1.0f, Line.m_GifHeight * GifScale);
			const float GifX = x + RealMsgPaddingX / 2.0f;
			const float GifY = y + Line.m_aYOffset[OffsetType] - GifH - RealMsgPaddingY / 2.0f;
			const IGraphics::CTextureHandle Texture = GifFrameTexture(Line);
			if(Texture.IsValid())
			{
				Graphics()->TextureSet(Texture);
				Graphics()->QuadsBegin();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, Blend);
				IGraphics::CQuadItem Quad(GifX, GifY, GifW, GifH);
				Graphics()->QuadsDrawTL(&Quad, 1);
				Graphics()->QuadsEnd();
			}
		}
	}
}

void CChat::EnsureCoherentFontSize() const
{
	// Adjust font size based on width
	if(g_Config.m_ClChatWidth / (float)g_Config.m_ClChatFontSize >= CHAT_FONTSIZE_WIDTH_RATIO)
		return;

	// We want to keep a ration between font size and font width so that we don't have a weird rendering
	g_Config.m_ClChatFontSize = g_Config.m_ClChatWidth / CHAT_FONTSIZE_WIDTH_RATIO;
}

void CChat::EnsureCoherentWidth() const
{
	// Adjust width based on font size
	if(g_Config.m_ClChatWidth / (float)g_Config.m_ClChatFontSize >= CHAT_FONTSIZE_WIDTH_RATIO)
		return;

	// We want to keep a ration between font size and font width so that we don't have a weird rendering
	g_Config.m_ClChatWidth = CHAT_FONTSIZE_WIDTH_RATIO * g_Config.m_ClChatFontSize;
}

// ----- send functions -----

void CChat::SendChat(int Team, const char *pLine)
{
	// don't send empty messages
	if(*str_utf8_skip_whitespaces(pLine) == '\0')
		return;

	m_LastChatSend = time();

	if(GameClient()->Client()->IsSixup())
	{
		protocol7::CNetMsg_Cl_Say Msg7;
		Msg7.m_Mode = Team == 1 ? protocol7::CHAT_TEAM : protocol7::CHAT_ALL;
		Msg7.m_Target = -1;
		Msg7.m_pMessage = pLine;
		Client()->SendPackMsgActive(&Msg7, MSGFLAG_VITAL, true);
		return;
	}

	// send chat message
	CNetMsg_Cl_Say Msg;
	Msg.m_Team = Team;
	Msg.m_pMessage = pLine;
	Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
}

void CChat::SendChatQueued(const char *pLine)
{
	if(!pLine || str_length(pLine) < 1)
		return;

	bool AddEntry = false;

	if(m_LastChatSend + time_freq() < time())
	{
		SendChat(m_Mode == MODE_ALL ? 0 : 1, pLine);
		AddEntry = true;
	}
	else if(m_PendingChatCounter < 3)
	{
		++m_PendingChatCounter;
		AddEntry = true;
	}

	if(AddEntry)
	{
		const int Length = str_length(pLine);
		CHistoryEntry *pEntry = m_History.Allocate(sizeof(CHistoryEntry) + Length);
		pEntry->m_Team = m_Mode == MODE_ALL ? 0 : 1;
		str_copy(pEntry->m_aText, pLine, Length + 1);
	}
}
