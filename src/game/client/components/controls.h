/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];
	bool m_aFirePressed[NUM_DUMMIES];

	// TClient
	CNetObj_PlayerInput m_aFastInput[NUM_DUMMIES];
	int m_aBestPlusDynamicMovementBoostUntil[NUM_DUMMIES];
	int m_aBestPlusDynamicAimHookBoostUntil[NUM_DUMMIES];
	int m_aSkillAssistLastDirection[NUM_DUMMIES];
	int m_aSkillAssistHookBufferUntil[NUM_DUMMIES];
	int m_aSkillAssistJumpBufferUntil[NUM_DUMMIES];
	int64_t m_aAutoFireNext[NUM_DUMMIES];
	int m_aAutoSwapGunHammerStage[NUM_DUMMIES];
	bool m_aAutoSwapReturnGunPending[NUM_DUMMIES];
	bool m_FastInputHookAction = false;
	bool m_FastInputFireAction = false;
	enum class EFastInputAutoTune
	{
		NONE = 0,
		MOVEMENT,
		AIM_HOOK,
	};

	struct SFastInputAutoTune
	{
		EFastInputAutoTune m_Type = EFastInputAutoTune::NONE;
		int64_t m_StartTime = 0;
		int64_t m_LastSampleTime = 0;
		int m_Samples = 0;
		int m_PingSum = 0;
		int m_PingMin = 9999;
		int m_PingMax = 0;
		int m_MovementFrames = 0;
		int m_MovementChanges = 0;
		int m_HookFrames = 0;
		int m_HookPresses = 0;
		float m_AimTravel = 0.0f;
		int m_LastDirection = 0;
		int m_LastJump = 0;
		int m_LastHook = 0;
		vec2 m_LastMousePos = vec2(0.0f, 0.0f);
		char m_aStatus[128] = "";
	};

	SFastInputAutoTune m_FastInputAutoTune;

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	bool CheckNewInput();
	void StartFastInputAutoTune(EFastInputAutoTune Type);
	bool FastInputAutoTuneRunning() const { return m_FastInputAutoTune.m_Type != EFastInputAutoTune::NONE; }

private:
	void ApplySkillAssist(CNetObj_PlayerInput &Input, int Dummy, bool InjectBufferedPress);
	int CloudeCurrentWeapon(int Dummy) const;
	void UpdateFastInputAutoTune();
	void RenderFastInputAutoTune();
	void RenderInputDoctor();
	void FinishFastInputAutoTune();
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputFireCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
