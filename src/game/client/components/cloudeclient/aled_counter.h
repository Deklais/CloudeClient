#ifndef GAME_CLIENT_COMPONENTS_CLOUDECLIENT_ALED_COUNTER_H
#define GAME_CLIENT_COMPONENTS_CLOUDECLIENT_ALED_COUNTER_H

#include <game/client/component.h>

class CAledCounter : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void OnInit() override;
	void OnShutdown() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnNewSnapshot() override;
	void OnRender() override;

	int Count() const { return m_AledCount; }
	bool ProfileValid() const { return m_ProfileValid; }

private:
	static constexpr const char *PROFILE_PATH = "tclient/aled_profile.dat";
	static constexpr const char *PROFILE_BACKUP_PATH = "tclient/aled_profile.bak";

	int m_AledCount = 0;
	int m_LastLocalAttackTick = -1;
	int m_LastRegisteredTick = -1;
	int m_PendingAutoRecordStartTick = -1;
	int m_PendingAutoRecordTick = -1;
	int m_LastAutoRecordTick = -1;
	unsigned m_ProfileNonce = 0;
	bool m_ProfileValid = true;
	bool m_Dirty = false;

	unsigned ProfileKey(unsigned Nonce) const;
	unsigned long long ProfileSignature(unsigned Nonce, unsigned EncodedCount, unsigned EncodedMirror) const;
	bool LoadBinaryProfile(const char *pPath, int &Count, unsigned &Nonce) const;
	bool LoadLegacyProfile(int &Count, unsigned &Nonce) const;
	bool LoadProfile();
	void SaveProfile();
	void ScheduleAutoRecord(int GameTick);
	void TrySavePendingAutoRecord();
	bool IsAledHitNow() const;
	bool HasHammerHitEventNear(vec2 TargetPos) const;
	bool TargetWasUnfrozenByHit(int ClientId) const;
	bool HasFreezeBetween(vec2 From, vec2 To) const;
	bool IsFreezeTile(int Tile) const;
};

#endif // GAME_CLIENT_COMPONENTS_CLOUDECLIENT_ALED_COUNTER_H
