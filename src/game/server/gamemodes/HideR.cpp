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
	IGameController(pGameServer), m_Teams(pGameServer), m_pLoadBestTimeResult(nullptr)
{
	m_pGameType = g_Config.m_SvTestingCommands ? TEST_TYPE_NAME : GAME_TYPE_NAME;
	m_GameFlags = GAMEFLAG_TEAMS;
	m_LastPlayerNum = 0;

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

	if((Server()->Tick() - m_RoundStartTick) > (15 * Server()->TickSpeed()))
		pChr->GetPlayer()->SetTeam(TEAM_RED, false);
}

void CGameControllerHideR::HandleCharacterTiles(CCharacter *pChr, int MapIndex)
{
}

void CGameControllerHideR::OnPlayerConnect(CPlayer *pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);
	int ClientID = pPlayer->GetCID();
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

	for(auto &Player : m_StartSeekers)
	{
		if(Player == pPlayer)
		{
			m_StartSeekers.erase(std::find(m_StartSeekers.begin(), m_StartSeekers.end(), Player));
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

		if((Server()->Tick() - m_RoundStartTick) == (15 * Server()->TickSpeed()))
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
		}else if ((Server()->Tick() - m_RoundStartTick) < (15 * Server()->TickSpeed()))
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
			bool NoSeeker = true, NoHider = true;
			for(auto &Player : GameServer()->m_apPlayers)
			{
				if(!Player)
					continue;
				if(Player->GetTeam() == TEAM_RED)
					NoSeeker = false;
				else if(Player->GetTeam() == TEAM_BLUE)
					NoHider = false;
			}

			if((NoSeeker || NoHider) && (Server()->Tick() - m_RoundStartTick) > (15 * Server()->TickSpeed()))
			{
				EndRound();

				if(NoSeeker)
				{
					for(auto &Player : GameServer()->m_apPlayers)
					{
						if(!Player)
							continue;
						if(Player->GetTeam() == TEAM_BLUE)
							GameServer()->Score()->SavePoint(Player->GetCID(), 3);
					}

					GameServer()->SendChatTarget(-1, "The hider win!");
				}else
				{
					for(auto &Player : m_StartSeekers)
					{
						if(!Player)
							continue;
						GameServer()->Score()->SavePoint(Player->GetCID(), 2);
					}

					GameServer()->SendChatTarget(-1, "The seeker win!");
				}
			}
			else if((Server()->Tick() - m_RoundStartTick) > (Config()->m_SvTimeLimit * 60 * Server()->TickSpeed()))
			{
				for(auto &Player : GameServer()->m_apPlayers)
				{
					if(!Player)
						continue;
					if(Player->GetTeam() == TEAM_BLUE)
						GameServer()->Score()->SavePoint(Player->GetCID(), 2);
				}
				EndRound();
				
				GameServer()->SendChatTarget(-1, "The hider win!");
			}
		}
	}
	
	m_LastPlayerNum = PlayerNum;
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
		Player->SetTeam(TEAM_BLUE, false);
		Player->Pause(CPlayer::PAUSE_NONE, true);
		PlayerNum ++;

		vpPlayers.push_back(Player);
	}

	int Num = maximum(1, PlayerNum / 8);
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
