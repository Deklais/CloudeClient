#include "aled_counter.h"

#include <base/system.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/demo.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/gamecore.h>

namespace
{
	unsigned Fnva32Update(unsigned Hash, const unsigned char *pData, int Size)
	{
		for(int i = 0; i < Size; ++i)
		{
			Hash ^= pData[i];
			Hash *= 16777619u;
		}
		return Hash;
	}

	unsigned long long Fnva64Update(unsigned long long Hash, const unsigned char *pData, int Size)
	{
		for(int i = 0; i < Size; ++i)
		{
			Hash ^= pData[i];
			Hash *= 1099511628211ull;
		}
		return Hash;
	}

	unsigned RotateLeft32(unsigned Value, int Bits)
	{
		return (Value << Bits) | (Value >> (32 - Bits));
	}

	struct SAledProfileDisk
	{
		char m_aMagic[8];
		unsigned m_Version;
		unsigned m_Nonce;
		unsigned m_EncodedCount;
		unsigned m_EncodedMirror;
		unsigned long long m_Signature;
	};

	static_assert(sizeof(SAledProfileDisk) == 32);

	constexpr char ALED_PROFILE_MAGIC[8] = {'C', 'L', 'D', 'A', 'L', 'E', 'D', '1'};
	constexpr unsigned ALED_PROFILE_VERSION = 2;
}

void CAledCounter::OnInit()
{
	LoadProfile();
}

void CAledCounter::OnShutdown()
{
	SaveProfile();
}

void CAledCounter::OnStateChange(int NewState, int OldState)
{
	if(NewState == IClient::STATE_ONLINE && OldState != IClient::STATE_ONLINE)
	{
		m_LastLocalAttackTick = -1;
		m_LastRegisteredTick = -1;
		m_PendingAutoRecordStartTick = -1;
		m_PendingAutoRecordTick = -1;
	}
}

unsigned CAledCounter::ProfileKey(unsigned Nonce) const
{
	unsigned Hash = 2166136261u;
	static constexpr char SALT[] = "Cloude:AledCounter:ProfileKey:v2";
	Hash = Fnva32Update(Hash, reinterpret_cast<const unsigned char *>(SALT), sizeof(SALT) - 1);
	Hash = Fnva32Update(Hash, reinterpret_cast<const unsigned char *>(&Nonce), sizeof(Nonce));
	Hash ^= 0x7f4a7c15u;
	Hash *= 16777619u;
	return Hash;
}

unsigned long long CAledCounter::ProfileSignature(unsigned Nonce, unsigned EncodedCount, unsigned EncodedMirror) const
{
	unsigned long long Hash = 14695981039346656037ull;
	static constexpr char SALT[] = "Cloude:AledCounter:SignedProfile:v2";
	Hash = Fnva64Update(Hash, reinterpret_cast<const unsigned char *>(SALT), sizeof(SALT) - 1);
	Hash = Fnva64Update(Hash, reinterpret_cast<const unsigned char *>(ALED_PROFILE_MAGIC), sizeof(ALED_PROFILE_MAGIC));
	Hash = Fnva64Update(Hash, reinterpret_cast<const unsigned char *>(&ALED_PROFILE_VERSION), sizeof(ALED_PROFILE_VERSION));
	Hash = Fnva64Update(Hash, reinterpret_cast<const unsigned char *>(&Nonce), sizeof(Nonce));
	Hash = Fnva64Update(Hash, reinterpret_cast<const unsigned char *>(&EncodedCount), sizeof(EncodedCount));
	Hash = Fnva64Update(Hash, reinterpret_cast<const unsigned char *>(&EncodedMirror), sizeof(EncodedMirror));
	return Hash;
}

bool CAledCounter::LoadBinaryProfile(const char *pPath, int &Count, unsigned &Nonce) const
{
	void *pData = nullptr;
	unsigned DataSize = 0;
	if(!Storage()->ReadFile(pPath, IStorage::TYPE_SAVE, &pData, &DataSize))
		return false;

	if(DataSize != sizeof(SAledProfileDisk))
	{
		free(pData);
		return false;
	}

	SAledProfileDisk Profile;
	mem_copy(&Profile, pData, sizeof(Profile));
	free(pData);

	if(mem_comp(Profile.m_aMagic, ALED_PROFILE_MAGIC, sizeof(ALED_PROFILE_MAGIC)) != 0 || Profile.m_Version != ALED_PROFILE_VERSION)
		return false;

	if(ProfileSignature(Profile.m_Nonce, Profile.m_EncodedCount, Profile.m_EncodedMirror) != Profile.m_Signature)
		return false;

	const unsigned Key = ProfileKey(Profile.m_Nonce);
	const unsigned DecodedCount = Profile.m_EncodedCount ^ Key ^ 0xa531c97du;
	const unsigned DecodedMirror = Profile.m_EncodedMirror ^ RotateLeft32(Key, 13) ^ 0x4f28d351u;
	if(DecodedMirror != ~DecodedCount || DecodedCount > 100000000u)
		return false;

	Count = (int)DecodedCount;
	Nonce = Profile.m_Nonce;
	return true;
}

bool CAledCounter::LoadLegacyProfile(int &Count, unsigned &Nonce) const
{
	void *pData = nullptr;
	unsigned DataSize = 0;

	if(!Storage()->ReadFile(PROFILE_PATH, IStorage::TYPE_SAVE, &pData, &DataSize))
		return false;

	char *pText = static_cast<char *>(pData);
	int ParsedCount = -1;
	unsigned Signature = 0;
	const int Parsed = sscanf(pText, "aled_count %d\nsignature %x", &ParsedCount, &Signature);
	free(pData);

	if(Parsed != 2 || ParsedCount < 0)
		return false;

	unsigned LegacyHash = 2166136261u;
	static constexpr char LEGACY_SALT[] = "IslandClient:AledCounter:v1";
	LegacyHash = Fnva32Update(LegacyHash, reinterpret_cast<const unsigned char *>(LEGACY_SALT), sizeof(LEGACY_SALT) - 1);
	LegacyHash = Fnva32Update(LegacyHash, reinterpret_cast<const unsigned char *>(&ParsedCount), sizeof(ParsedCount));
	LegacyHash = Fnva32Update(LegacyHash, reinterpret_cast<const unsigned char *>(g_Config.m_PlayerName), str_length(g_Config.m_PlayerName));
	if(LegacyHash != Signature)
		return false;

	Count = ParsedCount;
	Nonce = (unsigned)(time_get() ^ (unsigned long long)ParsedCount ^ 0x64f3b19bu);
	if(Nonce == 0)
		Nonce = 0x9e3779b9u;
	return true;
}

bool CAledCounter::LoadProfile()
{
	m_ProfileValid = true;

	int Count = 0;
	unsigned Nonce = 0;
	if(LoadBinaryProfile(PROFILE_PATH, Count, Nonce) || LoadBinaryProfile(PROFILE_BACKUP_PATH, Count, Nonce) || LoadLegacyProfile(Count, Nonce))
	{
		m_AledCount = Count;
		m_ProfileNonce = Nonce;
		m_Dirty = true;
		SaveProfile();
		return true;
	}

	if(Storage()->FileExists(PROFILE_PATH, IStorage::TYPE_SAVE) || Storage()->FileExists(PROFILE_BACKUP_PATH, IStorage::TYPE_SAVE))
		m_ProfileValid = false;

	m_AledCount = 0;
	m_ProfileNonce = (unsigned)(time_get() ^ 0x5a71edc0u);
	if(m_ProfileNonce == 0)
		m_ProfileNonce = 0x85ebca6bu;
	m_Dirty = true;
	SaveProfile();
	return true;
}

void CAledCounter::SaveProfile()
{
	if(!m_Dirty && m_ProfileValid)
		return;

	Storage()->CreateFolder("tclient", IStorage::TYPE_SAVE);
	if(m_ProfileNonce == 0)
		m_ProfileNonce = (unsigned)(time_get() ^ 0x9e3779b9u);

	SAledProfileDisk Profile{};
	mem_copy(Profile.m_aMagic, ALED_PROFILE_MAGIC, sizeof(ALED_PROFILE_MAGIC));
	Profile.m_Version = ALED_PROFILE_VERSION;
	Profile.m_Nonce = m_ProfileNonce;
	const unsigned Key = ProfileKey(Profile.m_Nonce);
	const unsigned Count = (unsigned)maximum(0, m_AledCount);
	Profile.m_EncodedCount = Count ^ Key ^ 0xa531c97du;
	Profile.m_EncodedMirror = (~Count) ^ RotateLeft32(Key, 13) ^ 0x4f28d351u;
	Profile.m_Signature = ProfileSignature(Profile.m_Nonce, Profile.m_EncodedCount, Profile.m_EncodedMirror);

	for(const char *pPath : {PROFILE_PATH, PROFILE_BACKUP_PATH})
	{
		IOHANDLE File = Storage()->OpenFile(pPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(!File)
			continue;
		io_write(File, &Profile, sizeof(Profile));
		io_close(File);
	}

	m_ProfileValid = true;
	m_Dirty = false;
}

void CAledCounter::ScheduleAutoRecord(int GameTick)
{
	if(!g_Config.m_TcAledAutoRecord || Client()->State() != IClient::STATE_ONLINE)
		return;

	Client()->DemoRecorder_UpdateReplayRecorder();
	if(!DemoRecorder(RECORDER_REPLAYS)->IsRecording())
		return;

	const int TickSpeed = Client()->GameTickSpeed();
	const int WantedSaveTick = GameTick + TickSpeed * 5;
	if(m_PendingAutoRecordTick >= 0)
	{
		m_PendingAutoRecordTick = maximum(m_PendingAutoRecordTick, WantedSaveTick);
		return;
	}

	if(m_LastAutoRecordTick >= 0 && GameTick - m_LastAutoRecordTick < TickSpeed * 3)
		return;

	m_PendingAutoRecordStartTick = maximum(0, GameTick - TickSpeed * 5);
	m_PendingAutoRecordTick = WantedSaveTick;
}

void CAledCounter::TrySavePendingAutoRecord()
{
	if(m_PendingAutoRecordTick < 0 || Client()->State() != IClient::STATE_ONLINE)
		return;
	if(!g_Config.m_TcAledAutoRecord)
	{
		m_PendingAutoRecordStartTick = -1;
		m_PendingAutoRecordTick = -1;
		return;
	}

	const int GameTick = Client()->GameTick(g_Config.m_ClDummy);
	if(GameTick < m_PendingAutoRecordTick)
		return;

	Client()->DemoRecorder_UpdateReplayRecorder();
	if(!DemoRecorder(RECORDER_REPLAYS)->IsRecording())
	{
		m_PendingAutoRecordStartTick = -1;
		m_PendingAutoRecordTick = -1;
		return;
	}

	Storage()->CreateFolder("demos", IStorage::TYPE_SAVE);
	Storage()->CreateFolder("demos/aleds", IStorage::TYPE_SAVE);

	char aTimestamp[20];
	str_timestamp(aTimestamp, sizeof(aTimestamp));
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "aled_%d_%s", m_AledCount, aTimestamp);

	const int StartTick = m_PendingAutoRecordStartTick >= 0 ? m_PendingAutoRecordStartTick : maximum(0, GameTick - Client()->GameTickSpeed() * 5);
	const int WantedLengthTicks = maximum(Client()->GameTickSpeed(), GameTick - StartTick);
	const int WantedLengthSeconds = (WantedLengthTicks + Client()->GameTickSpeed() - 1) / Client()->GameTickSpeed();
	Client()->DemoRecorder_SaveReplay(WantedLengthSeconds, aFilename, "demos/aleds", false);
	m_LastAutoRecordTick = GameTick;
	m_PendingAutoRecordStartTick = -1;
	m_PendingAutoRecordTick = -1;
}

bool CAledCounter::IsFreezeTile(int Tile) const
{
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
}

bool CAledCounter::HasFreezeBetween(vec2 From, vec2 To) const
{
	if(Collision()->IntersectLine(From, To, nullptr, nullptr))
		return false;

	const int FromIndex = Collision()->GetMapIndex(From);
	const int ToIndex = Collision()->GetMapIndex(To);
	const std::vector<int> vIndices = Collision()->GetMapIndices(From, To);
	for(const int Index : vIndices)
	{
		if(Index == FromIndex || Index == ToIndex)
			continue;
		if(IsFreezeTile(Collision()->GetTileIndex(Index)) || IsFreezeTile(Collision()->GetFrontTileIndex(Index)))
			return true;
	}

	return false;
}

bool CAledCounter::HasHammerHitEventNear(vec2 TargetPos) const
{
	constexpr float HAMMER_EVENT_MATCH_RADIUS = 34.0f;
	const int Num = Client()->SnapNumItems(IClient::SNAP_CURRENT);
	for(int Index = 0; Index < Num; ++Index)
	{
		const IClient::CSnapItem Item = Client()->SnapGetItem(IClient::SNAP_CURRENT, Index);
		if(Item.m_Type != NETEVENTTYPE_HAMMERHIT)
			continue;

		const CNetEvent_HammerHit *pEvent = static_cast<const CNetEvent_HammerHit *>(Item.m_pData);
		const vec2 EventPos = vec2(pEvent->m_X, pEvent->m_Y);
		if(distance(EventPos, TargetPos) <= HAMMER_EVENT_MATCH_RADIUS)
			return true;
	}
	return false;
}

bool CAledCounter::TargetWasUnfrozenByHit(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;

	const auto &Character = GameClient()->m_Snap.m_aCharacters[ClientId];
	if(!Character.m_HasExtendedData || Character.m_pPrevExtendedData == nullptr)
		return false;

	const int PrevFreezeEnd = Character.m_pPrevExtendedData->m_FreezeEnd;
	const int CurFreezeEnd = Character.m_ExtendedData.m_FreezeEnd;
	const bool WasFrozen = PrevFreezeEnd == -1 || PrevFreezeEnd > Client()->PrevGameTick(g_Config.m_ClDummy);
	const bool IsFrozenNow = CurFreezeEnd == -1 || CurFreezeEnd > Client()->GameTick(g_Config.m_ClDummy);
	const bool IsLiveFrozenNow = (Character.m_ExtendedData.m_Flags & CHARACTERFLAG_MOVEMENTS_DISABLED) != 0;

	return WasFrozen && !IsFrozenNow && !IsLiveFrozenNow;
}

bool CAledCounter::IsAledHitNow() const
{
	if(!g_Config.m_TcAledCounter || Client()->State() != IClient::STATE_ONLINE)
		return false;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_LocalClientId < 0)
		return false;
	if(!GameClient()->m_Snap.m_pLocalCharacter)
		return false;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	const CNetObj_Character *pLocalChar = GameClient()->m_Snap.m_pLocalCharacter;
	if(pLocalChar->m_Weapon != WEAPON_HAMMER)
		return false;

	const vec2 LocalPos = vec2(pLocalChar->m_X, pLocalChar->m_Y);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == LocalId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active || !GameClient()->m_Snap.m_apPlayerInfos[ClientId])
			continue;
		if(GameClient()->IsOtherTeam(ClientId))
			continue;

		const CNetObj_Character &Target = GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const vec2 TargetPos = vec2(Target.m_X, Target.m_Y);
		const float HitDist = distance(LocalPos, TargetPos);
		if(HitDist > 40.0f && HitDist < 90.0f && TargetWasUnfrozenByHit(ClientId) && HasHammerHitEventNear(TargetPos) && HasFreezeBetween(LocalPos, TargetPos))
			return true;
	}

	return false;
}

void CAledCounter::OnNewSnapshot()
{
	if(!g_Config.m_TcAledCounter || !GameClient()->m_Snap.m_pLocalCharacter)
	{
		TrySavePendingAutoRecord();
		return;
	}

	const int AttackTick = GameClient()->m_Snap.m_pLocalCharacter->m_AttackTick;
	const int GameTick = Client()->GameTick(g_Config.m_ClDummy);
	if(AttackTick == m_LastLocalAttackTick || AttackTick <= 0)
	{
		TrySavePendingAutoRecord();
		return;
	}
	m_LastLocalAttackTick = AttackTick;

	if(GameTick - m_LastRegisteredTick < Client()->GameTickSpeed() / 3)
	{
		TrySavePendingAutoRecord();
		return;
	}
	if(!IsAledHitNow())
	{
		TrySavePendingAutoRecord();
		return;
	}

	++m_AledCount;
	m_LastRegisteredTick = GameTick;
	m_Dirty = true;
	GameClient()->m_Effects.AledBurst(vec2(GameClient()->m_Snap.m_pLocalCharacter->m_X, GameClient()->m_Snap.m_pLocalCharacter->m_Y - 12.0f), 1.0f);
	ScheduleAutoRecord(GameTick);
	SaveProfile();
	TrySavePendingAutoRecord();
}

void CAledCounter::OnRender()
{
	if(g_Config.m_TcAledAutoRecord)
		Client()->DemoRecorder_UpdateReplayRecorder();
	TrySavePendingAutoRecord();
}
