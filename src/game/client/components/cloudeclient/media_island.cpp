#include "media_island.h"

#include <base/math.h>
#include <base/system.h>
#include <base/vmath.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/keys.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>
#include <game/teamscore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

#if defined(CONF_FAMILY_WINDOWS)
#define IStorage WindowsIStorage
#ifdef NOGDI
#undef NOGDI
#define MEDIA_ISLAND_DEFINED_NOGDI
#endif
#ifndef NOBITMAP
#define NOBITMAP
#define MEDIA_ISLAND_DEFINED_NOBITMAP
#endif
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#undef IStorage
#ifdef MEDIA_ISLAND_DEFINED_NOBITMAP
#undef NOBITMAP
#undef MEDIA_ISLAND_DEFINED_NOBITMAP
#endif
#ifdef MEDIA_ISLAND_DEFINED_NOGDI
#define NOGDI
#undef MEDIA_ISLAND_DEFINED_NOGDI
#endif
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif

static float ClampFloat(float Value, float Min, float Max)
{
	return std::clamp(Value, Min, Max);
}

static float SmoothProgress(float Value)
{
	Value = ClampFloat(Value, 0.0f, 1.0f);
	return Value * Value * (3.0f - 2.0f * Value);
}

static float DelayedProgress(float Value, float Start)
{
	return SmoothProgress((Value - Start) / maximum(0.001f, 1.0f - Start));
}

// How long the team-invite prompt stays open before it auto-dismisses.
static constexpr float TEAM_INVITE_DURATION_SECONDS = 40.0f;

static ColorRGBA MixColor(ColorRGBA a, ColorRGBA b, float Amount)
{
	Amount = ClampFloat(Amount, 0.0f, 1.0f);
	return ColorRGBA(
		mix(a.r, b.r, Amount),
		mix(a.g, b.g, Amount),
		mix(a.b, b.b, Amount),
		mix(a.a, b.a, Amount));
}

enum
{
	HUD_EDITOR_TARGET_NONE = 0,
	HUD_EDITOR_TARGET_ISLAND,
	HUD_EDITOR_TARGET_VOTE,

	MEDIA_ISLAND_COMMAND_NONE = 0,
	MEDIA_ISLAND_COMMAND_PREVIOUS,
	MEDIA_ISLAND_COMMAND_PLAY_PAUSE,
	MEDIA_ISLAND_COMMAND_NEXT,

	MEDIA_ISLAND_ICON_PLAY = 0,
	MEDIA_ISLAND_ICON_PAUSE,
	MEDIA_ISLAND_ICON_REWIND,
	MEDIA_ISLAND_ICON_FORWARD,
	MEDIA_ISLAND_ICON_SOUND,
	MEDIA_ISLAND_ICON_COUNT,
};

static ColorRGBA AdjustTrackColor(ColorRGBA Color, float SaturationDelta, float ValueDelta, float Alpha = 1.0f)
{
	ColorHSVA Hsv = color_cast<ColorHSVA>(Color);
	Hsv.s = ClampFloat(Hsv.s + SaturationDelta / 255.0f, 42.0f / 255.0f, 165.0f / 255.0f);
	Hsv.v = ClampFloat(Hsv.v + ValueDelta / 255.0f, 38.0f / 255.0f, 170.0f / 255.0f);
	ColorRGBA Rgba = color_cast<ColorRGBA>(Hsv);
	Rgba.a = Alpha;
	return Rgba;
}

static ColorRGBA ToAlbumColor(float R, float G, float B)
{
	return ColorRGBA(
		ClampFloat(R, 0.0f, 1.0f),
		ClampFloat(G, 0.0f, 1.0f),
		ClampFloat(B, 0.0f, 1.0f),
		1.0f);
}

static void RenderTextLeft(ITextRender *pTextRender, const CUIRect &Rect, float FontSize, const char *pText, ColorRGBA Color, int Flags = TEXTFLAG_RENDER | TEXTFLAG_DISALLOW_NEWLINE | TEXTFLAG_ELLIPSIS_AT_END)
{
	if(FontSize <= 0.0f || Rect.w <= 0.0f || pText == nullptr || pText[0] == '\0')
		return;
	const ColorRGBA OldTextColor = pTextRender->GetTextColor();
	const ColorRGBA OldOutlineColor = pTextRender->GetTextOutlineColor();
	CTextCursor Cursor;
	Cursor.SetPosition(vec2(Rect.x, Rect.y));
	Cursor.m_FontSize = FontSize;
	Cursor.m_LineWidth = Rect.w;
	Cursor.m_MaxLines = 1;
	Cursor.m_Flags = Flags;
	pTextRender->TextColor(Color);
	pTextRender->TextOutlineColor(ColorRGBA(0.0f, 0.0f, 0.0f, 0.16f * Color.a));
	pTextRender->TextEx(&Cursor, pText);
	pTextRender->TextOutlineColor(OldOutlineColor);
	pTextRender->TextColor(OldTextColor);
}

static void RenderIconCentered(ITextRender *pTextRender, const CUIRect &Rect, float FontSize, const char *pIcon, ColorRGBA Color)
{
	const ColorRGBA OldTextColor = pTextRender->GetTextColor();
	const ColorRGBA OldOutlineColor = pTextRender->GetTextOutlineColor();
	const unsigned OldRenderFlags = pTextRender->GetRenderFlags();
	pTextRender->SetFontPreset(EFontPreset::ICON_FONT);
	pTextRender->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING);
	const float IconWidth = pTextRender->TextWidth(FontSize, pIcon, -1, -1.0f);
	pTextRender->TextColor(Color);
	pTextRender->TextOutlineColor(ColorRGBA(0.0f, 0.0f, 0.0f, 0.14f * Color.a));
	CTextCursor Cursor;
	Cursor.SetPosition(vec2(Rect.x + (Rect.w - IconWidth) * 0.5f, Rect.y + (Rect.h - FontSize) * 0.5f));
	Cursor.m_FontSize = FontSize;
	Cursor.m_LineWidth = Rect.w;
	Cursor.m_MaxLines = 1;
	Cursor.m_Flags = TEXTFLAG_RENDER | TEXTFLAG_DISALLOW_NEWLINE;
	pTextRender->TextEx(&Cursor, pIcon);
	pTextRender->TextOutlineColor(OldOutlineColor);
	pTextRender->TextColor(OldTextColor);
	pTextRender->SetRenderFlags(OldRenderFlags);
	pTextRender->SetFontPreset(EFontPreset::DEFAULT_FONT);
}

static void RenderTextureIconCentered(IGraphics *pGraphics, IGraphics::CTextureHandle Texture, const vec4 &Uv, const CUIRect &Rect, float Size, ColorRGBA Color)
{
	if(!Texture.IsValid() || Size <= 0.0f || Color.a <= 0.0f)
		return;
	const float UvWidth = maximum(0.001f, Uv.z - Uv.x);
	const float UvHeight = maximum(0.001f, Uv.w - Uv.y);
	const float MaxUvSize = maximum(UvWidth, UvHeight);
	const float Width = Size * UvWidth / MaxUvSize;
	const float Height = Size * UvHeight / MaxUvSize;
	const CUIRect IconRect = {
		Rect.x + (Rect.w - Width) * 0.5f,
		Rect.y + (Rect.h - Height) * 0.5f,
		Width,
		Height};
	pGraphics->TextureSet(Texture);
	pGraphics->QuadsBegin();
	pGraphics->QuadsSetSubset(Uv.x, Uv.y, Uv.z, Uv.w);
	pGraphics->SetColor(Color.r, Color.g, Color.b, Color.a);
	IGraphics::CQuadItem Quad(IconRect.x, IconRect.y, IconRect.w, IconRect.h);
	pGraphics->QuadsDrawTL(&Quad, 1);
	pGraphics->QuadsEnd();
	pGraphics->TextureClear();
}

static std::string NormalizeSingleLineText(const std::string &Text)
{
	std::string Result;
	Result.reserve(Text.size());
	bool PreviousWasSpace = true;
	for(unsigned char Character : Text)
	{
		const bool IsControlSpace = Character < 32 || Character == 127;
		const bool IsAsciiSpace = Character == ' ';
		if(IsControlSpace || IsAsciiSpace)
		{
			if(!PreviousWasSpace)
				Result.push_back(' ');
			PreviousWasSpace = true;
		}
		else
		{
			Result.push_back((char)Character);
			PreviousWasSpace = false;
		}
	}
	if(!Result.empty() && Result.back() == ' ')
		Result.pop_back();
	return Result;
}

static void FormatMediaTime(int64_t TimeMs, bool Negative, char *pBuffer, size_t BufferSize)
{
	TimeMs = maximum<int64_t>(0, TimeMs);
	const int64_t TotalSeconds = TimeMs / 1000;
	const int64_t Hours = TotalSeconds / 3600;
	const int64_t Minutes = (TotalSeconds / 60) % 60;
	const int64_t Seconds = TotalSeconds % 60;
	if(Hours > 0)
		str_format(pBuffer, BufferSize, Negative ? "-%lld:%02lld:%02lld" : "%lld:%02lld:%02lld", (long long)Hours, (long long)Minutes, (long long)Seconds);
	else
		str_format(pBuffer, BufferSize, Negative ? "-%lld:%02lld" : "%lld:%02lld", (long long)(TotalSeconds / 60), (long long)Seconds);
}

static ColorRGBA PrepareVisualizerPrimaryColor(ColorRGBA Color, bool GrayscaleSource)
{
	Color = MixColor(Color, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), GrayscaleSource ? 0.44f : 0.26f);

	ColorHSVA Hsv = color_cast<ColorHSVA>(Color);
	if(GrayscaleSource)
		Hsv.s = minimum(Hsv.s, 0.05f);
	else
		Hsv.s = ClampFloat(Hsv.s + 0.06f, 0.26f, 1.0f);
	Hsv.v = maximum(Hsv.v, GrayscaleSource ? 0.68f : 0.56f);

	ColorRGBA Result = color_cast<ColorRGBA>(Hsv);
	Result.a = 1.0f;
	return Result;
}

static ColorRGBA PrepareVisualizerAccentColor(ColorRGBA Color, ColorRGBA PrimaryColor, bool GrayscaleSource)
{
	Color = MixColor(Color, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), GrayscaleSource ? 0.36f : 0.16f);

	ColorHSVA Hsv = color_cast<ColorHSVA>(Color);
	if(GrayscaleSource)
		Hsv.s = minimum(Hsv.s, 0.04f);
	else
		Hsv.s = ClampFloat(Hsv.s, 0.18f, 1.0f);
	Hsv.v = maximum(Hsv.v, GrayscaleSource ? 0.78f : 0.60f);

	ColorRGBA Result = color_cast<ColorRGBA>(Hsv);
	Result.a = 1.0f;
	return MixColor(PrimaryColor, Result, 0.82f);
}

static void ExtractTrackColors(const std::vector<uint8_t> &vThumbnailRgba, uint32_t Width, uint32_t Height, ColorRGBA &AccentColor, ColorRGBA &DarkColor, ColorRGBA &LightColor)
{
	if(vThumbnailRgba.empty() || Width == 0 || Height == 0)
		return;

	const int SamplesX = minimum<uint32_t>(24, Width);
	const int SamplesY = minimum<uint32_t>(24, Height);
	int ValidSamples = 0;
	float SumR = 0.0f;
	float SumG = 0.0f;
	float SumB = 0.0f;
	float SumSaturation = 0.0f;
	float BestAccentScore = -1.0f;
	ColorRGBA BrightestColor = LightColor;
	for(int sy = 0; sy < SamplesY; ++sy)
	{
		const uint32_t y = (uint32_t)((sy + 0.5f) * Height / SamplesY);
		for(int sx = 0; sx < SamplesX; ++sx)
		{
			const uint32_t x = (uint32_t)((sx + 0.5f) * Width / SamplesX);
			const size_t Index = ((size_t)minimum<uint32_t>(y, Height - 1) * Width + minimum<uint32_t>(x, Width - 1)) * 4;
			const int R8 = vThumbnailRgba[Index + 0];
			const int G8 = vThumbnailRgba[Index + 1];
			const int B8 = vThumbnailRgba[Index + 2];
			const int A8 = vThumbnailRgba[Index + 3];
			if(A8 < 24)
				continue;
			ColorRGBA Rgba(R8 / 255.0f, G8 / 255.0f, B8 / 255.0f, 1.0f);
			ColorHSVA Hsv = color_cast<ColorHSVA>(Rgba);
			if(Hsv.v < 26.0f / 255.0f)
				continue;

			SumR += Rgba.r;
			SumG += Rgba.g;
			SumB += Rgba.b;
			SumSaturation += Hsv.s;
			ValidSamples++;

			const float AccentScore = Hsv.v * 0.74f + Hsv.s * 0.26f;
			if(AccentScore > BestAccentScore)
			{
				BestAccentScore = AccentScore;
				BrightestColor = Rgba;
			}
		}
	}
	if(ValidSamples == 0)
		return;

	const float AverageSaturation = SumSaturation / ValidSamples;
	const ColorHSVA BrightestHsv = color_cast<ColorHSVA>(BrightestColor);
	const bool GrayscaleSource = AverageSaturation < 0.10f && BrightestHsv.s < 0.18f;

	AccentColor = PrepareVisualizerPrimaryColor(ToAlbumColor(SumR / ValidSamples, SumG / ValidSamples, SumB / ValidSamples), GrayscaleSource);
	DarkColor = AdjustTrackColor(AccentColor, -8.0f, -34.0f);
	LightColor = PrepareVisualizerAccentColor(ToAlbumColor(BrightestColor.r, BrightestColor.g, BrightestColor.b), AccentColor, GrayscaleSource);
}

#if defined(CONF_FAMILY_WINDOWS)
static constexpr int MEDIA_ISLAND_LOGO_SIZE = 64;
static constexpr int MEDIA_ISLAND_LOGO_CHANNELS = 3;
using TMediaIslandLogoPixels = std::array<uint8_t, MEDIA_ISLAND_LOGO_SIZE * MEDIA_ISLAND_LOGO_SIZE * MEDIA_ISLAND_LOGO_CHANNELS>;

static const wchar_t *ChromiumExecutableName(const char *pSource)
{
	if(pSource == nullptr || pSource[0] == '\0')
		return nullptr;
	if(str_find_nocase(pSource, "chrome") != nullptr)
		return L"chrome.exe";
	if(str_find_nocase(pSource, "chromium") != nullptr)
		return L"chromium.exe";
	if(str_find_nocase(pSource, "msedge") != nullptr || str_find_nocase(pSource, "microsoft edge") != nullptr || str_find_nocase(pSource, "microsoftedge") != nullptr)
		return L"msedge.exe";
	if(str_find_nocase(pSource, "brave") != nullptr)
		return L"brave.exe";
	if(str_find_nocase(pSource, "vivaldi") != nullptr)
		return L"vivaldi.exe";
	if(str_find_nocase(pSource, "opera gx") != nullptr || str_find_nocase(pSource, "operagx") != nullptr)
		return L"opera.exe";
	if(str_find_nocase(pSource, "opera") != nullptr)
		return L"opera.exe";
	if(str_find_nocase(pSource, "firefox") != nullptr)
		return L"firefox.exe";
	return nullptr;
}

class CMediaApplicationVolume
{
	std::vector<Microsoft::WRL::ComPtr<ISimpleAudioVolume>> m_vVolumes;

public:
	bool Refresh(const char *pSource)
	{
		m_vVolumes.clear();
		const wchar_t *pExecutableName = ChromiumExecutableName(pSource);
		if(pExecutableName == nullptr)
			return false;

		Microsoft::WRL::ComPtr<IMMDeviceEnumerator> DeviceEnumerator;
		Microsoft::WRL::ComPtr<IMMDevice> Device;
		Microsoft::WRL::ComPtr<IAudioSessionManager2> SessionManager;
		Microsoft::WRL::ComPtr<IAudioSessionEnumerator> SessionEnumerator;
		if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&DeviceEnumerator))) ||
			FAILED(DeviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &Device)) ||
			FAILED(Device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(SessionManager.GetAddressOf()))) ||
			FAILED(SessionManager->GetSessionEnumerator(&SessionEnumerator)))
			return false;

		int SessionCount = 0;
		if(FAILED(SessionEnumerator->GetCount(&SessionCount)))
			return false;
		for(int i = 0; i < SessionCount; ++i)
		{
			Microsoft::WRL::ComPtr<IAudioSessionControl> Control;
			Microsoft::WRL::ComPtr<IAudioSessionControl2> Control2;
			if(FAILED(SessionEnumerator->GetSession(i, &Control)) || FAILED(Control.As(&Control2)))
				continue;
			DWORD ProcessId = 0;
			if(FAILED(Control2->GetProcessId(&ProcessId)) || ProcessId == 0)
				continue;
			const HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);
			if(Process == nullptr)
				continue;
			std::array<wchar_t, 32768> aPath{};
			DWORD PathLength = static_cast<DWORD>(aPath.size());
			const bool HasPath = QueryFullProcessImageNameW(Process, 0, aPath.data(), &PathLength) != FALSE;
			CloseHandle(Process);
			if(!HasPath)
				continue;
			const wchar_t *pBaseName = aPath.data();
			if(const wchar_t *pSlash = wcsrchr(pBaseName, L'\\'))
				pBaseName = pSlash + 1;
			if(_wcsicmp(pBaseName, pExecutableName) != 0)
				continue;
			Microsoft::WRL::ComPtr<ISimpleAudioVolume> Volume;
			if(SUCCEEDED(Control.As(&Volume)))
				m_vVolumes.push_back(std::move(Volume));
		}
		return !m_vVolumes.empty();
	}

	bool Get(float &Volume) const
	{
		for(const auto &pVolume : m_vVolumes)
			if(SUCCEEDED(pVolume->GetMasterVolume(&Volume)))
				return true;
		return false;
	}

	bool Set(float Volume)
	{
		bool Applied = false;
		for(const auto &pSessionVolume : m_vVolumes)
			Applied |= SUCCEEDED(pSessionVolume->SetMasterVolume(ClampFloat(Volume, 0.0f, 1.0f), nullptr));
		return Applied;
	}
};

static bool IsRegularFile(const std::wstring &Path)
{
	const DWORD Attributes = GetFileAttributesW(Path.c_str());
	return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool FindExecutableInAppPaths(const wchar_t *pExecutableName, std::wstring &Path)
{
	const std::wstring Key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + pExecutableName;
	static const HKEY s_aRoots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
	for(HKEY Root : s_aRoots)
	{
		DWORD DataBytes = 0;
		const LSTATUS SizeResult = RegGetValueW(Root, Key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &DataBytes);
		if(SizeResult != ERROR_SUCCESS || DataBytes < sizeof(wchar_t))
			continue;
		std::vector<wchar_t> vPath(DataBytes / sizeof(wchar_t) + 1, L'\0');
		if(RegGetValueW(Root, Key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, vPath.data(), &DataBytes) != ERROR_SUCCESS)
			continue;
		Path = vPath.data();
		if(Path.size() >= 2 && Path.front() == L'"' && Path.back() == L'"')
			Path = Path.substr(1, Path.size() - 2);
		if(IsRegularFile(Path))
			return true;
	}
	return false;
}

static bool FindRunningExecutable(const wchar_t *pExecutableName, std::wstring &Path)
{
	const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(Snapshot == INVALID_HANDLE_VALUE)
		return false;
	PROCESSENTRY32W Entry{};
	Entry.dwSize = sizeof(Entry);
	bool Found = false;
	if(Process32FirstW(Snapshot, &Entry))
	{
		do
		{
			if(_wcsicmp(Entry.szExeFile, pExecutableName) != 0)
				continue;
			const HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID);
			if(Process == nullptr)
				continue;
			std::array<wchar_t, 32768> aPath{};
			DWORD PathLength = static_cast<DWORD>(aPath.size());
			if(QueryFullProcessImageNameW(Process, 0, aPath.data(), &PathLength) && PathLength > 0)
			{
				Path.assign(aPath.data(), PathLength);
				Found = IsRegularFile(Path);
			}
			CloseHandle(Process);
			if(Found)
				break;
		} while(Process32NextW(Snapshot, &Entry));
	}
	CloseHandle(Snapshot);
	return Found;
}

static bool RenderExecutableIcon(const std::wstring &Path, TMediaIslandLogoPixels &aPixels)
{
	SHFILEINFOW FileInfo{};
	if(SHGetFileInfoW(Path.c_str(), 0, &FileInfo, sizeof(FileInfo), SHGFI_ICON | SHGFI_LARGEICON) == 0 || FileInfo.hIcon == nullptr)
		return false;

	const HDC ScreenDc = GetDC(nullptr);
	const HDC MemoryDc = ScreenDc != nullptr ? CreateCompatibleDC(ScreenDc) : nullptr;
	const HBITMAP Bitmap = MemoryDc != nullptr ? CreateCompatibleBitmap(ScreenDc, MEDIA_ISLAND_LOGO_SIZE, MEDIA_ISLAND_LOGO_SIZE) : nullptr;
	if(ScreenDc != nullptr)
		ReleaseDC(nullptr, ScreenDc);
	if(MemoryDc == nullptr || Bitmap == nullptr)
	{
		if(Bitmap != nullptr)
			DeleteObject(Bitmap);
		if(MemoryDc != nullptr)
			DeleteDC(MemoryDc);
		DestroyIcon(FileInfo.hIcon);
		return false;
	}

	const HGDIOBJ OldBitmap = SelectObject(MemoryDc, Bitmap);
	if(OldBitmap == nullptr || OldBitmap == HGDI_ERROR)
	{
		DeleteObject(Bitmap);
		DeleteDC(MemoryDc);
		DestroyIcon(FileInfo.hIcon);
		return false;
	}
	PatBlt(MemoryDc, 0, 0, MEDIA_ISLAND_LOGO_SIZE, MEDIA_ISLAND_LOGO_SIZE, BLACKNESS);
	const bool Drawn = DrawIconEx(MemoryDc, 0, 0, FileInfo.hIcon, MEDIA_ISLAND_LOGO_SIZE, MEDIA_ISLAND_LOGO_SIZE, 0, nullptr, DI_NORMAL) != FALSE;
	bool ReadSucceeded = Drawn;
	if(Drawn)
	{
		for(int y = 0; y < MEDIA_ISLAND_LOGO_SIZE && ReadSucceeded; ++y)
		{
			for(int x = 0; x < MEDIA_ISLAND_LOGO_SIZE; ++x)
			{
				const COLORREF Color = GetPixel(MemoryDc, x, y);
				if(Color == CLR_INVALID)
				{
					ReadSucceeded = false;
					break;
				}
				const size_t Index = ((size_t)y * MEDIA_ISLAND_LOGO_SIZE + x) * MEDIA_ISLAND_LOGO_CHANNELS;
				aPixels[Index + 0] = GetRValue(Color);
				aPixels[Index + 1] = GetGValue(Color);
				aPixels[Index + 2] = GetBValue(Color);
			}
		}
	}
	SelectObject(MemoryDc, OldBitmap);
	DeleteObject(Bitmap);
	DeleteDC(MemoryDc);
	DestroyIcon(FileInfo.hIcon);
	return ReadSucceeded;
}

static bool LoadChromiumProductLogo(const char *pSource, TMediaIslandLogoPixels &aPixels)
{
	const wchar_t *pExecutableName = ChromiumExecutableName(pSource);
	if(pExecutableName == nullptr)
		return false;
	std::wstring Path;
	if(!FindExecutableInAppPaths(pExecutableName, Path) && !FindRunningExecutable(pExecutableName, Path))
		return false;
	return RenderExecutableIcon(Path, aPixels);
}

struct SMediaIslandLogoSignature
{
	std::array<float, 16 * 16 * 3> m_aColors{};
	uint64_t m_DifferenceHash = 0;
	float m_ActiveCoverage = 0.0f;
	bool m_Valid = false;
};

static SMediaIslandLogoSignature MakeLogoSignature(const TMediaIslandLogoPixels &aPixels)
{
	SMediaIslandLogoSignature Signature;
	std::array<float, 3> aBackground{};
	int BackgroundSamples = 0;
	for(int y = 0; y < MEDIA_ISLAND_LOGO_SIZE; ++y)
	{
		for(int x = 0; x < MEDIA_ISLAND_LOGO_SIZE; ++x)
		{
			if(!((x < 8 || x >= MEDIA_ISLAND_LOGO_SIZE - 8) && (y < 8 || y >= MEDIA_ISLAND_LOGO_SIZE - 8)))
				continue;
			const size_t Index = ((size_t)y * MEDIA_ISLAND_LOGO_SIZE + x) * MEDIA_ISLAND_LOGO_CHANNELS;
			for(int Channel = 0; Channel < 3; ++Channel)
				aBackground[Channel] += aPixels[Index + Channel];
			BackgroundSamples++;
		}
	}
	for(float &Background : aBackground)
		Background /= maximum(1, BackgroundSamples);
	auto IsBackground = [&](size_t Index) {
		float MaxDifference = 0.0f;
		for(int Channel = 0; Channel < 3; ++Channel)
			MaxDifference = maximum(MaxDifference, std::abs(aPixels[Index + Channel] - aBackground[Channel]));
		return MaxDifference <= 14.0f;
	};
	int MinX = MEDIA_ISLAND_LOGO_SIZE;
	int MinY = MEDIA_ISLAND_LOGO_SIZE;
	int MaxX = -1;
	int MaxY = -1;
	int ActivePixels = 0;
	for(int y = 0; y < MEDIA_ISLAND_LOGO_SIZE; ++y)
	{
		for(int x = 0; x < MEDIA_ISLAND_LOGO_SIZE; ++x)
		{
			const size_t Index = ((size_t)y * MEDIA_ISLAND_LOGO_SIZE + x) * MEDIA_ISLAND_LOGO_CHANNELS;
			if(IsBackground(Index))
				continue;
			MinX = minimum(MinX, x);
			MinY = minimum(MinY, y);
			MaxX = maximum(MaxX, x);
			MaxY = maximum(MaxY, y);
			ActivePixels++;
		}
	}
	if(MaxX < MinX || MaxY < MinY || ActivePixels < 64)
		return Signature;
	MinX = maximum(0, MinX - 1);
	MinY = maximum(0, MinY - 1);
	MaxX = minimum(MEDIA_ISLAND_LOGO_SIZE - 1, MaxX + 1);
	MaxY = minimum(MEDIA_ISLAND_LOGO_SIZE - 1, MaxY + 1);
	const float BoundsWidth = MaxX - MinX + 1.0f;
	const float BoundsHeight = MaxY - MinY + 1.0f;
	Signature.m_ActiveCoverage = ActivePixels / (BoundsWidth * BoundsHeight);

	auto Sample = [&](float NormalizedX, float NormalizedY, int Channel) {
		const float SourceX = ClampFloat(MinX + NormalizedX * BoundsWidth - 0.5f, (float)MinX, (float)MaxX);
		const float SourceY = ClampFloat(MinY + NormalizedY * BoundsHeight - 0.5f, (float)MinY, (float)MaxY);
		const int X0 = (int)std::floor(SourceX);
		const int Y0 = (int)std::floor(SourceY);
		const int X1 = minimum(MaxX, X0 + 1);
		const int Y1 = minimum(MaxY, Y0 + 1);
		const float Fx = SourceX - X0;
		const float Fy = SourceY - Y0;
		auto Value = [&](int x, int y) {
			const size_t Index = ((size_t)y * MEDIA_ISLAND_LOGO_SIZE + x) * MEDIA_ISLAND_LOGO_CHANNELS;
			return IsBackground(Index) ? 0.0f : aPixels[Index + Channel] / 255.0f;
		};
		return mix(mix(Value(X0, Y0), Value(X1, Y0), Fx), mix(Value(X0, Y1), Value(X1, Y1), Fx), Fy);
	};

	for(int y = 0; y < 16; ++y)
	{
		for(int x = 0; x < 16; ++x)
		{
			for(int Channel = 0; Channel < 3; ++Channel)
				Signature.m_aColors[((size_t)y * 16 + x) * 3 + Channel] = Sample((x + 0.5f) / 16.0f, (y + 0.5f) / 16.0f, Channel);
		}
	}
	for(int y = 0; y < 8; ++y)
	{
		for(int x = 0; x < 8; ++x)
		{
			auto Luminance = [&](int SampleX) {
				const float R = Sample((SampleX + 0.5f) / 9.0f, (y + 0.5f) / 8.0f, 0);
				const float G = Sample((SampleX + 0.5f) / 9.0f, (y + 0.5f) / 8.0f, 1);
				const float B = Sample((SampleX + 0.5f) / 9.0f, (y + 0.5f) / 8.0f, 2);
				return R * 0.2126f + G * 0.7152f + B * 0.0722f;
			};
			if(Luminance(x) > Luminance(x + 1))
				Signature.m_DifferenceHash |= (uint64_t)1 << (y * 8 + x);
		}
	}
	Signature.m_Valid = true;
	return Signature;
}

static bool IsMatchingProductLogo(const std::vector<uint8_t> &vThumbnailRgba, uint32_t ThumbnailWidth, uint32_t ThumbnailHeight, const TMediaIslandLogoPixels &aProductLogo)
{
	if(ThumbnailWidth == 0 || ThumbnailHeight == 0 || vThumbnailRgba.size() != (size_t)ThumbnailWidth * ThumbnailHeight * 4)
		return false;
	TMediaIslandLogoPixels aThumbnail{};
	for(int y = 0; y < MEDIA_ISLAND_LOGO_SIZE; ++y)
	{
		const uint32_t SourceY = minimum(ThumbnailHeight - 1, (uint32_t)((y + 0.5f) * ThumbnailHeight / MEDIA_ISLAND_LOGO_SIZE));
		for(int x = 0; x < MEDIA_ISLAND_LOGO_SIZE; ++x)
		{
			const uint32_t SourceX = minimum(ThumbnailWidth - 1, (uint32_t)((x + 0.5f) * ThumbnailWidth / MEDIA_ISLAND_LOGO_SIZE));
			const size_t SourcePixel = (size_t)SourceY * ThumbnailWidth + SourceX;
			const size_t TargetPixel = (size_t)y * MEDIA_ISLAND_LOGO_SIZE + x;
			const uint8_t Alpha = vThumbnailRgba[SourcePixel * 4 + 3];
			for(int Channel = 0; Channel < MEDIA_ISLAND_LOGO_CHANNELS; ++Channel)
				aThumbnail[TargetPixel * MEDIA_ISLAND_LOGO_CHANNELS + Channel] = (vThumbnailRgba[SourcePixel * 4 + Channel] * Alpha + 127) / 255;
		}
	}
	const SMediaIslandLogoSignature Thumbnail = MakeLogoSignature(aThumbnail);
	const SMediaIslandLogoSignature ProductLogo = MakeLogoSignature(aProductLogo);
	if(!Thumbnail.m_Valid || !ProductLogo.m_Valid)
		return false;

	float ColorError = 0.0f;
	for(size_t i = 0; i < Thumbnail.m_aColors.size(); ++i)
		ColorError += std::abs(Thumbnail.m_aColors[i] - ProductLogo.m_aColors[i]);
	ColorError /= Thumbnail.m_aColors.size();
	uint64_t DifferentBits = Thumbnail.m_DifferenceHash ^ ProductLogo.m_DifferenceHash;
	int HashDistance = 0;
	while(DifferentBits != 0)
	{
		DifferentBits &= DifferentBits - 1;
		HashDistance++;
	}
	const float CoverageDifference = std::abs(Thumbnail.m_ActiveCoverage - ProductLogo.m_ActiveCoverage);
	// All three checks are intentionally conservative. Square media artwork is
	// common, so dimensions alone must never be used to reject it.
	return HashDistance <= 9 && ColorError <= 0.14f && CoverageDifference <= 0.18f;
}

class CWinrtApartment final
{
public:
	CWinrtApartment()
	{
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	}

	~CWinrtApartment()
	{
		winrt::uninit_apartment();
	}
};

class CMediaIslandWorkerStopped final
{
};

template<typename TAsync>
static auto WaitForMediaIslandAsync(TAsync Async, const std::atomic<bool> &Stop)
{
	using winrt::Windows::Foundation::AsyncStatus;
	while(Async.Status() == AsyncStatus::Started)
	{
		if(Stop.load(std::memory_order_relaxed))
		{
			Async.Cancel();
			throw CMediaIslandWorkerStopped();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if(Stop.load(std::memory_order_relaxed))
		throw CMediaIslandWorkerStopped();
	return Async.GetResults();
}

class CAudioLoopbackBands
{
	Microsoft::WRL::ComPtr<IAudioClient> m_pAudioClient;
	Microsoft::WRL::ComPtr<IAudioCaptureClient> m_pCaptureClient;
	WAVEFORMATEX *m_pFormat = nullptr;
	std::vector<float> m_vSamples;
	std::array<float, 6> m_aBandCeilings = {0.004f, 0.004f, 0.004f, 0.004f, 0.004f, 0.004f};
	float m_GlobalCeiling = 0.004f;
	UINT32 m_Channels = 0;
	UINT32 m_SampleRate = 0;
	bool m_Started = false;

	float ReadSample(const BYTE *pData, UINT32 Frame, UINT32 Channel) const
	{
		const BYTE *pSample = pData + (Frame * m_Channels + Channel) * (m_pFormat->wBitsPerSample / 8);
		const WORD FormatTag = m_pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE *)m_pFormat)->SubFormat.Data1 : m_pFormat->wFormatTag;
		if(FormatTag == WAVE_FORMAT_IEEE_FLOAT && m_pFormat->wBitsPerSample == 32)
			return ((const float *)pSample)[0];
		if(FormatTag == WAVE_FORMAT_PCM)
		{
			if(m_pFormat->wBitsPerSample == 16)
				return ((const int16_t *)pSample)[0] / 32768.0f;
			if(m_pFormat->wBitsPerSample == 24)
			{
				int32_t Value = (int32_t)((pSample[2] << 24) | (pSample[1] << 16) | (pSample[0] << 8));
				return Value / 2147483648.0f;
			}
			if(m_pFormat->wBitsPerSample == 32)
				return ((const int32_t *)pSample)[0] / 2147483648.0f;
		}
		return 0.0f;
	}

	static void ComputeFftBands(const std::vector<float> &vSamples, UINT32 SampleRate, std::array<float, 6> &aMagnitudes)
	{
		static constexpr size_t FFT_SIZE = 1024;
		if(vSamples.size() < FFT_SIZE || SampleRate == 0)
			return;

		std::array<float, FFT_SIZE> aReal{};
		std::array<float, FFT_SIZE> aImag{};
		const size_t Start = vSamples.size() - FFT_SIZE;
		for(size_t i = 0; i < FFT_SIZE; ++i)
		{
			const float Window = 0.5f - 0.5f * std::cos(2.0f * pi * i / (FFT_SIZE - 1));
			aReal[i] = vSamples[Start + i] * Window;
		}

		for(size_t i = 1, j = 0; i < FFT_SIZE; ++i)
		{
			size_t Bit = FFT_SIZE >> 1;
			for(; j & Bit; Bit >>= 1)
				j ^= Bit;
			j ^= Bit;
			if(i < j)
			{
				std::swap(aReal[i], aReal[j]);
				std::swap(aImag[i], aImag[j]);
			}
		}

		for(size_t Len = 2; Len <= FFT_SIZE; Len <<= 1)
		{
			const float Angle = -2.0f * pi / Len;
			const float WLenR = std::cos(Angle);
			const float WLenI = std::sin(Angle);
			for(size_t i = 0; i < FFT_SIZE; i += Len)
			{
				float WR = 1.0f;
				float WI = 0.0f;
				for(size_t j = 0; j < Len / 2; ++j)
				{
					const size_t Even = i + j;
					const size_t Odd = i + j + Len / 2;
					const float OddR = aReal[Odd] * WR - aImag[Odd] * WI;
					const float OddI = aReal[Odd] * WI + aImag[Odd] * WR;
					aReal[Odd] = aReal[Even] - OddR;
					aImag[Odd] = aImag[Even] - OddI;
					aReal[Even] += OddR;
					aImag[Even] += OddI;

					const float NextWR = WR * WLenR - WI * WLenI;
					WI = WR * WLenI + WI * WLenR;
					WR = NextWR;
				}
			}
		}

		static constexpr std::array<float, 6> BAND_LOW = {30.0f, 120.0f, 250.0f, 600.0f, 2000.0f, 6000.0f};
		static constexpr std::array<float, 6> BAND_HIGH = {120.0f, 250.0f, 600.0f, 2000.0f, 6000.0f, 18000.0f};
		std::array<int, 6> aCounts{};
		for(size_t Bin = 1; Bin < FFT_SIZE / 2; ++Bin)
		{
			const float Frequency = Bin * (float)SampleRate / FFT_SIZE;
			const float Magnitude = std::sqrt(aReal[Bin] * aReal[Bin] + aImag[Bin] * aImag[Bin]) / FFT_SIZE;
			const float LogMagnitude = std::log1p(Magnitude * 220.0f);
			for(size_t Band = 0; Band < aMagnitudes.size(); ++Band)
			{
				if(Frequency >= BAND_LOW[Band] && Frequency < BAND_HIGH[Band] && Frequency < SampleRate * 0.48f)
				{
					aMagnitudes[Band] += LogMagnitude;
					aCounts[Band]++;
					break;
				}
			}
		}

		for(size_t Band = 0; Band < aMagnitudes.size(); ++Band)
		{
			if(aCounts[Band] > 0)
				aMagnitudes[Band] /= aCounts[Band];
		}
	}

public:
	~CAudioLoopbackBands()
	{
		if(m_pAudioClient && m_Started)
			m_pAudioClient->Stop();
		if(m_pFormat)
			CoTaskMemFree(m_pFormat);
	}

	bool Init()
	{
		Microsoft::WRL::ComPtr<IMMDeviceEnumerator> pEnumerator;
		if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator))))
			return false;
		Microsoft::WRL::ComPtr<IMMDevice> pDevice;
		if(FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice)))
			return false;
		if(FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_pAudioClient)))
			return false;
		if(FAILED(m_pAudioClient->GetMixFormat(&m_pFormat)) || !m_pFormat)
			return false;
		m_Channels = m_pFormat->nChannels;
		m_SampleRate = m_pFormat->nSamplesPerSec;
		if(m_Channels == 0 || m_SampleRate == 0)
			return false;
		if(FAILED(m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, m_pFormat, nullptr)))
			return false;
		if(FAILED(m_pAudioClient->GetService(IID_PPV_ARGS(&m_pCaptureClient))))
			return false;
		if(FAILED(m_pAudioClient->Start()))
			return false;
		m_Started = true;
		return true;
	}

	bool Update(float &Peak, std::array<float, 6> &aBands)
	{
		if(!m_pCaptureClient)
			return false;
		bool HasNewFrames = false;
		UINT32 PacketLength = 0;
		HRESULT Result = m_pCaptureClient->GetNextPacketSize(&PacketLength);
		if(FAILED(Result))
			return false;
		while(PacketLength > 0)
		{
			BYTE *pData = nullptr;
			UINT32 Frames = 0;
			DWORD Flags = 0;
			if(FAILED(m_pCaptureClient->GetBuffer(&pData, &Frames, &Flags, nullptr, nullptr)))
				return false;
			HasNewFrames |= Frames > 0;
			for(UINT32 Frame = 0; Frame < Frames; ++Frame)
			{
				float Mono = 0.0f;
				if(!(Flags & AUDCLNT_BUFFERFLAGS_SILENT))
				{
					for(UINT32 Channel = 0; Channel < m_Channels; ++Channel)
						Mono += ReadSample(pData, Frame, Channel);
					Mono /= m_Channels;
				}
				m_vSamples.push_back(ClampFloat(Mono, -1.0f, 1.0f));
			}
			if(FAILED(m_pCaptureClient->ReleaseBuffer(Frames)))
				return false;
			Result = m_pCaptureClient->GetNextPacketSize(&PacketLength);
			if(FAILED(Result))
				return false;
		}
		// Do not repeatedly publish the same FFT window when the endpoint did not
		// deliver a packet. The renderer will smoothly decay the last values.
		if(!HasNewFrames)
			return true;

		const size_t MaxSamples = minimum<size_t>(4096, m_SampleRate / 8);
		if(m_vSamples.size() > MaxSamples)
			m_vSamples.erase(m_vSamples.begin(), m_vSamples.end() - MaxSamples);
		if(m_vSamples.size() < 256)
			return true;

		float LocalPeak = 0.0f;
		float Rms = 0.0f;
		for(float Sample : m_vSamples)
		{
			LocalPeak = maximum(LocalPeak, absolute(Sample));
			Rms += Sample * Sample;
		}
		Rms = std::sqrt(Rms / m_vSamples.size());
		Peak = ClampFloat(maximum(LocalPeak * 3.2f, Rms * 8.0f), 0.0f, 1.0f);

		std::array<float, 6> aMagnitudes{};
		ComputeFftBands(m_vSamples, m_SampleRate, aMagnitudes);
		for(size_t i = 0; i < aBands.size(); ++i)
		{
			m_aBandCeilings[i] = maximum(m_aBandCeilings[i] * 0.996f, aMagnitudes[i]);
		}

		static constexpr std::array<float, 6> RESPONSE_WEIGHTS = {1.14f, 1.08f, 0.96f, 0.94f, 1.04f, 1.18f};
		float WeightedSum = 0.0f;
		float WeightedMax = 0.0f;
		for(size_t i = 0; i < aBands.size(); ++i)
		{
			aMagnitudes[i] *= RESPONSE_WEIGHTS[i];
			WeightedSum += aMagnitudes[i];
			WeightedMax = maximum(WeightedMax, aMagnitudes[i]);
		}
		m_GlobalCeiling = maximum(m_GlobalCeiling * 0.994f, WeightedMax);

		const float Loudness = ClampFloat(Peak * 0.85f + Rms * 9.0f, 0.0f, 1.0f);
		for(size_t i = 0; i < aBands.size(); ++i)
		{
			const float SpectralShare = WeightedSum > 0.000001f ? aMagnitudes[i] / WeightedSum : 0.0f;
			const float ExpectedShare = 1.0f / aBands.size();
			const float ShareAboveFlat = maximum(0.0f, SpectralShare - ExpectedShare * 0.82f);
			const float RelativePresence = ClampFloat(ShareAboveFlat / (ExpectedShare * 1.35f), 0.0f, 1.0f);
			const float AbsolutePresence = ClampFloat(aMagnitudes[i] / maximum(0.0008f, m_GlobalCeiling), 0.0f, 1.0f);
			aBands[i] = ClampFloat((std::pow(RelativePresence, 0.54f) * 0.62f + AbsolutePresence * 0.38f) * (0.24f + Loudness * 1.25f), 0.0f, 1.0f);
		}
		return true;
	}
};
#endif

CMediaIsland::~CMediaIsland()
{
#if defined(CONF_FAMILY_WINDOWS)
	StopWorker();
#endif
}

void CMediaIsland::ClearInteractionGeometry()
{
	m_HasLastIslandRect = false;
	m_HasLastIslandButtons = false;
	m_HasInviteButtons = false;
	m_LastIslandRect = {};
	m_aLastIslandButtons = {};
	m_LastIslandVolumeSlider = {};
	m_LastInviteAcceptButton = {};
	m_LastInviteIgnoreButton = {};
	m_IslandVolumeTrackLeft = 0.0f;
	m_IslandVolumeTrackRight = 0.0f;
}

void CMediaIsland::CloseInteraction()
{
	m_IslandInteractActive = false;
	m_IslandInteractMouseInitialized = false;
	m_IslandExpanded = false;
	m_IslandVolumeOpen = false;
	m_IslandVolumeDragging = false;
}

void CMediaIsland::ResetTransientState()
{
	CloseInteraction();
	ClearInteractionGeometry();
	m_HasLastVoteRect = false;
	m_LastVoteRect = {};
	m_HudEditorDragTarget = HUD_EDITOR_TARGET_NONE;
	m_HudEditorMouseInitialized = false;
	m_TeamInvitePending = -1;
	m_TeamInviteActive = false;
	m_TeamInviteProgress = 0.0f;
	m_aTeamInviteInviter[0] = '\0';
	m_IslandHoverProgress = 0.0f;
	m_IslandExpandProgress = 0.0f;
	m_IslandVolumeProgress = 0.0f;
	m_AnimatedIslandWidth = 0.0f;
	m_AnimatedIslandHeight = 0.0f;
	m_AnimatedIslandRadius = 0.0f;
	m_IslandGeometryInitialized = false;
	m_MediaTrackRatioInitialized = false;
	m_aIslandButtonHoverProgress = {};
	m_aInviteButtonHoverProgress = {};
	m_IslandVolumeHoverProgress = 0.0f;
}

void CMediaIsland::NotifyTeamInvite(int Team, const char *pInviter)
{
	if(!g_Config.m_TcMediaIsland || Client()->State() != IClient::STATE_ONLINE || Team <= TEAM_FLOCK || Team >= TEAM_SUPER)
		return;
	m_TeamInvitePending = Team;
	m_TeamInviteStartTime = time_get();
	str_copy(m_aTeamInviteInviter, pInviter ? pInviter : "");
	str_sanitize_cc(m_aTeamInviteInviter);
	m_TeamInviteActive = true;
	m_IslandExpanded = false;
	m_IslandVolumeOpen = false;
	m_IslandVolumeDragging = false;
}

void CMediaIsland::ResolveTeamInvite(bool Accept)
{
	if(!m_TeamInviteActive || m_TeamInvitePending < 0)
		return;
	if(Accept)
	{
		char aCmd[32];
		str_format(aCmd, sizeof(aCmd), "/team %d", m_TeamInvitePending);
		GameClient()->m_Chat.SendChat(0, aCmd);
	}
	m_TeamInviteActive = false;
	m_HasInviteButtons = false;
	CloseInteraction();
}

void CMediaIsland::OnConsoleInit()
{
	Console()->Register("tc_test_team_invite", "?i[team]", CFGFLAG_CLIENT, ConTestTeamInvite, this, "Preview the media island team-invite prompt (optional team number, default 5)");
}

void CMediaIsland::ConTestTeamInvite(IConsole::IResult *pResult, void *pUserData)
{
	CMediaIsland *pThis = static_cast<CMediaIsland *>(pUserData);
	const int Team = pResult->NumArguments() > 0 ? pResult->GetInteger(0) : 5;
	pThis->NotifyTeamInvite(Team, "TestPlayer");
}

void CMediaIsland::OnInit()
{
	static const char *s_apIconPaths[MEDIA_ISLAND_ICON_COUNT] = {
		"cloude/media_island/play.png",
		"cloude/media_island/pause.png",
		"cloude/media_island/rewind.png",
		"cloude/media_island/fast-forward.png",
		"cloude/media_island/sound.png",
	};
	static const char *s_apIconTextureNames[MEDIA_ISLAND_ICON_COUNT] = {
		"media-island-play",
		"media-island-pause",
		"media-island-rewind",
		"media-island-fast-forward",
		"media-island-sound",
	};
	for(size_t i = 0; i < m_aControlIconTextures.size(); ++i)
	{
		m_aControlIconUvs[i] = vec4(0.0f, 0.0f, 1.0f, 1.0f);
		CImageInfo Image;
		if(!Graphics()->LoadPng(Image, s_apIconPaths[i], IStorage::TYPE_ALL) || Image.m_pData == nullptr)
			continue;
		if(Image.m_Format != CImageInfo::FORMAT_RGBA)
		{
			Image.Free();
			continue;
		}

		int MinX = Image.m_Width;
		int MinY = Image.m_Height;
		int MaxX = -1;
		int MaxY = -1;
		const size_t PixelCount = (size_t)Image.m_Width * Image.m_Height;
		for(size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			uint8_t *pPixel = &Image.m_pData[Pixel * 4];
			// Keep transparent texels white as well, otherwise mipmap filtering pulls
			// their black RGB into the antialiased edge and creates a dark halo.
			pPixel[0] = 255;
			pPixel[1] = 255;
			pPixel[2] = 255;
			if(pPixel[3] <= 4)
				continue;
			const int x = static_cast<int>(Pixel % static_cast<size_t>(Image.m_Width));
			const int y = static_cast<int>(Pixel / static_cast<size_t>(Image.m_Width));
			MinX = minimum(MinX, x);
			MinY = minimum(MinY, y);
			MaxX = maximum(MaxX, x);
			MaxY = maximum(MaxY, y);
		}
		if(MaxX < MinX || MaxY < MinY)
		{
			Image.Free();
			continue;
		}
		constexpr int IconPadding = 3;
		MinX = maximum(0, MinX - IconPadding);
		MinY = maximum(0, MinY - IconPadding);
		MaxX = minimum(static_cast<int>(Image.m_Width) - 1, MaxX + IconPadding);
		MaxY = minimum(static_cast<int>(Image.m_Height) - 1, MaxY + IconPadding);
		m_aControlIconUvs[i] = vec4(
			MinX / (float)Image.m_Width,
			MinY / (float)Image.m_Height,
			(MaxX + 1) / (float)Image.m_Width,
			(MaxY + 1) / (float)Image.m_Height);
		m_aControlIconTextures[i] = Graphics()->LoadTextureRawMove(Image, 0, s_apIconTextureNames[i]);
		m_aControlIconLoaded[i] = m_aControlIconTextures[i].IsValid();
	}
#if defined(CONF_FAMILY_WINDOWS)
	StartWorker();
#endif
}

void CMediaIsland::OnShutdown()
{
#if defined(CONF_FAMILY_WINDOWS)
	StopWorker();
#endif
	ResetTransientState();
	if(m_HasThumbnailTexture)
	{
		Graphics()->UnloadTexture(&m_ThumbnailTexture);
		m_HasThumbnailTexture = false;
	}
	if(m_HasPreviousThumbnailTexture)
	{
		Graphics()->UnloadTexture(&m_PreviousThumbnailTexture);
		m_HasPreviousThumbnailTexture = false;
	}
	for(size_t i = 0; i < m_aControlIconTextures.size(); ++i)
	{
		if(m_aControlIconLoaded[i])
		{
			Graphics()->UnloadTexture(&m_aControlIconTextures[i]);
			m_aControlIconLoaded[i] = false;
		}
	}
}

void CMediaIsland::RenderSdfRoundedRect(const CUIRect &Rect, const SSdfRoundedRectStyle &Style)
{
	if(Rect.w <= 0.0f || Rect.h <= 0.0f || (Style.m_Fill.a <= 0.0f && Style.m_Stroke.a <= 0.0f && Style.m_Glow.a <= 0.0f))
		return;

	const float Radius = ClampFloat(Style.m_Radius, 0.0f, minimum(Rect.w, Rect.h) * 0.5f);
	const float Softness = maximum(0.0f, Style.m_Softness);
	const float Progress = ClampFloat(Style.m_Progress, 0.0f, 1.0f);
	if(Progress <= 0.001f)
		return;

	if(Style.m_Glow.a > 0.0f && Style.m_GlowSize > 0.0f)
	{
		const float GlowSteps = 3.0f;
		for(int i = (int)GlowSteps; i >= 1; --i)
		{
			const float t = i / GlowSteps;
			CUIRect GlowRect = Rect;
			GlowRect.Margin(-Style.m_GlowSize * t, &GlowRect);
			ColorRGBA GlowColor = Style.m_Glow;
			GlowColor.a *= Progress * 0.20f * (1.0f - t * 0.35f);
			Graphics()->DrawRect(GlowRect.x, GlowRect.y, GlowRect.w, GlowRect.h, GlowColor, IGraphics::CORNER_ALL, Radius + Style.m_GlowSize * t);
		}
	}

	if(Style.m_Fill.a > 0.0f)
	{
		ColorRGBA Fill = Style.m_Fill;
		Fill.a *= Progress;
		Graphics()->DrawRect(Rect.x, Rect.y, Rect.w, Rect.h, Fill, IGraphics::CORNER_ALL, Radius);
	}

	if(Style.m_Stroke.a > 0.0f && Style.m_StrokeWidth > 0.0f)
	{
		const int Steps = maximum(1, round_to_int(Style.m_StrokeWidth / maximum(0.35f, Softness)));
		for(int i = 0; i < Steps; ++i)
		{
			const float Inset = i * (Style.m_StrokeWidth / Steps);
			CUIRect StrokeRect = Rect;
			StrokeRect.Margin(Inset, &StrokeRect);
			ColorRGBA StrokeColor = Style.m_Stroke;
			StrokeColor.a *= Progress * (1.0f - i / (float)Steps * 0.55f);
			Graphics()->DrawRect(StrokeRect.x, StrokeRect.y, StrokeRect.w, StrokeRect.h, StrokeColor, IGraphics::CORNER_ALL, maximum(0.0f, Radius - Inset));
		}
		CUIRect Inner = Rect;
		Inner.Margin(Style.m_StrokeWidth, &Inner);
		if(Inner.w > 0.0f && Inner.h > 0.0f)
		{
			ColorRGBA InnerFill = Style.m_Fill;
			InnerFill.a *= Progress;
			Graphics()->DrawRect(Inner.x, Inner.y, Inner.w, Inner.h, InnerFill, IGraphics::CORNER_ALL, maximum(0.0f, Radius - Style.m_StrokeWidth));
		}
	}
}

void CMediaIsland::RenderSmoothPill(const CUIRect &Rect, ColorRGBA Color)
{
	RenderSmoothRoundedRect(Rect, Color, minimum(Rect.w, Rect.h) * 0.5f);
}

void CMediaIsland::RenderSmoothRoundedRect(const CUIRect &Rect, ColorRGBA Color, float Radius)
{
	if(Rect.w <= 0.0f || Rect.h <= 0.0f || Color.a <= 0.0f)
		return;
	Radius = ClampFloat(Radius, 0.0f, minimum(Rect.w, Rect.h) * 0.5f);

	SSdfRoundedRectStyle Style;
	Style.m_Fill = Color;
	Style.m_Radius = Radius;
	Style.m_Softness = 0.85f;
	RenderSdfRoundedRect(Rect, Style);
}

void CMediaIsland::RenderSoftVisualizerBar(const CUIRect &Rect, ColorRGBA Color, float Alpha, float PixelWidth, float PixelHeight, float GlowStrength)
{
	if(Rect.w <= 0.0f || Rect.h <= 0.0f || Alpha <= 0.0f)
		return;

	const float Radius = minimum(Rect.w, Rect.h) * 0.5f;
	const float FeatherX = maximum(PixelWidth * 0.72f, Rect.w * 0.20f);
	const float FeatherY = maximum(PixelHeight * 0.58f, Rect.w * 0.16f);

	ColorRGBA GlowColor = Color;
	GlowColor.a = Alpha * ClampFloat(GlowStrength, 0.0f, 1.0f) * 0.28f;
	if(GlowColor.a > 0.001f)
		RenderSmoothRoundedRect({Rect.x - FeatherX * 1.25f, Rect.y - FeatherY * 1.25f, Rect.w + FeatherX * 2.5f, Rect.h + FeatherY * 2.5f}, GlowColor, Radius + FeatherX);

	ColorRGBA SoftEdge = Color;
	SoftEdge.a = Alpha * 0.26f;
	RenderSmoothRoundedRect({Rect.x - FeatherX, Rect.y - FeatherY, Rect.w + FeatherX * 2.0f, Rect.h + FeatherY * 2.0f}, SoftEdge, Radius + FeatherX);

	ColorRGBA MidEdge = Color;
	MidEdge.a = Alpha * 0.54f;
	RenderSmoothRoundedRect({Rect.x - FeatherX * 0.38f, Rect.y - FeatherY * 0.38f, Rect.w + FeatherX * 0.76f, Rect.h + FeatherY * 0.76f}, MidEdge, Radius + FeatherX * 0.35f);

	ColorRGBA Core = Color;
	Core.a = Alpha;
	RenderSmoothRoundedRect(Rect, Core, Radius);

	ColorRGBA Highlight = MixColor(Color, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.22f);
	Highlight.a = Alpha * 0.20f;
	RenderSmoothRoundedRect({Rect.x + Rect.w * 0.16f, Rect.y + Rect.w * 0.24f, Rect.w * 0.26f, maximum(Rect.w * 0.60f, Rect.h - Rect.w * 0.48f)}, Highlight, Rect.w * 0.13f);
}

void CMediaIsland::RenderVoteExample(const CUIRect &Rect)
{
	SSdfRoundedRectStyle Style;
	Style.m_Fill = ColorRGBA(0.0f, 0.0f, 0.0f, 0.46f);
	Style.m_Stroke = ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f);
	Style.m_Glow = ColorRGBA(0.0f, 0.0f, 0.0f, 0.12f);
	Style.m_Radius = 3.0f;
	Style.m_StrokeWidth = 0.45f;
	Style.m_GlowSize = 1.6f;
	Style.m_Softness = 0.28f;
	RenderSdfRoundedRect(Rect, Style);

	CUIRect View = Rect;
	View.Margin(3.0f, &View);

	SLabelProperties Props;
	Props.m_EllipsisAtEnd = true;
	Props.m_MaxWidth = View.w;

	CUIRect Row, LeftColumn, RightColumn;
	View.HSplitTop(6.0f, &Row, &View);
	Ui()->DoLabel(&Row, "Vote example", 6.0f, TEXTALIGN_ML, Props);

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(5.0f, &Row, &View);
	Ui()->DoLabel(&Row, "Reason: HUD editor", 4.5f, TEXTALIGN_ML, Props);

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(4.0f, &Row, &View);
	RenderSmoothPill(Row, ColorRGBA(0.8f, 0.8f, 0.8f, 0.28f));
	CUIRect YesArea = Row;
	YesArea.w *= 0.58f;
	RenderSmoothPill(YesArea, ColorRGBA(0.22f, 0.92f, 0.32f, 0.70f));
	CUIRect NoArea = Row;
	NoArea.x += Row.w * 0.74f;
	NoArea.w *= 0.26f;
	RenderSmoothPill(NoArea, ColorRGBA(0.94f, 0.22f, 0.22f, 0.66f));

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(6.0f, &Row, &View);
	Row.VSplitMid(&LeftColumn, &RightColumn, 4.0f);
	Ui()->DoLabel(&LeftColumn, "F3 - Yes", 5.0f, TEXTALIGN_ML);
	Ui()->DoLabel(&RightColumn, "No - F4", 5.0f, TEXTALIGN_MR);
}

void CMediaIsland::RenderHudEditorOutline(const CUIRect &Rect, float Radius, bool Active, float PixelHeight)
{
	CUIRect Outline = Rect;
	Outline.Margin(-2.0f * PixelHeight, &Outline);
	SSdfRoundedRectStyle OutlineStyle;
	OutlineStyle.m_Fill = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
	OutlineStyle.m_Stroke = ColorRGBA(1.0f, 1.0f, 1.0f, Active ? 0.95f : 0.78f);
	OutlineStyle.m_Glow = ColorRGBA(1.0f, 1.0f, 1.0f, Active ? 0.22f : 0.14f);
	OutlineStyle.m_Radius = Radius + 2.0f * PixelHeight;
	OutlineStyle.m_StrokeWidth = 1.25f * PixelHeight;
	OutlineStyle.m_GlowSize = 2.5f * PixelHeight;
	OutlineStyle.m_Softness = 0.3f;
	OutlineStyle.m_Progress = 1.0f;
	RenderSdfRoundedRect(Outline, OutlineStyle);
}

bool CMediaIsland::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return false;
	if(!g_Config.m_TcMediaIsland && !g_Config.m_TcHudEditor)
		return false;
	if(g_Config.m_TcHudEditor && m_IslandInteractActive)
		CloseInteraction();

	const float ScreenHeight = 300.0f;
	const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
	Ui()->ConvertMouseMove(&x, &y, CursorType);

	if(m_IslandInteractActive && !g_Config.m_TcHudEditor)
	{
		if(!m_IslandInteractMouseInitialized)
		{
			m_IslandInteractMouse = m_HasLastIslandRect ? m_LastIslandRect.Center() : vec2(ScreenWidth * 0.5f, ScreenHeight * 0.08f);
			m_IslandInteractMouseInitialized = true;
		}
		m_IslandInteractMouse.x = ClampFloat(m_IslandInteractMouse.x + x, 0.0f, ScreenWidth);
		m_IslandInteractMouse.y = ClampFloat(m_IslandInteractMouse.y + y, 0.0f, ScreenHeight);
		if(m_IslandVolumeDragging && m_IslandVolumeOpen)
		{
			const float TrackWidth = maximum(1.0f, m_IslandVolumeTrackRight - m_IslandVolumeTrackLeft);
			const float Ratio = ClampFloat((m_IslandInteractMouse.x - m_IslandVolumeTrackLeft) / TrackWidth, 0.0f, 1.0f);
#if defined(CONF_FAMILY_WINDOWS)
			m_MediaIsland.m_SourceVolume.store(Ratio);
			m_MediaIsland.m_RequestedSourceVolume.store(Ratio);
#endif
		}
		return true;
	}

	if(!g_Config.m_TcHudEditor)
		return false;

	if(!m_HudEditorMouseInitialized)
	{
		m_HudEditorMouse = m_HasLastIslandRect ? m_LastIslandRect.Center() : vec2(ScreenWidth * 0.5f, ScreenHeight * 0.08f);
		m_HudEditorMouseInitialized = true;
	}

	m_HudEditorMouse.x = ClampFloat(m_HudEditorMouse.x + x, 0.0f, ScreenWidth);
	m_HudEditorMouse.y = ClampFloat(m_HudEditorMouse.y + y, 0.0f, ScreenHeight);

	if(m_HudEditorDragTarget != HUD_EDITOR_TARGET_NONE)
	{
		const bool EditingIsland = m_HudEditorDragTarget == HUD_EDITOR_TARGET_ISLAND && m_HasLastIslandRect;
		const bool EditingVote = m_HudEditorDragTarget == HUD_EDITOR_TARGET_VOTE && m_HasLastVoteRect;
		const CUIRect TargetRect = EditingIsland ? m_LastIslandRect : m_LastVoteRect;
		if(!EditingIsland && !EditingVote)
			return true;

		const float NewLeft = m_HudEditorMouse.x - m_HudEditorDragOffset.x;
		const float NewTop = m_HudEditorMouse.y - m_HudEditorDragOffset.y;
		float NewCenterX = NewLeft + TargetRect.w * 0.5f;
		float SnappedTop = NewTop;
		const float SnapDistance = 10.0f;
		if(std::abs(NewCenterX - ScreenWidth * 0.5f) < SnapDistance)
			NewCenterX = ScreenWidth * 0.5f;
		if(std::abs((NewTop + TargetRect.h * 0.5f) - ScreenHeight * 0.5f) < SnapDistance)
			SnappedTop = ScreenHeight * 0.5f - TargetRect.h * 0.5f;

		if(EditingIsland)
		{
			g_Config.m_TcHudIslandX = std::clamp(round_to_int((NewCenterX / ScreenWidth) * 1000.0f), 0, 1000);
			g_Config.m_TcHudIslandY = std::clamp(round_to_int((SnappedTop / ScreenHeight) * 1000.0f), 0, 1000);
		}
		else
		{
			g_Config.m_TcHudVoteX = std::clamp(round_to_int((NewLeft / ScreenWidth) * 1000.0f), 0, 1000);
			g_Config.m_TcHudVoteY = std::clamp(round_to_int((SnappedTop / ScreenHeight) * 1000.0f), 0, 1000);
		}
	}
	return true;
}

bool CMediaIsland::OnInput(const IInput::CEvent &Event)
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return false;
	if(!g_Config.m_TcMediaIsland && !g_Config.m_TcHudEditor)
		return false;
	if(g_Config.m_TcHudEditor && m_IslandInteractActive)
		CloseInteraction();

	if(Event.m_Flags & IInput::FLAG_PRESS)
	{
		if(m_TeamInviteActive && (Event.m_Key == KEY_F3 || Event.m_Key == KEY_F4))
		{
			ResolveTeamInvite(Event.m_Key == KEY_F3);
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_3 && g_Config.m_TcMediaIsland && !g_Config.m_TcHudEditor && (m_HasLastIslandRect || m_TeamInviteActive))
		{
			if(!m_IslandInteractActive)
			{
				const float ScreenHeight = 300.0f;
				const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
				m_IslandInteractActive = true;
				const vec2 StartPosition = m_HasLastIslandRect ? m_LastIslandRect.Center() : vec2(ScreenWidth * 0.5f, ScreenHeight * 0.08f);
				m_IslandInteractMouse = vec2(ClampFloat(StartPosition.x, 0.0f, ScreenWidth), ClampFloat(StartPosition.y, 0.0f, ScreenHeight));
				m_IslandInteractMouseInitialized = true;
			}
			else
				CloseInteraction();
			return true;
		}
		if(m_IslandInteractActive && !g_Config.m_TcHudEditor && Event.m_Key == KEY_ESCAPE)
		{
			CloseInteraction();
			return true;
		}
		if(m_IslandInteractActive && !g_Config.m_TcHudEditor && Event.m_Key == KEY_MOUSE_1)
		{
			if(m_TeamInviteActive)
			{
				if(m_HasInviteButtons && m_LastInviteAcceptButton.Inside(m_IslandInteractMouse))
				{
					ResolveTeamInvite(true);
					return true;
				}
				if(m_HasInviteButtons && m_LastInviteIgnoreButton.Inside(m_IslandInteractMouse))
				{
					ResolveTeamInvite(false);
					return true;
				}
				// While an invite is showing, swallow other clicks so the island
				// doesn't expand into media controls underneath the prompt.
				return true;
			}
			if(m_IslandExpanded && m_HasLastIslandButtons)
			{
				if(m_IslandVolumeOpen && m_LastIslandVolumeSlider.Inside(m_IslandInteractMouse))
				{
					m_IslandVolumeDragging = true;
					const float TrackWidth = maximum(1.0f, m_IslandVolumeTrackRight - m_IslandVolumeTrackLeft);
					const float Ratio = ClampFloat((m_IslandInteractMouse.x - m_IslandVolumeTrackLeft) / TrackWidth, 0.0f, 1.0f);
#if defined(CONF_FAMILY_WINDOWS)
					m_MediaIsland.m_SourceVolume.store(Ratio);
					m_MediaIsland.m_RequestedSourceVolume.store(Ratio);
#endif
					return true;
				}
				for(size_t i = 0; i < m_aLastIslandButtons.size(); ++i)
				{
					if(m_aLastIslandButtons[i].Inside(m_IslandInteractMouse))
					{
						if(i == 0)
						{
							m_IslandExpanded = false;
							m_IslandVolumeOpen = false;
							m_IslandVolumeDragging = false;
						}
						else if(i == 1)
						{
#if defined(CONF_FAMILY_WINDOWS)
							m_MediaIsland.m_Command.store(MEDIA_ISLAND_COMMAND_PREVIOUS);
#endif
							m_IslandVolumeOpen = false;
						}
						else if(i == 2)
						{
#if defined(CONF_FAMILY_WINDOWS)
							m_MediaIsland.m_Command.store(MEDIA_ISLAND_COMMAND_PLAY_PAUSE);
#endif
							m_IslandVolumeOpen = false;
						}
						else if(i == 3)
						{
#if defined(CONF_FAMILY_WINDOWS)
							m_MediaIsland.m_Command.store(MEDIA_ISLAND_COMMAND_NEXT);
#endif
							m_IslandVolumeOpen = false;
						}
						else if(i == 4)
						{
							m_IslandVolumeOpen = !m_IslandVolumeOpen;
							m_IslandVolumeDragging = false;
						}
						return true;
					}
				}
			}
			if(m_HasLastIslandRect && m_LastIslandRect.Inside(m_IslandInteractMouse))
			{
				m_IslandExpanded = true;
			}
			else
			{
				m_IslandExpanded = false;
				m_IslandVolumeOpen = false;
				m_IslandVolumeDragging = false;
			}
			return true;
		}
		if(!g_Config.m_TcHudEditor)
			return false;

		if(Event.m_Key == KEY_ESCAPE)
		{
			g_Config.m_TcHudEditor = 0;
			m_HudEditorDragTarget = HUD_EDITOR_TARGET_NONE;
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_1)
		{
			const bool InsideIsland = m_HasLastIslandRect && m_LastIslandRect.Inside(m_HudEditorMouse);
			const bool InsideVote = m_HasLastVoteRect && m_LastVoteRect.Inside(m_HudEditorMouse);
			const int ClickTarget = InsideIsland ? HUD_EDITOR_TARGET_ISLAND : (InsideVote ? HUD_EDITOR_TARGET_VOTE : HUD_EDITOR_TARGET_NONE);
			if(ClickTarget != HUD_EDITOR_TARGET_NONE)
			{
				const int64_t Now = time_get();
				if(m_HudEditorLastClickTarget == ClickTarget && m_HudEditorLastClickTime != 0 && Now - m_HudEditorLastClickTime <= time_freq() / 3)
				{
					if(ClickTarget == HUD_EDITOR_TARGET_ISLAND)
					{
						g_Config.m_TcHudIslandX = 500;
						g_Config.m_TcHudIslandY = 33;
					}
					else
					{
						g_Config.m_TcHudVoteX = 0;
						g_Config.m_TcHudVoteY = 200;
					}
					m_HudEditorDragTarget = HUD_EDITOR_TARGET_NONE;
					m_HudEditorLastClickTime = 0;
					m_HudEditorLastClickTarget = HUD_EDITOR_TARGET_NONE;
					return true;
				}
				m_HudEditorLastClickTime = Now;
				m_HudEditorLastClickTarget = ClickTarget;
			}
			else
			{
				m_HudEditorLastClickTime = 0;
				m_HudEditorLastClickTarget = HUD_EDITOR_TARGET_NONE;
			}

			m_HudEditorDragTarget = ClickTarget;
			if(m_HudEditorDragTarget != HUD_EDITOR_TARGET_NONE)
			{
				const CUIRect TargetRect = m_HudEditorDragTarget == HUD_EDITOR_TARGET_ISLAND ? m_LastIslandRect : m_LastVoteRect;
				m_HudEditorDragOffset = m_HudEditorMouse - TargetRect.TopLeft();
			}
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_WHEEL_UP)
		{
			if(m_HudEditorDragTarget == HUD_EDITOR_TARGET_ISLAND || (m_HasLastIslandRect && m_LastIslandRect.Inside(m_HudEditorMouse)))
				g_Config.m_TcHudIslandScale = std::clamp(g_Config.m_TcHudIslandScale + 2, 50, 160);
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_WHEEL_DOWN)
		{
			if(m_HudEditorDragTarget == HUD_EDITOR_TARGET_ISLAND || (m_HasLastIslandRect && m_LastIslandRect.Inside(m_HudEditorMouse)))
				g_Config.m_TcHudIslandScale = std::clamp(g_Config.m_TcHudIslandScale - 2, 50, 160);
			return true;
		}
	}
	if((Event.m_Flags & IInput::FLAG_RELEASE) && Event.m_Key == KEY_MOUSE_1)
	{
		m_IslandVolumeDragging = false;
		m_HudEditorDragTarget = HUD_EDITOR_TARGET_NONE;
		// Always propagate releases. The corresponding press may have happened
		// before the island/editor took focus, and swallowing this event could
		// leave firing or movement bound in its pressed state.
		return false;
	}
	if(Event.m_Flags & IInput::FLAG_RELEASE)
		return false;
	// The virtual cursor only owns its explicit button presses and shortcuts.
	return g_Config.m_TcHudEditor;
}

void CMediaIsland::OnRender()
{
	if(!g_Config.m_TcMediaIsland && !g_Config.m_TcHudEditor)
	{
		m_ActivationProgress = 0.0f;
		m_MediaRevealProgress = 0.0f;
		m_LastVisualizerUpdate = 0;
		ResetTransientState();
		return;
	}
	if(Client()->State() != IClient::STATE_ONLINE)
	{
		m_TeamInviteActive = false;
		m_TeamInvitePending = -1;
		m_TeamInviteProgress = 0.0f;
		m_aTeamInviteInviter[0] = '\0';
		CloseInteraction();
	}

	bool Active = false;
	bool Playing = false;
	ColorRGBA AccentColor(0.36f, 0.36f, 0.40f, 1.0f);
	ColorRGBA DarkColor(0.12f, 0.12f, 0.14f, 1.0f);
	ColorRGBA LightColor(0.44f, 0.44f, 0.50f, 1.0f);
	std::string Title = "No media";
	std::string Artist;
	float AudioPeak = 0.0f;
	int64_t MediaPositionMs = 0;
	int64_t MediaDurationMs = 0;
	std::array<float, 6> aAudioBands{};
	std::vector<uint8_t> vThumbnailRgba;
	uint32_t ThumbnailWidth = 0;
	uint32_t ThumbnailHeight = 0;
	bool ThumbnailDirty = false;
	float SourceVolume = 1.0f;
	bool SourceVolumeAvailable = false;

#if defined(CONF_FAMILY_WINDOWS)
	SourceVolume = m_MediaIsland.m_SourceVolume.load();
	SourceVolumeAvailable = m_MediaIsland.m_SourceVolumeAvailable.load();
	{
		std::lock_guard<std::mutex> Lock(m_MediaIsland.m_Mutex);
		Active = m_MediaIsland.m_Active;
		Playing = m_MediaIsland.m_Playing;
		AccentColor = m_MediaIsland.m_AccentColor;
		DarkColor = m_MediaIsland.m_DarkColor;
		LightColor = m_MediaIsland.m_LightColor;
		Title = m_MediaIsland.m_Title;
		Artist = m_MediaIsland.m_Artist;
		AudioPeak = m_MediaIsland.m_AudioPeak;
		MediaPositionMs = m_MediaIsland.m_PositionMs;
		MediaDurationMs = m_MediaIsland.m_DurationMs;
		aAudioBands = m_MediaIsland.m_aAudioBands;
		if(m_MediaIsland.m_ThumbnailDirty)
		{
			vThumbnailRgba = m_MediaIsland.m_vThumbnailRgba;
			ThumbnailWidth = m_MediaIsland.m_ThumbnailWidth;
			ThumbnailHeight = m_MediaIsland.m_ThumbnailHeight;
			m_MediaIsland.m_ThumbnailDirty = false;
			ThumbnailDirty = true;
		}
	}
#endif
	Title = NormalizeSingleLineText(Title);
	Artist = NormalizeSingleLineText(Artist);
	if(Title.empty())
		Title = Active ? "Unknown track" : "No media";

	if(ThumbnailDirty)
	{
		if(m_HasPreviousThumbnailTexture)
		{
			Graphics()->UnloadTexture(&m_PreviousThumbnailTexture);
			m_HasPreviousThumbnailTexture = false;
		}
		if(m_HasThumbnailTexture)
		{
			m_PreviousThumbnailTexture = m_ThumbnailTexture;
			m_HasPreviousThumbnailTexture = true;
			m_ThumbnailTexture = IGraphics::CTextureHandle();
			m_HasThumbnailTexture = false;
		}
		m_ThumbnailTransitionProgress = 0.0f;

		const size_t ExpectedDataSize = (size_t)ThumbnailWidth * ThumbnailHeight * 4;
		if(ThumbnailWidth > 0 && ThumbnailHeight > 0 && vThumbnailRgba.size() == ExpectedDataSize)
		{
			CImageInfo Image;
			Image.m_Width = ThumbnailWidth;
			Image.m_Height = ThumbnailHeight;
			Image.m_Format = CImageInfo::FORMAT_RGBA;
			Image.m_pData = static_cast<uint8_t *>(malloc(vThumbnailRgba.size()));
			if(Image.m_pData != nullptr)
			{
				mem_copy(Image.m_pData, vThumbnailRgba.data(), vThumbnailRgba.size());
				// Bake a strong, clearly visible radius into the texture itself. This
				// keeps the cover rounded in both compact and expanded layouts.
				const float Radius = minimum(ThumbnailWidth, ThumbnailHeight) * 0.25f;
				for(uint32_t y = 0; y < ThumbnailHeight; ++y)
				{
					for(uint32_t x = 0; x < ThumbnailWidth; ++x)
					{
						const vec2 PixelCenter(x + 0.5f, y + 0.5f);
						const vec2 CornerCenter(
							ClampFloat(PixelCenter.x, Radius, ThumbnailWidth - Radius),
							ClampFloat(PixelCenter.y, Radius, ThumbnailHeight - Radius));
						const float Dist = distance(PixelCenter, CornerCenter);
						if(Dist > Radius - 0.5f)
						{
							const float Alpha = ClampFloat(Radius + 0.5f - Dist, 0.0f, 1.0f);
							uint8_t *pAlpha = &Image.m_pData[(y * ThumbnailWidth + x) * 4 + 3];
							*pAlpha = (uint8_t)(*pAlpha * Alpha);
						}
					}
				}
				m_ThumbnailTexture = Graphics()->LoadTextureRawMove(Image, 0, "media-island-thumbnail");
				m_HasThumbnailTexture = m_ThumbnailTexture.IsValid();
			}
		}
	}

	const float ScreenHeight = 300.0f;
	const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, ScreenWidth, ScreenHeight);
	const float PixelWidth = ScreenWidth / maximum(1, Graphics()->ScreenWidth());
	const float PixelHeight = ScreenHeight / maximum(1, Graphics()->ScreenHeight());

	const int64_t Now = time_get();
	float DeltaTime = 1.0f / 60.0f;
	if(m_LastVisualizerUpdate != 0)
		DeltaTime = ClampFloat((Now - m_LastVisualizerUpdate) / (float)time_freq(), 0.0001f, 0.20f);
	m_LastVisualizerUpdate = Now;

	// Auto-dismiss the invite once its time runs out. Its payload remains cached
	// until the exit animation is finished, so no "team -1" frame can appear.
	if(m_TeamInviteActive && (Now - m_TeamInviteStartTime) >= (int64_t)(TEAM_INVITE_DURATION_SECONDS * time_freq()))
		ResolveTeamInvite(false);

	const bool HasRaceTimer = GameClient()->m_Snap.m_pGameInfoObj && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_SUDDENDEATH);
	const bool HasMediaSession = Active && Client()->State() == IClient::STATE_ONLINE;
	const bool EditorActive = g_Config.m_TcHudEditor && Client()->State() == IClient::STATE_ONLINE;
	if(EditorActive)
		CloseInteraction();
	const bool InviteVisible = m_TeamInviteActive || m_TeamInviteProgress > 0.01f;
	const bool HasPlayableHud = Client()->State() == IClient::STATE_ONLINE && GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active;
	const bool HasWidget = (HasPlayableHud && HasRaceTimer) || HasMediaSession || EditorActive || InviteVisible;
	if(!HasWidget)
		CloseInteraction();

	const float AnimSpeedScale = ClampFloat(g_Config.m_TcMediaIslandAnimSpeed / 100.0f, 0.0f, 3.0f);
	auto AnimateValue = [&](float Current, float Target, float Speed) {
		if(g_Config.m_TcMediaIslandAnimSpeed <= 0 || Speed <= 0.0f)
			return Target;
		return Current + (Target - Current) * (1.0f - std::exp(-Speed * AnimSpeedScale * DeltaTime));
	};
	auto AnimateColor = [&](ColorRGBA Current, ColorRGBA Target, float Speed) {
		return ColorRGBA(
			AnimateValue(Current.r, Target.r, Speed),
			AnimateValue(Current.g, Target.g, Speed),
			AnimateValue(Current.b, Target.b, Speed),
			AnimateValue(Current.a, Target.a, Speed));
	};

	m_AnimatedAccentColor = AnimateColor(m_AnimatedAccentColor, AccentColor, 7.0f);
	m_AnimatedDarkColor = AnimateColor(m_AnimatedDarkColor, DarkColor, 7.0f);
	m_AnimatedLightColor = AnimateColor(m_AnimatedLightColor, LightColor, 7.0f);
	AccentColor = m_AnimatedAccentColor;
	DarkColor = m_AnimatedDarkColor;
	LightColor = m_AnimatedLightColor;
	m_ThumbnailTransitionProgress = AnimateValue(m_ThumbnailTransitionProgress, 1.0f, 9.0f);
	if(m_HasPreviousThumbnailTexture && m_ThumbnailTransitionProgress > 0.995f)
	{
		Graphics()->UnloadTexture(&m_PreviousThumbnailTexture);
		m_HasPreviousThumbnailTexture = false;
		m_ThumbnailTransitionProgress = 1.0f;
	}

	if(!m_MetadataInitialized)
	{
		m_DisplayedTitle = Title;
		m_DisplayedArtist = Artist;
		m_MetadataInitialized = true;
	}
	else if(Title != m_DisplayedTitle || Artist != m_DisplayedArtist)
	{
		m_PreviousTitle = m_DisplayedTitle;
		m_PreviousArtist = m_DisplayedArtist;
		m_DisplayedTitle = Title;
		m_DisplayedArtist = Artist;
		m_MetadataTransitionProgress = 0.0f;
	}
	m_MetadataTransitionProgress = AnimateValue(m_MetadataTransitionProgress, 1.0f, 10.0f);
	if(m_MetadataTransitionProgress > 0.995f)
	{
		m_MetadataTransitionProgress = 1.0f;
		m_PreviousTitle.clear();
		m_PreviousArtist.clear();
	}

	const float TargetProgress = HasWidget ? 1.0f : 0.0f;
	const float ProgressSpeed = HasWidget ? 16.0f : 10.0f;
	m_ActivationProgress = AnimateValue(m_ActivationProgress, TargetProgress, ProgressSpeed);
	const float Progress = ClampFloat(m_ActivationProgress, 0.0f, 1.0f);
	if(Progress <= 0.01f)
	{
		ClearInteractionGeometry();
		return;
	}

	if(EditorActive)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.42f);
		IGraphics::CQuadItem Shade(0.0f, 0.0f, ScreenWidth, ScreenHeight);
		Graphics()->QuadsDrawTL(&Shade, 1);
		Graphics()->QuadsEnd();
	}

	char aTimeLabel[16];
	int RaceTime = 0;
	if(HasRaceTimer)
	{
		if(GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit && GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer <= 0)
		{
			RaceTime = GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit * 60 - ((Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / Client()->GameTickSpeed());
			if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER)
				RaceTime = 0;
		}
		else if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME)
		{
			RaceTime = (Client()->GameTick(g_Config.m_ClDummy) + GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer) / Client()->GameTickSpeed();
		}
		else
			RaceTime = (Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / Client()->GameTickSpeed();
	}
	RaceTime = maximum(0, RaceTime);
	if(HasRaceTimer)
		str_time((int64_t)RaceTime * 100, ETimeFormat::DAYS, aTimeLabel, sizeof(aTimeLabel));
	else if(HasMediaSession)
		FormatMediaTime(MediaPositionMs, false, aTimeLabel, sizeof(aTimeLabel));
	else
		str_copy(aTimeLabel, "0:00");
	// A paused session must stay available so its play button can be used even if
	// the source does not expose artwork or loopback audio.
	const bool HasMediaVisual = HasMediaSession;
	if(!HasMediaVisual)
	{
		m_IslandExpanded = false;
		m_IslandVolumeOpen = false;
		m_IslandVolumeDragging = false;
	}
	const float TargetMediaReveal = HasMediaVisual ? 1.0f : 0.0f;
	m_MediaRevealProgress = AnimateValue(m_MediaRevealProgress, TargetMediaReveal, HasMediaVisual ? 10.0f : 7.5f);
	const float MediaReveal = SmoothProgress(m_MediaRevealProgress);

	// Reveal animation for the team-invite prompt.
	const bool InviteActive = m_TeamInviteActive;
	m_TeamInviteProgress = AnimateValue(m_TeamInviteProgress, InviteActive ? 1.0f : 0.0f, InviteActive ? 12.0f : 14.0f);
	const float InviteProgress = SmoothProgress(m_TeamInviteProgress);
	float InviteRemaining = 0.0f;
	if(InviteActive)
		InviteRemaining = ClampFloat(1.0f - (Now - m_TeamInviteStartTime) / (TEAM_INVITE_DURATION_SECONDS * (float)time_freq()), 0.0f, 1.0f);
	else if(m_TeamInviteProgress < 0.005f && m_TeamInvitePending >= 0)
	{
		m_TeamInvitePending = -1;
		m_aTeamInviteInviter[0] = '\0';
	}

	const float WidgetScale = ClampFloat(g_Config.m_TcHudIslandScale / 100.0f, 0.50f, 1.60f) * 1.06f;
	const float CompactHeightPx = mix(25.0f, 33.0f, MediaReveal) * WidgetScale;
	const float CompactHeight = CompactHeightPx * PixelHeight;
	const float CoverSize = CompactHeight - 7.0f * PixelHeight * WidgetScale;
	const float PaddingX = 6.0f * PixelWidth * WidgetScale;
	const float Gap = 6.0f * PixelWidth * WidgetScale;
	const float VisualizerWidth = 30.0f * PixelWidth * WidgetScale;
	const float TimeFont = maximum(10.5f, 19.5f * WidgetScale) * PixelHeight;
	const float TimeTextWidth = TextRender()->TextWidth(TimeFont, aTimeLabel, -1, -1.0f);
	const bool LongRaceTime = RaceTime >= 60 * 60 || str_find(aTimeLabel, "d") != nullptr;
	const float TextBlockWidth = TimeTextWidth + (LongRaceTime ? 10.0f : 5.0f) * PixelWidth * WidgetScale;
	const float CompactMediaExtraWidth = 30.0f * PixelWidth * WidgetScale;
	const float MinIslandWidth = 146.0f * PixelWidth * WidgetScale;
	const float RequiredIslandWidth = PaddingX * 2.0f + CoverSize + Gap + TextBlockWidth + Gap + VisualizerWidth + CompactMediaExtraWidth;
	const float MaxIslandWidth = minimum(ScreenWidth * 0.60f, 360.0f * PixelWidth * WidgetScale);
	const float MediaIslandWidth = ClampFloat(RequiredIslandWidth, MinIslandWidth, MaxIslandWidth);
	const float TimerOnlyWidth = maximum(58.0f * PixelWidth * WidgetScale, TimeTextWidth + 16.0f * PixelWidth * WidgetScale);
	const float CompactIslandWidth = mix(TimerOnlyWidth, MediaIslandWidth, MediaReveal);
	const float RequestedCenterX = ScreenWidth * ClampFloat(g_Config.m_TcHudIslandX / 1000.0f, 0.0f, 1.0f);
	const float RequestedTopY = ScreenHeight * ClampFloat(g_Config.m_TcHudIslandY / 1000.0f, 0.0f, 1.0f) - (1.0f - SmoothProgress(Progress)) * 8.0f * PixelHeight;
	const float TargetExpand = m_IslandExpanded ? 1.0f : 0.0f;
	m_IslandExpandProgress = AnimateValue(m_IslandExpandProgress, TargetExpand, m_IslandExpanded ? 12.0f : 14.0f);
	const float ExpandProgress = SmoothProgress(m_IslandExpandProgress);
	const float TargetVolume = (m_IslandExpanded && m_IslandVolumeOpen) ? 1.0f : 0.0f;
	m_IslandVolumeProgress = AnimateValue(m_IslandVolumeProgress, TargetVolume, m_IslandVolumeOpen ? 13.0f : 16.0f);
	const float VolumeProgress = SmoothProgress(m_IslandVolumeProgress);

	CUIRect HoverHitRect = m_LastIslandRect;
	if(m_IslandHoverProgress > 0.05f)
		HoverHitRect.Margin(-2.0f * PixelHeight * WidgetScale, &HoverHitRect);
	const bool IslandHovered = m_IslandInteractActive && m_HasLastIslandRect && HoverHitRect.Inside(m_IslandInteractMouse);
	const float HoverTarget = IslandHovered && !m_IslandExpanded && !InviteActive ? 1.0f : 0.0f;
	m_IslandHoverProgress = AnimateValue(m_IslandHoverProgress, HoverTarget, IslandHovered ? 14.0f : 10.0f);
	const float HoverSize = SmoothProgress(m_IslandHoverProgress) * 0.032f * (1.0f - ExpandProgress) * (1.0f - InviteProgress);

	const float ExpandedWidth = maximum(CompactIslandWidth + 164.0f * PixelWidth * WidgetScale, 316.0f * PixelWidth * WidgetScale);
	const float ExpandedHeight = (158.0f + 40.0f * VolumeProgress) * PixelHeight * WidgetScale;
	const float InviteWidth = maximum(CompactIslandWidth, 158.0f * PixelWidth * WidgetScale);
	const float InviteHeight = 67.0f * PixelHeight * WidgetScale;
	const float ScreenMarginX = 3.0f * PixelWidth;
	const float ScreenMarginY = 3.0f * PixelHeight;
	const bool InviteGeometry = InviteActive || m_TeamInviteProgress > 0.25f;
	float TargetIslandWidth = CompactIslandWidth * (1.0f + HoverSize);
	float TargetIslandHeight = CompactHeight * (1.0f + HoverSize);
	float TargetIslandRadius = TargetIslandHeight * 0.5f;
	if(m_IslandExpanded)
	{
		TargetIslandWidth = ExpandedWidth;
		TargetIslandHeight = ExpandedHeight;
		TargetIslandRadius = 18.0f * PixelHeight * WidgetScale;
	}
	if(InviteGeometry)
	{
		TargetIslandWidth = InviteWidth;
		TargetIslandHeight = InviteHeight;
		TargetIslandRadius = 8.0f * PixelHeight * WidgetScale;
	}
	TargetIslandWidth = minimum(TargetIslandWidth, maximum(1.0f, ScreenWidth - ScreenMarginX * 2.0f));
	TargetIslandHeight = minimum(TargetIslandHeight, maximum(1.0f, ScreenHeight - ScreenMarginY * 2.0f));
	TargetIslandRadius = minimum(TargetIslandRadius, minimum(TargetIslandWidth, TargetIslandHeight) * 0.5f);
	if(!m_IslandGeometryInitialized)
	{
		m_AnimatedIslandWidth = TargetIslandWidth;
		m_AnimatedIslandHeight = TargetIslandHeight;
		m_AnimatedIslandRadius = TargetIslandRadius;
		m_IslandGeometryInitialized = true;
	}
	else
	{
		m_AnimatedIslandWidth = AnimateValue(m_AnimatedIslandWidth, TargetIslandWidth, 11.5f);
		m_AnimatedIslandHeight = AnimateValue(m_AnimatedIslandHeight, TargetIslandHeight, 11.5f);
		m_AnimatedIslandRadius = AnimateValue(m_AnimatedIslandRadius, TargetIslandRadius, 12.5f);
	}
	const float IslandWidth = minimum(m_AnimatedIslandWidth, maximum(1.0f, ScreenWidth - ScreenMarginX * 2.0f));
	const float IslandHeight = minimum(m_AnimatedIslandHeight, maximum(1.0f, ScreenHeight - ScreenMarginY * 2.0f));
	const float IslandX = ClampFloat(RequestedCenterX - IslandWidth * 0.5f, ScreenMarginX, maximum(ScreenMarginX, ScreenWidth - IslandWidth - ScreenMarginX));
	const float IslandY = ClampFloat(RequestedTopY, ScreenMarginY, maximum(ScreenMarginY, ScreenHeight - IslandHeight - ScreenMarginY));
	CUIRect Island = {IslandX, IslandY, IslandWidth, IslandHeight};
	CUIRect CompactIsland = {Island.x + (Island.w - CompactIslandWidth) * 0.5f, Island.y, CompactIslandWidth, CompactHeight};
	m_LastIslandRect = Island;
	m_HasLastIslandRect = true;
	m_HasLastIslandButtons = false;
	m_LastIslandVolumeSlider = {};
	m_IslandVolumeTrackLeft = 0.0f;
	m_IslandVolumeTrackRight = 0.0f;
	if(EditorActive && !m_HudEditorMouseInitialized)
	{
		m_HudEditorMouse = Island.Center();
		m_HudEditorMouseInitialized = true;
	}
	else if(!EditorActive)
	{
		m_HudEditorMouseInitialized = false;
		m_HudEditorDragTarget = HUD_EDITOR_TARGET_NONE;
	}
	if(m_IslandInteractActive && !m_IslandInteractMouseInitialized)
	{
		m_IslandInteractMouse = Island.Center();
		m_IslandInteractMouseInitialized = true;
	}
	const float CompactProgress = Progress * (1.0f - ExpandProgress) * (1.0f - InviteProgress);
	const float CompactMediaProgress = CompactProgress * MediaReveal;

	const float IslandRadius = ClampFloat(m_AnimatedIslandRadius, 0.0f, minimum(IslandWidth, IslandHeight) * 0.5f);
	const float ShadowGrow = 2.2f * PixelHeight * WidgetScale;
	CUIRect ShadowOuter = Island;
	ShadowOuter.x -= ShadowGrow;
	ShadowOuter.y += 1.2f * PixelHeight * WidgetScale;
	ShadowOuter.w += ShadowGrow * 2.0f;
	ShadowOuter.h += ShadowGrow * 1.65f;
	Graphics()->TextureClear();
	Graphics()->DrawRect(ShadowOuter.x, ShadowOuter.y, ShadowOuter.w, ShadowOuter.h, ColorRGBA(0.0f, 0.0f, 0.0f, 0.10f * Progress), IGraphics::CORNER_ALL, IslandRadius + ShadowGrow);
	CUIRect ShadowInner = Island;
	ShadowInner.x -= 1.15f * PixelWidth * WidgetScale;
	ShadowInner.y += 0.75f * PixelHeight * WidgetScale;
	ShadowInner.w += 2.3f * PixelWidth * WidgetScale;
	ShadowInner.h += 1.4f * PixelHeight * WidgetScale;
	Graphics()->DrawRect(ShadowInner.x, ShadowInner.y, ShadowInner.w, ShadowInner.h, ColorRGBA(0.0f, 0.0f, 0.0f, 0.23f * Progress), IGraphics::CORNER_ALL, IslandRadius + 1.0f * PixelHeight * WidgetScale);
	Graphics()->DrawRect(Island.x - 0.35f * PixelWidth, Island.y - 0.35f * PixelHeight, Island.w + 0.7f * PixelWidth, Island.h + 0.7f * PixelHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.32f * Progress), IGraphics::CORNER_ALL, IslandRadius + 0.35f * PixelHeight);
	const float IslandBodyAlpha = mix(1.0f, 0.88f, ExpandProgress) * Progress;
	Graphics()->DrawRect(Island.x, Island.y, Island.w, Island.h, ColorRGBA(0.0f, 0.0f, 0.0f, IslandBodyAlpha), IGraphics::CORNER_ALL, IslandRadius);

	// CUi uses a 600-unit-high coordinate system, while the HUD is mapped to
	// 300 units. Convert the island before enabling the UI clip; passing the HUD
	// rect directly would create a half-sized scissor at the wrong position and
	// hide every piece of media content.
	const CUIRect *pUiScreen = Ui()->Screen();
	const CUIRect IslandClip = {
		Island.x * pUiScreen->w / ScreenWidth,
		Island.y * pUiScreen->h / ScreenHeight,
		Island.w * pUiScreen->w / ScreenWidth,
		Island.h * pUiScreen->h / ScreenHeight};
	Ui()->ClipEnable(&IslandClip);
	auto DrawThumbnailLayer = [&](IGraphics::CTextureHandle Texture, const CUIRect &Rect, float Alpha) {
		if(!Texture.IsValid() || Alpha <= 0.001f)
			return;
		Graphics()->TextureSet(Texture);
		Graphics()->QuadsBegin();
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
		IGraphics::CQuadItem Quad(Rect.x, Rect.y, Rect.w, Rect.h);
		Graphics()->QuadsDrawTL(&Quad, 1);
		Graphics()->QuadsEnd();
		Graphics()->TextureClear();
	};
	const float ThumbnailBlend = SmoothProgress(m_ThumbnailTransitionProgress);
	auto DrawThumbnailCrossfade = [&](const CUIRect &Rect, float Alpha) {
		if(m_HasPreviousThumbnailTexture)
			DrawThumbnailLayer(m_PreviousThumbnailTexture, Rect, Alpha * (1.0f - ThumbnailBlend));
		if(m_HasThumbnailTexture)
			DrawThumbnailLayer(m_ThumbnailTexture, Rect, Alpha * ThumbnailBlend);
	};

	CUIRect Inner;
	CompactIsland.Margin(PaddingX, &Inner);

	CUIRect PreviewBlock, TextBlock, VisualizerBlock;
	PreviewBlock = {Inner.x, CompactIsland.y + (CompactIsland.h - CoverSize) * 0.5f, CoverSize, CoverSize};
	VisualizerBlock = Inner;
	VisualizerBlock.x = Inner.x + Inner.w - VisualizerWidth;
	VisualizerBlock.w = VisualizerWidth;
	VisualizerBlock.y = CompactIsland.y + CompactIsland.h * 0.24f;
	VisualizerBlock.h = CompactIsland.h * 0.52f;
	// Keep the timer on the island's geometric center. The artwork and visualizer
	// occupy the side zones independently and must never push the time sideways.
	TextBlock.x = CompactIsland.x + (CompactIsland.w - TimeTextWidth) * 0.5f;
	TextBlock.y = CompactIsland.y + (CompactIsland.h - TimeFont) * 0.5f - 0.18f * PixelHeight;
	TextBlock.w = TimeTextWidth;
	TextBlock.h = TimeFont;

	Graphics()->TextureClear();
	if(CompactMediaProgress > 0.01f)
		Graphics()->DrawRect(PreviewBlock.x, PreviewBlock.y, PreviewBlock.w, PreviewBlock.h, ColorRGBA(0.018f, 0.020f, 0.022f, 0.86f * CompactMediaProgress), IGraphics::CORNER_ALL, PreviewBlock.h * 0.25f);
	CUIRect PreviewInner;
	PreviewBlock.Margin(1.0f * PixelHeight, &PreviewInner);
	PreviewInner.y += 0.05f;
	const float PreviewPulse = std::sin(LocalTime() * 2.5f) * 0.5f + 0.5f;
	const ColorRGBA PreviewColor(
		AccentColor.r * (0.22f + 0.06f * PreviewPulse),
		AccentColor.g * (0.20f + 0.05f * PreviewPulse),
		AccentColor.b * (0.22f + 0.05f * PreviewPulse),
		CompactMediaProgress);
	RenderSmoothRoundedRect(PreviewInner, PreviewColor, PreviewInner.h * 0.25f);
	DrawThumbnailCrossfade(PreviewInner, CompactMediaProgress);
	if(!m_HasThumbnailTexture && !m_HasPreviousThumbnailTexture)
	{
		const float ScanlineY = PreviewInner.y + PreviewInner.h * 0.66f;
		Graphics()->DrawRect(PreviewInner.x + 0.8f * PixelWidth, ScanlineY, maximum(0.0f, PreviewInner.w - 1.6f * PixelWidth), 0.8f * PixelHeight, ColorRGBA(AccentColor.r, AccentColor.g, AccentColor.b, CompactMediaProgress * 0.82f), IGraphics::CORNER_ALL, 0.4f * PixelHeight);
	}

	const ColorRGBA TimeColor(0.96f, 0.99f, 1.0f, 0.99f * CompactProgress);
	CUIRect WeightedTimeBlock = TextBlock;
	TextBlock.x -= 0.19f * PixelWidth;
	WeightedTimeBlock.x += 0.19f * PixelWidth;
	RenderTextLeft(TextRender(), TextBlock, TimeFont, aTimeLabel, TimeColor);
	RenderTextLeft(TextRender(), WeightedTimeBlock, TimeFont, aTimeLabel, ColorRGBA(TimeColor.r, TimeColor.g, TimeColor.b, TimeColor.a * 0.62f));

	const float SubBass = aAudioBands[0];
	const float Bass = aAudioBands[1];
	const float LowMid = aAudioBands[2];
	const float Mid = aAudioBands[3];
	const float Presence = aAudioBands[4];
	const float Treble = aAudioBands[5];
	const float Overall = std::accumulate(aAudioBands.begin(), aAudioBands.end(), 0.0f) / aAudioBands.size();
	std::array<float, 6> aRawTargets = {
		SubBass,
		Bass,
		LowMid,
		Mid,
		Presence,
		Treble,
	};
	const float AverageTarget = std::accumulate(aRawTargets.begin(), aRawTargets.end(), 0.0f) / aRawTargets.size();
	const float MaxTarget = *std::max_element(aRawTargets.begin(), aRawTargets.end());
	std::array<float, 6> aJumps{};
	for(size_t i = 0; i < aRawTargets.size(); ++i)
		aJumps[i] = maximum(0.0f, aRawTargets[i] - m_aPreviousRawTargets[i]);
	const float AverageJump = std::accumulate(aJumps.begin(), aJumps.end(), 0.0f) / aJumps.size();

	static constexpr std::array<float, 6> BAND_WEIGHTS = {1.10f, 0.90f, 0.98f, 1.04f, 0.94f, 1.16f};
	static constexpr std::array<float, 6> CURVES = {0.58f, 0.66f, 0.60f, 0.54f, 0.62f, 0.44f};
	const float Gain = ClampFloat(1.08f + AudioPeak * 1.12f, 1.08f, 2.2f);
	const float QuietGate = ClampFloat(AudioPeak * 7.0f + Overall * 0.40f, 0.0f, 1.0f);
	for(size_t i = 0; i < m_aVisualizerValues.size(); ++i)
	{
		const float Dominant = ClampFloat((aRawTargets[i] - AverageTarget - 0.014f) * Gain * 3.85f * BAND_WEIGHTS[i], 0.0f, 1.0f);
		const float LocalJump = maximum(0.0f, aJumps[i] - AverageJump * 0.55f);
		const float Transient = ClampFloat(LocalJump * 6.2f * BAND_WEIGHTS[i], 0.0f, 0.28f);
		const float Body = std::pow(ClampFloat((Dominant - 0.018f) / 0.982f, 0.0f, 1.0f), CURVES[i]);
		float Target = ClampFloat((Body * 0.54f + Transient) * QuietGate, 0.0f, 0.66f);
		const bool HasAudibleInput = Overall > 0.018f || AudioPeak > 0.012f;
		if(!HasAudibleInput)
			Target = 0.0f;
		else
			Target = ClampFloat(Target * 1.12f + 0.012f, 0.0f, 1.0f);
		const float Response = Target > m_aVisualizerValues[i] ? 78.0f : 27.0f;
		const float Alpha = 1.0f - std::exp(-Response * DeltaTime);
		m_aVisualizerValues[i] = mix(m_aVisualizerValues[i], Target, Alpha);
		m_aPreviousRawTargets[i] = aRawTargets[i];
	}

	const int BarCount = 4;
	const float BAR_GAP = 2.65f * PixelWidth * WidgetScale;
	const float BAR_WIDTH = 4.45f * PixelWidth * WidgetScale;
	const float IDLE_HEIGHT = 7.4f * PixelHeight * WidgetScale;
	const float MAX_HEIGHT = 17.6f * PixelHeight * WidgetScale;
	const float TotalBarsWidth = BarCount * BAR_WIDTH + (BarCount - 1) * BAR_GAP;
	const float StartX = VisualizerBlock.x + (VisualizerBlock.w - TotalBarsWidth) * 0.5f;
	const float CenterY = VisualizerBlock.y + VisualizerBlock.h * 0.5f;
	static constexpr std::array<float, 4> BAR_SHAPES = {0.92f, 0.78f, 1.00f, 0.86f};
	const std::array<float, 4> aDisplayBars = {
		mix(m_aVisualizerValues[0], m_aVisualizerValues[1], 0.45f),
		m_aVisualizerValues[2],
		mix(m_aVisualizerValues[3], m_aVisualizerValues[4], 0.35f),
		m_aVisualizerValues[5],
	};
	// Bar colors are derived from the preview/album palette (AccentColor is the
	// primary tone, LightColor the brightened highlight) so the visualizer matches
	// the cover art instead of a fixed yellow.
	ColorRGBA BarBottom = AccentColor;
	ColorRGBA BarTop = LightColor;
	for(int i = 0; i < BarCount; ++i)
	{
		const float Interpolated = aDisplayBars[i];
		const float Current = ClampFloat(maximum(Interpolated, HasMediaSession ? 0.012f : 0.0f), 0.0f, 1.0f);
		const float Shape = BAR_SHAPES[i];
		const float Motion = std::pow(Current, mix(1.00f, 0.78f, Shape));
		const float Height = mix(IDLE_HEIGHT, MAX_HEIGHT, Motion);
		const float SideMix = std::abs(i - (BarCount - 1) * 0.5f) / ((BarCount - 1) * 0.5f);
		const float Width = BAR_WIDTH + Motion * mix(0.10f, 0.32f, Shape) * (1.0f - SideMix * 0.18f) * PixelWidth * WidgetScale;
		const float X = StartX + i * (BAR_WIDTH + BAR_GAP);
		const float Y = CenterY - Height * 0.5f;
		const float RowT = BarCount > 1 ? i / (float)(BarCount - 1) : 0.0f;
		const float RowSmooth = RowT * RowT * (3.0f - 2.0f * RowT);
		const float RowShade = mix(1.18f, 1.03f, RowSmooth);
		ColorRGBA BarColor = MixColor(BarBottom, BarTop, 0.36f);
		BarColor.r = ClampFloat(BarColor.r * RowShade, 0.0f, 1.0f);
		BarColor.g = ClampFloat(BarColor.g * RowShade, 0.0f, 1.0f);
		BarColor.b = ClampFloat(BarColor.b * RowShade, 0.0f, 1.0f);
		CUIRect Bar = {X, Y, Width, Height};
		const float BarAlpha = CompactMediaProgress * (HasMediaSession && Playing ? 0.98f : 0.70f);
		const float Glow = CompactMediaProgress * (0.040f + Motion * 0.085f);
		RenderSoftVisualizerBar(Bar, BarColor, BarAlpha, PixelWidth, PixelHeight, Glow * 6.0f);
	}

	if(ExpandProgress > 0.01f)
	{
		const float HeaderReveal = DelayedProgress(ExpandProgress, 0.16f) * (1.0f - InviteProgress);
		const float TimelineReveal = DelayedProgress(ExpandProgress, 0.34f) * (1.0f - InviteProgress);
		const float ControlsReveal = DelayedProgress(ExpandProgress, 0.52f) * (1.0f - InviteProgress);
		const float ExpandedAlpha = Progress * HeaderReveal;
		const float TimelineAlpha = Progress * TimelineReveal;
		const float ControlsAlpha = Progress * ControlsReveal;
		CUIRect Expanded = Island;
		const float ExpandedRadius = IslandRadius;
		Graphics()->TextureClear();
		Graphics()->DrawRect(Expanded.x, Expanded.y, Expanded.w, Expanded.h, ColorRGBA(0.0f, 0.0f, 0.0f, 0.12f * ExpandedAlpha), IGraphics::CORNER_ALL, ExpandedRadius);

		const float CoverLarge = 55.0f * PixelHeight * WidgetScale;
		CUIRect CoverRect = {Expanded.x + 17.0f * PixelWidth * WidgetScale, Expanded.y + 18.0f * PixelHeight * WidgetScale, CoverLarge, CoverLarge};
		Graphics()->TextureClear();
		Graphics()->DrawRect(CoverRect.x, CoverRect.y, CoverRect.w, CoverRect.h, ColorRGBA(0.08f, 0.08f, 0.085f, 0.94f * ExpandedAlpha), IGraphics::CORNER_ALL, 14.0f * PixelHeight * WidgetScale);
		CUIRect CoverInner;
		CoverRect.Margin(1.0f * PixelHeight * WidgetScale, &CoverInner);
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor4(
			ColorRGBA(AccentColor.r * 0.92f, AccentColor.g * 0.86f, AccentColor.b * 0.86f, ExpandedAlpha),
			ColorRGBA(AccentColor.r * 0.55f, AccentColor.g * 0.60f, AccentColor.b * 0.62f, ExpandedAlpha),
			ColorRGBA(DarkColor.r, DarkColor.g, DarkColor.b, ExpandedAlpha),
			ColorRGBA(AccentColor.r * 0.62f, AccentColor.g * 0.42f, AccentColor.b * 0.48f, ExpandedAlpha));
		IGraphics::CQuadItem CoverQuad(CoverInner.x, CoverInner.y, CoverInner.w, CoverInner.h);
		Graphics()->QuadsDrawTL(&CoverQuad, 1);
		Graphics()->QuadsEnd();
		Graphics()->TextureClear();
		DrawThumbnailCrossfade(CoverInner, ExpandedAlpha);

		const float StatusCenterX = Expanded.x + Expanded.w - 76.0f * PixelWidth * WidgetScale;
		const float StatusCenterY = Expanded.y + 36.5f * PixelHeight * WidgetScale;
		const float StatusSize = 24.0f * PixelHeight * WidgetScale;
		const CUIRect StatusRect = {StatusCenterX - StatusSize * 0.5f, StatusCenterY - StatusSize * 0.5f, StatusSize, StatusSize};
		RenderSmoothRoundedRect(StatusRect, ColorRGBA(1.0f, 1.0f, 1.0f, 0.045f * ExpandedAlpha), StatusSize * 0.5f);
		const int StatusIcon = Playing ? MEDIA_ISLAND_ICON_PAUSE : MEDIA_ISLAND_ICON_PLAY;
		if(m_aControlIconLoaded[StatusIcon])
			RenderTextureIconCentered(Graphics(), m_aControlIconTextures[StatusIcon], m_aControlIconUvs[StatusIcon], StatusRect, 12.5f * PixelHeight * WidgetScale, ColorRGBA(0.78f, 0.81f, 0.88f, 0.90f * ExpandedAlpha));
		else
			RenderIconCentered(TextRender(), StatusRect, 11.5f * PixelHeight * WidgetScale, Playing ? FontIcon::PAUSE : FontIcon::PLAY, ColorRGBA(0.78f, 0.81f, 0.88f, 0.90f * ExpandedAlpha));

		CUIRect TextArea = {CoverRect.x + CoverRect.w + 12.0f * PixelWidth * WidgetScale, Expanded.y + 18.0f * PixelHeight * WidgetScale, 0.0f, 38.0f * PixelHeight * WidgetScale};
		TextArea.w = maximum(0.0f, StatusRect.x - 8.0f * PixelWidth * WidgetScale - TextArea.x);
		const float TitleFont = maximum(11.0f, 15.5f * WidgetScale) * PixelHeight;
		const float ArtistFont = maximum(9.0f, 11.5f * WidgetScale) * PixelHeight;
		const CUIRect TitleRect = {TextArea.x, TextArea.y, TextArea.w, TitleFont};
		const CUIRect ArtistRect = {TextArea.x, TextArea.y + TitleFont + 3.0f * PixelHeight * WidgetScale, TextArea.w, ArtistFont};
		const float MetadataBlend = SmoothProgress(m_MetadataTransitionProgress);
		auto RenderMetadata = [&](const std::string &TrackTitle, const std::string &TrackArtist, float Alpha, float OffsetY) {
			if(Alpha <= 0.001f)
				return;
			CUIRect AnimatedTitle = TitleRect;
			CUIRect AnimatedArtist = ArtistRect;
			AnimatedTitle.y += OffsetY;
			AnimatedArtist.y += OffsetY;
			RenderTextLeft(TextRender(), AnimatedTitle, TitleFont, TrackTitle.c_str(), ColorRGBA(0.98f, 0.99f, 1.0f, 0.99f * ExpandedAlpha * Alpha));
			const char *pArtist = TrackArtist.empty() ? "Unknown artist" : TrackArtist.c_str();
			RenderTextLeft(TextRender(), AnimatedArtist, ArtistFont, pArtist, ColorRGBA(0.78f, 0.82f, 0.90f, 0.95f * ExpandedAlpha * Alpha));
		};
		if(!m_PreviousTitle.empty() || !m_PreviousArtist.empty())
			RenderMetadata(m_PreviousTitle, m_PreviousArtist, 1.0f - MetadataBlend, -2.0f * PixelHeight * WidgetScale * MetadataBlend);
		RenderMetadata(m_DisplayedTitle, m_DisplayedArtist, MetadataBlend, 2.0f * PixelHeight * WidgetScale * (1.0f - MetadataBlend));

		const float MiniVizX = Expanded.x + Expanded.w - 42.0f * PixelWidth * WidgetScale;
		const float MiniVizCenterY = StatusCenterY;
		for(int i = 0; i < 4; ++i)
		{
			const float Motion = std::pow(aDisplayBars[i], 0.78f);
			const float H = mix(7.6f, 17.0f, Motion) * PixelHeight * WidgetScale;
			const float W = 4.2f * PixelWidth * WidgetScale;
			const float X = MiniVizX + i * 6.8f * PixelWidth * WidgetScale;
			const float Y = MiniVizCenterY - H * 0.5f;
			ColorRGBA MiniColor = MixColor(BarBottom, BarTop, 0.58f);
			RenderSoftVisualizerBar({X, Y, W, H}, MiniColor, 0.92f * ExpandedAlpha, PixelWidth, PixelHeight, 0.34f + Motion * 0.42f);
		}

		const int64_t SafeDurationMs = maximum<int64_t>(0, MediaDurationMs);
		const int64_t SafePositionMs = SafeDurationMs > 0 ? std::clamp(MediaPositionMs, (int64_t)0, SafeDurationMs) : maximum<int64_t>(0, MediaPositionMs);
		const float TargetTrackRatio = SafeDurationMs > 0 ? ClampFloat(SafePositionMs / (float)SafeDurationMs, 0.0f, 1.0f) : 0.0f;
		if(!m_MediaTrackRatioInitialized || std::abs(TargetTrackRatio - m_AnimatedMediaTrackRatio) > 0.28f)
		{
			m_AnimatedMediaTrackRatio = TargetTrackRatio;
			m_MediaTrackRatioInitialized = true;
		}
		else
			m_AnimatedMediaTrackRatio = AnimateValue(m_AnimatedMediaTrackRatio, TargetTrackRatio, 11.0f);
		const float TrackRatio = ClampFloat(m_AnimatedMediaTrackRatio, 0.0f, 1.0f);
		char aMediaElapsed[32];
		char aMediaRemaining[32];
		FormatMediaTime(SafePositionMs, false, aMediaElapsed, sizeof(aMediaElapsed));
		if(SafeDurationMs > 0)
			FormatMediaTime(SafeDurationMs - SafePositionMs, true, aMediaRemaining, sizeof(aMediaRemaining));
		else
			str_copy(aMediaRemaining, "--:--");

		const float MediaTimeFont = maximum(8.0f, 10.5f * WidgetScale) * PixelHeight;
		const float ElapsedWidth = TextRender()->TextWidth(MediaTimeFont, aMediaElapsed, -1, -1.0f);
		const float RemainingWidth = TextRender()->TextWidth(MediaTimeFont, aMediaRemaining, -1, -1.0f);
		const float TimelineLeft = Expanded.x + 17.0f * PixelWidth * WidgetScale;
		const float TimelineRight = Expanded.x + Expanded.w - 17.0f * PixelWidth * WidgetScale;
		const float LabelGap = 9.0f * PixelWidth * WidgetScale;
		const float TrackLeft = TimelineLeft + ElapsedWidth + LabelGap;
		const float TrackRight = TimelineRight - RemainingWidth - LabelGap;
		CUIRect ProgressTrack = {TrackLeft, Expanded.y + 84.0f * PixelHeight * WidgetScale, maximum(1.0f * PixelWidth, TrackRight - TrackLeft), 3.2f * PixelHeight * WidgetScale};
		RenderSmoothRoundedRect(ProgressTrack, ColorRGBA(0.31f, 0.32f, 0.35f, 0.90f * TimelineAlpha), ProgressTrack.h * 0.5f);
		const float FilledWidth = ProgressTrack.w * TrackRatio;
		if(FilledWidth > 0.01f)
			RenderSmoothRoundedRect({ProgressTrack.x, ProgressTrack.y, FilledWidth, ProgressTrack.h}, ColorRGBA(LightColor.r, LightColor.g, LightColor.b, 0.96f * TimelineAlpha), ProgressTrack.h * 0.5f);
		const float ThumbSize = 5.2f * PixelHeight * WidgetScale;
		const float ThumbX = ProgressTrack.x + ProgressTrack.w * TrackRatio;
		RenderSmoothRoundedRect({ThumbX - ThumbSize * 0.5f, ProgressTrack.y + ProgressTrack.h * 0.5f - ThumbSize * 0.5f, ThumbSize, ThumbSize}, ColorRGBA(0.95f, 0.95f, 0.92f, 0.95f * TimelineAlpha), ThumbSize * 0.5f);
		RenderTextLeft(TextRender(), {TimelineLeft, ProgressTrack.y - (MediaTimeFont - ProgressTrack.h) * 0.5f, ElapsedWidth + 1.0f * PixelWidth, MediaTimeFont}, MediaTimeFont, aMediaElapsed, ColorRGBA(0.72f, 0.75f, 0.82f, 0.92f * TimelineAlpha));
		RenderTextLeft(TextRender(), {TrackRight + LabelGap, ProgressTrack.y - (MediaTimeFont - ProgressTrack.h) * 0.5f, RemainingWidth + 1.0f * PixelWidth, MediaTimeFont}, MediaTimeFont, aMediaRemaining, ColorRGBA(0.72f, 0.75f, 0.82f, 0.92f * TimelineAlpha));

		const float ControlsY = Expanded.y + 119.0f * PixelHeight * WidgetScale;
		const float Slot = Expanded.w / 6.0f;
		const float ButtonRadius = 21.0f * PixelHeight * WidgetScale;
		const std::array<float, 5> aButtonCenters = {
			Expanded.x + Slot * 1.0f,
			Expanded.x + Slot * 2.0f,
			Expanded.x + Slot * 3.0f,
			Expanded.x + Slot * 4.0f,
			Expanded.x + Slot * 5.0f,
		};
		const std::array<const char *, 5> apIcons = {
			FontIcon::CHEVRON_DOWN,
			FontIcon::BACKWARD_STEP,
			Playing ? FontIcon::PAUSE : FontIcon::PLAY,
			FontIcon::FORWARD_STEP,
			"",
		};
		m_HasLastIslandButtons = m_IslandExpanded && ControlsReveal > 0.72f && InviteProgress < 0.05f;
		for(size_t i = 0; i < m_aLastIslandButtons.size(); ++i)
		{
			m_aLastIslandButtons[i] = {
				aButtonCenters[i] - ButtonRadius,
				ControlsY - ButtonRadius,
				ButtonRadius * 2.0f,
				ButtonRadius * 2.0f};
			const bool Hot = m_HasLastIslandButtons && m_IslandInteractActive && m_aLastIslandButtons[i].Inside(m_IslandInteractMouse);
			m_aIslandButtonHoverProgress[i] = AnimateValue(m_aIslandButtonHoverProgress[i], Hot ? 1.0f : 0.0f, Hot ? 18.0f : 13.0f);
			const float ButtonHover = SmoothProgress(m_aIslandButtonHoverProgress[i]);
			const bool ActiveButton = i == 4 && m_IslandVolumeOpen;
			if(ButtonHover > 0.01f || ActiveButton || i == 2 || i == 4)
			{
				const float BaseVisualRadius = i == 2 ? 18.0f : (i == 4 ? 17.0f : 15.0f);
				const float VisualRadius = (BaseVisualRadius + ButtonHover * 1.4f) * PixelHeight * WidgetScale;
				ColorRGBA ButtonFill = i == 2 ? ColorRGBA(1.0f, 1.0f, 1.0f, (0.11f + ButtonHover * 0.08f) * ControlsAlpha) : ColorRGBA(1.0f, 1.0f, 1.0f, (0.035f + ButtonHover * 0.075f) * ControlsAlpha);
				if(i == 4)
					ButtonFill = ActiveButton ? ColorRGBA(AccentColor.r * 0.16f, AccentColor.g * 0.16f, AccentColor.b * 0.20f, 0.92f * ControlsAlpha) : ColorRGBA(0.075f, 0.078f, 0.090f, (0.62f + ButtonHover * 0.25f) * ControlsAlpha);
				RenderSmoothRoundedRect({aButtonCenters[i] - VisualRadius, ControlsY - VisualRadius, VisualRadius * 2.0f, VisualRadius * 2.0f}, ButtonFill, VisualRadius);
			}

			if(i != 4)
			{
				const bool MainControl = i >= 1 && i <= 3;
				const float HoverScale = 1.0f + ButtonHover * 0.10f;
				const float IconSizePx = i == 2 ? 19.0f : (i == 0 ? 13.5f : 18.0f);
				const float IconSize = IconSizePx * PixelHeight * WidgetScale * HoverScale;
				CUIRect IconRect = {aButtonCenters[i] - ButtonRadius, ControlsY - ButtonRadius, ButtonRadius * 2.0f, ButtonRadius * 2.0f};
				ColorRGBA IconColor = MainControl ? ColorRGBA(1.0f, 1.0f, 1.0f, 0.98f * ControlsAlpha) : ColorRGBA(0.66f, 0.70f, 0.78f, 0.94f * ControlsAlpha);
				IconColor = MixColor(IconColor, ColorRGBA(1.0f, 1.0f, 1.0f, IconColor.a), ButtonHover * 0.52f);
				RenderIconCentered(TextRender(), IconRect, IconSize, apIcons[i], IconColor);
			}
			else
			{
				const ColorRGBA VolumeColor = ActiveButton ? ColorRGBA(LightColor.r, LightColor.g, LightColor.b, 1.0f * ControlsAlpha) : ColorRGBA(0.78f, 0.80f, 0.84f, 0.90f * ControlsAlpha);
				const float S = 18.5f * PixelHeight * WidgetScale * (1.0f + ButtonHover * 0.10f);
				RenderIconCentered(TextRender(), m_aLastIslandButtons[i], S, "\uF028", VolumeColor);
			}
		}

		const float VolumeReveal = DelayedProgress(VolumeProgress, 0.18f) * (1.0f - InviteProgress);
		if(VolumeReveal > 0.01f)
		{
			const float VolumeAlpha = Progress * ControlsReveal * VolumeReveal;
			const float SliderY = Expanded.y + (161.0f + (1.0f - VolumeReveal) * 7.0f) * PixelHeight * WidgetScale;
			const float LeftIconX = Expanded.x + 45.0f * PixelWidth * WidgetScale;
			const float RightIconX = Expanded.x + Expanded.w - 45.0f * PixelWidth * WidgetScale;
			const float TrackLeft = Expanded.x + 78.0f * PixelWidth * WidgetScale;
			const float TrackRight = Expanded.x + Expanded.w - 78.0f * PixelWidth * WidgetScale;
			const float TrackH = 4.3f * PixelHeight * WidgetScale;
			const float VolumeRatio = ClampFloat(SourceVolume, 0.0f, 1.0f);
			m_IslandVolumeTrackLeft = TrackLeft;
			m_IslandVolumeTrackRight = TrackRight;
			const bool VolumeInteractive = m_IslandExpanded && m_IslandVolumeOpen && SourceVolumeAvailable && VolumeReveal > 0.72f && InviteProgress < 0.05f;
			m_LastIslandVolumeSlider = VolumeInteractive ? CUIRect{TrackLeft - 16.0f * PixelWidth * WidgetScale, SliderY - 14.0f * PixelHeight * WidgetScale, TrackRight - TrackLeft + 32.0f * PixelWidth * WidgetScale, 28.0f * PixelHeight * WidgetScale} : CUIRect{};

			auto DrawTinyVolume = [&](float Cx, float Scale, ColorRGBA Color) {
				const float S = 13.0f * PixelHeight * WidgetScale * Scale;
				RenderSmoothRoundedRect({Cx - S * 0.48f, SliderY - S * 0.18f, S * 0.22f, S * 0.36f}, Color, S * 0.04f);
				RenderSmoothRoundedRect({Cx - S * 0.28f, SliderY - S * 0.32f, S * 0.24f, S * 0.64f}, Color, S * 0.06f);
				for(int Wave = 0; Wave < 2; ++Wave)
				{
					const float WaveH = S * (0.36f + Wave * 0.24f);
					RenderSmoothRoundedRect({Cx + S * (0.10f + Wave * 0.18f), SliderY - WaveH * 0.5f, S * 0.07f, WaveH}, Color, S * 0.035f);
				}
			};
			DrawTinyVolume(LeftIconX, 0.82f, ColorRGBA(0.72f, 0.74f, 0.78f, 0.86f * VolumeAlpha));
			DrawTinyVolume(RightIconX, 1.0f, ColorRGBA(0.72f, 0.74f, 0.78f, 0.86f * VolumeAlpha));
			RenderSmoothRoundedRect({TrackLeft, SliderY - TrackH * 0.5f, TrackRight - TrackLeft, TrackH}, ColorRGBA(0.31f, 0.32f, 0.35f, 0.95f * VolumeAlpha), TrackH * 0.5f);
			const float FilledWidth = (TrackRight - TrackLeft) * VolumeRatio;
			const ColorRGBA VolumeFill = MixColor(ColorRGBA(0.90f, 0.92f, 0.96f, 1.0f), LightColor, 0.30f);
			if(FilledWidth > 0.01f)
				RenderSmoothRoundedRect({TrackLeft, SliderY - TrackH * 0.5f, FilledWidth, TrackH}, ColorRGBA(VolumeFill.r, VolumeFill.g, VolumeFill.b, 0.98f * VolumeAlpha), TrackH * 0.5f);

			// Draggable handle ("dot") at the current volume position; grabbing it or
			// clicking anywhere on the padded track adjusts the volume.
			const bool VolumeHovered = VolumeInteractive && m_IslandInteractActive && m_LastIslandVolumeSlider.Inside(m_IslandInteractMouse);
			m_IslandVolumeHoverProgress = AnimateValue(m_IslandVolumeHoverProgress, (VolumeHovered || m_IslandVolumeDragging) ? 1.0f : 0.0f, (VolumeHovered || m_IslandVolumeDragging) ? 18.0f : 13.0f);
			const float KnobHover = SmoothProgress(m_IslandVolumeHoverProgress);
			const float KnobSize = (8.5f + KnobHover * 2.2f) * PixelHeight * WidgetScale;
			const float KnobX = TrackLeft + (TrackRight - TrackLeft) * VolumeRatio;
			const float KnobPad = 1.6f * PixelHeight * WidgetScale;
			RenderSmoothRoundedRect({KnobX - KnobSize * 0.5f - KnobPad, SliderY - KnobSize * 0.5f - KnobPad, KnobSize + KnobPad * 2.0f, KnobSize + KnobPad * 2.0f}, ColorRGBA(0.0f, 0.0f, 0.0f, 0.32f * VolumeAlpha), (KnobSize + KnobPad * 2.0f) * 0.5f);
			RenderSmoothRoundedRect({KnobX - KnobSize * 0.5f, SliderY - KnobSize * 0.5f, KnobSize, KnobSize}, ColorRGBA(0.97f, 0.97f, 0.95f, 0.99f * VolumeAlpha), KnobSize * 0.5f);
			const ColorRGBA KnobCore = MixColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), LightColor, 0.35f);
			RenderSmoothRoundedRect({KnobX - KnobSize * 0.26f, SliderY - KnobSize * 0.26f, KnobSize * 0.52f, KnobSize * 0.52f}, ColorRGBA(KnobCore.r, KnobCore.g, KnobCore.b, 0.95f * VolumeAlpha), KnobSize * 0.26f);
		}
		else
		{
			m_IslandVolumeHoverProgress = AnimateValue(m_IslandVolumeHoverProgress, 0.0f, 13.0f);
			m_IslandVolumeDragging = false;
			m_LastIslandVolumeSlider = {};
			m_IslandVolumeTrackLeft = 0.0f;
			m_IslandVolumeTrackRight = 0.0f;
		}
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	if(InviteProgress > 0.01f)
	{
		const float InviteAlpha = Progress * InviteProgress;
		const float InviteContentProgress = DelayedProgress(InviteProgress, 0.22f);
		const float InviteContentAlpha = Progress * InviteContentProgress;
		CUIRect Panel = Island;
		const float PanelRadius = IslandRadius;

		// Vector stroke helper (rotated quad) for the check/cross marks.
		auto DrawStroke = [&](float Cx, float Cy, float Len, float Thick, float Angle, ColorRGBA Col) {
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(Col.r, Col.g, Col.b, Col.a);
			Graphics()->QuadsSetRotation(Angle);
			IGraphics::CQuadItem Quad(Cx, Cy, Len, Thick);
			Graphics()->QuadsDraw(&Quad, 1);
			Graphics()->QuadsSetRotation(0.0f);
			Graphics()->QuadsEnd();
		};

		Graphics()->TextureClear();
		Graphics()->DrawRect(Panel.x, Panel.y, Panel.w, Panel.h, ColorRGBA(0.09f, 0.10f, 0.10f, 0.97f * InviteAlpha), IGraphics::CORNER_ALL, PanelRadius);

		const float Pad = 8.0f * PixelWidth * WidgetScale;
		const float TopY = Panel.y + 7.0f * PixelHeight * WidgetScale;
		const float IconSize = 20.0f * PixelHeight * WidgetScale;
		m_LastInviteAcceptButton = {Panel.x + Pad, TopY - 2.0f * PixelHeight * WidgetScale, IconSize + 5.0f * PixelWidth * WidgetScale, IconSize + 4.0f * PixelHeight * WidgetScale};
		m_LastInviteIgnoreButton = {Panel.x + Panel.w - Pad - IconSize, TopY - 2.0f * PixelHeight * WidgetScale, IconSize + 4.0f * PixelWidth * WidgetScale, IconSize + 4.0f * PixelHeight * WidgetScale};
		m_HasInviteButtons = InviteActive && InviteContentProgress > 0.60f;

		const bool AcceptHot = m_HasInviteButtons && m_IslandInteractActive && m_LastInviteAcceptButton.Inside(m_IslandInteractMouse);
		const bool IgnoreHot = m_HasInviteButtons && m_IslandInteractActive && m_LastInviteIgnoreButton.Inside(m_IslandInteractMouse);
		m_aInviteButtonHoverProgress[1] = AnimateValue(m_aInviteButtonHoverProgress[1], AcceptHot ? 1.0f : 0.0f, AcceptHot ? 18.0f : 13.0f);
		m_aInviteButtonHoverProgress[0] = AnimateValue(m_aInviteButtonHoverProgress[0], IgnoreHot ? 1.0f : 0.0f, IgnoreHot ? 18.0f : 13.0f);
		const float AcceptHover = SmoothProgress(m_aInviteButtonHoverProgress[1]);
		const float IgnoreHover = SmoothProgress(m_aInviteButtonHoverProgress[0]);
		const ColorRGBA AcceptColor(0.38f + AcceptHover * 0.14f, 0.95f, 0.58f, InviteContentAlpha);
		const ColorRGBA IgnoreColor(0.96f, 0.35f + IgnoreHover * 0.12f, 0.43f, InviteContentAlpha);

		// Door and entering arrow, matching the compact reference icon.
		const vec2 AcceptCenter = m_LastInviteAcceptButton.Center();
		const float S = 15.0f * PixelHeight * WidgetScale;
		RenderSmoothRoundedRect({AcceptCenter.x + S * 0.18f, AcceptCenter.y - S * 0.43f, S * 0.14f, S * 0.86f}, AcceptColor, S * 0.04f);
		DrawStroke(AcceptCenter.x - S * 0.05f, AcceptCenter.y, S * 0.62f, S * 0.14f, 0.0f, AcceptColor);
		DrawStroke(AcceptCenter.x - S * 0.29f, AcceptCenter.y - S * 0.17f, S * 0.34f, S * 0.14f, pi * 0.25f, AcceptColor);
		DrawStroke(AcceptCenter.x - S * 0.29f, AcceptCenter.y + S * 0.17f, S * 0.34f, S * 0.14f, -pi * 0.25f, AcceptColor);

		const vec2 IgnoreCenter = m_LastInviteIgnoreButton.Center();
		DrawStroke(IgnoreCenter.x, IgnoreCenter.y, S * 0.62f, S * 0.13f, pi * 0.25f, IgnoreColor);
		DrawStroke(IgnoreCenter.x, IgnoreCenter.y, S * 0.62f, S * 0.13f, -pi * 0.25f, IgnoreColor);

		const int RemainingSeconds = maximum(0, round_to_int(InviteRemaining * TEAM_INVITE_DURATION_SECONDS));
		char aCountdown[16];
		str_format(aCountdown, sizeof(aCountdown), "00:%02d", RemainingSeconds);
		const float TimerFont = maximum(11.0f, 16.0f * WidgetScale) * PixelHeight;
		const float TimerWidth = TextRender()->TextWidth(TimerFont, aCountdown, -1, -1.0f);
		RenderTextLeft(TextRender(), {Panel.x + (Panel.w - TimerWidth) * 0.5f, TopY, TimerWidth, TimerFont}, TimerFont, aCountdown, ColorRGBA(0.92f, 0.94f, 0.94f, InviteContentAlpha));

		const float BarH = 2.7f * PixelHeight * WidgetScale;
		const float BarY = Panel.y + 29.0f * PixelHeight * WidgetScale;
		const CUIRect BarBg = {Panel.x + Pad, BarY, Panel.w - Pad * 2.0f, BarH};
		RenderSmoothRoundedRect(BarBg, ColorRGBA(0.18f, 0.22f, 0.23f, 0.92f * InviteContentAlpha), BarH * 0.5f);
		if(InviteRemaining > 0.001f)
			RenderSmoothRoundedRect({BarBg.x, BarBg.y, BarBg.w * InviteRemaining, BarH}, ColorRGBA(0.10f, 0.72f, 0.92f, 0.98f * InviteContentAlpha), BarH * 0.5f);

		const char *pInviter = m_aTeamInviteInviter[0] != '\0' ? m_aTeamInviteInviter : "Unknown";
		char aSub[64];
		str_format(aSub, sizeof(aSub), "Invite you to team %d", m_TeamInvitePending);
		const float NameFont = maximum(8.5f, 11.5f * WidgetScale) * PixelHeight;
		const float SubFont = maximum(7.5f, 9.5f * WidgetScale) * PixelHeight;
		const float NameWidth = TextRender()->TextWidth(NameFont, pInviter, -1, -1.0f);
		const float SubWidth = TextRender()->TextWidth(SubFont, aSub, -1, -1.0f);
		RenderTextLeft(TextRender(), {Panel.x + (Panel.w - NameWidth) * 0.5f, Panel.y + 37.0f * PixelHeight * WidgetScale, NameWidth, NameFont}, NameFont, pInviter, ColorRGBA(0.92f, 0.94f, 0.94f, 0.98f * InviteContentAlpha));
		RenderTextLeft(TextRender(), {Panel.x + (Panel.w - SubWidth) * 0.5f, Panel.y + 51.0f * PixelHeight * WidgetScale, SubWidth, SubFont}, SubFont, aSub, ColorRGBA(0.70f, 0.73f, 0.75f, 0.96f * InviteContentAlpha));
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else
	{
		m_HasInviteButtons = false;
		m_aInviteButtonHoverProgress[0] = AnimateValue(m_aInviteButtonHoverProgress[0], 0.0f, 13.0f);
		m_aInviteButtonHoverProgress[1] = AnimateValue(m_aInviteButtonHoverProgress[1], 0.0f, 13.0f);
	}
	Ui()->ClipDisable();

	if(EditorActive)
	{
		const float VoteWidth = 120.0f;
		const float VoteHeight = 38.0f;
		CUIRect VoteExample = {
			ScreenWidth * ClampFloat(g_Config.m_TcHudVoteX / 1000.0f, 0.0f, 1.0f),
			ScreenHeight * ClampFloat(g_Config.m_TcHudVoteY / 1000.0f, 0.0f, 1.0f),
			VoteWidth,
			VoteHeight};
		VoteExample.x = ClampFloat(VoteExample.x, 0.0f, maximum(0.0f, ScreenWidth - VoteExample.w));
		VoteExample.y = ClampFloat(VoteExample.y, 0.0f, maximum(0.0f, ScreenHeight - VoteExample.h));
		m_LastVoteRect = VoteExample;
		m_HasLastVoteRect = true;
		RenderVoteExample(VoteExample);

		const CUIRect ActiveSnapRect = m_HudEditorDragTarget == HUD_EDITOR_TARGET_VOTE ? VoteExample : Island;
		const float ActiveCenterX = ActiveSnapRect.x + ActiveSnapRect.w * 0.5f;
		const float ActiveCenterY = ActiveSnapRect.y + ActiveSnapRect.h * 0.5f;
		const bool SnapX = std::abs(ActiveCenterX - ScreenWidth * 0.5f) < 1.2f * PixelWidth;
		const bool SnapY = std::abs(ActiveCenterY - ScreenHeight * 0.5f) < 1.2f * PixelHeight;
		if(SnapX || SnapY)
		{
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.34f);
			if(SnapX)
			{
				IGraphics::CQuadItem Line(ScreenWidth * 0.5f - 0.5f * PixelWidth, 0.0f, PixelWidth, ScreenHeight);
				Graphics()->QuadsDrawTL(&Line, 1);
			}
			if(SnapY)
			{
				IGraphics::CQuadItem Line(0.0f, ScreenHeight * 0.5f - 0.5f * PixelHeight, ScreenWidth, PixelHeight);
				Graphics()->QuadsDrawTL(&Line, 1);
			}
			Graphics()->QuadsEnd();
		}

		RenderHudEditorOutline(Island, IslandRadius, m_HudEditorDragTarget == HUD_EDITOR_TARGET_ISLAND, PixelHeight);
		RenderHudEditorOutline(VoteExample, 3.0f * PixelHeight, m_HudEditorDragTarget == HUD_EDITOR_TARGET_VOTE, PixelHeight);

		const float HintWidth = minimum(ScreenWidth - 20.0f * PixelWidth, 360.0f * PixelWidth);
		const float HintHeight = 19.0f * PixelHeight;
		CUIRect Hint = {ScreenWidth * 0.5f - HintWidth * 0.5f, ScreenHeight - 29.0f * PixelHeight, HintWidth, HintHeight};
		SSdfRoundedRectStyle HintStyle;
		HintStyle.m_Fill = ColorRGBA(0.0f, 0.0f, 0.0f, 0.52f);
		HintStyle.m_Stroke = ColorRGBA(1.0f, 1.0f, 1.0f, 0.14f);
		HintStyle.m_Radius = Hint.h * 0.5f;
		HintStyle.m_StrokeWidth = 0.45f * PixelHeight;
		HintStyle.m_Softness = 0.25f;
		RenderSdfRoundedRect(Hint, HintStyle);
		Hint.Margin(7.0f * PixelWidth, &Hint);
		CTextCursor HintCursor;
		const float HintFont = 10.0f * PixelHeight;
		HintCursor.SetPosition(vec2(Hint.x, Hint.y + (Hint.h - HintFont) * 0.5f));
		HintCursor.m_FontSize = HintFont;
		HintCursor.m_LineWidth = Hint.w;
		HintCursor.m_MaxLines = 1;
		HintCursor.m_Flags = TEXTFLAG_RENDER | TEXTFLAG_DISALLOW_NEWLINE | TEXTFLAG_ELLIPSIS_AT_END;
		TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.82f));
		TextRender()->TextEx(&HintCursor, "Drag  |  Wheel: scale  |  Double-click: reset  |  Esc: close");
		TextRender()->TextColor(TextRender()->DefaultTextColor());

		RenderTools()->RenderCursor(m_HudEditorMouse, 19.0f * PixelHeight, 0.82f);
	}
	else
	{
		m_HasLastVoteRect = false;
	}
	if(m_IslandInteractActive && !EditorActive)
		RenderTools()->RenderCursor(m_IslandInteractMouse, 19.0f * PixelHeight, 0.90f);
}

#if defined(CONF_FAMILY_WINDOWS)
void CMediaIsland::UpdateState(bool Active, bool Playing, const char *pTitle, const char *pArtist, const char *pSource, std::vector<uint8_t> &&vThumbnailRgba, uint32_t ThumbnailWidth, uint32_t ThumbnailHeight, bool ThumbnailDirty, ColorRGBA AccentColor, ColorRGBA DarkColor, ColorRGBA LightColor, float AudioPeak, int64_t PositionMs, int64_t DurationMs, const std::array<float, 6> &aAudioBands)
{
	std::lock_guard<std::mutex> Lock(m_MediaIsland.m_Mutex);
	m_MediaIsland.m_Active = Active;
	m_MediaIsland.m_Playing = Playing;
	m_MediaIsland.m_AccentColor = AccentColor;
	m_MediaIsland.m_DarkColor = DarkColor;
	m_MediaIsland.m_LightColor = LightColor;
	m_MediaIsland.m_AudioPeak = AudioPeak;
	m_MediaIsland.m_PositionMs = PositionMs;
	m_MediaIsland.m_DurationMs = DurationMs;
	m_MediaIsland.m_aAudioBands = aAudioBands;
	m_MediaIsland.m_Title = pTitle ? pTitle : "";
	m_MediaIsland.m_Artist = pArtist ? pArtist : "";
	m_MediaIsland.m_Source = pSource ? pSource : "";
	if(ThumbnailDirty)
	{
		m_MediaIsland.m_vThumbnailRgba = std::move(vThumbnailRgba);
		m_MediaIsland.m_ThumbnailWidth = ThumbnailWidth;
		m_MediaIsland.m_ThumbnailHeight = ThumbnailHeight;
		m_MediaIsland.m_ThumbnailDirty = true;
	}
}

void CMediaIsland::StartWorker()
{
	StopWorker();
	m_MediaIsland.m_Stop = false;
	m_MediaIsland.m_Worker = std::thread([this]() {
		try
		{
			CWinrtApartment Apartment;
			winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager Manager = nullptr;
			std::unique_ptr<CAudioLoopbackBands> pAudioBands;
			std::string LastTrackKey;
			ColorRGBA LastAccentColor(0.36f, 0.36f, 0.40f, 1.0f);
			ColorRGBA LastDarkColor(0.12f, 0.12f, 0.14f, 1.0f);
			ColorRGBA LastLightColor(0.44f, 0.44f, 0.50f, 1.0f);
			size_t LastThumbnailHash = 0;
			int ThumbnailRefreshTick = 0;
			std::string ProductLogoSource;
			TMediaIslandLogoPixels aProductLogo{};
			bool ProductLogoAvailable = false;
			bool ProductLogoFiltered = false;
			int ProductLogoFastRetries = 0;
			int ProductLogoRetryTicks = 0;
			int ConsecutiveQueryFailures = 0;
			int ManagerRetryTicks = 0;
			int AudioRetryTicks = 0;
			CMediaApplicationVolume ApplicationVolume;
			std::string ApplicationVolumeSource;
			int ApplicationVolumeRefreshTicks = 0;
			int ApplicationVolumeQueryTicks = 0;
			while(!m_MediaIsland.m_Stop)
			{
				if(!Manager)
				{
					m_MediaIsland.m_Command.exchange(MEDIA_ISLAND_COMMAND_NONE);
					if(ManagerRetryTicks > 0)
						ManagerRetryTicks--;
					else
					{
						try
						{
							Manager = WaitForMediaIslandAsync(winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync(), m_MediaIsland.m_Stop);
							ConsecutiveQueryFailures = 0;
						}
						catch(const CMediaIslandWorkerStopped &)
						{
							throw;
						}
						catch(...)
						{
							Manager = nullptr;
							ManagerRetryTicks = 32;
						}
					}
					if(!Manager)
					{
						m_MediaIsland.m_SourceVolumeAvailable.store(false);
						for(int i = 0; i < 3 && !m_MediaIsland.m_Stop; ++i)
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
						continue;
					}
				}

				bool Active = false;
				bool Playing = false;
				std::string Title;
				std::string Artist;
				std::string Source;
				std::vector<uint8_t> vThumbnailRgba;
				uint32_t ThumbnailWidth = 0;
				uint32_t ThumbnailHeight = 0;
				bool HasThumbnailUpdate = false;
				ColorRGBA SessionAccentColor = LastAccentColor;
				ColorRGBA SessionDarkColor = LastDarkColor;
				ColorRGBA SessionLightColor = LastLightColor;
				float AudioPeak = 0.0f;
				int64_t PositionMs = 0;
				int64_t DurationMs = 0;
				std::array<float, 6> aAudioBands{};
				const int Command = m_MediaIsland.m_Command.exchange(MEDIA_ISLAND_COMMAND_NONE);
				bool QuerySucceeded = true;

				if(!pAudioBands && AudioRetryTicks <= 0)
				{
					auto pCandidate = std::make_unique<CAudioLoopbackBands>();
					if(pCandidate->Init())
						pAudioBands = std::move(pCandidate);
					else
						AudioRetryTicks = 160;
				}
				else if(!pAudioBands)
					AudioRetryTicks--;
				if(pAudioBands && !pAudioBands->Update(AudioPeak, aAudioBands))
				{
					pAudioBands.reset();
					AudioRetryTicks = 160;
				}

				try
				{
					winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession Session = nullptr;
					auto Sessions = Manager.GetSessions();
					for(uint32_t i = 0; i < Sessions.Size(); ++i)
					{
						auto Candidate = Sessions.GetAt(i);
						if(!Candidate)
							continue;
						auto CandidatePlaybackInfo = Candidate.GetPlaybackInfo();
						auto CandidateStatus = CandidatePlaybackInfo.PlaybackStatus();
						if(CandidateStatus == winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
						{
							Session = Candidate;
							break;
						}
						if(Session == nullptr)
							Session = Candidate;
					}
					if(Session == nullptr)
						Session = Manager.GetCurrentSession();
					if(Session)
					{
						if(Command == MEDIA_ISLAND_COMMAND_PREVIOUS)
							WaitForMediaIslandAsync(Session.TrySkipPreviousAsync(), m_MediaIsland.m_Stop);
						else if(Command == MEDIA_ISLAND_COMMAND_PLAY_PAUSE)
							WaitForMediaIslandAsync(Session.TryTogglePlayPauseAsync(), m_MediaIsland.m_Stop);
						else if(Command == MEDIA_ISLAND_COMMAND_NEXT)
							WaitForMediaIslandAsync(Session.TrySkipNextAsync(), m_MediaIsland.m_Stop);

						auto PlaybackInfo = Session.GetPlaybackInfo();
						auto Status = PlaybackInfo.PlaybackStatus();
						auto Properties = WaitForMediaIslandAsync(Session.TryGetMediaPropertiesAsync(), m_MediaIsland.m_Stop);
						auto Timeline = Session.GetTimelineProperties();
						auto ToMilliseconds = [](auto Duration) -> int64_t {
							return std::chrono::duration_cast<std::chrono::milliseconds>(Duration).count();
						};
						PositionMs = maximum<int64_t>(0, ToMilliseconds(Timeline.Position()));
						const int64_t StartMs = ToMilliseconds(Timeline.StartTime());
						const int64_t EndMs = ToMilliseconds(Timeline.EndTime());
						DurationMs = maximum<int64_t>(0, EndMs - StartMs);
						if(DurationMs <= 0)
							DurationMs = maximum<int64_t>(0, EndMs);
						if(DurationMs > 0)
							PositionMs = std::clamp(PositionMs - maximum<int64_t>(0, StartMs), (int64_t)0, DurationMs);
						Active = true;
						Playing = Status == winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
						Title = winrt::to_string(Properties.Title());
						Artist = winrt::to_string(Properties.Artist());
						Source = winrt::to_string(Session.SourceAppUserModelId());

						if(ApplicationVolumeSource != Source || ApplicationVolumeRefreshTicks <= 0)
						{
							ApplicationVolumeSource = Source;
							const bool Available = ApplicationVolume.Refresh(Source.c_str());
							m_MediaIsland.m_SourceVolumeAvailable.store(Available);
							ApplicationVolumeRefreshTicks = Available ? 90 : 25;
							ApplicationVolumeQueryTicks = 0;
						}
						else
							ApplicationVolumeRefreshTicks--;

						const float RequestedVolume = m_MediaIsland.m_RequestedSourceVolume.exchange(-1.0f);
						if(RequestedVolume >= 0.0f)
						{
							const bool Applied = ApplicationVolume.Set(RequestedVolume);
							m_MediaIsland.m_SourceVolumeAvailable.store(Applied);
							if(Applied)
								m_MediaIsland.m_SourceVolume.store(ClampFloat(RequestedVolume, 0.0f, 1.0f));
							else
								ApplicationVolumeRefreshTicks = 0;
						}
						if(ApplicationVolumeQueryTicks <= 0)
						{
							float CurrentVolume = 1.0f;
							const bool Available = ApplicationVolume.Get(CurrentVolume);
							m_MediaIsland.m_SourceVolumeAvailable.store(Available);
							if(Available)
								m_MediaIsland.m_SourceVolume.store(ClampFloat(CurrentVolume, 0.0f, 1.0f));
							ApplicationVolumeQueryTicks = 15;
						}
						else
							ApplicationVolumeQueryTicks--;

						std::string TrackKey = Title;
						TrackKey += '\n';
						TrackKey += Artist;
						TrackKey += '\n';
						TrackKey += Source;
						const bool TrackChanged = TrackKey != LastTrackKey;
						if(ProductLogoSource != Source)
						{
							ProductLogoSource = Source;
							ProductLogoAvailable = false;
							ProductLogoFiltered = false;
							ProductLogoRetryTicks = 0;
							aProductLogo = {};
						}
						if(!ProductLogoAvailable)
						{
							if(ProductLogoRetryTicks > 0)
								ProductLogoRetryTicks--;
							else
							{
								aProductLogo = {};
								ProductLogoAvailable = LoadChromiumProductLogo(Source.c_str(), aProductLogo);
								ProductLogoRetryTicks = ProductLogoAvailable ? 0 : 150;
							}
						}
						ThumbnailRefreshTick++;
						if(TrackChanged || ThumbnailRefreshTick >= 30)
						{
							if(TrackChanged)
							{
								ProductLogoFastRetries = 4;
								ProductLogoFiltered = false;
								LastThumbnailHash = 0;
								HasThumbnailUpdate = true;
								LastAccentColor = ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f);
								LastDarkColor = ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f);
								LastLightColor = ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f);
								SessionAccentColor = LastAccentColor;
								SessionDarkColor = LastDarkColor;
								SessionLightColor = LastLightColor;
							}
							ThumbnailRefreshTick = 0;
							auto Thumbnail = Properties.Thumbnail();
							if(Thumbnail)
							{
								auto Stream = WaitForMediaIslandAsync(Thumbnail.OpenReadAsync(), m_MediaIsland.m_Stop);
								if(Stream)
								{
									auto Decoder = WaitForMediaIslandAsync(winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(Stream), m_MediaIsland.m_Stop);
									const uint32_t PreviewSize = 128;
									winrt::Windows::Graphics::Imaging::BitmapTransform Transform;
									const uint32_t SourceWidth = Decoder.PixelWidth();
									const uint32_t SourceHeight = Decoder.PixelHeight();
									const uint32_t OrientedWidth = Decoder.OrientedPixelWidth();
									const uint32_t OrientedHeight = Decoder.OrientedPixelHeight();
									if(SourceWidth > 0 && SourceHeight > 0 && OrientedWidth > 0 && OrientedHeight > 0)
									{
										// BitmapTransform applies scale before crop, so crop bounds must
										// be expressed in the scaled and EXIF-oriented coordinate space.
										// Scale the short edge to the preview size, then center-crop.
										const float Scale = PreviewSize / (float)minimum(OrientedWidth, OrientedHeight);
										const uint32_t ScaledSourceWidth = maximum(1U, (uint32_t)std::round(SourceWidth * Scale));
										const uint32_t ScaledSourceHeight = maximum(1U, (uint32_t)std::round(SourceHeight * Scale));
										const uint32_t ScaledOrientedWidth = maximum(PreviewSize, (uint32_t)std::round(OrientedWidth * Scale));
										const uint32_t ScaledOrientedHeight = maximum(PreviewSize, (uint32_t)std::round(OrientedHeight * Scale));
										Transform.ScaledWidth(ScaledSourceWidth);
										Transform.ScaledHeight(ScaledSourceHeight);
										winrt::Windows::Graphics::Imaging::BitmapBounds CropBounds{};
										CropBounds.X = (ScaledOrientedWidth - PreviewSize) / 2;
										CropBounds.Y = (ScaledOrientedHeight - PreviewSize) / 2;
										CropBounds.Width = PreviewSize;
										CropBounds.Height = PreviewSize;
										Transform.Bounds(CropBounds);
									}
									auto PixelData = WaitForMediaIslandAsync(Decoder.GetPixelDataAsync(
															 winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Rgba8,
															 winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Straight,
															 Transform,
															 winrt::Windows::Graphics::Imaging::ExifOrientationMode::RespectExifOrientation,
															 winrt::Windows::Graphics::Imaging::ColorManagementMode::DoNotColorManage),
										m_MediaIsland.m_Stop);
									auto RawPixels = PixelData.DetachPixelData();
									vThumbnailRgba.assign(RawPixels.begin(), RawPixels.end());
									const size_t ExpectedPixelBytes = (size_t)PreviewSize * PreviewSize * 4;
									if(vThumbnailRgba.size() != ExpectedPixelBytes)
									{
										vThumbnailRgba.clear();
										ThumbnailWidth = 0;
										ThumbnailHeight = 0;
										LastThumbnailHash = 0;
										HasThumbnailUpdate = true;
									}
									else
									{
										ThumbnailWidth = PreviewSize;
										ThumbnailHeight = PreviewSize;
										if(ProductLogoAvailable && IsMatchingProductLogo(vThumbnailRgba, ThumbnailWidth, ThumbnailHeight, aProductLogo))
										{
											// Chromium deliberately publishes its application icon when
											// the page did not provide artwork. GMTC does not expose that
											// distinction, so compare it with the installed executable's
											// icon and treat a conservative match as no thumbnail.
											vThumbnailRgba.clear();
											ThumbnailWidth = 0;
											ThumbnailHeight = 0;
											if(!ProductLogoFiltered)
											{
												ProductLogoFiltered = true;
												LastThumbnailHash = 0;
												HasThumbnailUpdate = true;
												LastAccentColor = ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f);
												LastDarkColor = ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f);
												LastLightColor = ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f);
												SessionAccentColor = LastAccentColor;
												SessionDarkColor = LastDarkColor;
												SessionLightColor = LastLightColor;
											}
											if(ProductLogoFastRetries > 0)
											{
												// Artwork can arrive shortly after metadata. Retry quickly a
												// few times, then return to the regular refresh cadence.
												ThumbnailRefreshTick = 24;
												ProductLogoFastRetries--;
											}
										}
										else
										{
											ProductLogoFastRetries = 0;
											ProductLogoFiltered = false;
											size_t ThumbnailHash = 1469598103934665603ull;
											for(uint8_t Byte : vThumbnailRgba)
											{
												ThumbnailHash ^= Byte;
												ThumbnailHash *= 1099511628211ull;
											}
											if(TrackChanged || ThumbnailHash != LastThumbnailHash)
											{
												LastThumbnailHash = ThumbnailHash;
												HasThumbnailUpdate = true;
												ExtractTrackColors(vThumbnailRgba, ThumbnailWidth, ThumbnailHeight, SessionAccentColor, SessionDarkColor, SessionLightColor);
												LastAccentColor = SessionAccentColor;
												LastDarkColor = SessionDarkColor;
												LastLightColor = SessionLightColor;
											}
											else
											{
												vThumbnailRgba.clear();
												ThumbnailWidth = 0;
												ThumbnailHeight = 0;
											}
										}
									}
								}
							}
							else if(TrackChanged)
							{
								LastThumbnailHash = 0;
								HasThumbnailUpdate = true;
							}
							// Commit the key only after the artwork query completed. If WinRT
							// throws above, the next pass retries immediately instead of keeping
							// artwork from the previous track until the periodic refresh.
							if(TrackChanged)
								LastTrackKey = TrackKey;
						}
					}
					else
					{
						m_MediaIsland.m_SourceVolumeAvailable.store(false);
						if(!LastTrackKey.empty())
						{
							LastTrackKey.clear();
							LastThumbnailHash = 0;
							ThumbnailRefreshTick = 0;
							ProductLogoSource.clear();
							ProductLogoAvailable = false;
							ProductLogoFiltered = false;
							ProductLogoFastRetries = 0;
							ProductLogoRetryTicks = 0;
							HasThumbnailUpdate = true;
							LastAccentColor = ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f);
							LastDarkColor = ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f);
							LastLightColor = ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f);
							SessionAccentColor = LastAccentColor;
							SessionDarkColor = LastDarkColor;
							SessionLightColor = LastLightColor;
						}
					}
				}
				catch(const CMediaIslandWorkerStopped &)
				{
					throw;
				}
				catch(...)
				{
					QuerySucceeded = false;
				}

				if(QuerySucceeded)
				{
					ConsecutiveQueryFailures = 0;
				}
				else
				{
					ConsecutiveQueryFailures++;
					// A single WinRT query can fail while the player switches tracks.
					// Preserve the last good state briefly instead of flashing the island off.
					if(ConsecutiveQueryFailures < 5)
					{
						for(int i = 0; i < 3 && !m_MediaIsland.m_Stop; ++i)
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
						continue;
					}
					if(ConsecutiveQueryFailures == 5)
					{
						Manager = nullptr;
						ManagerRetryTicks = 32;
						LastTrackKey.clear();
						LastThumbnailHash = 0;
						ThumbnailRefreshTick = 0;
						ProductLogoSource.clear();
						ProductLogoAvailable = false;
						ProductLogoFiltered = false;
						ProductLogoFastRetries = 0;
						ProductLogoRetryTicks = 0;
						HasThumbnailUpdate = true;
						SessionAccentColor = ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f);
						SessionDarkColor = ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f);
						SessionLightColor = ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f);
						LastAccentColor = SessionAccentColor;
						LastDarkColor = SessionDarkColor;
						LastLightColor = SessionLightColor;
					}
					Active = false;
					Playing = false;
					Title.clear();
					Artist.clear();
					Source.clear();
					PositionMs = 0;
					DurationMs = 0;
				}

				UpdateState(Active, Playing, Title.c_str(), Artist.c_str(), Source.c_str(), std::move(vThumbnailRgba), ThumbnailWidth, ThumbnailHeight, HasThumbnailUpdate, SessionAccentColor, SessionDarkColor, SessionLightColor, AudioPeak, PositionMs, DurationMs, aAudioBands);

				for(int i = 0; i < 3 && !m_MediaIsland.m_Stop; ++i)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		catch(const CMediaIslandWorkerStopped &)
		{
		}
		catch(...)
		{
			std::array<float, 6> aAudioBands{};
			UpdateState(false, false, "", "", "", {}, 0, 0, true, ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f), ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f), ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f), 0.0f, 0, 0, aAudioBands);
		}
	});
}

void CMediaIsland::StopWorker()
{
	m_MediaIsland.m_Stop = true;
	if(m_MediaIsland.m_Worker.joinable())
		m_MediaIsland.m_Worker.join();
}
#endif
