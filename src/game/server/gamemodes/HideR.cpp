/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
/* Based on Race mod stuff and tweaked by GreYFoX@GTi and others to fit our DDRace needs. */
#include "HideR.h"

#include <engine/server.h>
#include <engine/shared/config.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/version.h>

#define GAME_TYPE_NAME "HideRemake"
#define TEST_TYPE_NAME "TestHideRemake"

CGameControllerHideR::CGameControllerHideR(class CGameContext *pGameServer) :
	IGameController(pGameServer), m_Teams(pGameServer)
{
	m_pGameType = g_Config.m_SvTestingCommands ? TEST_TYPE_NAME : GAME_TYPE_NAME;
	m_LastPlayerNum = 0;
	m_GameFlags = GAMEFLAG_TEAMS | GAMEFLAG_FLAGS;

	m_LastHider = nullptr;
	m_BestSeeker = nullptr;

	m_SeekerNum = 0;
	m_HiderNum = 0;

	m_TeleCheckOuts.clear();
	m_TeleOuts.clear();
	m_StartSeekers.clear();

	InitTeleporter();
}

CGameControllerHideR::~CGameControllerHideR() = default;

CScore *CGameControllerHideR::Score()
{
	return GameServer()->Score();
}

void CGameControllerHideR::OnCharacterSpawn(CCharacter *pChr)
{
	IGameController::OnCharacterSpawn(pChr);
	pChr->SetTeams(&m_Teams);
	pChr->SetTeleports(&m_TeleOuts, &m_TeleCheckOuts);
	m_Teams.OnCharacterSpawn(pChr->GetPlayer()->GetCID());
	if((Server()->Tick() - m_RoundStartTick) > (Config()->m_SvTimeStart * Server()->TickSpeed()))
		pChr->GetPlayer()->SetTeam(TEAM_RED, false);

}

void CGameControllerHideR::HandleCharacterTiles(CCharacter *pChr, int MapIndex)
{
}

void CGameControllerHideR::OnPlayerConnect(CPlayer *pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);
	int ClientID = pPlayer->GetCID();
	if((Server()->Tick() - m_RoundStartTick) > (Config()->m_SvTimeStart * Server()->TickSpeed()))
		pPlayer->SetTeam(TEAM_RED, false);
	else
		pPlayer->SetTeam(TEAM_BLUE, false);

	if(!Server()->ClientPrevIngame(ClientID))
	{
		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' entered and joined the %s", Server()->ClientName(ClientID), GetTeamName(pPlayer->GetTeam()));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf, -1, CGameContext::CHAT_SIX);

		GameServer()->SendChatTarget(ClientID, "HiderR Mod. Version: " MOD_VERSION);
	}
}

void CGameControllerHideR::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	int ClientID = pPlayer->GetCID();
	bool WasModerator = pPlayer->m_Moderating && Server()->ClientIngame(ClientID);

	for(unsigned i = 0; i < m_StartSeekers.size() ; i ++)
	{
		if(m_StartSeekers[i] == pPlayer)
		{
			m_StartSeekers.erase(m_StartSeekers.begin() + i);
		}
	}

	IGameController::OnPlayerDisconnect(pPlayer, pReason);

	if(!GameServer()->PlayerModerating() && WasModerator)
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "Server kick/spec votes are no longer actively moderated.");

	if(g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO)
		m_Teams.SetForceCharacterTeam(ClientID, TEAM_FLOCK);
}

void CGameControllerHideR::OnReset()
{
	IGameController::OnReset();
	m_Teams.Reset();
}

void CGameControllerHideR::Tick()
{
	IGameController::Tick();
	m_Teams.ProcessSaveTeam();
	m_Teams.Tick();
	
	int PlayerNum = 0;

	for(auto &Player : GameServer()->m_apPlayers)
	{
		if(!Player)
			continue;
		if(Player->GetTeam() == TEAM_SPECTATORS)
			continue;
		PlayerNum ++;
	}

	if(m_GameOverTick == -1)
	{
		if(PlayerNum >= 2 && m_LastPlayerNum < 2)
		{
			EndRound();
			return;
		}else if(PlayerNum < 2)
		{
			m_RoundStartTick ++;
		}

		m_LastHider = nullptr;
		m_BestSeeker = nullptr;
		
		m_SeekerNum = 0;
		m_HiderNum = 0;
		for(auto &Player : GameServer()->m_apPlayers)
		{
			if(!Player)
				continue;
			if(Player->GetTeam() == TEAM_RED)
			{
				m_SeekerNum ++;
				if(!m_BestSeeker)
					m_BestSeeker = Player;
				else if(Player->m_KillNum > m_BestSeeker->m_KillNum)
					m_BestSeeker = Player;
			}
			else if(Player->GetTeam() == TEAM_BLUE)
			{
				m_HiderNum ++;
				if(!m_LastHider)
					m_LastHider = Player;
			}
		}

		if(m_HiderNum > 1)
			m_LastHider = nullptr;

		if((Server()->Tick() - m_RoundStartTick) == (Config()->m_SvTimeStart * Server()->TickSpeed()))
		{
			for(auto &Player : GameServer()->m_apPlayers)
			{
				if(!Player)
					continue;
				if(Player->GetTeam() != TEAM_RED)
					continue;
				Player->Pause(CPlayer::PAUSE_NONE, true);
			}
			GameServer()->SendChatTarget(-1, "The seekers are released!");
			GameServer()->SendBroadcast("The seekers are released!", -1);
		}else if ((Server()->Tick() - m_RoundStartTick) < (Config()->m_SvTimeStart * Server()->TickSpeed()))
		{
			for(auto &Player : GameServer()->m_apPlayers)
			{
				if(!Player)
					continue;
				if(Player->GetTeam() != TEAM_RED)
					continue;
				Player->Pause(CPlayer::PAUSE_PAUSED, true);
				Player->SpectatePlayerName(Server()->ClientName(Player->GetCID()));
			}
		}else 
		{

			if((!m_SeekerNum || !m_HiderNum) && (Server()->Tick() - m_RoundStartTick) > (Config()->m_SvTimeStart * Server()->TickSpeed()))
			{
				EndRound();

				if(!m_SeekerNum)
				{
					for(auto &Player : GameServer()->m_apPlayers)
					{
						if(!Player)
							continue;
						if(Player->GetTeam() == TEAM_BLUE)
							GameServer()->Score()->SavePoint(Player->GetCID(), Player->m_CureNum);
					}

					GameServer()->SendChatTarget(-1, "The hider win!");
				}else
				{
					for(auto &Player : GameServer()->m_apPlayers)
					{
						if(!Player)
							continue;
						GameServer()->Score()->SavePoint(Player->GetCID(), Player->m_KillNum);
					}

					GameServer()->SendChatTarget(-1, "The seeker win!");
				}
			}
			else if((Server()->Tick() - m_RoundStartTick) > (Config()->m_SvTimeLimit * 60 * Server()->TickSpeed()))
			{
				std::vector<CPlayer *> LastBlues;
				for(auto &Player : GameServer()->m_apPlayers)
				{
					if(!Player)
						continue;
					if(Player->GetTeam() == TEAM_BLUE)
					{
						int Points = maximum(1, 6 - (int) LastBlues.size());
						Points += Player->m_CureNum;
						GameServer()->Score()->SavePoint(Player->GetCID(), Points);
					}else if(Player->GetTeam() == TEAM_RED)
					{
						int Points = Player->m_KillNum;
						GameServer()->Score()->SavePoint(Player->GetCID(), Points);
					}
				}
				
				EndRound();
				
				GameServer()->SendChatTarget(-1, "The hider win!");
			}
		}
	}
	
	m_LastPlayerNum = PlayerNum;
}

void CGameControllerHideR::Snap(int SnappingClient)
{
	CNetObj_GameInfo *pGameInfoObj = Server()->SnapNewItem<CNetObj_GameInfo>(0);
	if(!pGameInfoObj)
		return;

	pGameInfoObj->m_GameFlags = m_GameFlags;
	if(GameServer()->m_apPlayers[SnappingClient] && 
		(GameServer()->m_apPlayers[SnappingClient]->m_PlayerFlags&PLAYERFLAG_SCOREBOARD ||
		GameServer()->m_apPlayers[SnappingClient]->m_LastPlayerFlags&PLAYERFLAG_SCOREBOARD ||
		GameServer()->m_apPlayers[SnappingClient]->m_LastPrevPlayerFlags&PLAYERFLAG_SCOREBOARD))
		pGameInfoObj->m_GameFlags = 0;

	pGameInfoObj->m_GameStateFlags = 0;
	if(m_GameOverTick != -1)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(m_SuddenDeath)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(GameServer()->m_World.m_Paused)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	pGameInfoObj->m_RoundStartTick = m_RoundStartTick;
	pGameInfoObj->m_WarmupTimer = m_Warmup;

	pGameInfoObj->m_RoundNum = 0;
	pGameInfoObj->m_RoundCurrent = m_RoundCount + 1;

	CCharacter *pChr;
	CPlayer *pPlayer = SnappingClient != SERVER_DEMO_CLIENT ? GameServer()->m_apPlayers[SnappingClient] : 0;
	CPlayer *pPlayer2;

	if(pPlayer && (pPlayer->m_TimerType == CPlayer::TIMERTYPE_GAMETIMER || pPlayer->m_TimerType == CPlayer::TIMERTYPE_GAMETIMER_AND_BROADCAST) && pPlayer->GetClientVersion() >= VERSION_DDNET_GAMETICK)
	{
		if((pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused()) && pPlayer->m_SpectatorID != SPEC_FREEVIEW && (pPlayer2 = GameServer()->m_apPlayers[pPlayer->m_SpectatorID]))
		{
			if((pChr = pPlayer2->GetCharacter()) && pChr->m_DDRaceState == DDRACE_STARTED)
			{
				pGameInfoObj->m_WarmupTimer = -pChr->m_StartTime;
				pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_RACETIME;
			}
		}
		else if((pChr = pPlayer->GetCharacter()) && pChr->m_DDRaceState == DDRACE_STARTED)
		{
			pGameInfoObj->m_WarmupTimer = -pChr->m_StartTime;
			pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_RACETIME;
		}
	}

	if(pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS)
	{
		CNetObj_GameData *pGameData = Server()->SnapNewItem<CNetObj_GameData>(0);
		pGameData->m_TeamscoreBlue = m_HiderNum;
		pGameData->m_TeamscoreRed = m_SeekerNum;
		pGameData->m_FlagCarrierRed = FLAG_ATSTAND;
		pGameData->m_FlagCarrierBlue = FLAG_ATSTAND;

		if(m_BestSeeker)
			pGameData->m_FlagCarrierRed = m_BestSeeker->GetCID();
		if(m_LastHider)
			pGameData->m_FlagCarrierBlue = m_LastHider->GetCID(); 
	}

	CNetObj_GameInfoEx *pGameInfoEx = Server()->SnapNewItem<CNetObj_GameInfoEx>(0);
	if(!pGameInfoEx)
		return;

	pGameInfoEx->m_Flags =
		GAMEINFOFLAG_TIMESCORE |
		GAMEINFOFLAG_GAMETYPE_RACE |
		GAMEINFOFLAG_GAMETYPE_DDRACE |
		GAMEINFOFLAG_GAMETYPE_DDNET |
		GAMEINFOFLAG_UNLIMITED_AMMO |
		GAMEINFOFLAG_RACE_RECORD_MESSAGE |
		GAMEINFOFLAG_ALLOW_EYE_WHEEL |
		GAMEINFOFLAG_ALLOW_HOOK_COLL |
		// GAMEINFOFLAG_ALLOW_ZOOM |
		GAMEINFOFLAG_BUG_DDRACE_GHOST |
		GAMEINFOFLAG_BUG_DDRACE_INPUT |
		GAMEINFOFLAG_PREDICT_DDRACE |
		GAMEINFOFLAG_PREDICT_DDRACE_TILES |
		GAMEINFOFLAG_ENTITIES_DDNET |
		GAMEINFOFLAG_ENTITIES_DDRACE |
		GAMEINFOFLAG_ENTITIES_RACE |
		GAMEINFOFLAG_RACE;
	pGameInfoEx->m_Flags2 = GAMEINFOFLAG2_HUD_DDRACE;
	if(g_Config.m_SvNoWeakHook)
		pGameInfoEx->m_Flags2 |= GAMEINFOFLAG2_NO_WEAK_HOOK;
	pGameInfoEx->m_Version = GAMEINFO_CURVERSION;

	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_GameData *pGameData = Server()->SnapNewItem<protocol7::CNetObj_GameData>(0);
		if(!pGameData)
			return;

		pGameData->m_GameStartTick = m_RoundStartTick;
		pGameData->m_GameStateFlags = 0;
		if(m_GameOverTick != -1)
			pGameData->m_GameStateFlags |= protocol7::GAMESTATEFLAG_GAMEOVER;
		if(m_SuddenDeath)
			pGameData->m_GameStateFlags |= protocol7::GAMESTATEFLAG_SUDDENDEATH;
		if(GameServer()->m_World.m_Paused)
			pGameData->m_GameStateFlags |= protocol7::GAMESTATEFLAG_PAUSED;

		pGameData->m_GameStateEndTick = 0;

		protocol7::CNetObj_GameDataRace *pRaceData = Server()->SnapNewItem<protocol7::CNetObj_GameDataRace>(0);
		if(!pRaceData)
			return;

		pRaceData->m_BestTime = round_to_int(m_CurrentRecord * 1000);
		pRaceData->m_Precision = 0;
		pRaceData->m_RaceFlags = protocol7::RACEFLAG_HIDE_KILLMSG | protocol7::RACEFLAG_KEEP_WANTED_WEAPON;
	}

	if(!GameServer()->Switchers().empty())
	{
		int Team = pPlayer && pPlayer->GetCharacter() ? pPlayer->GetCharacter()->Team() : 0;

		if(pPlayer && (pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused()) && pPlayer->m_SpectatorID != SPEC_FREEVIEW && GameServer()->m_apPlayers[pPlayer->m_SpectatorID] && GameServer()->m_apPlayers[pPlayer->m_SpectatorID]->GetCharacter())
			Team = GameServer()->m_apPlayers[pPlayer->m_SpectatorID]->GetCharacter()->Team();

		if(Team == TEAM_SUPER)
			return;

		int SentTeam = Team;
		if(g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
			SentTeam = 0;

		CNetObj_SwitchState *pSwitchState = Server()->SnapNewItem<CNetObj_SwitchState>(SentTeam);
		if(!pSwitchState)
			return;

		pSwitchState->m_HighestSwitchNumber = clamp((int)GameServer()->Switchers().size() - 1, 0, 255);
		mem_zero(pSwitchState->m_aStatus, sizeof(pSwitchState->m_aStatus));

		std::vector<std::pair<int, int>> vEndTicks; // <EndTick, SwitchNumber>

		for(int i = 0; i <= pSwitchState->m_HighestSwitchNumber; i++)
		{
			int Status = (int)GameServer()->Switchers()[i].m_aStatus[Team];
			pSwitchState->m_aStatus[i / 32] |= (Status << (i % 32));

			int EndTick = GameServer()->Switchers()[i].m_aEndTick[Team];
			if(EndTick > 0 && EndTick < Server()->Tick() + 3 * Server()->TickSpeed() && GameServer()->Switchers()[i].m_aLastUpdateTick[Team] < Server()->Tick())
			{
				// only keep track of EndTicks that have less than three second left and are not currently being updated by a player being present on a switch tile, to limit how often these are sent
				vEndTicks.emplace_back(GameServer()->Switchers()[i].m_aEndTick[Team], i);
			}
		}

		// send the endtick of switchers that are about to toggle back (up to four, prioritizing those with the earliest endticks)
		mem_zero(pSwitchState->m_aSwitchNumbers, sizeof(pSwitchState->m_aSwitchNumbers));
		mem_zero(pSwitchState->m_aEndTicks, sizeof(pSwitchState->m_aEndTicks));

		std::sort(vEndTicks.begin(), vEndTicks.end());
		const int NumTimedSwitchers = minimum((int)vEndTicks.size(), (int)std::size(pSwitchState->m_aEndTicks));

		for(int i = 0; i < NumTimedSwitchers; i++)
		{
			pSwitchState->m_aSwitchNumbers[i] = vEndTicks[i].second;
			pSwitchState->m_aEndTicks[i] = vEndTicks[i].first;
		}
	}
}

void CGameControllerHideR::StartRound()
{
	ResetGame();

	m_RoundStartTick = Server()->Tick();
	m_SuddenDeath = 0;
	m_GameOverTick = -1;
	GameServer()->m_World.m_Paused = false;
	m_ForceBalanced = false;
	Server()->DemoRecorder_HandleAutoStart();
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "start round type='%s' teamplay='%d'", m_pGameType, m_GameFlags & GAMEFLAG_TEAMS);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	
	int PlayerNum = 0;

	std::vector<CPlayer*> vpPlayers;

	for(auto &Player : GameServer()->m_apPlayers)
	{
		if(!Player)
			continue;

		Player->Pause(CPlayer::PAUSE_NONE, true);
		Player->m_KillNum = 0;
		Player->m_CureNum = 0;

		if(Player->GetTeam() == TEAM_SPECTATORS)
			continue;
		Player->SetTeam(TEAM_BLUE, false);
		PlayerNum ++;

		vpPlayers.push_back(Player);
	}

	m_StartSeekers.clear();

	int Num = maximum(1, (PlayerNum < 8) ? (PlayerNum / 4) : ((PlayerNum / 8) + 1));
	for(int i = 0; i < Num; i ++)
	{
		CPlayer *pRandomPlayer = vpPlayers[round_to_int(random_float(vpPlayers.size()-1))];
		pRandomPlayer->SetTeam(TEAM_RED, false);
		m_StartSeekers.push_back(pRandomPlayer);
	}
}

void CGameControllerHideR::DoTeamChange(class CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	Team = ClampTeam(Team);
	if(Team == pPlayer->GetTeam())
		return;

	CCharacter *pCharacter = pPlayer->GetCharacter();

	if(Team == TEAM_SPECTATORS)
	{
		if(g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO && pCharacter)
		{
			// Joining spectators should not kill a locked team, but should still
			// check if the team finished by you leaving it.
			int DDRTeam = pCharacter->Team();
			m_Teams.SetForceCharacterTeam(pPlayer->GetCID(), TEAM_FLOCK);
			m_Teams.CheckTeamFinished(DDRTeam);
		}
	}

	IGameController::DoTeamChange(pPlayer, Team, DoChatMsg);
}

CClientMask CGameControllerHideR::GetMaskForPlayerWorldEvent(int Asker, int ExceptID)
{
	if(Asker == -1)
		return CClientMask().set().reset(ExceptID);

	return m_Teams.TeamMask(GetPlayerTeam(Asker), ExceptID, Asker);
}

void CGameControllerHideR::InitTeleporter()
{
	if(!GameServer()->Collision()->Layers()->TeleLayer())
		return;
	int Width = GameServer()->Collision()->Layers()->TeleLayer()->m_Width;
	int Height = GameServer()->Collision()->Layers()->TeleLayer()->m_Height;

	for(int i = 0; i < Width * Height; i++)
	{
		int Number = GameServer()->Collision()->TeleLayer()[i].m_Number;
		int Type = GameServer()->Collision()->TeleLayer()[i].m_Type;
		if(Number > 0)
		{
			if(Type == TILE_TELEOUT)
			{
				m_TeleOuts[Number - 1].push_back(
					vec2(i % Width * 32 + 16, i / Width * 32 + 16));
			}
			else if(Type == TILE_TELECHECKOUT)
			{
				m_TeleCheckOuts[Number - 1].push_back(
					vec2(i % Width * 32 + 16, i / Width * 32 + 16));
			}
		}
	}
}

int CGameControllerHideR::GetPlayerTeam(int ClientID) const
{
	return 0;
}
