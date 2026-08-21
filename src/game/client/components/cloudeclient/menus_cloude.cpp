#include <base/math.h>
#include <base/system.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/http.h>
#include <engine/shared/json.h>
#include <engine/shared/linereader.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/client/ui_scrollregion.h>
#include <game/localization.h>

#include <algorithm>
#include <string>
#include <vector>

static constexpr const char *CLOUDE_CONFIG_DIR = "tclient/cloude_configs";
static constexpr const char *CLOUDE_CONFIG_MAGIC = "CLOUDE_CONFIG_V1";

static void CloudeConfigAppendLine(std::string &Output, const char *pLine)
{
	Output.append(pLine);
	Output.push_back('\n');
}

static void CloudeConfigAppendInt(std::string &Output, const char *pName, int Value)
{
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "%s=%d", pName, Value);
	CloudeConfigAppendLine(Output, aBuf);
}

static void CloudeConfigAppendFloat(std::string &Output, const char *pName, float Value)
{
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "%s=%.6g", pName, Value);
	CloudeConfigAppendLine(Output, aBuf);
}

static void CloudeConfigAppendStr(std::string &Output, const char *pName, const char *pValue)
{
	char aBuf[768];
	str_format(aBuf, sizeof(aBuf), "%s=%s", pName, pValue);
	CloudeConfigAppendLine(Output, aBuf);
}

static bool CloudeConfigIsSafeFilename(const char *pName)
{
	if(!pName || !pName[0] || str_length(pName) >= 96 || str_find(pName, ".."))
		return false;
	for(const char *p = pName; *p; ++p)
	{
		if((unsigned char)*p <= ' ' || *p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
			return false;
	}
	return true;
}

static bool CloudeConfigMakePath(const char *pFilename, char *pPath, int PathSize)
{
	if(!CloudeConfigIsSafeFilename(pFilename))
		return false;

	char aFilename[128];
	str_copy(aFilename, pFilename);
	if(!str_endswith(aFilename, ".cloude"))
		str_append(aFilename, ".cloude", sizeof(aFilename));

	if(!CloudeConfigIsSafeFilename(aFilename))
		return false;
	str_format(pPath, PathSize, "%s/%s", CLOUDE_CONFIG_DIR, aFilename);
	return true;
}

static char *CloudeConfigTrim(char *pStr)
{
	while(*pStr == ' ' || *pStr == '\t' || *pStr == '\r' || *pStr == '\n')
		++pStr;
	char *pEnd = pStr + str_length(pStr);
	while(pEnd > pStr && (pEnd[-1] == ' ' || pEnd[-1] == '\t' || pEnd[-1] == '\r' || pEnd[-1] == '\n'))
		*--pEnd = '\0';
	return pStr;
}

static int CloudeConfigReadInt(const char *pValue, int Min, int Max)
{
	int Value = 0;
	if(!str_toint(pValue, &Value))
		return Min;
	return std::clamp(Value, Min, Max);
}

static float CloudeConfigReadFloat(const char *pValue, float Min, float Max)
{
	float Value = 0.0f;
	if(!str_tofloat(pValue, &Value))
		return Min;
	return std::clamp(Value, Min, Max);
}

static void CloudeConfigApplyValue(const char *pName, const char *pValue)
{
	if(!str_comp(pName, "tc_fast_input"))
		g_Config.m_TcFastInput = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_fast_input_mode"))
		g_Config.m_TcFastInputMode = CloudeConfigReadInt(pValue, 0, 3);
	else if(!str_comp(pName, "tc_fast_input_amount"))
		g_Config.m_TcFastInputAmount = CloudeConfigReadInt(pValue, 1, 60);
	else if(!str_comp(pName, "tc_fast_input_saiko_amount"))
		g_Config.m_TcFastInputSaikoAmount = CloudeConfigReadFloat(pValue, 0.0f, 3.0f);
	else if(!str_comp(pName, "tc_fast_input_best_amount"))
		g_Config.m_TcFastInputBestAmount = CloudeConfigReadInt(pValue, 1, 60);
	else if(!str_comp(pName, "tc_fast_input_bestplus_aggression"))
		g_Config.m_TcFastInputBestPlusAggression = CloudeConfigReadInt(pValue, 0, 100);
	else if(!str_comp(pName, "tc_fast_input_bestplus_movement"))
		g_Config.m_TcFastInputBestPlusMovement = CloudeConfigReadInt(pValue, 14, 60);
	else if(!str_comp(pName, "tc_fast_input_bestplus_aimhook"))
		g_Config.m_TcFastInputBestPlusAimHook = CloudeConfigReadInt(pValue, 14, 60);
	else if(!str_comp(pName, "tc_fast_input_bestplus_dynamic_boost"))
		g_Config.m_TcFastInputBestPlusDynamicBoost = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_fast_input_others"))
		g_Config.m_TcFastInputOthers = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_micro_direction_assist"))
		g_Config.m_TcMicroDirectionAssist = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_hook_timing_buffer"))
		g_Config.m_TcHookTimingBuffer = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_jump_buffer"))
		g_Config.m_TcJumpBuffer = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_ignore_tag"))
		g_Config.m_TcIgnoreTag = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_focus_mode"))
		g_Config.m_TcFocusMode = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_play_mode_choice"))
		g_Config.m_TcPlayModeChoice = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_aled_counter"))
		g_Config.m_TcAledCounter = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_aled_auto_record"))
		g_Config.m_TcAledAutoRecord = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_aled_record_notify"))
		g_Config.m_TcAledRecordNotify = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_input_doctor"))
		g_Config.m_TcInputDoctor = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_motion_blur"))
		g_Config.m_TcMotionBlur = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_motion_blur_strength"))
		g_Config.m_TcMotionBlurStrength = CloudeConfigReadInt(pValue, 0, 95);
	else if(!str_comp(pName, "tc_team_name_gradient"))
		g_Config.m_TcTeamNameGradient = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_chat_gif_preview"))
		g_Config.m_TcChatGifPreview = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_rain_visual"))
		g_Config.m_TcRainVisual = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_rain_amount"))
		g_Config.m_TcRainAmount = CloudeConfigReadInt(pValue, 5, 200);
	else if(!str_comp(pName, "tc_rain_strength"))
		g_Config.m_TcRainStrength = CloudeConfigReadInt(pValue, 1, 100);
	else if(!str_comp(pName, "tc_rain_speed"))
		g_Config.m_TcRainSpeed = CloudeConfigReadInt(pValue, 20, 300);
	else if(!str_comp(pName, "tc_auto_fire"))
		g_Config.m_TcAutoFire = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_auto_fire_speed"))
		g_Config.m_TcAutoFireSpeed = CloudeConfigReadInt(pValue, 1, 25);
	else if(!str_comp(pName, "tc_auto_swap_gun_hammer"))
		g_Config.m_TcAutoSwapGunHammer = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_auto_swap_return_gun"))
		g_Config.m_TcAutoSwapReturnGun = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_cloude_dev_presence_url"))
		str_copy(g_Config.m_TcCloudeDevPresenceUrl, pValue, sizeof(g_Config.m_TcCloudeDevPresenceUrl));
	else if(!str_comp(pName, "tc_media_island"))
		g_Config.m_TcMediaIsland = CloudeConfigReadInt(pValue, 0, 1);
	else if(!str_comp(pName, "tc_hud_island_x"))
		g_Config.m_TcHudIslandX = CloudeConfigReadInt(pValue, 0, 1000);
	else if(!str_comp(pName, "tc_hud_island_y"))
		g_Config.m_TcHudIslandY = CloudeConfigReadInt(pValue, 0, 1000);
	else if(!str_comp(pName, "tc_hud_island_scale"))
		g_Config.m_TcHudIslandScale = CloudeConfigReadInt(pValue, 50, 160);
	else if(!str_comp(pName, "tc_hud_vote_x"))
		g_Config.m_TcHudVoteX = CloudeConfigReadInt(pValue, 0, 1000);
	else if(!str_comp(pName, "tc_hud_vote_y"))
		g_Config.m_TcHudVoteY = CloudeConfigReadInt(pValue, 0, 1000);
	else if(!str_comp(pName, "tc_cloude_language"))
		g_Config.m_TcCloudeLanguage = CloudeConfigReadInt(pValue, 0, 1);
}

static std::string BuildCloudeConfigText()
{
	std::string Output;
	CloudeConfigAppendLine(Output, CLOUDE_CONFIG_MAGIC);
	CloudeConfigAppendInt(Output, "tc_fast_input", g_Config.m_TcFastInput);
	CloudeConfigAppendInt(Output, "tc_fast_input_mode", g_Config.m_TcFastInputMode);
	CloudeConfigAppendInt(Output, "tc_fast_input_amount", g_Config.m_TcFastInputAmount);
	CloudeConfigAppendFloat(Output, "tc_fast_input_saiko_amount", g_Config.m_TcFastInputSaikoAmount);
	CloudeConfigAppendInt(Output, "tc_fast_input_best_amount", g_Config.m_TcFastInputBestAmount);
	CloudeConfigAppendInt(Output, "tc_fast_input_bestplus_aggression", g_Config.m_TcFastInputBestPlusAggression);
	CloudeConfigAppendInt(Output, "tc_fast_input_bestplus_movement", g_Config.m_TcFastInputBestPlusMovement);
	CloudeConfigAppendInt(Output, "tc_fast_input_bestplus_aimhook", g_Config.m_TcFastInputBestPlusAimHook);
	CloudeConfigAppendInt(Output, "tc_fast_input_bestplus_dynamic_boost", g_Config.m_TcFastInputBestPlusDynamicBoost);
	CloudeConfigAppendInt(Output, "tc_fast_input_others", g_Config.m_TcFastInputOthers);
	CloudeConfigAppendInt(Output, "tc_micro_direction_assist", g_Config.m_TcMicroDirectionAssist);
	CloudeConfigAppendInt(Output, "tc_hook_timing_buffer", g_Config.m_TcHookTimingBuffer);
	CloudeConfigAppendInt(Output, "tc_jump_buffer", g_Config.m_TcJumpBuffer);
	CloudeConfigAppendInt(Output, "tc_ignore_tag", g_Config.m_TcIgnoreTag);
	CloudeConfigAppendInt(Output, "tc_focus_mode", g_Config.m_TcFocusMode);
	CloudeConfigAppendInt(Output, "tc_play_mode_choice", g_Config.m_TcPlayModeChoice);
	CloudeConfigAppendInt(Output, "tc_aled_counter", g_Config.m_TcAledCounter);
	CloudeConfigAppendInt(Output, "tc_aled_auto_record", g_Config.m_TcAledAutoRecord);
	CloudeConfigAppendInt(Output, "tc_aled_record_notify", g_Config.m_TcAledRecordNotify);
	CloudeConfigAppendInt(Output, "tc_input_doctor", g_Config.m_TcInputDoctor);
	CloudeConfigAppendInt(Output, "tc_motion_blur", g_Config.m_TcMotionBlur);
	CloudeConfigAppendInt(Output, "tc_motion_blur_strength", g_Config.m_TcMotionBlurStrength);
	CloudeConfigAppendInt(Output, "tc_team_name_gradient", g_Config.m_TcTeamNameGradient);
	CloudeConfigAppendInt(Output, "tc_chat_gif_preview", g_Config.m_TcChatGifPreview);
	CloudeConfigAppendInt(Output, "tc_rain_visual", g_Config.m_TcRainVisual);
	CloudeConfigAppendInt(Output, "tc_rain_amount", g_Config.m_TcRainAmount);
	CloudeConfigAppendInt(Output, "tc_rain_strength", g_Config.m_TcRainStrength);
	CloudeConfigAppendInt(Output, "tc_rain_speed", g_Config.m_TcRainSpeed);
	CloudeConfigAppendInt(Output, "tc_auto_fire", g_Config.m_TcAutoFire);
	CloudeConfigAppendInt(Output, "tc_auto_fire_speed", g_Config.m_TcAutoFireSpeed);
	CloudeConfigAppendInt(Output, "tc_auto_swap_gun_hammer", g_Config.m_TcAutoSwapGunHammer);
	CloudeConfigAppendInt(Output, "tc_auto_swap_return_gun", g_Config.m_TcAutoSwapReturnGun);
	CloudeConfigAppendStr(Output, "tc_cloude_dev_presence_url", g_Config.m_TcCloudeDevPresenceUrl);
	CloudeConfigAppendInt(Output, "tc_media_island", g_Config.m_TcMediaIsland);
	CloudeConfigAppendInt(Output, "tc_hud_island_x", g_Config.m_TcHudIslandX);
	CloudeConfigAppendInt(Output, "tc_hud_island_y", g_Config.m_TcHudIslandY);
	CloudeConfigAppendInt(Output, "tc_hud_island_scale", g_Config.m_TcHudIslandScale);
	CloudeConfigAppendInt(Output, "tc_hud_vote_x", g_Config.m_TcHudVoteX);
	CloudeConfigAppendInt(Output, "tc_hud_vote_y", g_Config.m_TcHudVoteY);
	CloudeConfigAppendInt(Output, "tc_cloude_language", g_Config.m_TcCloudeLanguage);
	return Output;
}

static bool WriteCloudeConfigText(IStorage *pStorage, const char *pFilename, const char *pText)
{
	pStorage->CreateFolder("tclient", IStorage::TYPE_SAVE);
	pStorage->CreateFolder(CLOUDE_CONFIG_DIR, IStorage::TYPE_SAVE);

	char aPath[IO_MAX_PATH_LENGTH];
	if(!CloudeConfigMakePath(pFilename, aPath, sizeof(aPath)))
		return false;

	IOHANDLE File = pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return false;
	const int Length = str_length(pText);
	const bool Success = io_write(File, pText, Length) == (unsigned)Length;
	io_close(File);
	return Success;
}

static bool ExportCloudeConfig(IStorage *pStorage, const char *pFilename)
{
	const std::string Text = BuildCloudeConfigText();
	return WriteCloudeConfigText(pStorage, pFilename, Text.c_str());
}

static bool ImportCloudeConfig(IStorage *pStorage, const char *pFilename)
{
	char aPath[IO_MAX_PATH_LENGTH];
	if(!CloudeConfigMakePath(pFilename, aPath, sizeof(aPath)))
		return false;

	CLineReader LineReader;
	if(!LineReader.OpenFile(pStorage->OpenFile(aPath, IOFLAG_READ, IStorage::TYPE_SAVE)))
		return false;

	char *pLine = const_cast<char *>(LineReader.Get());
	if(!pLine || str_comp(CloudeConfigTrim(pLine), CLOUDE_CONFIG_MAGIC))
		return false;

	while((pLine = const_cast<char *>(LineReader.Get())))
	{
		pLine = CloudeConfigTrim(pLine);
		if(!pLine[0] || pLine[0] == '#')
			continue;

		char *pSeparator = const_cast<char *>(str_find(pLine, "="));
		if(!pSeparator)
			continue;
		*pSeparator = '\0';
		char *pName = CloudeConfigTrim(pLine);
		char *pValue = CloudeConfigTrim(pSeparator + 1);
		CloudeConfigApplyValue(pName, pValue);
	}
	return true;
}

static bool ImportCloudeConfigText(const char *pText)
{
	if(!pText)
		return false;

	std::string Text(pText);
	size_t LineStart = 0;
	bool CheckedMagic = false;
	while(LineStart <= Text.size())
	{
		size_t LineEnd = Text.find('\n', LineStart);
		if(LineEnd == std::string::npos)
			LineEnd = Text.size();

		std::string Line = Text.substr(LineStart, LineEnd - LineStart);
		char aLine[1024];
		str_copy(aLine, Line.c_str());
		char *pLine = CloudeConfigTrim(aLine);

		if(!CheckedMagic)
		{
			CheckedMagic = true;
			if(str_comp(pLine, CLOUDE_CONFIG_MAGIC))
				return false;
		}
		else if(pLine[0] && pLine[0] != '#')
		{
			char *pSeparator = const_cast<char *>(str_find(pLine, "="));
			if(pSeparator)
			{
				*pSeparator = '\0';
				char *pName = CloudeConfigTrim(pLine);
				char *pValue = CloudeConfigTrim(pSeparator + 1);
				CloudeConfigApplyValue(pName, pValue);
			}
		}

		if(LineEnd == Text.size())
			break;
		LineStart = LineEnd + 1;
	}
	return CheckedMagic;
}

static bool ReadCloudeConfigText(IStorage *pStorage, const char *pFilename, std::string &Output)
{
	char aPath[IO_MAX_PATH_LENGTH];
	if(!CloudeConfigMakePath(pFilename, aPath, sizeof(aPath)))
		return false;

	void *pData = nullptr;
	unsigned DataSize = 0;
	if(!pStorage->ReadFile(aPath, IStorage::TYPE_SAVE, &pData, &DataSize))
		return false;

	Output.assign((const char *)pData, DataSize);
	free(pData);
	return str_startswith(Output.c_str(), CLOUDE_CONFIG_MAGIC);
}

static bool CloudeConfigShareEndpoint(char *pBuf, int BufSize, const char *pPath)
{
	if(g_Config.m_TcCloudeDevPresenceUrl[0] == '\0')
		return false;

	const int UrlLen = str_length(g_Config.m_TcCloudeDevPresenceUrl);
	const bool HasSlash = UrlLen > 0 && g_Config.m_TcCloudeDevPresenceUrl[UrlLen - 1] == '/';
	str_format(pBuf, BufSize, "%s%s%s", g_Config.m_TcCloudeDevPresenceUrl, HasSlash ? "" : "/", pPath);
	return true;
}

static bool CloudeConfigIsShareCode(const char *pCode)
{
	if(str_length(pCode) != 6)
		return false;
	for(const char *p = pCode; *p; ++p)
	{
		if(*p < '0' || *p > '9')
			return false;
	}
	return true;
}

struct SCloudeConfigFile
{
	char m_aName[128];
};

static int CloudeConfigListCallback(const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser)
{
	if(IsDir || StorageType != IStorage::TYPE_SAVE || !str_endswith(pInfo->m_pName, ".cloude") || !CloudeConfigIsSafeFilename(pInfo->m_pName))
		return 0;

	auto *pvFiles = (std::vector<SCloudeConfigFile> *)pUser;
	SCloudeConfigFile File;
	str_copy(File.m_aName, pInfo->m_pName);
	pvFiles->push_back(File);
	return 0;
}

struct SFastInputAutoSettings
{
	bool m_Running = false;
	int m_Mode = 0;
	int m_Preference = 1;
	int m_Samples = 0;
	int m_Sum = 0;
	int m_Min = 9999;
	int m_Max = 0;
	int64_t m_StartTime = 0;
	int64_t m_LastSampleTime = 0;
	char m_aStatus[128] = "";
};

static float FastInputPreferenceOffset(int Preference)
{
	if(Preference == 0)
		return -4.0f;
	if(Preference == 2)
		return 5.0f;
	return 0.0f;
}

static int FastInputAutoAmountMs(int Mode, int AvgPing, int Jitter, int Preference)
{
	const float Base = Mode == 2 ? 15.0f : 10.0f;
	const float PingFactor = Mode == 2 ? 0.12f : 0.10f;
	const int MaxAmount = Mode == 2 ? 40 : 38;
	return std::clamp(round_to_int(Base + AvgPing * PingFactor + Jitter * 0.35f + FastInputPreferenceOffset(Preference)), 6, MaxAmount);
}

static float FastInputAutoSaikoAmount(int AvgPing, int Jitter, int Preference)
{
	return std::clamp((FastInputAutoAmountMs(1, AvgPing, Jitter, Preference) / 20.0f) + Jitter / 160.0f, 0.4f, 2.4f);
}

static int FastInputAutoBestPlusAggression(int AvgPing, int Jitter, int Preference)
{
	const int PreferenceOffset = Preference == 0 ? -12 : Preference == 2 ? 10 :
									       0;
	return std::clamp(round_to_int(72.0f + AvgPing * 0.08f - Jitter * 1.8f + PreferenceOffset), 35, 90);
}

static int FastInputAutoBestPlusMovementMs(int AvgPing, int Jitter, int Preference)
{
	const int PreferenceOffset = Preference == 0 ? -4 : Preference == 2 ? 5 :
									      0;
	return std::clamp(round_to_int(20.0f + AvgPing * 0.18f - Jitter * 0.12f + PreferenceOffset), 14, 60);
}

static int FastInputAutoBestPlusAimHookMs(int AvgPing, int Jitter, int Preference)
{
	const int PreferenceOffset = Preference == 0 ? -4 : Preference == 2 ? 4 :
									      0;
	return std::clamp(round_to_int(18.0f + AvgPing * 0.16f - Jitter * 0.16f + PreferenceOffset), 14, 60);
}

static const char *FastInputPreferenceName(int Preference)
{
	if(Preference == 0)
		return "Small";
	if(Preference == 2)
		return "Big";
	return "Medium";
}

static void FastInputApplyAutoSettings(int Mode, int AvgPing, int Jitter, int Preference)
{
	if(Mode == 0)
		g_Config.m_TcFastInputAmount = FastInputAutoAmountMs(Mode, AvgPing, Jitter, Preference);
	else if(Mode == 1)
		g_Config.m_TcFastInputSaikoAmount = FastInputAutoSaikoAmount(AvgPing, Jitter, Preference);
	else if(Mode == 2)
		g_Config.m_TcFastInputBestAmount = FastInputAutoAmountMs(Mode, AvgPing, Jitter, Preference);
	else if(Mode == 3)
	{
		g_Config.m_TcFastInputBestPlusAggression = FastInputAutoBestPlusAggression(AvgPing, Jitter, Preference);
		g_Config.m_TcFastInputBestPlusMovement = FastInputAutoBestPlusMovementMs(AvgPing, Jitter, Preference);
		g_Config.m_TcFastInputBestPlusAimHook = FastInputAutoBestPlusAimHookMs(AvgPing, Jitter, Preference);
	}
}

const float FontSize = 14.0f;
const float EditBoxFontSize = 12.0f;
const float LineSize = 20.0f;
const float HeadlineFontSize = 20.0f;
const float HeadlineHeight = HeadlineFontSize + 0.0f;
const float Margin = 10.0f;
const float MarginSmall = 5.0f;
const float MarginExtraSmall = 2.5f;
const float MarginBetweenSections = 30.0f;
const float MarginBetweenViews = 30.0f;

void CMenus::RenderSettingsTClientCloudeInput(CUIRect &Column)
{
	CUIRect Button;

	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFastInput, TCLocalize("Fast Input (reduced visual delay)"), &g_Config.m_TcFastInput, &Column, LineSize);
	if(g_Config.m_TcFastInput)
	{
		static CButtonContainer s_TaterModeButton;
		static CButtonContainer s_SaikoModeButton;
		static CButtonContainer s_CloudeModeButton;
		static CButtonContainer s_CloudePlusModeButton;
		CUIRect ModeLabel, ModeRow, TaterButton, SaikoButton, CloudeButton, CloudePlusButton;
		Column.HSplitTop(LineSize, &ModeLabel, &Column);
		Ui()->DoLabel(&ModeLabel, TCLocalize("Fast Input Mode"), FontSize, TEXTALIGN_ML);
		Column.HSplitTop(LineSize, &ModeRow, &Column);
		ModeRow.VSplitLeft(ModeRow.w / 4.0f, &TaterButton, &ModeRow);
		ModeRow.VSplitLeft(MarginExtraSmall, nullptr, &ModeRow);
		ModeRow.VSplitLeft(ModeRow.w / 3.0f, &SaikoButton, &ModeRow);
		ModeRow.VSplitLeft(MarginExtraSmall, nullptr, &ModeRow);
		ModeRow.VSplitLeft(ModeRow.w / 2.0f, &CloudeButton, &CloudePlusButton);
		CloudeButton.VSplitLeft(MarginExtraSmall, nullptr, &CloudeButton);
		CloudePlusButton.VSplitLeft(MarginExtraSmall, nullptr, &CloudePlusButton);
		if(DoButton_MenuTab(&s_TaterModeButton, "Tater", g_Config.m_TcFastInputMode == 0, &TaterButton, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 5.0f))
			g_Config.m_TcFastInputMode = 0;
		if(DoButton_MenuTab(&s_SaikoModeButton, "Saiko", g_Config.m_TcFastInputMode == 1, &SaikoButton, IGraphics::CORNER_NONE, nullptr, nullptr, nullptr, nullptr, 5.0f))
			g_Config.m_TcFastInputMode = 1;
		if(DoButton_MenuTab(&s_CloudeModeButton, "Cloude", g_Config.m_TcFastInputMode == 2, &CloudeButton, IGraphics::CORNER_NONE, nullptr, nullptr, nullptr, nullptr, 5.0f))
			g_Config.m_TcFastInputMode = 2;
		if(DoButton_MenuTab(&s_CloudePlusModeButton, "Cloude+", g_Config.m_TcFastInputMode == 3, &CloudePlusButton, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 5.0f))
			g_Config.m_TcFastInputMode = 3;

		Column.HSplitTop(MarginExtraSmall, nullptr, &Column);
		Column.HSplitTop(LineSize, &Button, &Column);
		if(g_Config.m_TcFastInputMode == 0)
		{
			DoSliderWithScaledValue(&g_Config.m_TcFastInputAmount, &g_Config.m_TcFastInputAmount, &Button, TCLocalize("Tater Amount"), 1, 60, 1, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_NOCLAMPVALUE, "ms");
		}
		else if(g_Config.m_TcFastInputMode == 1)
		{
			DoSliderWithFloatValue(&g_Config.m_TcFastInputSaikoAmount, &g_Config.m_TcFastInputSaikoAmount, &Button, TCLocalize("Saiko Amount"), 0.0f, 3.0f);
		}
		else if(g_Config.m_TcFastInputMode == 2)
		{
			DoSliderWithScaledValue(&g_Config.m_TcFastInputBestAmount, &g_Config.m_TcFastInputBestAmount, &Button, TCLocalize("Cloude Amount"), 1, 60, 1, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_NOCLAMPVALUE, "ms");
		}
		else
		{
			DoSliderWithScaledValue(&g_Config.m_TcFastInputBestPlusMovement, &g_Config.m_TcFastInputBestPlusMovement, &Button, TCLocalize("Cloude+ Movement"), 14, 60, 1, &CUi::ms_LinearScrollbarScale, 0, "ms");
			Column.HSplitTop(LineSize, &Button, &Column);
			DoSliderWithScaledValue(&g_Config.m_TcFastInputBestPlusAimHook, &g_Config.m_TcFastInputBestPlusAimHook, &Button, TCLocalize("Cloude+ Aim/Hook"), 14, 60, 1, &CUi::ms_LinearScrollbarScale, 0, "ms");
			static CButtonContainer s_AutoMovementButton;
			static CButtonContainer s_AutoAimHookButton;
			CUIRect AutoMovementButton, AutoAimHookButton;
			Column.HSplitTop(MarginExtraSmall, nullptr, &Column);
			Column.HSplitTop(LineSize, &AutoMovementButton, &Column);
			AutoMovementButton.VSplitLeft(AutoMovementButton.w / 2.0f, &AutoMovementButton, &AutoAimHookButton);
			AutoAimHookButton.VSplitLeft(MarginExtraSmall, nullptr, &AutoAimHookButton);
			if(DoButton_Menu(&s_AutoMovementButton, TCLocalize("Auto Movement"), GameClient()->m_Controls.FastInputAutoTuneRunning() ? 1 : 0, &AutoMovementButton) && !GameClient()->m_Controls.FastInputAutoTuneRunning())
				GameClient()->m_Controls.StartFastInputAutoTune(CControls::EFastInputAutoTune::MOVEMENT);
			if(DoButton_Menu(&s_AutoAimHookButton, TCLocalize("Auto Aim/Hook"), GameClient()->m_Controls.FastInputAutoTuneRunning() ? 1 : 0, &AutoAimHookButton) && !GameClient()->m_Controls.FastInputAutoTuneRunning())
				GameClient()->m_Controls.StartFastInputAutoTune(CControls::EFastInputAutoTune::AIM_HOOK);
			if(GameClient()->m_Controls.m_FastInputAutoTune.m_aStatus[0] != '\0')
			{
				CUIRect AutoTuneStatus;
				Column.HSplitTop(MarginExtraSmall, nullptr, &Column);
				Column.HSplitTop(LineSize, &AutoTuneStatus, &Column);
				Ui()->DoLabel(&AutoTuneStatus, GameClient()->m_Controls.m_FastInputAutoTune.m_aStatus, FontSize, TEXTALIGN_ML);
			}
		}

		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFastInputBestPlusDynamicBoost, TCLocalize("Dynamic Boost"), &g_Config.m_TcFastInputBestPlusDynamicBoost, &Column, LineSize);

		static CButtonContainer s_AutoSettingsButton;
		static CButtonContainer s_AutoSmallButton;
		static CButtonContainer s_AutoMediumButton;
		static CButtonContainer s_AutoBigButton;
		static SFastInputAutoSettings s_AutoSettings;
		static int s_AutoPreference = 1;
		const int64_t Now = time_get();
		const int64_t Freq = time_freq();
		if(s_AutoSettings.m_Running)
		{
			if(s_AutoSettings.m_Mode != g_Config.m_TcFastInputMode)
			{
				s_AutoSettings.m_Running = false;
				str_copy(s_AutoSettings.m_aStatus, TCLocalize("Auto settings cancelled: mode changed"));
			}
			else if(GameClient()->m_Snap.m_pLocalInfo && Now - s_AutoSettings.m_LastSampleTime >= Freq / 5)
			{
				const int Ping = std::clamp(GameClient()->m_Snap.m_pLocalInfo->m_Latency, 0, 999);
				s_AutoSettings.m_Samples++;
				s_AutoSettings.m_Sum += Ping;
				s_AutoSettings.m_Min = minimum(s_AutoSettings.m_Min, Ping);
				s_AutoSettings.m_Max = maximum(s_AutoSettings.m_Max, Ping);
				s_AutoSettings.m_LastSampleTime = Now;
			}

			if(s_AutoSettings.m_Running && Now - s_AutoSettings.m_StartTime >= Freq * 10)
			{
				s_AutoSettings.m_Running = false;
				if(s_AutoSettings.m_Samples > 0)
				{
					const int AvgPing = s_AutoSettings.m_Sum / s_AutoSettings.m_Samples;
					const int Jitter = s_AutoSettings.m_Max - s_AutoSettings.m_Min;
					FastInputApplyAutoSettings(s_AutoSettings.m_Mode, AvgPing, Jitter, s_AutoSettings.m_Preference);
					str_format(s_AutoSettings.m_aStatus, sizeof(s_AutoSettings.m_aStatus), TCLocalize("Applied %s: avg %d ms, jitter %d ms"), FastInputPreferenceName(s_AutoSettings.m_Preference), AvgPing, Jitter);
				}
				else
				{
					str_copy(s_AutoSettings.m_aStatus, TCLocalize("Auto settings failed: no ping samples"));
				}
			}
		}

		CUIRect AutoHint;
		Column.HSplitTop(MarginExtraSmall, nullptr, &Column);
		Column.HSplitTop(LineSize, &AutoHint, &Column);
		if(GameClient()->m_Snap.m_pLocalInfo)
			Ui()->DoLabel(&AutoHint, TCLocalize("Auto settings analyzes ping for 10 seconds."), FontSize, TEXTALIGN_ML);
		else
			Ui()->DoLabel(&AutoHint, TCLocalize("Auto settings: join any server first."), FontSize, TEXTALIGN_ML);

		CUIRect PreferenceLabel;
		Column.HSplitTop(LineSize, &PreferenceLabel, &Column);
		Ui()->DoLabel(&PreferenceLabel, "Preference", FontSize, TEXTALIGN_ML);

		CUIRect PresetRow, SmallButton, MediumButton, BigButton;
		Column.HSplitTop(LineSize, &PresetRow, &Column);
		PresetRow.VSplitLeft(PresetRow.w / 3.0f, &SmallButton, &PresetRow);
		PresetRow.VSplitLeft(MarginExtraSmall, nullptr, &PresetRow);
		PresetRow.VSplitLeft(PresetRow.w / 2.0f, &MediumButton, &BigButton);
		BigButton.VSplitLeft(MarginExtraSmall, nullptr, &BigButton);
		if(!s_AutoSettings.m_Running)
		{
			if(DoButton_MenuTab(&s_AutoSmallButton, TCLocalize("Small"), s_AutoPreference == 0, &SmallButton, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 5.0f))
				s_AutoPreference = 0;
			if(DoButton_MenuTab(&s_AutoMediumButton, TCLocalize("Medium"), s_AutoPreference == 1, &MediumButton, IGraphics::CORNER_NONE, nullptr, nullptr, nullptr, nullptr, 5.0f))
				s_AutoPreference = 1;
			if(DoButton_MenuTab(&s_AutoBigButton, TCLocalize("Big"), s_AutoPreference == 2, &BigButton, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 5.0f))
				s_AutoPreference = 2;
		}
		else
		{
			DoButton_MenuTab(&s_AutoSmallButton, TCLocalize("Small"), s_AutoSettings.m_Preference == 0, &SmallButton, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 5.0f);
			DoButton_MenuTab(&s_AutoMediumButton, TCLocalize("Medium"), s_AutoSettings.m_Preference == 1, &MediumButton, IGraphics::CORNER_NONE, nullptr, nullptr, nullptr, nullptr, 5.0f);
			DoButton_MenuTab(&s_AutoBigButton, TCLocalize("Big"), s_AutoSettings.m_Preference == 2, &BigButton, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 5.0f);
		}

		Column.HSplitTop(MarginExtraSmall, nullptr, &Column);
		Column.HSplitTop(LineSize, &Button, &Column);
		char aAutoButton[64];
		if(s_AutoSettings.m_Running)
		{
			const int Remaining = std::clamp(10 - (int)((Now - s_AutoSettings.m_StartTime) / Freq), 0, 10);
			str_format(aAutoButton, sizeof(aAutoButton), TCLocalize("Auto settings... %ds"), Remaining);
		}
		else
		{
			str_copy(aAutoButton, TCLocalize("Auto settings"));
		}

		if(DoButton_Menu(&s_AutoSettingsButton, aAutoButton, s_AutoSettings.m_Running ? 1 : 0, &Button) && !s_AutoSettings.m_Running)
		{
			if(!GameClient()->m_Snap.m_pLocalInfo)
			{
				str_copy(s_AutoSettings.m_aStatus, TCLocalize("Auto settings needs server ping"));
			}
			else
			{
				s_AutoSettings = SFastInputAutoSettings{};
				s_AutoSettings.m_Running = true;
				s_AutoSettings.m_Mode = g_Config.m_TcFastInputMode;
				s_AutoSettings.m_Preference = s_AutoPreference;
				s_AutoSettings.m_StartTime = Now;
				s_AutoSettings.m_LastSampleTime = Now - Freq;
				str_format(s_AutoSettings.m_aStatus, sizeof(s_AutoSettings.m_aStatus), TCLocalize("Measuring %s for 10 seconds..."), FastInputPreferenceName(s_AutoSettings.m_Preference));
			}
		}
		if(s_AutoSettings.m_aStatus[0] != '\0')
		{
			CUIRect AutoStatus;
			Column.HSplitTop(MarginExtraSmall, nullptr, &Column);
			Column.HSplitTop(LineSize, &AutoStatus, &Column);
			Ui()->DoLabel(&AutoStatus, s_AutoSettings.m_aStatus, FontSize, TEXTALIGN_ML);
		}

		Column.HSplitTop(MarginSmall, nullptr, &Column);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFastInputOthers, TCLocalize("Fast Input others"), &g_Config.m_TcFastInputOthers, &Column, LineSize);
	}
	else
	{
		Column.HSplitTop(LineSize, nullptr, &Column);
	}
}

void CMenus::RenderSettingsTClientIslandClient(CUIRect MainView)
{
	CUIRect Label, Body, Card, LeftColumn, RightColumn, Content, Button;
	const bool CloudeRu = g_Config.m_TcCloudeLanguage == 1;

	static int s_CurCloudeTab = 0;
	CUIRect Header, TabBar, SettingsButton, GameButton, ConfigsButton, InfoButton, LanguageButtons, EnglishButton, RussianButton;
	MainView.HSplitTop(LineSize, &TabBar, &MainView);
	TabBar.VSplitLeft(180.0f, &Header, &TabBar);
	Ui()->DoLabel(&Header, "Cloude", HeadlineFontSize, TEXTALIGN_ML);
	TabBar.VSplitRight(132.0f, &TabBar, &LanguageButtons);
	LanguageButtons.VSplitMid(&EnglishButton, &RussianButton, MarginExtraSmall);
	static CButtonContainer s_CloudeLanguageEnglishButton;
	static CButtonContainer s_CloudeLanguageRussianButton;
	if(DoButton_MenuTab(&s_CloudeLanguageEnglishButton, "ENG", !CloudeRu, &EnglishButton, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 5.0f))
		g_Config.m_TcCloudeLanguage = 0;
	if(DoButton_MenuTab(&s_CloudeLanguageRussianButton, "RUS", CloudeRu, &RussianButton, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 5.0f))
		g_Config.m_TcCloudeLanguage = 1;
	TabBar.VSplitLeft(220.0f, &TabBar, nullptr);
	TabBar.VSplitLeft(100.0f, &SettingsButton, &TabBar);
	TabBar.VSplitLeft(MarginExtraSmall, nullptr, &TabBar);
	TabBar.VSplitLeft(90.0f, &GameButton, &TabBar);
	TabBar.VSplitLeft(MarginExtraSmall, nullptr, &TabBar);
	TabBar.VSplitLeft(100.0f, &ConfigsButton, &TabBar);
	TabBar.VSplitLeft(MarginExtraSmall, nullptr, &TabBar);
	TabBar.VSplitLeft(90.0f, &InfoButton, nullptr);
	static CButtonContainer s_CloudeSettingsTab;
	static CButtonContainer s_CloudeGameTab;
	static CButtonContainer s_CloudeConfigsTab;
	static CButtonContainer s_CloudeInfoTab;
	if(DoButton_MenuTab(&s_CloudeSettingsTab, CloudeRu ? "Настройки" : "Settings", s_CurCloudeTab == 0, &SettingsButton, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 5.0f))
		s_CurCloudeTab = 0;
	if(DoButton_MenuTab(&s_CloudeGameTab, CloudeRu ? "Игра" : "Game", s_CurCloudeTab == 1, &GameButton, IGraphics::CORNER_NONE, nullptr, nullptr, nullptr, nullptr, 5.0f))
		s_CurCloudeTab = 1;
	if(DoButton_MenuTab(&s_CloudeConfigsTab, CloudeRu ? "Конфиги" : "Configs", s_CurCloudeTab == 2, &ConfigsButton, IGraphics::CORNER_NONE, nullptr, nullptr, nullptr, nullptr, 5.0f))
		s_CurCloudeTab = 2;
	if(DoButton_MenuTab(&s_CloudeInfoTab, CloudeRu ? "Инфо" : "Info", s_CurCloudeTab == 3, &InfoButton, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 5.0f))
		s_CurCloudeTab = 3;
	MainView.HSplitTop(Margin, nullptr, &MainView);
	if(s_CurCloudeTab == 1)
	{
		RenderSettingsTClientCloudeGame(MainView);
		return;
	}
	if(s_CurCloudeTab == 2)
	{
		RenderSettingsTClientCloudeConfigs(MainView);
		return;
	}
	if(s_CurCloudeTab == 3)
	{
		RenderSettingsTClientCloudeInfo(MainView);
		return;
	}

	static CScrollRegion s_ScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 60.0f;
	ScrollParams.m_Flags = CScrollRegionParams::FLAG_CONTENT_STATIC_WIDTH;
	ScrollParams.m_ScrollbarMargin = 5.0f;
	s_ScrollRegion.Begin(&MainView, &ScrollOffset, &ScrollParams);
	MainView.y += ScrollOffset.y;
	MainView.VSplitRight(5.0f, &MainView, nullptr);
	MainView.VSplitLeft(5.0f, nullptr, &MainView);

	MainView.HSplitTop(MarginSmall, nullptr, &Body);

	Body.VSplitMid(&LeftColumn, &RightColumn, MarginBetweenViews);

	const auto RenderCloudeCardFrame = [](CUIRect CardRect) {
		CardRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_ALL, 10.0f);
	};

	const float CloudeInputCardHeight = 606.0f + (g_Config.m_TcAutoFire ? 40.0f : 0.0f) + (g_Config.m_TcAutoSwapGunHammer ? 34.0f : 0.0f);
	LeftColumn.HSplitTop(CloudeInputCardHeight, &Card, &LeftColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Cloude инпут" : "Cloude Input", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	RenderSettingsTClientCloudeInput(Content);
	Content.HSplitTop(MarginBetweenSections, nullptr, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Автокликер" : "Auto Fire", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcAutoFire, CloudeRu ? "Автокликер fire" : "Auto fire", &g_Config.m_TcAutoFire, &Content, LineSize);
	if(g_Config.m_TcAutoFire)
	{
		Content.HSplitTop(MarginSmall, nullptr, &Content);
		Content.HSplitTop(LineSize, &Button, &Content);
		DoSliderWithScaledValue(&g_Config.m_TcAutoFireSpeed, &g_Config.m_TcAutoFireSpeed, &Button, CloudeRu ? "Скорость ударов" : "Fire speed", 1, 25, 1, &CUi::ms_LinearScrollbarScale, 0, "/s");
	}
	Content.HSplitTop(MarginBetweenSections, nullptr, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Автосвапер" : "Auto Swapper", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcAutoSwapGunHammer, CloudeRu ? "Пистолет -> молоток при fire" : "Gun -> hammer on fire", &g_Config.m_TcAutoSwapGunHammer, &Content, LineSize);
	if(g_Config.m_TcAutoSwapGunHammer)
	{
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcAutoSwapReturnGun, CloudeRu ? "Вернуть на пистолет" : "Return to gun", &g_Config.m_TcAutoSwapReturnGun, &Content, LineSize);
	}
	LeftColumn.y = maximum(LeftColumn.y, Content.y + MarginSmall);

	RightColumn.HSplitTop(132.0f, &Card, &RightColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, TCLocalize("Aled"), HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcAledCounter, CloudeRu ? "Aled счетчик" : "Aled counter", &g_Config.m_TcAledCounter, &Content, LineSize);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcAledAutoRecord, CloudeRu ? "Записывать Aled" : "Auto record Aleds", &g_Config.m_TcAledAutoRecord, &Content, LineSize);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcAledRecordNotify, CloudeRu ? "Уведомлять о сохранении" : "Notify when saved", &g_Config.m_TcAledRecordNotify, &Content, LineSize);
	char aAledBuf[128];
	str_format(aAledBuf, sizeof(aAledBuf), "%s: %d", CloudeRu ? "Текущие Aled" : "Current Aleds", GameClient()->m_AledCounter.Count());
	Content.HSplitTop(MarginExtraSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, aAledBuf, FontSize, TEXTALIGN_ML);
	if(!GameClient()->m_AledCounter.ProfileValid())
	{
		Content.HSplitTop(LineSize, &Label, &Content);
		Ui()->DoLabel(&Label, CloudeRu ? "Проверка профиля не прошла" : "Profile check failed", FontSize, TEXTALIGN_ML);
	}

	RightColumn.HSplitTop(MarginBetweenSections, nullptr, &RightColumn);
	RightColumn.HSplitTop(118.0f, &Card, &RightColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, TCLocalize("HUD"), HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	static CButtonContainer s_HudEditorButton;
	const bool Online = Client()->State() == IClient::STATE_ONLINE;
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcMediaIsland, CloudeRu ? "Показывать островок сверху" : "Show media island", &g_Config.m_TcMediaIsland, &Content, LineSize);
	Content.HSplitTop(LineSize, &Button, &Content);
	if(DoButton_Menu(&s_HudEditorButton, Online ? (CloudeRu ? "Редактировать HUD в игре" : "Edit HUD in game") : (CloudeRu ? "Сначала зайди на сервер" : "Join a server first"), g_Config.m_TcHudEditor ? 1 : 0, &Button) && Online)
	{
		g_Config.m_TcHudEditor = 1;
		SetActive(false);
	}

	RightColumn.HSplitTop(MarginBetweenSections, nullptr, &RightColumn);
	RightColumn.HSplitTop(92.0f, &Card, &RightColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Input Doctor" : "Input Doctor", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcInputDoctor, CloudeRu ? "Показывать диагностику инпута" : "Show input diagnostics", &g_Config.m_TcInputDoctor, &Content, LineSize);

	RightColumn.HSplitTop(MarginBetweenSections, nullptr, &RightColumn);
	RightColumn.HSplitTop(112.0f, &Card, &RightColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Меню игры" : "Play Menu", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcPlayModeChoice, CloudeRu ? "Показывать выбор режима игры" : "Show play mode choice", &g_Config.m_TcPlayModeChoice, &Content, LineSize);
	if(g_Config.m_TcPlayModeChoice)
	{
		char aNormalName[128];
		str_format(aNormalName, sizeof(aNormalName), "%s: %s", CloudeRu ? "Обычный ник" : "Normal name", g_Config.m_PlayerName[0] ? g_Config.m_PlayerName : "nameless tee");
		Content.HSplitTop(MarginExtraSmall, nullptr, &Content);
		Content.HSplitTop(LineSize, &Label, &Content);
		Ui()->DoLabel(&Label, aNormalName, FontSize, TEXTALIGN_ML);
	}

	RightColumn.HSplitTop(MarginBetweenSections, nullptr, &RightColumn);
	RightColumn.HSplitTop(112.0f, &Card, &RightColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Фокус режим" : "Focus Mode", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	RenderSettingsTClientFocusMode(Content);

	RightColumn.HSplitTop(MarginBetweenSections, nullptr, &RightColumn);
	RightColumn.HSplitTop(146.0f, &Card, &RightColumn);
	RenderCloudeCardFrame(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Микро ассист" : "Micro Assist", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	RenderSettingsTClientMicroAssist(Content);
	RightColumn.y = maximum(RightColumn.y, Content.y + MarginSmall);

	CUIRect ScrollRegion;
	ScrollRegion.x = MainView.x;
	ScrollRegion.y = maximum(LeftColumn.y, RightColumn.y) + MarginSmall;
	ScrollRegion.w = MainView.w;
	ScrollRegion.h = 0.0f;
	s_ScrollRegion.AddRect(ScrollRegion);
	s_ScrollRegion.End();
}

void CMenus::RenderSettingsTClientCloudeGame(CUIRect MainView)
{
	CUIRect Body, Card, Content, Label, Button;
	const bool CloudeRu = g_Config.m_TcCloudeLanguage == 1;

	MainView.VSplitRight(5.0f, &MainView, nullptr);
	MainView.VSplitLeft(5.0f, nullptr, &MainView);
	MainView.HSplitTop(MarginSmall, nullptr, &Body);

	const float GameCardHeight = g_Config.m_TcMotionBlur ? 180.0f : 140.0f;
	Body.HSplitTop(GameCardHeight, &Card, &Body);
	Card.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_ALL, 10.0f);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Игра" : "Game", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcMotionBlur, CloudeRu ? "Motion Blur" : "Motion Blur", &g_Config.m_TcMotionBlur, &Content, LineSize);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcTeamNameGradient, CloudeRu ? "Градиентные ники по цвету тимы" : "Team-colored name gradients", &g_Config.m_TcTeamNameGradient, &Content, LineSize);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcChatGifPreview, CloudeRu ? "Показывать GIF в чате" : "Show GIF previews in chat", &g_Config.m_TcChatGifPreview, &Content, LineSize);
	if(g_Config.m_TcMotionBlur)
	{
		Content.HSplitTop(MarginSmall, nullptr, &Content);
		Content.HSplitTop(LineSize, &Button, &Content);
		DoSliderWithScaledValue(&g_Config.m_TcMotionBlurStrength, &g_Config.m_TcMotionBlurStrength, &Button, CloudeRu ? "Сила смешивания кадров" : "Frame blend strength", 0, 95, 1, &CUi::ms_LinearScrollbarScale, 0, "%");
	}

	Body.HSplitTop(MarginSmall, nullptr, &Body);
	const float RainCardHeight = g_Config.m_TcRainVisual ? 154.0f : 82.0f;
	Body.HSplitTop(RainCardHeight, &Card, &Body);
	Card.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_ALL, 10.0f);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Дождь" : "Rain Visual", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcRainVisual, CloudeRu ? "Быстрый диагональный дождь" : "Fast diagonal rain", &g_Config.m_TcRainVisual, &Content, LineSize);
	if(g_Config.m_TcRainVisual)
	{
		Content.HSplitTop(MarginSmall, nullptr, &Content);
		Content.HSplitTop(LineSize, &Button, &Content);
		DoSliderWithScaledValue(&g_Config.m_TcRainAmount, &g_Config.m_TcRainAmount, &Button, CloudeRu ? "Количество" : "Amount", 5, 200, 1, &CUi::ms_LinearScrollbarScale, 0, "%");
		Content.HSplitTop(LineSize, &Button, &Content);
		DoSliderWithScaledValue(&g_Config.m_TcRainStrength, &g_Config.m_TcRainStrength, &Button, CloudeRu ? "Сила" : "Strength", 1, 100, 1, &CUi::ms_LinearScrollbarScale, 0, "%");
		Content.HSplitTop(LineSize, &Button, &Content);
		DoSliderWithScaledValue(&g_Config.m_TcRainSpeed, &g_Config.m_TcRainSpeed, &Button, CloudeRu ? "Скорость" : "Speed", 20, 300, 1, &CUi::ms_LinearScrollbarScale, 0, "%");
	}
}

void CMenus::RenderSettingsTClientCloudeConfigs(CUIRect MainView)
{
	const bool CloudeRu = g_Config.m_TcCloudeLanguage == 1;
	static CLineInputBuffered<128> s_SaveNameInput;
	static CLineInputBuffered<16> s_ShareCodeInput;
	static char s_aSelectedConfig[128] = "";
	static char s_aStatus[256] = "";
	static char s_aShareCode[16] = "";
	static std::shared_ptr<CHttpRequest> s_pShareRequest = nullptr;
	static std::shared_ptr<CHttpRequest> s_pGetRequest = nullptr;
	static std::string s_SharePresetText;

	if(s_SaveNameInput.GetString()[0] == '\0')
		s_SaveNameInput.Set("my_cloude");

	if(s_pShareRequest && s_pShareRequest->Done())
	{
		if(s_pShareRequest->State() == EHttpState::DONE && s_pShareRequest->StatusCode() == 200)
		{
			json_value *pJson = s_pShareRequest->ResultJson();
			const json_value *pCode = pJson && pJson->type == json_object ? json_object_get(pJson, "code") : nullptr;
			if(pCode && pCode->type == json_string)
			{
				str_copy(s_aShareCode, json_string_get(pCode), sizeof(s_aShareCode));
				str_format(s_aStatus, sizeof(s_aStatus), "%s: %s", CloudeRu ? "Код" : "Code", s_aShareCode);
			}
			else
			{
				str_copy(s_aStatus, CloudeRu ? "VPS не вернул код." : "VPS did not return a code.");
			}
			if(pJson)
				json_value_free(pJson);
		}
		else
		{
			str_copy(s_aStatus, CloudeRu ? "Не удалось поделиться preset-ом." : "Failed to share preset.");
		}
		s_pShareRequest = nullptr;
	}

	if(s_pGetRequest && s_pGetRequest->Done())
	{
		if(s_pGetRequest->State() == EHttpState::DONE && s_pGetRequest->StatusCode() == 200)
		{
			json_value *pJson = s_pGetRequest->ResultJson();
			const json_value *pPreset = pJson && pJson->type == json_object ? json_object_get(pJson, "preset") : nullptr;
			const char *pPresetText = pPreset && pPreset->type == json_string ? json_string_get(pPreset) : nullptr;
			if(pPresetText && ImportCloudeConfigText(pPresetText))
			{
				char aName[64];
				str_format(aName, sizeof(aName), "shared_%s", s_ShareCodeInput.GetString());
				WriteCloudeConfigText(Storage(), aName, pPresetText);
				str_format(s_aSelectedConfig, sizeof(s_aSelectedConfig), "%s.cloude", aName);
				str_format(s_aStatus, sizeof(s_aStatus), "%s: %s", CloudeRu ? "Получено и применено" : "Received and applied", s_aSelectedConfig);
			}
			else
			{
				str_copy(s_aStatus, CloudeRu ? "Код не содержит Cloude preset." : "Code does not contain a Cloude preset.");
			}
			if(pJson)
				json_value_free(pJson);
		}
		else
		{
			str_copy(s_aStatus, CloudeRu ? "Код не найден или истёк." : "Code not found or expired.");
		}
		s_pGetRequest = nullptr;
	}

	Storage()->CreateFolder("tclient", IStorage::TYPE_SAVE);
	Storage()->CreateFolder(CLOUDE_CONFIG_DIR, IStorage::TYPE_SAVE);
	std::vector<SCloudeConfigFile> vFiles;
	Storage()->ListDirectoryInfo(IStorage::TYPE_SAVE, CLOUDE_CONFIG_DIR, CloudeConfigListCallback, &vFiles);
	std::sort(vFiles.begin(), vFiles.end(), [](const SCloudeConfigFile &a, const SCloudeConfigFile &b) { return str_comp(a.m_aName, b.m_aName) < 0; });
	if(s_aSelectedConfig[0] == '\0' && !vFiles.empty())
		str_copy(s_aSelectedConfig, vFiles[0].m_aName);

	std::string SelectedText;
	const bool HasSelection = s_aSelectedConfig[0] != '\0' && ReadCloudeConfigText(Storage(), s_aSelectedConfig, SelectedText);

	CUIRect Left, Right, Card, Content, Label, Button, ButtonA, ButtonB, ButtonC;
	MainView.VSplitRight(5.0f, &MainView, nullptr);
	MainView.VSplitLeft(5.0f, nullptr, &MainView);
	MainView.HSplitTop(MarginSmall, nullptr, &MainView);
	MainView.VSplitLeft(245.0f, &Left, &Right);
	Right.VSplitLeft(MarginBetweenViews, nullptr, &Right);

	const auto RenderCard = [](CUIRect Rect) {
		Rect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_ALL, 10.0f);
	};

	Left.HSplitTop(390.0f, &Card, &Left);
	RenderCard(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Мои Cloude presets" : "My Cloude Presets", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	if(vFiles.empty())
	{
		Content.HSplitTop(LineSize, &Label, &Content);
		Ui()->DoLabel(&Label, CloudeRu ? "Пока пусто." : "No presets yet.", FontSize, TEXTALIGN_ML);
	}
	else
	{
		const int MaxVisible = minimum<int>((int)vFiles.size(), 13);
		for(int i = 0; i < MaxVisible; ++i)
		{
			Content.HSplitTop(LineSize, &Button, &Content);
			static CButtonContainer s_aFileButtons[13];
			if(DoButton_Menu(&s_aFileButtons[i], vFiles[i].m_aName, str_comp(s_aSelectedConfig, vFiles[i].m_aName) == 0, &Button))
				str_copy(s_aSelectedConfig, vFiles[i].m_aName);
			Content.HSplitTop(MarginExtraSmall, nullptr, &Content);
		}
	}

	Right.HSplitTop(116.0f, &Card, &Right);
	RenderCard(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Сохранить текущие настройки" : "Save Current Settings", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Button, &Content);
	Ui()->DoEditBox(&s_SaveNameInput, &Button, EditBoxFontSize);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Button, &Content);
	static CButtonContainer s_SaveCurrentButton;
	if(DoButton_Menu(&s_SaveCurrentButton, CloudeRu ? "Сохранить preset" : "Save preset", 0, &Button))
	{
		if(ExportCloudeConfig(Storage(), s_SaveNameInput.GetString()))
		{
			char aPath[IO_MAX_PATH_LENGTH];
			CloudeConfigMakePath(s_SaveNameInput.GetString(), aPath, sizeof(aPath));
			str_format(s_aStatus, sizeof(s_aStatus), "%s: %s", CloudeRu ? "Сохранено" : "Saved", aPath);
			char aSelected[128];
			str_copy(aSelected, s_SaveNameInput.GetString());
			if(!str_endswith(aSelected, ".cloude"))
				str_append(aSelected, ".cloude", sizeof(aSelected));
			str_copy(s_aSelectedConfig, aSelected);
		}
		else
			str_copy(s_aStatus, CloudeRu ? "Плохое имя файла." : "Bad file name.");
	}

	Right.HSplitTop(MarginBetweenSections, nullptr, &Right);
	Right.HSplitTop(250.0f, &Card, &Right);
	RenderCard(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, HasSelection ? s_aSelectedConfig : (CloudeRu ? "Preset не выбран" : "No preset selected"), HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Button, &Content);
	Button.VSplitLeft(120.0f, &ButtonA, &Button);
	Button.VSplitLeft(MarginSmall, nullptr, &Button);
	Button.VSplitLeft(120.0f, &ButtonB, &Button);
	Button.VSplitLeft(MarginSmall, nullptr, &Button);
	Button.VSplitLeft(120.0f, &ButtonC, nullptr);
	static CButtonContainer s_ApplySelectedButton;
	static CButtonContainer s_ShareSelectedButton;
	static CButtonContainer s_ShareCurrentButton;
	if(DoButton_Menu(&s_ApplySelectedButton, CloudeRu ? "Применить" : "Apply", 0, &ButtonA) && HasSelection)
	{
		if(ImportCloudeConfig(Storage(), s_aSelectedConfig))
			str_format(s_aStatus, sizeof(s_aStatus), "%s: %s", CloudeRu ? "Применено" : "Applied", s_aSelectedConfig);
		else
			str_copy(s_aStatus, CloudeRu ? "Это не Cloude preset." : "Not a Cloude preset.");
	}
	if(DoButton_Menu(&s_ShareSelectedButton, CloudeRu ? "Поделиться" : "Share", 0, &ButtonB) && HasSelection && !s_pShareRequest)
		s_SharePresetText = SelectedText;
	if(DoButton_Menu(&s_ShareCurrentButton, CloudeRu ? "Поделиться текущим" : "Share current", 0, &ButtonC) && !s_pShareRequest)
		s_SharePresetText = BuildCloudeConfigText();
	if(!s_SharePresetText.empty() && !s_pShareRequest)
	{
		char aUrl[300];
		if(s_SharePresetText.size() > 12 * 1024)
		{
			str_copy(s_aStatus, CloudeRu ? "Preset слишком большой для шаринга." : "Preset is too large to share.");
		}
		else if(CloudeConfigShareEndpoint(aUrl, sizeof(aUrl), "config/share"))
		{
			std::vector<char> vEscapedPreset(s_SharePresetText.size() * 2 + 64);
			EscapeJson(vEscapedPreset.data(), (int)vEscapedPreset.size(), s_SharePresetText.c_str());
			std::string Json = "{\"preset\":\"";
			Json += vEscapedPreset.data();
			Json += "\"}";
			s_pShareRequest = HttpPostJson(aUrl, Json.c_str());
			s_pShareRequest->HeaderString("X-Cloude-Dev-Token", g_Config.m_TcCloudeDevPresenceToken);
			s_pShareRequest->Timeout(CTimeout{2000, 5000, 0, 0});
			Http()->Run(s_pShareRequest);
			str_copy(s_aStatus, CloudeRu ? "Отправляю preset на VPS..." : "Sending preset to VPS...");
			s_aShareCode[0] = '\0';
		}
		else
			str_copy(s_aStatus, CloudeRu ? "URL VPS пустой." : "VPS URL is empty.");
		s_SharePresetText.clear();
	}
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, s_aShareCode[0] ? s_aShareCode : (CloudeRu ? "Код появится после Share." : "Code appears after Share."), FontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	const int PreviewLines = 7;
	size_t Pos = 0;
	for(int i = 0; i < PreviewLines; ++i)
	{
		Content.HSplitTop(LineSize, &Label, &Content);
		if(!HasSelection || Pos >= SelectedText.size())
		{
			Ui()->DoLabel(&Label, i == 0 ? (CloudeRu ? "Выбери preset слева, чтобы посмотреть его." : "Select a preset on the left to preview it.") : "", FontSize, TEXTALIGN_ML);
			continue;
		}
		size_t End = SelectedText.find('\n', Pos);
		if(End == std::string::npos)
			End = SelectedText.size();
		char aLine[160];
		str_truncate(aLine, sizeof(aLine), SelectedText.c_str() + Pos, (int)(End - Pos));
		Ui()->DoLabel(&Label, aLine, FontSize, TEXTALIGN_ML);
		Pos = End + 1;
	}

	Right.HSplitTop(MarginBetweenSections, nullptr, &Right);
	Right.HSplitTop(118.0f, &Card, &Right);
	RenderCard(Card);
	Card.Margin(MarginSmall + 2.0f, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "Получить по коду" : "Get By Code", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Button, &Content);
	Button.VSplitLeft(120.0f, &ButtonA, &Button);
	Button.VSplitLeft(MarginSmall, nullptr, &Button);
	Button.VSplitLeft(120.0f, &ButtonB, nullptr);
	Ui()->DoEditBox(&s_ShareCodeInput, &ButtonA, EditBoxFontSize);
	static CButtonContainer s_GetByCodeButton;
	if(DoButton_Menu(&s_GetByCodeButton, CloudeRu ? "Получить" : "Get", 0, &ButtonB) && !s_pGetRequest)
	{
		char aUrl[300];
		if(!CloudeConfigIsShareCode(s_ShareCodeInput.GetString()))
		{
			str_copy(s_aStatus, CloudeRu ? "Код должен быть из 6 цифр." : "Code must be 6 digits.");
		}
		else if(CloudeConfigShareEndpoint(aUrl, sizeof(aUrl), "config/get"))
		{
			char aCode[32];
			EscapeJson(aCode, sizeof(aCode), s_ShareCodeInput.GetString());
			char aJson[96];
			str_format(aJson, sizeof(aJson), "{\"code\":\"%s\"}", aCode);
			s_pGetRequest = HttpPostJson(aUrl, aJson);
			s_pGetRequest->Timeout(CTimeout{2000, 5000, 0, 0});
			Http()->Run(s_pGetRequest);
			str_copy(s_aStatus, CloudeRu ? "Проверяю код..." : "Checking code...");
		}
		else
			str_copy(s_aStatus, CloudeRu ? "URL VPS пустой." : "VPS URL is empty.");
	}
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, s_aStatus[0] ? s_aStatus : (CloudeRu ? "Preset-ы лежат в tclient/cloude_configs." : "Presets are stored in tclient/cloude_configs."), FontSize, TEXTALIGN_ML);
}

void CMenus::RenderSettingsTClientCloudeInfo(CUIRect MainView)
{
	CUIRect LeftView, RightView, Label, Button, Card, Content;
	const bool CloudeRu = g_Config.m_TcCloudeLanguage == 1;
	MainView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);
	LeftView.VSplitLeft(5.0f, nullptr, &LeftView);
	RightView.VSplitRight(5.0f, &RightView, nullptr);

	LeftView.HSplitTop(HeadlineHeight, &Label, &LeftView);
	Ui()->DoLabel(&Label, "Cloude client dv", HeadlineFontSize, TEXTALIGN_ML);
	LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

	const float TeeSize = 50.0f;
	const float CardSize = TeeSize + MarginSmall * 2.0f;
	LeftView.HSplitTop(CardSize, &Card, &LeftView);
	Card.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.24f), IGraphics::CORNER_ALL, 10.0f);
	Card.Margin(MarginSmall, &Content);
	CUIRect TeeRect;
	Content.VSplitLeft(TeeSize + MarginSmall, &TeeRect, &Content);
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, "Deklais", LineSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginExtraSmall, nullptr, &Content);
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, "Modify / greyfox", FontSize, TEXTALIGN_ML);
	RenderDevSkin(TeeRect.Center(), 50.0f, "greyfox", "greyfox", true, 0, 0, 0, false, true, ColorRGBA(0.45f, 0.45f, 0.45f, 1.00f), ColorRGBA(0.72f, 0.72f, 0.72f, 1.00f));

	LeftView.HSplitTop(MarginBetweenSections, nullptr, &LeftView);
	LeftView.HSplitTop(110.0f, &Card, &LeftView);
	Card.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.24f), IGraphics::CORNER_ALL, 10.0f);
	Card.Margin(MarginSmall, &Content);
	Content.HSplitTop(HeadlineHeight, &Label, &Content);
	Ui()->DoLabel(&Label, CloudeRu ? "О клиенте" : "About", HeadlineFontSize, TEXTALIGN_ML);
	Content.HSplitTop(MarginSmall, nullptr, &Content);
	const char *pAbout1 = CloudeRu ? "Cloude - отдельный блок наших функций внутри TClient." : "Cloude is our separate feature layer inside TClient.";
	const char *pAbout2 = CloudeRu ? "Здесь собраны input, assist, HUD editor, Aled и игровые улучшения." : "It keeps input, assist, HUD editor, Aled and gameplay improvements together.";
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, pAbout1, FontSize, TEXTALIGN_ML);
	Content.HSplitTop(LineSize, &Label, &Content);
	Ui()->DoLabel(&Label, pAbout2, FontSize, TEXTALIGN_ML);

	CUIRect LanguagePanel, RuButton, EnButton;
	LeftView.HSplitBottom(LineSize, &LeftView, &LanguagePanel);
	LanguagePanel.VSplitMid(&EnButton, &RuButton, MarginSmall);
	static CButtonContainer s_CloudeInfoEnglishButton;
	static CButtonContainer s_CloudeInfoRussianButton;
	if(DoButton_MenuTab(&s_CloudeInfoEnglishButton, "ENG", !CloudeRu, &EnButton, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 5.0f))
		g_Config.m_TcCloudeLanguage = 0;
	if(DoButton_MenuTab(&s_CloudeInfoRussianButton, "RUS", CloudeRu, &RuButton, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 5.0f))
		g_Config.m_TcCloudeLanguage = 1;

	RightView.HSplitTop(HeadlineHeight, &Label, &RightView);
	Ui()->DoLabel(&Label, CloudeRu ? "Фишки Cloude" : "Cloude Features", HeadlineFontSize, TEXTALIGN_ML);
	RightView.HSplitTop(MarginSmall, nullptr, &RightView);

	static CScrollRegion s_CloudeInfoScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 45.0f;
	ScrollParams.m_Flags = CScrollRegionParams::FLAG_CONTENT_STATIC_WIDTH;
	ScrollParams.m_ScrollbarMargin = 5.0f;
	s_CloudeInfoScrollRegion.Begin(&RightView, &ScrollOffset, &ScrollParams);
	RightView.y += ScrollOffset.y;
	RightView.VSplitRight(5.0f, &RightView, nullptr);

	auto RenderFeatureInfo = [&](const char *pTitle, const char *pLine1, const char *pLine2) {
		CUIRect FeatureCard, FeatureContent, Title, Text;
		RightView.HSplitTop(68.0f, &FeatureCard, &RightView);
		RightView.HSplitTop(MarginExtraSmall, nullptr, &RightView);
		if(!s_CloudeInfoScrollRegion.AddRect(FeatureCard))
			return;
		FeatureCard.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.28f), IGraphics::CORNER_ALL, 6.0f);
		FeatureCard.Margin(MarginSmall, &FeatureContent);
		FeatureContent.HSplitTop(LineSize, &Title, &FeatureContent);
		Ui()->DoLabel(&Title, pTitle, FontSize, TEXTALIGN_ML);
		FeatureContent.HSplitTop(MarginExtraSmall, nullptr, &FeatureContent);
		FeatureContent.HSplitTop(FontSize + 2.0f, &Text, &FeatureContent);
		Ui()->DoLabel(&Text, pLine1, FontSize * 0.82f, TEXTALIGN_ML);
		FeatureContent.HSplitTop(FontSize + 2.0f, &Text, &FeatureContent);
		Ui()->DoLabel(&Text, pLine2, FontSize * 0.82f, TEXTALIGN_ML);
	};

	if(CloudeRu)
	{
		RenderFeatureInfo("Cloude Input", "Tater, Saiko, Cloude и Cloude+ собраны в одной настройке.", "Movement и Aim/Hook можно настраивать отдельно.");
		RenderFeatureInfo("Auto Settings", "10 секунд анализирует ping и подбирает значения.", "Small, Medium и Big меняют агрессивность результата.");
		RenderFeatureInfo("Dynamic Boost", "Новые direction, jump и hook получают короткий boost.", "Работает во всех fast input режимах.");
		RenderFeatureInfo("Skill Assist", "Micro Direction Assist убирает A/D neutral gap.", "Hook и Jump buffers держат короткий intent.");
		RenderFeatureInfo("Focus Mode", "Пока идет timer, виден только team chat.", "Меньше шума во время рана.");
		RenderFeatureInfo("HUD Editor", "Можно двигать Media Island и Vote HUD прямо в игре.", "Двойной клик возвращает объект в дефолт.");
		RenderFeatureInfo("Aled Counter", "Локально считает только реальные hammer-unfreeze аледы.", "Профиль сохраняется и проверяется подписью.");
	}
	else
	{
		RenderFeatureInfo("Cloude Input", "Tater, Saiko, Cloude and Cloude+ live in one settings block.", "Movement and Aim/Hook can be tuned separately.");
		RenderFeatureInfo("Auto Settings", "Measures ping for 10 seconds and chooses values.", "Small, Medium and Big change how aggressive it feels.");
		RenderFeatureInfo("Dynamic Boost", "Fresh direction, jump and hook inputs get a short boost.", "Works across all fast input modes.");
		RenderFeatureInfo("Skill Assist", "Micro Direction Assist removes tiny A/D neutral gaps.", "Hook and Jump buffers keep short input intent.");
		RenderFeatureInfo("Focus Mode", "While your timer is running, only team chat is shown.", "Less noise during a run.");
		RenderFeatureInfo("HUD Editor", "Media Island and Vote HUD can be moved in-game.", "Double click resets an object to default.");
		RenderFeatureInfo("Aled Counter", "Locally counts only real hammer-unfreeze Aleds.", "The profile is saved and signature checked.");
	}

	CUIRect ScrollEnd;
	RightView.HSplitTop(MarginSmall, &ScrollEnd, &RightView);
	s_CloudeInfoScrollRegion.AddRect(ScrollEnd);
	s_CloudeInfoScrollRegion.End();
}
