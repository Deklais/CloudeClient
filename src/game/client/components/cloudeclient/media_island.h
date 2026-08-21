#ifndef GAME_CLIENT_COMPONENTS_CLOUDECLIENT_MEDIA_ISLAND_H
#define GAME_CLIENT_COMPONENTS_CLOUDECLIENT_MEDIA_ISLAND_H

#include <engine/console.h>
#include <engine/graphics.h>

#include <game/client/component.h>
#include <game/client/ui_rect.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CMediaIsland : public CComponent
{
#if defined(CONF_FAMILY_WINDOWS)
	struct SMediaIslandState
	{
		std::atomic<bool> m_Stop{false};
		std::atomic<int> m_Command{0};
		std::atomic<float> m_RequestedSourceVolume{-1.0f};
		std::atomic<float> m_SourceVolume{1.0f};
		std::atomic<bool> m_SourceVolumeAvailable{false};
		std::thread m_Worker;
		std::mutex m_Mutex;
		bool m_Active = false;
		bool m_Playing = false;
		std::string m_Title;
		std::string m_Artist;
		std::string m_Source;
		std::vector<uint8_t> m_vThumbnailRgba;
		uint32_t m_ThumbnailWidth = 0;
		uint32_t m_ThumbnailHeight = 0;
		bool m_ThumbnailDirty = false;
		ColorRGBA m_AccentColor = ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f);
		ColorRGBA m_DarkColor = ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f);
		ColorRGBA m_LightColor = ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f);
		float m_AudioPeak = 0.0f;
		int64_t m_PositionMs = 0;
		int64_t m_DurationMs = 0;
		std::array<float, 6> m_aAudioBands{};
	};

	SMediaIslandState m_MediaIsland;

	void StartWorker();
	void StopWorker();
	void UpdateState(bool Active, bool Playing, const char *pTitle, const char *pArtist, const char *pSource, std::vector<uint8_t> &&vThumbnailRgba, uint32_t ThumbnailWidth, uint32_t ThumbnailHeight, bool ThumbnailDirty, ColorRGBA AccentColor, ColorRGBA DarkColor, ColorRGBA LightColor, float AudioPeak, int64_t PositionMs, int64_t DurationMs, const std::array<float, 6> &aAudioBands);
#endif // GAME_CLIENT_COMPONENTS_CLOUDECLIENT_MEDIA_ISLAND_H
	IGraphics::CTextureHandle m_ThumbnailTexture;
	IGraphics::CTextureHandle m_PreviousThumbnailTexture;
	std::array<IGraphics::CTextureHandle, 5> m_aControlIconTextures{};
	std::array<vec4, 5> m_aControlIconUvs{};
	std::array<bool, 5> m_aControlIconLoaded{};
	bool m_HasThumbnailTexture = false;
	bool m_HasPreviousThumbnailTexture = false;
	float m_ThumbnailTransitionProgress = 1.0f;
	std::array<float, 6> m_aVisualizerValues{};
	std::array<float, 6> m_aPreviousRawTargets{};
	ColorRGBA m_AnimatedAccentColor = ColorRGBA(0.36f, 0.36f, 0.40f, 1.0f);
	ColorRGBA m_AnimatedDarkColor = ColorRGBA(0.12f, 0.12f, 0.14f, 1.0f);
	ColorRGBA m_AnimatedLightColor = ColorRGBA(0.44f, 0.44f, 0.50f, 1.0f);
	std::string m_DisplayedTitle;
	std::string m_DisplayedArtist;
	std::string m_PreviousTitle;
	std::string m_PreviousArtist;
	bool m_MetadataInitialized = false;
	float m_MetadataTransitionProgress = 1.0f;
	float m_AnimatedMediaTrackRatio = 0.0f;
	bool m_MediaTrackRatioInitialized = false;
	int64_t m_LastVisualizerUpdate = 0;
	float m_ActivationProgress = 0.0f;
	float m_MediaRevealProgress = 0.0f;
	CUIRect m_LastIslandRect{};
	CUIRect m_LastVoteRect{};
	std::array<CUIRect, 5> m_aLastIslandButtons{};
	CUIRect m_LastIslandVolumeSlider{};
	float m_IslandVolumeTrackLeft = 0.0f;
	float m_IslandVolumeTrackRight = 0.0f;
	bool m_IslandVolumeDragging = false;

	// Team invite prompt (triggered from chat "... invited you to team N ...")
	int m_TeamInvitePending = -1;
	int64_t m_TeamInviteStartTime = 0;
	float m_TeamInviteProgress = 0.0f;
	char m_aTeamInviteInviter[32] = "";
	bool m_TeamInviteActive = false;
	bool m_HasInviteButtons = false;
	CUIRect m_LastInviteAcceptButton{};
	CUIRect m_LastInviteIgnoreButton{};
	void ResolveTeamInvite(bool Accept);
	static void ConTestTeamInvite(IConsole::IResult *pResult, void *pUserData);
	bool m_HasLastIslandRect = false;
	bool m_HasLastVoteRect = false;
	bool m_HasLastIslandButtons = false;
	vec2 m_HudEditorMouse = vec2(0.0f, 0.0f);
	vec2 m_HudEditorDragOffset = vec2(0.0f, 0.0f);
	vec2 m_IslandInteractMouse = vec2(0.0f, 0.0f);
	int64_t m_HudEditorLastClickTime = 0;
	int m_HudEditorLastClickTarget = 0;
	bool m_HudEditorMouseInitialized = false;
	bool m_IslandInteractActive = false;
	bool m_IslandInteractMouseInitialized = false;
	bool m_IslandExpanded = false;
	bool m_IslandVolumeOpen = false;
	float m_IslandHoverProgress = 0.0f;
	float m_IslandExpandProgress = 0.0f;
	float m_IslandVolumeProgress = 0.0f;
	float m_AnimatedIslandWidth = 0.0f;
	float m_AnimatedIslandHeight = 0.0f;
	float m_AnimatedIslandRadius = 0.0f;
	bool m_IslandGeometryInitialized = false;
	std::array<float, 5> m_aIslandButtonHoverProgress{};
	std::array<float, 2> m_aInviteButtonHoverProgress{};
	float m_IslandVolumeHoverProgress = 0.0f;
	int m_HudEditorDragTarget = 0;

	struct SSdfRoundedRectStyle
	{
		ColorRGBA m_Fill;
		ColorRGBA m_Stroke = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
		ColorRGBA m_Glow = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
		float m_Radius = 0.0f;
		float m_StrokeWidth = 0.0f;
		float m_GlowSize = 0.0f;
		float m_Softness = 1.0f;
		float m_Progress = 1.0f;
	};

	void RenderSdfRoundedRect(const CUIRect &Rect, const SSdfRoundedRectStyle &Style);
	void RenderSmoothPill(const CUIRect &Rect, ColorRGBA Color);
	void RenderSmoothRoundedRect(const CUIRect &Rect, ColorRGBA Color, float Radius);
	void RenderSoftVisualizerBar(const CUIRect &Rect, ColorRGBA Color, float Alpha, float PixelWidth, float PixelHeight, float GlowStrength);
	void RenderVoteExample(const CUIRect &Rect);
	void RenderHudEditorOutline(const CUIRect &Rect, float Radius, bool Active, float PixelHeight);
	void ClearInteractionGeometry();
	void CloseInteraction();
	void ResetTransientState();

public:
	~CMediaIsland() override;
	int Sizeof() const override { return sizeof(*this); }
	bool ShouldHideDefaultTimer() const { return m_ActivationProgress > 0.05f; }
	void NotifyTeamInvite(int Team, const char *pInviter);
	void OnInit() override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	bool OnInput(const IInput::CEvent &Event) override;
	void OnRender() override;
	void OnShutdown() override;
	void OnConsoleInit() override;
};

#endif
