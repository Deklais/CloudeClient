#include "cloude_input.h"

#include <base/math.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>

#include <algorithm>
#include <cmath>

namespace CloudeInput
{

	bool IsTickMode()
	{
		return g_Config.m_TcFastInputMode == 0 || g_Config.m_TcFastInputMode == 2 || g_Config.m_TcFastInputMode == 3;
	}

	bool IsBestMode()
	{
		return g_Config.m_TcFastInputMode == 2;
	}

	bool IsBestPlusMode()
	{
		return g_Config.m_TcFastInputMode == 3;
	}

	bool IsBestTargetMode()
	{
		return IsBestMode() || IsBestPlusMode();
	}

	static int LegacyBestPlusAmountMs(const CGameClient *pGameClient)
	{
		int Ping = 40;
		if(pGameClient && pGameClient->m_Snap.m_pLocalInfo)
			Ping = std::clamp(pGameClient->m_Snap.m_pLocalInfo->m_Latency, 0, 160);

		const float Aggression = std::clamp(g_Config.m_TcFastInputBestPlusAggression, 0, 100) / 100.0f;
		const float Stable = 14.0f + Ping / 10.0f;
		const float Sharp = 20.0f + Ping / 5.0f;
		return std::clamp(round_to_int(mix(Stable, Sharp, Aggression)), 14, 60);
	}

	static bool MovementBoostActive(const CGameClient *pGameClient)
	{
		if(!g_Config.m_TcFastInputBestPlusDynamicBoost || !pGameClient)
			return false;
		const int Dummy = g_Config.m_ClDummy;
		return pGameClient->Client()->GameTick(Dummy) < pGameClient->m_Controls.m_aBestPlusDynamicMovementBoostUntil[Dummy];
	}

	static bool AimHookBoostActive(const CGameClient *pGameClient)
	{
		if(!g_Config.m_TcFastInputBestPlusDynamicBoost || !pGameClient)
			return false;
		const int Dummy = g_Config.m_ClDummy;
		return pGameClient->Client()->GameTick(Dummy) < pGameClient->m_Controls.m_aBestPlusDynamicAimHookBoostUntil[Dummy];
	}

	int MovementAmountMs(const CGameClient *pGameClient)
	{
		if(IsBestPlusMode())
		{
			int Amount = std::clamp(g_Config.m_TcFastInputBestPlusMovement, 14, 60);
			if(MovementBoostActive(pGameClient))
				Amount += 8;
			return std::clamp(Amount, 14, 60);
		}

		int Amount = IsBestMode() ? g_Config.m_TcFastInputBestAmount : g_Config.m_TcFastInputAmount;
		Amount = std::clamp(Amount, 1, 60);
		if(MovementBoostActive(pGameClient))
			Amount += 8;
		return std::clamp(Amount, 1, 60);
	}

	int AimHookAmountMs(const CGameClient *pGameClient)
	{
		if(IsBestPlusMode())
		{
			int Amount = std::clamp(g_Config.m_TcFastInputBestPlusAimHook, 14, 60);
			if(AimHookBoostActive(pGameClient))
				Amount += 5;
			return std::clamp(Amount, 14, 60);
		}
		return MovementAmountMs(pGameClient);
	}

	int AmountMs(const CGameClient *pGameClient)
	{
		if(IsBestPlusMode() && g_Config.m_TcFastInputBestPlusMovement == 32 && g_Config.m_TcFastInputBestPlusAimHook == 28)
			return LegacyBestPlusAmountMs(pGameClient);
		return MovementAmountMs(pGameClient);
	}

	float LeadAmountTicks(const CGameClient *pGameClient)
	{
		if(!g_Config.m_TcFastInput)
			return 0.0f;
		if(IsTickMode())
			return AmountMs(pGameClient) / 20.0f;
		float Amount = g_Config.m_TcFastInputSaikoAmount;
		if(MovementBoostActive(pGameClient))
			Amount += 0.40f;
		if(AimHookBoostActive(pGameClient))
			Amount += 0.25f;
		return std::clamp(Amount, 0.0f, 3.65f);
	}

	int PredictionTicks(const CGameClient *pGameClient)
	{
		if(!g_Config.m_TcFastInput)
			return 0;
		if(IsTickMode())
			return (MovementAmountMs(pGameClient) + 19) / 20;
		return (int)std::ceil(LeadAmountTicks(pGameClient));
	}

	int AimHookPredictionTicks(const CGameClient *pGameClient)
	{
		if(!g_Config.m_TcFastInput)
			return 0;
		if(IsTickMode())
			return (AimHookAmountMs(pGameClient) + 19) / 20;
		return (int)std::ceil(LeadAmountTicks(pGameClient));
	}

	void ApplyLead(int &Tick, float &Intra, const CGameClient *pGameClient)
	{
		if(!g_Config.m_TcFastInput)
			return;

		if(IsTickMode())
		{
			const int FastInputAmount = MovementAmountMs(pGameClient);
			const float FastInputIntra = (FastInputAmount % 20) / 20.0f;
			const int BaseFastInputTicks = FastInputAmount / 20;
			float IntraCarry = 0.0f;
			Intra = std::modf(Intra + FastInputIntra, &IntraCarry);
			Tick += BaseFastInputTicks + (int)IntraCarry;
			return;
		}

		const float TotalTick = (Tick - 1) + Intra + LeadAmountTicks(pGameClient);
		Tick = (int)TotalTick + 1;
		Intra = TotalTick - (int)TotalTick;
		if(Intra < 0.0f && Tick > 0)
		{
			Tick -= 1;
			Intra += 1.0f;
		}
	}

	void ApplyMouseTarget(CNetObj_PlayerInput &Input, vec2 MousePos, bool ScaleMouseDistance, bool Spectating, int MaxMouseDistance)
	{
		if(ScaleMouseDistance && !Spectating && MaxMouseDistance > 5 && MaxMouseDistance < 1000)
			MousePos *= 1000.0f / (float)MaxMouseDistance;

		Input.m_TargetX = (int)MousePos.x;
		Input.m_TargetY = (int)MousePos.y;
		if(!Input.m_TargetX && !Input.m_TargetY)
			Input.m_TargetX = 1;
	}

	void KeepMovementOnly(CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &BaseInput)
	{
		Input.m_TargetX = BaseInput.m_TargetX;
		Input.m_TargetY = BaseInput.m_TargetY;
		Input.m_Hook = BaseInput.m_Hook;
		Input.m_Fire = BaseInput.m_Fire;
		Input.m_WantedWeapon = BaseInput.m_WantedWeapon;
		Input.m_NextWeapon = BaseInput.m_NextWeapon;
		Input.m_PrevWeapon = BaseInput.m_PrevWeapon;
	}

}

bool CGameClient::GetDummyFastInput(CNetObj_PlayerInput &DummyFastInput, const CNetObj_PlayerInput *pDummyInputData, const CCharacter *pDummyChar, int LocalTee, int DummyTee) const
{
	if(!PredictDummy() || !pDummyChar)
		return false;

	if(g_Config.m_ClDummyHammer)
	{
		DummyFastInput = m_HammerInput;
		return true;
	}

	if(g_Config.m_ClDummyCopyMoves)
	{
		DummyFastInput = m_Controls.m_aFastInput[LocalTee];
		DummyFastInput.m_Fire = m_Controls.m_aFastInput[DummyTee].m_Fire;
		DummyFastInput.m_WantedWeapon = m_Controls.m_aFastInput[DummyTee].m_WantedWeapon;
		DummyFastInput.m_NextWeapon = m_Controls.m_aFastInput[DummyTee].m_NextWeapon;
		DummyFastInput.m_PrevWeapon = m_Controls.m_aFastInput[DummyTee].m_PrevWeapon;
		if(g_Config.m_ClDummyControl)
		{
			const CNetObj_PlayerInput BaseDummyInput = pDummyInputData ? *pDummyInputData : CNetObj_PlayerInput{};
			DummyFastInput.m_Jump = BaseDummyInput.m_Jump;
			DummyFastInput.m_Fire = BaseDummyInput.m_Fire;
			DummyFastInput.m_Hook = BaseDummyInput.m_Hook;
		}
		return true;
	}

	if(g_Config.m_ClDummyControl)
	{
		const CNetObj_PlayerInput BaseDummyInput = pDummyInputData ? *pDummyInputData : CNetObj_PlayerInput{};
		DummyFastInput = BaseDummyInput;
		DummyFastInput.m_Direction = m_Controls.m_aFastInput[DummyTee].m_Direction;
		DummyFastInput.m_PlayerFlags = m_Controls.m_aFastInput[DummyTee].m_PlayerFlags;
		DummyFastInput.m_TargetX = m_Controls.m_aFastInput[DummyTee].m_TargetX;
		DummyFastInput.m_TargetY = m_Controls.m_aFastInput[DummyTee].m_TargetY;
		DummyFastInput.m_WantedWeapon = m_Controls.m_aFastInput[DummyTee].m_WantedWeapon;
		DummyFastInput.m_NextWeapon = m_Controls.m_aFastInput[DummyTee].m_NextWeapon;
		DummyFastInput.m_PrevWeapon = m_Controls.m_aFastInput[DummyTee].m_PrevWeapon;
		return true;
	}

	return false;
}

vec2 CGameClient::GetFastInputPos(int ClientId)
{
	float PredIntraTick = Client()->PredIntraGameTick(g_Config.m_ClDummy);
	int PredTick = Client()->PredGameTick(g_Config.m_ClDummy);

	vec2 Pos = mix(m_aClients[ClientId].m_PrevPredicted.m_Pos, m_aClients[ClientId].m_Predicted.m_Pos, PredIntraTick);
	float FinalIntra = PredIntraTick;
	int FinalTick = PredTick;
	CloudeInput::ApplyLead(FinalTick, FinalIntra, this);
	const int FastInputTicks = CloudeInput::PredictionTicks(this);

	if(FinalTick > 0 &&
		m_aClients[ClientId].m_aPredTick[(FinalTick - 1) % 200] >= Client()->PrevGameTick(g_Config.m_ClDummy) &&
		m_aClients[ClientId].m_aPredTick[FinalTick % 200] <= Client()->PredGameTick(g_Config.m_ClDummy) + FastInputTicks)
	{
		Pos = mix(m_aClients[ClientId].m_aPredPos[(FinalTick - 1) % 200], m_aClients[ClientId].m_aPredPos[FinalTick % 200], FinalIntra);
	}

	return Pos;
}
