#ifndef GAME_CLIENT_CLOUDE_INPUT_H
#define GAME_CLIENT_CLOUDE_INPUT_H

#include <base/vmath.h>

#include <generated/protocol.h>

class CGameClient;

namespace CloudeInput
{
	bool IsTickMode();
	bool IsBestMode();
	bool IsBestPlusMode();
	bool IsBestTargetMode();
	int AmountMs(const CGameClient *pGameClient = nullptr);
	int MovementAmountMs(const CGameClient *pGameClient = nullptr);
	int AimHookAmountMs(const CGameClient *pGameClient = nullptr);
	float LeadAmountTicks(const CGameClient *pGameClient = nullptr);
	int PredictionTicks(const CGameClient *pGameClient = nullptr);
	int AimHookPredictionTicks(const CGameClient *pGameClient = nullptr);
	void ApplyLead(int &Tick, float &Intra, const CGameClient *pGameClient = nullptr);
	void ApplyMouseTarget(CNetObj_PlayerInput &Input, vec2 MousePos, bool ScaleMouseDistance, bool Spectating, int MaxMouseDistance);
	void KeepMovementOnly(CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &BaseInput);
}

#endif // GAME_CLIENT_CLOUDE_INPUT_H
