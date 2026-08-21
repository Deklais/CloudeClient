/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/cloude_input.h>
#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/ui.h>
#include <game/collision.h>

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	mem_zero(&m_aFastInput, sizeof(m_aFastInput));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
	std::fill(std::begin(m_aFirePressed), std::end(m_aFirePressed), false);
	std::fill(std::begin(m_aBestPlusDynamicMovementBoostUntil), std::end(m_aBestPlusDynamicMovementBoostUntil), -1);
	std::fill(std::begin(m_aBestPlusDynamicAimHookBoostUntil), std::end(m_aBestPlusDynamicAimHookBoostUntil), -1);
	std::fill(std::begin(m_aSkillAssistLastDirection), std::end(m_aSkillAssistLastDirection), 0);
	std::fill(std::begin(m_aSkillAssistHookBufferUntil), std::end(m_aSkillAssistHookBufferUntil), -1);
	std::fill(std::begin(m_aSkillAssistJumpBufferUntil), std::end(m_aSkillAssistJumpBufferUntil), -1);
	std::fill(std::begin(m_aAutoFireNext), std::end(m_aAutoFireNext), 0);
	std::fill(std::begin(m_aAutoSwapGunHammerStage), std::end(m_aAutoSwapGunHammerStage), 0);
	std::fill(std::begin(m_aAutoSwapReturnGunPending), std::end(m_aAutoSwapReturnGunPending), false);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];
	m_aFastInput[Dummy] = m_aInputData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
	m_aFirePressed[Dummy] = false;
	m_aBestPlusDynamicMovementBoostUntil[Dummy] = -1;
	m_aBestPlusDynamicAimHookBoostUntil[Dummy] = -1;
	m_aSkillAssistLastDirection[Dummy] = 0;
	m_aSkillAssistHookBufferUntil[Dummy] = -1;
	m_aSkillAssistJumpBufferUntil[Dummy] = -1;
	m_aAutoFireNext[Dummy] = 0;
	m_aAutoSwapGunHammerStage[Dummy] = 0;
	m_aAutoSwapReturnGunPending[Dummy] = false;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

int CControls::CloudeCurrentWeapon(int Dummy) const
{
	int ClientId = GameClient()->m_aLocalIds[Dummy];
	if(Dummy == g_Config.m_ClDummy)
		ClientId = GameClient()->m_Snap.m_LocalClientId;
	CCharacter *pCharacter = ClientId >= 0 ? GameClient()->m_PredictedWorld.GetCharacterById(ClientId) : nullptr;
	if(pCharacter)
		return pCharacter->GetActiveWeapon();
	if(Dummy == g_Config.m_ClDummy && GameClient()->m_Snap.m_pLocalCharacter)
		return GameClient()->m_Snap.m_pLocalCharacter->m_Weapon;
	return -1;
}

void CControls::ConKeyInputFireCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	const int Dummy = g_Config.m_ClDummy;
	const bool Pressed = pResult->GetInteger(0) != 0;
	pState->m_pControls->m_aFirePressed[Dummy] = Pressed;

	int *pVariable = pState->m_apVariables[Dummy];
	if(Pressed && g_Config.m_TcAutoSwapGunHammer && pState->m_pControls->CloudeCurrentWeapon(Dummy) == WEAPON_GUN)
	{
		pState->m_pControls->m_aInputData[Dummy].m_WantedWeapon = WEAPON_HAMMER + 1;
		pState->m_pControls->m_aAutoSwapGunHammerStage[Dummy] = 1;
		if((*pVariable & 1) != 0)
			(*pVariable)++;
		*pVariable &= INPUT_STATE_MASK;
		return;
	}

	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputFireCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

void CControls::ApplySkillAssist(CNetObj_PlayerInput &Input, int Dummy, bool InjectBufferedPress)
{
	const int CurrentTick = Client()->GameTick(Dummy);
	if(g_Config.m_TcMicroDirectionAssist)
	{
		if(Input.m_Direction != 0)
			m_aSkillAssistLastDirection[Dummy] = Input.m_Direction;
		else if(m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy] && m_aSkillAssistLastDirection[Dummy] != 0)
			Input.m_Direction = m_aSkillAssistLastDirection[Dummy];
	}
	else if(Input.m_Direction != 0)
	{
		m_aSkillAssistLastDirection[Dummy] = Input.m_Direction;
	}

	if(g_Config.m_TcHookTimingBuffer)
	{
		if(Input.m_Hook != 0)
			m_aSkillAssistHookBufferUntil[Dummy] = CurrentTick + 2;
		else if(InjectBufferedPress && CurrentTick <= m_aSkillAssistHookBufferUntil[Dummy])
		{
			Input.m_Hook = 1;
			m_aSkillAssistHookBufferUntil[Dummy] = -1;
		}
		else
			m_aSkillAssistHookBufferUntil[Dummy] = -1;
	}
	else
	{
		m_aSkillAssistHookBufferUntil[Dummy] = -1;
	}

	if(g_Config.m_TcJumpBuffer)
	{
		int ClientId = GameClient()->m_aLocalIds[Dummy];
		if(Dummy == g_Config.m_ClDummy)
			ClientId = GameClient()->m_Snap.m_LocalClientId;
		CCharacter *pCharacter = ClientId >= 0 ? GameClient()->m_PredictedWorld.GetCharacterById(ClientId) : nullptr;
		const bool Grounded = pCharacter && pCharacter->IsGrounded();

		if(Input.m_Jump != 0)
		{
			if(!Grounded)
				m_aSkillAssistJumpBufferUntil[Dummy] = CurrentTick + 2;
			else
				m_aSkillAssistJumpBufferUntil[Dummy] = -1;
		}
		else if(InjectBufferedPress && Grounded && CurrentTick <= m_aSkillAssistJumpBufferUntil[Dummy])
		{
			Input.m_Jump = 1;
			m_aSkillAssistJumpBufferUntil[Dummy] = -1;
		}
		else
			m_aSkillAssistJumpBufferUntil[Dummy] = -1;
	}
	else
	{
		m_aSkillAssistJumpBufferUntil[Dummy] = -1;
	}
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if(g_Config.m_TcHideChatBubbles && Client()->RconAuthed())
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		m_aAutoFireNext[g_Config.m_ClDummy] = 0;
		m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 0;
		m_aAutoSwapReturnGunPending[g_Config.m_ClDummy] = false;
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		ApplySkillAssist(m_aInputData[g_Config.m_ClDummy], g_Config.m_ClDummy, false);

		if(g_Config.m_TcAutoFire && (m_aInputData[g_Config.m_ClDummy].m_Fire & 1) != 0)
		{
			const int64_t Now = time_get();
			const int Speed = std::clamp(g_Config.m_TcAutoFireSpeed, 1, 25);
			const int64_t Interval = time_freq() / Speed;
			if(m_aAutoFireNext[g_Config.m_ClDummy] == 0)
			{
				m_aAutoFireNext[g_Config.m_ClDummy] = Now + Interval;
			}
			else if(Now >= m_aAutoFireNext[g_Config.m_ClDummy])
			{
				m_aInputData[g_Config.m_ClDummy].m_Fire = (m_aInputData[g_Config.m_ClDummy].m_Fire + 2) & INPUT_STATE_MASK;
				m_aAutoFireNext[g_Config.m_ClDummy] = Now + Interval;
			}
		}
		else
		{
			m_aAutoFireNext[g_Config.m_ClDummy] = 0;
		}

		if(g_Config.m_TcAutoSwapGunHammer && GameClient()->m_Snap.m_pLocalCharacter)
		{
			const bool FireHeld = m_aFirePressed[g_Config.m_ClDummy];
			const bool FireWasHeld = (m_aLastData[g_Config.m_ClDummy].m_Fire & 1) != 0;
			const int CurrentWeapon = CloudeCurrentWeapon(g_Config.m_ClDummy);

			if(FireHeld && m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] == 1)
			{
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = WEAPON_HAMMER + 1;
				if((m_aInputData[g_Config.m_ClDummy].m_Fire & 1) != 0)
					m_aInputData[g_Config.m_ClDummy].m_Fire = (m_aInputData[g_Config.m_ClDummy].m_Fire + 1) & INPUT_STATE_MASK;
				m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 2;
			}
			else if(FireHeld && m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] == 2)
			{
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = WEAPON_HAMMER + 1;
				m_aAutoSwapReturnGunPending[g_Config.m_ClDummy] = g_Config.m_TcAutoSwapReturnGun != 0;
				if((m_aInputData[g_Config.m_ClDummy].m_Fire & 1) == 0)
					m_aInputData[g_Config.m_ClDummy].m_Fire = (m_aInputData[g_Config.m_ClDummy].m_Fire + 1) & INPUT_STATE_MASK;
				m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 0;
			}
			else if(FireHeld && !FireWasHeld && CurrentWeapon == WEAPON_GUN)
			{
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = WEAPON_HAMMER + 1;
				if((m_aInputData[g_Config.m_ClDummy].m_Fire & 1) != 0)
					m_aInputData[g_Config.m_ClDummy].m_Fire = (m_aInputData[g_Config.m_ClDummy].m_Fire + 1) & INPUT_STATE_MASK;
				m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 2;
			}
			else if(!FireHeld && FireWasHeld && m_aAutoSwapReturnGunPending[g_Config.m_ClDummy])
			{
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = WEAPON_GUN + 1;
				m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 0;
				m_aAutoSwapReturnGunPending[g_Config.m_ClDummy] = false;
			}
			else if(!FireHeld)
				m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 0;
		}
		else
		{
			m_aAutoSwapGunHammerStage[g_Config.m_ClDummy] = 0;
			m_aAutoSwapReturnGunPending[g_Config.m_ClDummy] = false;
		}

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	UpdateFastInputAutoTune();

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}

	RenderFastInputAutoTune();
	RenderInputDoctor();
}

void CControls::StartFastInputAutoTune(EFastInputAutoTune Type)
{
	m_FastInputAutoTune = SFastInputAutoTune{};
	m_FastInputAutoTune.m_Type = Type;
	m_FastInputAutoTune.m_StartTime = time_get();
	m_FastInputAutoTune.m_LastSampleTime = m_FastInputAutoTune.m_StartTime - time_freq();
	const int Dummy = g_Config.m_ClDummy;
	m_FastInputAutoTune.m_LastDirection = (m_aInputDirectionRight[Dummy] != 0) - (m_aInputDirectionLeft[Dummy] != 0);
	m_FastInputAutoTune.m_LastJump = m_aInputData[Dummy].m_Jump;
	m_FastInputAutoTune.m_LastHook = m_aInputData[Dummy].m_Hook;
	m_FastInputAutoTune.m_LastMousePos = m_aMousePos[Dummy];
	str_format(m_FastInputAutoTune.m_aStatus, sizeof(m_FastInputAutoTune.m_aStatus), "%s auto tuning started", Type == EFastInputAutoTune::MOVEMENT ? "Movement" : "Aim/Hook");
}

void CControls::UpdateFastInputAutoTune()
{
	if(!FastInputAutoTuneRunning())
		return;

	const int64_t Now = time_get();
	const int64_t Freq = time_freq();
	if(!GameClient()->m_Snap.m_pLocalInfo)
	{
		m_FastInputAutoTune.m_Type = EFastInputAutoTune::NONE;
		str_copy(m_FastInputAutoTune.m_aStatus, "Auto tuning cancelled: join a server first");
		return;
	}

	if(Now - m_FastInputAutoTune.m_LastSampleTime >= Freq / 20)
	{
		const int Dummy = g_Config.m_ClDummy;
		const int Ping = std::clamp(GameClient()->m_Snap.m_pLocalInfo->m_Latency, 0, 999);
		m_FastInputAutoTune.m_Samples++;
		m_FastInputAutoTune.m_PingSum += Ping;
		m_FastInputAutoTune.m_PingMin = minimum(m_FastInputAutoTune.m_PingMin, Ping);
		m_FastInputAutoTune.m_PingMax = maximum(m_FastInputAutoTune.m_PingMax, Ping);

		const int Direction = (m_aInputDirectionRight[Dummy] != 0) - (m_aInputDirectionLeft[Dummy] != 0);
		const int Jump = m_aInputData[Dummy].m_Jump;
		const int Hook = m_aInputData[Dummy].m_Hook;
		if(Direction != 0 || Jump)
			m_FastInputAutoTune.m_MovementFrames++;
		if(Direction != m_FastInputAutoTune.m_LastDirection || Jump != m_FastInputAutoTune.m_LastJump)
			m_FastInputAutoTune.m_MovementChanges++;
		if(Hook)
			m_FastInputAutoTune.m_HookFrames++;
		if(Hook && !m_FastInputAutoTune.m_LastHook)
			m_FastInputAutoTune.m_HookPresses++;

		m_FastInputAutoTune.m_AimTravel += minimum(distance(m_aMousePos[Dummy], m_FastInputAutoTune.m_LastMousePos), 600.0f);
		m_FastInputAutoTune.m_LastDirection = Direction;
		m_FastInputAutoTune.m_LastJump = Jump;
		m_FastInputAutoTune.m_LastHook = Hook;
		m_FastInputAutoTune.m_LastMousePos = m_aMousePos[Dummy];
		m_FastInputAutoTune.m_LastSampleTime = Now;
	}

	if(Now - m_FastInputAutoTune.m_StartTime >= Freq * 10)
		FinishFastInputAutoTune();
}

void CControls::FinishFastInputAutoTune()
{
	if(m_FastInputAutoTune.m_Samples <= 0)
	{
		str_copy(m_FastInputAutoTune.m_aStatus, "Auto tuning failed: no samples");
		m_FastInputAutoTune.m_Type = EFastInputAutoTune::NONE;
		return;
	}

	const int AvgPing = m_FastInputAutoTune.m_PingSum / m_FastInputAutoTune.m_Samples;
	const int Jitter = m_FastInputAutoTune.m_PingMax - m_FastInputAutoTune.m_PingMin;
	const float MovementActivity = m_FastInputAutoTune.m_MovementFrames / (float)m_FastInputAutoTune.m_Samples;
	const float HookActivity = m_FastInputAutoTune.m_HookFrames / (float)m_FastInputAutoTune.m_Samples;
	const float AimTravelPerSample = m_FastInputAutoTune.m_AimTravel / (float)m_FastInputAutoTune.m_Samples;
	const float PingScore = std::clamp(AvgPing / 80.0f, 0.0f, 1.0f);
	const float StabilityScore = std::clamp(1.0f - Jitter / 45.0f, 0.0f, 1.0f);

	if(m_FastInputAutoTune.m_Type == EFastInputAutoTune::MOVEMENT)
	{
		const float ChangeRate = std::clamp(m_FastInputAutoTune.m_MovementChanges / (float)m_FastInputAutoTune.m_Samples, 0.0f, 0.35f);
		const float ActivityScore = std::clamp(MovementActivity * 0.75f + ChangeRate * 1.25f, 0.0f, 1.0f);
		const int Amount = std::clamp(round_to_int(18.0f + PingScore * 16.0f + StabilityScore * 8.0f + ActivityScore * 14.0f), 14, 60);
		g_Config.m_TcFastInputBestPlusMovement = Amount;
		str_format(m_FastInputAutoTune.m_aStatus, sizeof(m_FastInputAutoTune.m_aStatus), "Movement applied: %d ms (avg %d, jitter %d)", Amount, AvgPing, Jitter);
	}
	else if(m_FastInputAutoTune.m_Type == EFastInputAutoTune::AIM_HOOK)
	{
		const float HookPressScore = std::clamp(m_FastInputAutoTune.m_HookPresses / 8.0f, 0.0f, 1.0f);
		const float AimScore = std::clamp(AimTravelPerSample / 85.0f, 0.0f, 1.0f);
		const float HookScore = std::clamp(HookActivity * 0.5f + HookPressScore * 0.35f + AimScore * 0.15f, 0.0f, 1.0f);
		const int Amount = std::clamp(round_to_int(16.0f + PingScore * 14.0f + StabilityScore * 7.0f + HookScore * 13.0f), 14, 60);
		g_Config.m_TcFastInputBestPlusAimHook = Amount;
		str_format(m_FastInputAutoTune.m_aStatus, sizeof(m_FastInputAutoTune.m_aStatus), "Aim/Hook applied: %d ms (avg %d, jitter %d)", Amount, AvgPing, Jitter);
	}

	m_FastInputAutoTune.m_Type = EFastInputAutoTune::NONE;
}

void CControls::RenderFastInputAutoTune()
{
	if(!FastInputAutoTuneRunning())
		return;

	const float Height = 300.0f;
	const float Width = Height * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, Width, Height);

	const int Remaining = std::clamp(10 - (int)((time_get() - m_FastInputAutoTune.m_StartTime) / time_freq()), 0, 10);
	char aBuf[160];
	if(m_FastInputAutoTune.m_Type == EFastInputAutoTune::MOVEMENT)
		str_format(aBuf, sizeof(aBuf), "Movement auto tuning: %ds left - move and jump", Remaining);
	else
		str_format(aBuf, sizeof(aBuf), "Aim/Hook auto tuning: %ds left - hook and aim", Remaining);

	const float FontSize = 10.0f;
	const float TextWidth = TextRender()->TextWidth(FontSize, aBuf);
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
	TextRender()->Text(Width / 2.0f - TextWidth / 2.0f, 42.0f, FontSize, aBuf);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CControls::RenderInputDoctor()
{
	if(!g_Config.m_TcInputDoctor || Client()->State() != IClient::STATE_ONLINE || GameClient()->m_Menus.IsActive())
		return;

	const float ScreenHeight = 300.0f;
	const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, ScreenWidth, ScreenHeight);

	CUIRect Panel;
	Panel.x = 8.0f;
	Panel.y = 42.0f;
	Panel.w = 128.0f;
	Panel.h = 68.0f;
	Panel.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.48f), IGraphics::CORNER_ALL, 6.0f);

	CUIRect Content = Panel;
	Content.Margin(6.0f, &Content);

	const int Dummy = g_Config.m_ClDummy;
	const int CurrentTick = Client()->GameTick(Dummy);
	const CNetObj_PlayerInput &Real = m_aInputData[Dummy];
	const CNetObj_PlayerInput &Fast = m_aFastInput[Dummy];

	const char *pMode = "off";
	if(g_Config.m_TcFastInput)
	{
		switch(g_Config.m_TcFastInputMode)
		{
		case 0: pMode = "tater"; break;
		case 1: pMode = "saiko"; break;
		case 2: pMode = "cloude"; break;
		case 3: pMode = "cloude+"; break;
		default: pMode = "?"; break;
		}
	}

	char aLine[160];
	const float FontSize = 6.5f;
	auto RenderLine = [&](const char *pText) {
		CUIRect Row;
		Content.HSplitTop(9.0f, &Row, &Content);
		Ui()->DoLabel(&Row, pText, FontSize, TEXTALIGN_ML);
	};

	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.92f));
	RenderLine("Input Doctor");
	str_format(aLine, sizeof(aLine), "real  H:%d J:%d F:%d D:%d", Real.m_Hook != 0, Real.m_Jump != 0, Real.m_Fire & 1, Real.m_Direction);
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "fast  H:%d J:%d F:%d D:%d", Fast.m_Hook != 0, Fast.m_Jump != 0, Fast.m_Fire & 1, Fast.m_Direction);
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "mode  %s  M:%d A:%d ticks", pMode, CloudeInput::PredictionTicks(GameClient()), CloudeInput::AimHookPredictionTicks(GameClient()));
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "ms    M:%d  A:%d", CloudeInput::MovementAmountMs(GameClient()), CloudeInput::AimHookAmountMs(GameClient()));
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "buf   hook:%d jump:%d boost:%d/%d",
		maximum(0, m_aSkillAssistHookBufferUntil[Dummy] - CurrentTick),
		maximum(0, m_aSkillAssistJumpBufferUntil[Dummy] - CurrentTick),
		maximum(0, m_aBestPlusDynamicMovementBoostUntil[Dummy] - CurrentTick),
		maximum(0, m_aBestPlusDynamicAimHookBoostUntil[Dummy] - CurrentTick));
	RenderLine(aLine);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return false;

	if(GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive() || !(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
		{
			m_aFastInput[Dummy] = m_aInputData[Dummy];
			m_aBestPlusDynamicMovementBoostUntil[Dummy] = -1;
			m_aBestPlusDynamicAimHookBoostUntil[Dummy] = -1;
			m_aSkillAssistHookBufferUntil[Dummy] = -1;
			m_aSkillAssistJumpBufferUntil[Dummy] = -1;
		}
		m_FastInputHookAction = false;
		m_FastInputFireAction = false;
		return false;
	}

	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;

			const int CurrentTick = Client()->GameTick(Dummy);
			ApplySkillAssist(TestInput, Dummy, true);
			if(m_aInputData[Dummy].m_Hook != 0)
				TestInput.m_Hook = m_aInputData[Dummy].m_Hook;
			else if(m_aFastInput[Dummy].m_Hook != 0 && CurrentTick > m_aSkillAssistHookBufferUntil[Dummy])
				TestInput.m_Hook = 0;

			if(CloudeInput::IsBestTargetMode())
			{
				const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
				CloudeInput::ApplyMouseTarget(TestInput, m_aMousePos[Dummy], g_Config.m_TcScaleMouseDistance, GameClient()->m_Snap.m_SpecInfo.m_Active, MaxDistance);
			}
		}

		if(Dummy == g_Config.m_ClDummy && g_Config.m_TcFastInputBestPlusDynamicBoost && g_Config.m_TcFastInput)
		{
			const int CurrentTick = Client()->GameTick(g_Config.m_ClDummy);
			const bool DirectionChanged = m_aFastInput[Dummy].m_Direction != TestInput.m_Direction;
			const bool JumpPressed = m_aFastInput[Dummy].m_Jump != TestInput.m_Jump && TestInput.m_Jump != 0;
			const bool HookPressed = m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook != 0;
			if(DirectionChanged || JumpPressed)
				m_aBestPlusDynamicMovementBoostUntil[Dummy] = maximum(m_aBestPlusDynamicMovementBoostUntil[Dummy], CurrentTick + 2);
			if(HookPressed)
				m_aBestPlusDynamicAimHookBoostUntil[Dummy] = maximum(m_aBestPlusDynamicAimHookBoostUntil[Dummy], CurrentTick + 1);
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;
		if(Dummy == g_Config.m_ClDummy && CloudeInput::IsBestTargetMode() && (m_aFastInput[Dummy].m_TargetX != TestInput.m_TargetX || m_aFastInput[Dummy].m_TargetY != TestInput.m_TargetY))
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == g_Config.m_ClDummy)
		{
			if(CloudeInput::IsBestTargetMode())
			{
				SetMousePos = true;
			}
			else if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			if(CloudeInput::IsBestTargetMode())
			{
				const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
				CloudeInput::ApplyMouseTarget(TestInput, m_aMousePos[Dummy], g_Config.m_TcScaleMouseDistance, GameClient()->m_Snap.m_SpecInfo.m_Active, MaxDistance);
			}
			else
			{
				TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
				TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
			}
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	if(NewInput[0] || NewInput[1])
		return true;
	else
		return false;
}
