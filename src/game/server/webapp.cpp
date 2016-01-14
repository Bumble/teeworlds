/* Webapp class by Sushi and Redix */
#if defined(CONF_TEERACE)

#include <stdio.h>

#include <base/tl/algorithm.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/http.h>
#include <engine/external/json/reader.h>
#include <engine/storage.h>

#include <game/teerace.h>
#include <game/version.h>

#include "score/wa_score.h"
#include "gamecontext.h"
#include "webapp.h"
#include "data.h"

CBufferRequest *CServerWebapp::CreateAuthedApiRequest(int Method, const char *pURI)
{
	CBufferRequest *pRequest = ITeerace::CreateApiRequest(Method, pURI);
	RegisterFields(pRequest);
	return pRequest;
}

void CServerWebapp::RegisterFields(IRequest *pRequest)
{
	pRequest->AddField("API-AUTH", g_Config.m_WaApiKey);
	pRequest->AddField("API-GAMESERVER-VERSION", TEERACE_GAMESERVER_VERSION);
}

void CServerWebapp::CheckStatusCode(IConsole *pConsole, int Status)
{
	if(Status == 432)
	{
		pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "webapp",
			"This server is outdated and cannot fully cooperate with Teerace, hence its support is currently disabled. Please notify the server administrator.");
	}
	if(Status == 403)
	{
		pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "webapp",
			"This server was denied access to Teerace network. Please notify the server administator.");
	}
}

void CServerWebapp::Download(CGameContext *pGameServer, const char *pFilename, const char *pURI, FHttpCallback pfnCallback)
{
	IOHANDLE File = pGameServer->Server()->Storage()->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	CBufferRequest *pRequest = new CBufferRequest(IRequest::HTTP_GET, pURI);
	CRequestInfo *pInfo = new CRequestInfo(ITeerace::Host(), File, pFilename);
	pInfo->SetCallback(pfnCallback, pGameServer);
	pInfo->SetPriority(HTTP_PRIORITY_LOW);
	pGameServer->Server()->SendHttp(pInfo, pRequest);
}

void CServerWebapp::Upload(CGameContext *pGameServer, const char *pFilename, const char *pURI, const char *pUploadName, FHttpCallback pfnCallback)
{
	CWebUploadData *pUserData = new CWebUploadData(pGameServer);
	str_copy(pUserData->m_aFilename, pFilename, sizeof(pUserData->m_aFilename));
	IOHANDLE File = pGameServer->Server()->Storage()->OpenFile(pFilename, IOFLAG_READ, IStorage::TYPE_SAVE);
	CFileRequest *pRequest = ITeerace::CreateApiUpload(pURI);
	RegisterFields(pRequest);
	pRequest->SetFile(File, pFilename, pUploadName);
	CRequestInfo *pInfo = new CRequestInfo(ITeerace::Host());
	pInfo->SetCallback(pfnCallback, pUserData);
	pInfo->SetPriority(HTTP_PRIORITY_LOW);
	pGameServer->Server()->SendHttp(pInfo, pRequest);
}

CServerWebapp::CServerWebapp(CGameContext *pGameServer)
	: m_pGameServer(pGameServer), m_pServer(pGameServer->Server()), m_LastMapListLoad(-1)
{
	// load maps
	m_pServer->Storage()->ListDirectory(IStorage::TYPE_SAVE, "maps/teerace", MaplistFetchCallback, this);
	m_lUploads.clear();
}

void CServerWebapp::OnUserAuth(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebUserAuthData *pUser = (CWebUserAuthData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	IServer *pServer = pGameServer->Server();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	int ClientID = pUser->m_ClientID;
	if(pGameServer->m_apPlayers[ClientID])
	{
		if(Error)
		{
			pGameServer->SendChatTarget(ClientID, "unknown error");
			delete pUser;
			return;
		}

		int SendRconCmds = pUser->m_SendRconCmds;
		int UserID = 0;

		Json::Value JsonData;
		Json::Reader Reader;
		const char *pBody = ((CBufferResponse*)pResponse)->GetBody();
		if(str_comp(pBody, "false") != 0 && Reader.parse(pBody, pBody + pResponse->Size(), JsonData))
			UserID = JsonData["id"].asInt();

		if(UserID > 0)
		{
			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "%s has logged in as %s", pServer->ClientName(ClientID), JsonData["username"].asCString());
			pGameServer->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			pServer->SetUserID(ClientID, UserID);
			pServer->SetUserName(ClientID, JsonData["username"].asCString());

			// auth staff members
			if(JsonData["is_staff"].asBool())
				pServer->StaffAuth(ClientID, SendRconCmds);

			((CWebappScore*)pGameServer->Score())->LoadScore(ClientID, true);
		}
		else
		{
			pGameServer->SendChatTarget(ClientID, "wrong username and/or password");
		}
	}
	delete pUser;
}

void CServerWebapp::OnUserFind(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebUserRankData *pUser = (CWebUserRankData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	IServer *pServer = pGameServer->Server();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	pUser->m_UserID = 0;

	Json::Value JsonData;
	Json::Reader Reader;
	const char *pBody = ((CBufferResponse*)pResponse)->GetBody();
	if(!Error && Reader.parse(pBody, pBody + pResponse->Size(), JsonData))
		pUser->m_UserID = JsonData["id"].asInt();

	if(pUser->m_UserID > 0)
	{
		str_copy(pUser->m_aName, JsonData["username"].asCString(), sizeof(pUser->m_aName));

		char aURI[128];
		str_format(aURI, sizeof(aURI), "/users/rank/%d/", pUser->m_UserID);
		CBufferRequest *pRequest = CreateAuthedApiRequest(IRequest::HTTP_GET, aURI);
		CRequestInfo *pInfo = new CRequestInfo(ITeerace::Host());
		pInfo->SetCallback(OnUserRankGlobal, pUser);  // do not delete userdata here
		pServer->SendHttp(pInfo, pRequest);
	}
	else if(pUser->m_PrintRank)
	{
		if(pGameServer->m_apPlayers[pUser->m_ClientID])
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "No match found for \"%s\".", pUser->m_aName);
			pGameServer->SendChatTarget(pUser->m_ClientID, aBuf);
		}
		delete pUser;
	}
}

void CServerWebapp::OnUserRankGlobal(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebUserRankData *pUser = (CWebUserRankData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	IServer *pServer = pGameServer->Server();
	CServerWebapp *pWebapp = pGameServer->Webapp();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	pUser->m_GlobalRank = 0;
	if(!Error)
		pUser->m_GlobalRank = str_toint(((CBufferResponse*)pResponse)->GetBody());

	char aURI[128];
	str_format(aURI, sizeof(aURI), "/users/map_rank/%d/%d/", pUser->m_UserID, pWebapp->CurrentMap()->m_ID);
	CBufferRequest *pRequest = CreateAuthedApiRequest(IRequest::HTTP_GET, aURI);
	CRequestInfo *pInfo = new CRequestInfo(ITeerace::Host());
	pInfo->SetCallback(OnUserRankMap, pUser); // do not delete userdata here
	pServer->SendHttp(pInfo, pRequest);
}

void CServerWebapp::OnUserRankMap(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebUserRankData *pUser = (CWebUserRankData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	IServer *pServer = pGameServer->Server();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	int GlobalRank = pUser->m_GlobalRank;
	int MapRank = 0;
	CPlayerData Run;

	Json::Value JsonData;
	Json::Reader Reader;
	const char *pBody = ((CBufferResponse*)pResponse)->GetBody();
	if(!Error && Reader.parse(pBody, pBody + pResponse->Size(), JsonData))
	{
		MapRank = JsonData["position"].asInt();
		if(MapRank)
		{
			float Time = str_tofloat(JsonData["bestrun"]["time"].asCString());
			float aCheckpointTimes[25] = { 0.0f };
			Json::Value Checkpoint = JsonData["bestrun"]["checkpoints_list"];
			for(unsigned int i = 0; i < Checkpoint.size(); i++)
				aCheckpointTimes[i] = str_tofloat(Checkpoint[i].asCString());
			Run.Set(Time, aCheckpointTimes);
		}
	}

	if(pGameServer->m_apPlayers[pUser->m_ClientID])
	{
		bool Own = pUser->m_UserID == pServer->GetUserID(pUser->m_ClientID);
		if(Own && MapRank)
			pGameServer->Score()->PlayerData(pUser->m_ClientID)->Set(Run.m_Time, Run.m_aCpTime);

		if(pUser->m_PrintRank)
		{
			char aBuf[256];
			if(!MapRank && !GlobalRank)
			{
				// do not send the rank to everyone if the player is not ranked at all
				if(Own)
					pGameServer->SendChatTarget(pUser->m_ClientID, "You are neither globally ranked nor on this map yet.");
				else
				{
					str_format(aBuf, sizeof(aBuf), "%s is neither globally ranked nor on this map yet.", pUser->m_aName);
					pGameServer->SendChatTarget(pUser->m_ClientID, aBuf);
				}

				delete pUser;
				return; // we don't need the rest of this function here!
			}
			else if(!MapRank)
			{
				str_format(aBuf, sizeof(aBuf), "%s: Global Rank: %d | Map Rank: Not ranked yet (%s)",
					pUser->m_aName, GlobalRank, pServer->ClientName(pUser->m_ClientID));
			}
			else if(!GlobalRank)
			{
				if(Run.m_Time < 60.0f)
					str_format(aBuf, sizeof(aBuf), "%s: Not globally ranked yet | Map Rank: %d | Time: %.3f (%s)",
						pUser->m_aName, MapRank, Run.m_Time, pServer->ClientName(pUser->m_ClientID));
				else
					str_format(aBuf, sizeof(aBuf), "%s: Not globally ranked yet | Map Rank: %d | Time: %02d:%06.3f (%s)",
						pUser->m_aName, MapRank, (int)Run.m_Time / 60,
						fmod(Run.m_Time, 60), pServer->ClientName(pUser->m_ClientID));
			}
			else
			{
				if(Run.m_Time < 60.0f)
					str_format(aBuf, sizeof(aBuf), "%s: Global Rank: %d | Map Rank: %d | Time: %.3f (%s)",
						pUser->m_aName, GlobalRank, MapRank, Run.m_Time, pServer->ClientName(pUser->m_ClientID));
				else
					str_format(aBuf, sizeof(aBuf), "%s: Global Rank: %d | Map Rank: %d | Time: %02d:%06.3f (%s)",
						pUser->m_aName, GlobalRank, MapRank, (int)Run.m_Time / 60,
						fmod(Run.m_Time, 60), pServer->ClientName(pUser->m_ClientID));
			}

			if(g_Config.m_SvShowTimes)
				pGameServer->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			else
				pGameServer->SendChatTarget(pUser->m_ClientID, aBuf);
		}
	}
	delete pUser;
}

void CServerWebapp::OnUserTop(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebUserTopData *pUser = (CWebUserTopData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	Json::Value JsonData;
	Json::Reader Reader;
	const char *pBody = ((CBufferResponse*)pResponse)->GetBody();
	if(!Error && Reader.parse(pBody, pBody + pResponse->Size(), JsonData))
	{
		int ClientID = pUser->m_ClientID;
		if(pGameServer->m_apPlayers[ClientID])
		{
			char aBuf[256];
			float LastTime = 0.0f;
			int SameTimeCount = 0;
			pGameServer->SendChatTarget(ClientID, "----------- Top 5 -----------");
			for(unsigned int i = 0; i < JsonData.size() && i < 5; i++)
			{
				Json::Value Run = JsonData[i];
				float Time = str_tofloat(Run["run"]["time"].asCString());

				if(Time == LastTime)
					SameTimeCount++;
				else
					SameTimeCount = 0;

				str_format(aBuf, sizeof(aBuf), "%d. %s Time: %d minute(s) %.3f second(s)",
					i + pUser->m_StartRank - SameTimeCount, Run["run"]["user"]["username"].asCString(), (int)Time / 60, fmod(Time, 60));
				pGameServer->SendChatTarget(ClientID, aBuf);

				LastTime = Time;
			}
			pGameServer->SendChatTarget(ClientID, "------------------------------");
		}
	}
	delete pUser;
}

void CServerWebapp::OnPingPing(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CGameContext *pGameServer = (CGameContext*)pUserData;
	IServer *pServer = pGameServer->Server();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	dbg_msg("webapp", "webapp is%s online", Error ? " not" : "");
	if(Error)
		return;

	Json::Value JsonData;
	Json::Reader Reader;
	const char *pBody = ((CBufferResponse*)pResponse)->GetBody();
	if(Reader.parse(pBody, pBody + pResponse->Size(), JsonData))
	{
		Json::Value Awards = JsonData["awards"];
		for(unsigned int i = 0; i < Awards.size(); i++)
		{
			Json::Value Award = Awards[i];
			int UserID = Award["user_id"].isInt() ? Award["user_id"].asInt() : 0;

			if(!UserID)
				return;

			// show awards to everyone only if the player is there
			for(int j = 0; j < MAX_CLIENTS; j++)
			{
				if(UserID != pServer->GetUserID(j))
					continue;

				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "%s achieved award \"%s\".", pServer->ClientName(j), Award["name"].asCString());
				pGameServer->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			}
		}
	}
}

void CServerWebapp::OnMapList(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CGameContext *pGameServer = (CGameContext*)pUserData;
	CServerWebapp *pWebapp = pGameServer->Webapp();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	Json::Value JsonData;
	Json::Reader Reader;
	const char *pBody = ((CBufferResponse*)pResponse)->GetBody();
	if(!Error && Reader.parse(pBody, pBody + pResponse->Size(), JsonData))
	{
		char aFilename[256];
		const char *pPath = "/maps/teerace/%s.map";
		bool Change = false;

		for(unsigned int i = 0; i < JsonData.size(); i++)
		{
			Json::Value Map = JsonData[i];
			if(!Map["crc"].isString())
				continue; // skip maps without crc

			CMapInfo Info;
			str_copy(Info.m_aName, Map["name"].asCString(), sizeof(Info.m_aName));
			sscanf(Map["crc"].asCString(), "%08x", &Info.m_Crc);
			Info.m_ID = Map["id"].asInt();
			Info.m_RunCount = Map["run_count"].asInt();
			Info.m_MapType = Map["get_map_type"].asInt();
			str_copy(Info.m_aURL, Map["get_download_url"].asCString(), sizeof(Info.m_aURL));
			str_copy(Info.m_aAuthor, Map["author"].asCString(), sizeof(Info.m_aAuthor));

			array<CMapInfo>::range r = find_linear(pWebapp->m_lMapList.all(), Info);
			if(r.empty()) // new entry
			{
				Info.m_State = CMapInfo::MAPSTATE_DOWNLOADING;
				pWebapp->m_lMapList.add(Info);
				dbg_msg("webapp", "added map info: '%s' (%d)", Info.m_aName, Info.m_ID);

				str_format(aFilename, sizeof(aFilename), pPath, Info.m_aName);
				Download(pGameServer, aFilename, Map["get_download_url"].asCString(), OnDownloadMap);
			}
			else if(r.front().m_Crc != Info.m_Crc) // we have a wrong version
			{
				Info.m_State = CMapInfo::MAPSTATE_DOWNLOADING;
				r.front() = Info;
				dbg_msg("webapp", "updated map info: '%s' (%d)", Info.m_aName, Info.m_ID);

				str_format(aFilename, sizeof(aFilename), pPath, Info.m_aName);
				Download(pGameServer, aFilename, Map["get_download_url"].asCString(), OnDownloadMap);
			}
			else if(r.front().m_Crc == Info.m_Crc) // we already have this 
			{
				if(r.front().m_State == CMapInfo::MAPSTATE_INFO_MISSING)
				{
					Info.m_State = CMapInfo::MAPSTATE_COMPLETE;
					r.front() = Info;
					dbg_msg("webapp", "added map info: '%s' (%d)", r.front().m_aName, r.front().m_ID);
					Change = true;
				}
				else if(r.front().m_State == CMapInfo::MAPSTATE_FILE_MISSING)
				{
					r.front().m_State = CMapInfo::MAPSTATE_DOWNLOADING;
					str_format(aFilename, sizeof(aFilename), pPath, Info.m_aName);
					Download(pGameServer, aFilename, Map["get_download_url"].asCString(), OnDownloadMap);
				}
			}
		}

		if(Change)
		{
			pWebapp->m_lMapList.sort_range();
			pWebapp->OnInit();
		}
	}
}

void CServerWebapp::OnDownloadMap(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CFileResponse *pRes = (CFileResponse*)pResponse;
	CGameContext *pGameServer = (CGameContext*)pUserData;
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	if(!Error)
	{
		CMapInfo *pInfo = pGameServer->Webapp()->AddMap(pRes->GetFilename());
		if(pInfo && str_comp(pInfo->m_aName, g_Config.m_SvMap) == 0)
			pGameServer->Server()->ReloadMap();
	}
	else
	{
		pGameServer->Server()->Storage()->RemoveFile(pRes->GetPath(), IStorage::TYPE_SAVE);
		dbg_msg("webapp", "could not download map: '%s'", pRes->GetPath());
	}
}

void CServerWebapp::OnRunPost(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebRunData *pUser = (CWebRunData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	CServerWebapp *pWebapp = pGameServer->Webapp();
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	if(!Error && pUser->m_Tick > -1)
	{
		char aFilename[256];
		char aURL[128];

		str_format(aFilename, sizeof(aFilename), "/demos/teerace/%d_%d_%d.demo", pUser->m_Tick, g_Config.m_SvPort, pUser->m_ClientID);
		str_format(aURL, sizeof(aURL), "files/demo/%d/%d/", pUser->m_UserID, pWebapp->CurrentMap()->m_ID);
		pWebapp->AddUpload(aFilename, aURL, "demo_file", OnUploadFile, time_get() + time_freq() * 2);

		str_format(aFilename, sizeof(aFilename), "/ghosts/teerace/%d_%d_%d.gho", pUser->m_Tick, g_Config.m_SvPort, pUser->m_ClientID);
		str_format(aURL, sizeof(aURL), "files/ghost/%d/%d/", pUser->m_UserID, pWebapp->CurrentMap()->m_ID);
		pWebapp->AddUpload(aFilename, aURL, "ghost_file", OnUploadFile);
	}
	delete pUser;
}

void CServerWebapp::OnUploadFile(IResponse *pResponse, bool ConnError, void *pUserData)
{
	CWebUploadData *pUser = (CWebUploadData*)pUserData;
	CGameContext *pGameServer = pUser->m_pGameServer;
	bool Error = ConnError || pResponse->StatusCode() != 200;
	CheckStatusCode(pGameServer->Console(), pResponse->StatusCode());

	if(!Error)
		dbg_msg("webapp", "uploaded file: '%s'", pUser->m_aFilename);
	else
		dbg_msg("webapp", "could not upload file: '%s'", pUser->m_aFilename);
	pGameServer->Server()->Storage()->RemoveFile(pUser->m_aFilename, IStorage::TYPE_SAVE);
	delete pUser;
}

int CServerWebapp::MaplistFetchCallback(const char *pName, int IsDir, int StorageType, void *pUser)
{
	CServerWebapp *pWebapp = (CServerWebapp*)pUser;
	int Length = str_length(pName);
	if(!IsDir && Length >= 4 && str_comp(pName+Length-4, ".map") == 0)
		pWebapp->AddMap(pName);
	return 0;
}

CServerWebapp::CMapInfo *CServerWebapp::AddMap(const char *pFilename)
{
	char aFile[256];
	str_format(aFile, sizeof(aFile), "maps/teerace/%s", pFilename);
	CDataFileReader DataFile;
	if(!DataFile.Open(m_pServer->Storage(), aFile, IStorage::TYPE_SAVE))
		return 0;

	CMapInfo Info;
	Info.m_Crc = DataFile.Crc();
	DataFile.Close();
	str_copy(Info.m_aName, pFilename, min((int)sizeof(Info.m_aName),str_length(pFilename)-3));

	array<CMapInfo>::range r = find_linear(m_lMapList.all(), Info);
	if(r.empty()) // new entry
	{
		Info.m_State = CMapInfo::MAPSTATE_INFO_MISSING;
		int Num = m_lMapList.add(Info);
		dbg_msg("", "added map: '%s' (%08x)", Info.m_aName, Info.m_Crc);
		return &m_lMapList[Num];
	}
	else if(r.front().m_State == CMapInfo::MAPSTATE_DOWNLOADING) // entry already exists
	{
		if(r.front().m_Crc == Info.m_Crc)
		{
			r.front().m_State = CMapInfo::MAPSTATE_COMPLETE;
			dbg_msg("", "added map: '%s' (%08x)", Info.m_aName, Info.m_Crc);
			m_lMapList.sort_range();
			AddMapVotes();
			return &r.front();
		}
		// something went wrong
		r.front().m_State = CMapInfo::MAPSTATE_FILE_MISSING;
	}
	Server()->Storage()->RemoveFile(aFile, IStorage::TYPE_SAVE);
	return 0;
}

void CServerWebapp::OnInit()
{
	m_CurrentMap.m_ID = -1;
	str_copy(m_CurrentMap.m_aName, g_Config.m_SvMap, sizeof(m_CurrentMap.m_aName));
	array<CMapInfo>::range r = find_linear(m_lMapList.all(), m_CurrentMap);
	if(!r.empty() && r.front().m_State == CMapInfo::MAPSTATE_COMPLETE)
	{
		m_CurrentMap = r.front();
		dbg_msg("webapp", "current map: '%s' (%d)", m_CurrentMap.m_aName, m_CurrentMap.m_ID);

		// add votes
		AddMapVotes();
	}
}

void CServerWebapp::Tick()
{
	// do uploads
	for(int i = 0; i < m_lUploads.size(); i++)
	{
		if(m_lUploads[i].m_StartTime <= time_get())
		{
			Upload(GameServer(), m_lUploads[i].m_aFilename, m_lUploads[i].m_aURL, m_lUploads[i].m_aUploadname, m_lUploads[i].m_pfnCallback);
			m_lUploads.remove_index_fast(i);
			i--; // since one item was removed
		}
	}

	// load maplist every 20 minutes
	if(m_LastMapListLoad == -1 || m_LastMapListLoad + Server()->TickSpeed() * 60 * 20 < Server()->Tick())
	{
		LoadMapList();
		m_LastMapListLoad = Server()->Tick();
	}
}

void CServerWebapp::LoadMapList()
{
	CBufferRequest *pRequest = CreateAuthedApiRequest(IRequest::HTTP_GET, "/maps/list/");
	CRequestInfo *pInfo = new CRequestInfo(ITeerace::Host());
	pInfo->SetCallback(OnMapList, GameServer());
	Server()->SendHttp(pInfo, pRequest);
}

void CServerWebapp::AddUpload(const char *pFilename, const char *pURL, const char *pUploadName, FHttpCallback pfnCallback, int64 StartTime)
{
	m_lUploads.add(CUpload(pFilename, pURL, pUploadName, pfnCallback, StartTime));
}

void CServerWebapp::AddMapVotes()
{
	if(g_Config.m_WaAutoAddMaps && m_CurrentMap.m_ID != -1)
	{
		for(int i = 0; i < m_lMapList.size(); i++)
		{
			CMapInfo *pMapInfo = &m_lMapList[i];
			if(pMapInfo->m_State == CMapInfo::MAPSTATE_COMPLETE && m_CurrentMap.m_MapType == pMapInfo->m_MapType)
			{
				char aVoteDescription[128];
				if(str_find(g_Config.m_WaVoteDescription, "%s"))
					str_format(aVoteDescription, sizeof(aVoteDescription), g_Config.m_WaVoteDescription, pMapInfo->m_aName);
				else
					str_format(aVoteDescription, sizeof(aVoteDescription), "change map to %s", pMapInfo->m_aName);

				// check for duplicate entry
				int OptionFound = false;
				CVoteOptionServer *pOption = GameServer()->m_pVoteOptionFirst;
				while(pOption)
				{
					if(str_comp_nocase(aVoteDescription, pOption->m_aDescription) == 0)
					{
						OptionFound = true;
						break;
					}
					pOption = pOption->m_pNext;
				}

				if(OptionFound)
					continue;

				// add the option
				++GameServer()->m_NumVoteOptions;
				char aCommand[128];
				str_format(aCommand, sizeof(aCommand), "sv_map %s", pMapInfo->m_aName);
				int Len = str_length(aCommand);

				pOption = (CVoteOptionServer *)GameServer()->m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
				pOption->m_pNext = 0;
				pOption->m_pPrev = GameServer()->m_pVoteOptionLast;
				if(pOption->m_pPrev)
					pOption->m_pPrev->m_pNext = pOption;
				GameServer()->m_pVoteOptionLast = pOption;
				if(!GameServer()->m_pVoteOptionFirst)
					GameServer()->m_pVoteOptionFirst = pOption;

				str_copy(pOption->m_aDescription, aVoteDescription, sizeof(pOption->m_aDescription));
				mem_copy(pOption->m_aCommand, aCommand, Len+1);
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "added option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
				GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

				// inform clients about added option
				CNetMsg_Sv_VoteOptionAdd OptionMsg;
				OptionMsg.m_pDescription = pOption->m_aDescription;
				GameServer()->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);
			}
		}
	}
} 

#endif
