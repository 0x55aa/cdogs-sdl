/*
    C-Dogs SDL
    A port of the legendary (and fun) action/arcade cdogs.
    Copyright (C) 1995 Ronny Wester
    Copyright (C) 2003 Jeremy Chin
    Copyright (C) 2003-2007 Lucas Martin-King

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    This file incorporates work covered by the following copyright and
    permission notice:

    Copyright (c) 2013-2015, Cong Xu
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/
#include "game.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include <cdogs/actors.h>
#include <cdogs/ai.h>
#include <cdogs/ai_coop.h>
#include <cdogs/ammo.h>
#include <cdogs/automap.h>
#include <cdogs/camera.h>
#include <cdogs/config.h>
#include <cdogs/events.h>
#include <cdogs/game_events.h>
#include <cdogs/handle_game_events.h>
#include <cdogs/joystick.h>
#include <cdogs/los.h>
#include <cdogs/mission.h>
#include <cdogs/music.h>
#include <cdogs/net_client.h>
#include <cdogs/net_server.h>
#include <cdogs/objs.h>
#include <cdogs/palette.h>
#include <cdogs/particle.h>
#include <cdogs/pic_manager.h>
#include <cdogs/pics.h>
#include <cdogs/powerup.h>
#include <cdogs/triggers.h>


static void PlayerSpecialCommands(TActor *actor, const int cmd)
{
	if ((cmd & CMD_BUTTON2) && CMD_HAS_DIRECTION(cmd))
	{
		if (ConfigGetEnum(&gConfig, "Game.SwitchMoveStyle") == SWITCHMOVE_SLIDE)
		{
			SlideActor(actor, cmd);
		}
	}
	else if (
		(actor->lastCmd & CMD_BUTTON2) &&
		!(cmd & CMD_BUTTON2) &&
		!actor->specialCmdDir &&
		!actor->CanPickupSpecial &&
		!(ConfigGetEnum(&gConfig, "Game.SwitchMoveStyle") == SWITCHMOVE_SLIDE && CMD_HAS_DIRECTION(cmd)) &&
		ActorCanSwitchGun(actor))
	{
		GameEvent e = GameEventNew(GAME_EVENT_ACTOR_SWITCH_GUN);
		e.u.ActorSwitchGun.UID = actor->uid;
		e.u.ActorSwitchGun.GunIdx = (actor->gunIndex + 1) % actor->guns.size;
		GameEventsEnqueue(&gGameEvents, e);
	}
}


// TODO: reimplement in camera
Vec2i GetPlayerCenter(
	GraphicsDevice *device, const Camera *camera,
	const PlayerData *pData, const int playerIdx)
{
	if (pData->ActorUID < 0)
	{
		// Player is dead
		return Vec2iZero();
	}
	Vec2i center = Vec2iZero();
	int w = device->cachedConfig.Res.x;
	int h = device->cachedConfig.Res.y;

	if (GetNumPlayers(PLAYER_ALIVE_OR_DYING, true, true) == 1 ||
		GetNumPlayers(PLAYER_ALIVE_OR_DYING, false , true) == 1 ||
		CameraIsSingleScreen())
	{
		const Vec2i pCenter = camera->lastPosition;
		const Vec2i screenCenter =
			Vec2iNew(w / 2, device->cachedConfig.Res.y / 2);
		const TActor *actor = ActorGetByUID(pData->ActorUID);
		const Vec2i p = Vec2iNew(actor->tileItem.x, actor->tileItem.y);
		center = Vec2iAdd(
			Vec2iAdd(p, Vec2iScale(pCenter, -1)), screenCenter);
	}
	else
	{
		const int numLocalPlayers = GetNumPlayers(PLAYER_ANY, false, true);
		if (numLocalPlayers == 2)
		{
			center.x = playerIdx == 0 ? w / 4 : w * 3 / 4;
			center.y = h / 2;
		}
		else if (numLocalPlayers >= 3 && numLocalPlayers <= 4)
		{
			center.x = (playerIdx & 1) ? w * 3 / 4 : w / 4;
			center.y = (playerIdx >= 2) ? h * 3 / 4 : h / 4;
		}
		else
		{
			CASSERT(false, "invalid number of players");
		}
	}
	// Add draw buffer offset
	return Vec2iMinus(center, Vec2iNew(camera->Buffer.dx, camera->Buffer.dy));
}

typedef struct
{
	struct MissionOptions *m;
	Map *map;
	Camera Camera;
	int frames;
	// TODO: turn the following into a screen system?
	input_device_e pausingDevice;	// INPUT_DEVICE_UNSET if not paused
	bool isMap;
	int cmds[MAX_LOCAL_PLAYERS];
	int lastCmds[MAX_LOCAL_PLAYERS];
	PowerupSpawner healthSpawner;
	CArray ammoSpawners;	// of PowerupSpawner
	GameLoopData loop;
} RunGameData;
static void RunGameInput(void *data);
static GameLoopResult RunGameUpdate(void *data);
static void RunGameDraw(void *data);
bool RunGame(struct MissionOptions *m, Map *map)
{
	RunGameData data;
	memset(&data, 0, sizeof data);
	data.m = m;
	data.map = map;

	CameraInit(&data.Camera);
	HealthSpawnerInit(&data.healthSpawner, map);
	CArrayInit(&data.ammoSpawners, sizeof(PowerupSpawner));
	for (int i = 0; i < AmmoGetNumClasses(&gAmmo); i++)
	{
		PowerupSpawner ps;
		AmmoSpawnerInit(&ps, map, i);
		CArrayPushBack(&data.ammoSpawners, &ps);
	}

	if (MusicGetStatus(&gSoundDevice) != MUSIC_OK)
	{
		HUDDisplayMessage(
			&data.Camera.HUD, MusicGetErrorMessage(&gSoundDevice), 140);
	}

	m->time = 0;
	m->pickupTime = 0;
	m->state = MISSION_STATE_PLAY;
	m->isDone = false;
	Pic *crosshair = PicManagerGetPic(&gPicManager, "crosshair");
	crosshair->offset.x = -crosshair->size.x / 2;
	crosshair->offset.y = -crosshair->size.y / 2;
	EventReset(&gEventHandlers, crosshair);

	NetServerSendGameStartMessages(&gNetServer, NET_SERVER_BCAST);
	GameEvent start = GameEventNew(GAME_EVENT_GAME_START);
	GameEventsEnqueue(&gGameEvents, start);

	// Set mission complete and display exit if it is complete
	MissionSetMessageIfComplete(m);

	data.loop = GameLoopDataNew(
		&data, RunGameUpdate, &data, RunGameDraw);
	data.loop.InputData = &data;
	data.loop.InputFunc = RunGameInput;
	data.loop.FPS = FPS_FRAMELIMIT;
	data.loop.InputEverySecondFrame = true;
	GameLoop(&data.loop);

	PowerupSpawnerTerminate(&data.healthSpawner);
	for (int i = 0; i < (int)data.ammoSpawners.size; i++)
	{
		PowerupSpawnerTerminate(CArrayGet(&data.ammoSpawners, i));
	}
	CArrayTerminate(&data.ammoSpawners);
	CameraTerminate(&data.Camera);

	return
		m->state == MISSION_STATE_PICKUP &&
		m->pickupTime + PICKUP_LIMIT <= m->time;
}
static void RunGameInput(void *data)
{
	RunGameData *rData = data;

	if (gEventHandlers.HasQuit)
	{
		GameEvent e = GameEventNew(GAME_EVENT_MISSION_END);
		GameEventsEnqueue(&gGameEvents, e);
		return;
	}

	memset(rData->cmds, 0, sizeof rData->cmds);
	int cmdAll = 0;
	int idx = 0;
	input_device_e pausingDevice = INPUT_DEVICE_UNSET;
	for (int i = 0; i < (int)gPlayerDatas.size; i++, idx++)
	{
		const PlayerData *p = CArrayGet(&gPlayerDatas, i);
		if (!p->IsLocal)
		{
			idx--;
			continue;
		}
		rData->cmds[idx] = GetGameCmd(
			&gEventHandlers,
			p,
			GetPlayerCenter(&gGraphicsDevice, &rData->Camera, p, idx));
		cmdAll |= rData->cmds[idx];

		// Only allow the first player to escape
		// Use keypress otherwise the player will quit immediately
		if (idx == 0 &&
			(rData->cmds[idx] & CMD_ESC) && !(rData->lastCmds[idx] & CMD_ESC))
		{
			pausingDevice = p->inputDevice;
		}
		rData->lastCmds[idx] = rData->cmds[idx];
	}
	if (KeyIsPressed(&gEventHandlers.keyboard, SDLK_ESCAPE))
	{
		pausingDevice = INPUT_DEVICE_KEYBOARD;
	}

	// Check if automap key is pressed by any player
	rData->isMap =
		IsAutoMapEnabled(gCampaign.Entry.Mode) &&
		(KeyIsDown(&gEventHandlers.keyboard, ConfigGetInt(&gConfig, "Input.PlayerKeys0.map")) ||
		(cmdAll & CMD_MAP));

	// Check if escape was pressed
	// If the game was not paused, enter pause mode
	// If the game was paused, exit the game
	if (AnyButton(cmdAll))
	{
		rData->pausingDevice = INPUT_DEVICE_UNSET;
	}
	else if (pausingDevice != INPUT_DEVICE_UNSET)
	{
		// Escape pressed
		if (rData->pausingDevice != INPUT_DEVICE_UNSET)
		{
			// Exit
			GameEvent e = GameEventNew(GAME_EVENT_MISSION_END);
			GameEventsEnqueue(&gGameEvents, e);
			// Need to unpause to process the quit
			rData->pausingDevice = INPUT_DEVICE_UNSET;
		}
		else
		{
			// Pause the game
			rData->pausingDevice = pausingDevice;
		}
	}
}
static void CheckMissionCompletion(const struct MissionOptions *mo);
static GameLoopResult RunGameUpdate(void *data)
{
	RunGameData *rData = data;

	// Detect exit
	if (rData->m->isDone)
	{
		return UPDATE_RESULT_EXIT;
	}

	// If we're not hosting a net game,
	// don't update if the game has paused or has automap shown
	if (!gCampaign.IsClient && !ConfigGetBool(&gConfig, "StartServer") &&
		(rData->pausingDevice != INPUT_DEVICE_UNSET || rData->isMap))
	{
		return UPDATE_RESULT_DRAW;
	}

	// If slow motion, update every other frame
	if (ConfigGetBool(&gConfig, "Game.SlowMotion") && (rData->loop.Frames & 1))
	{
		return UPDATE_RESULT_OK;
	}

	// Update all the things in the game
	const int ticksPerFrame = 1;

	LOSReset(&gMap);
	for (int i = 0, idx = 0; i < (int)gPlayerDatas.size; i++, idx++)
	{
		const PlayerData *p = CArrayGet(&gPlayerDatas, i);
		if (p->ActorUID == -1) continue;
		TActor *player = ActorGetByUID(p->ActorUID);
		if (player->dead > DEATH_MAX) continue;
		// Calculate LOS for all players alive or dying
		LOSCalcFrom(
			&gMap,
			Vec2iToTile(Vec2iNew(player->tileItem.x, player->tileItem.y)),
			!gCampaign.IsClient);

		if (player->dead) continue;

		// Only handle inputs/commands for local players
		if (!p->IsLocal)
		{
			idx--;
			continue;
		}
		if (p->inputDevice == INPUT_DEVICE_AI)
		{
			rData->cmds[idx] = AICoopGetCmd(player, ticksPerFrame);
		}
		PlayerSpecialCommands(player, rData->cmds[idx]);
		CommandActor(player, rData->cmds[idx], ticksPerFrame);
	}

	if (!gCampaign.IsClient)
	{
		CommandBadGuys(ticksPerFrame);
	}

	// If split screen never and players are too close to the
	// edge of the screen, forcefully pull them towards the center
	if (ConfigGetEnum(&gConfig, "Interface.Splitscreen") == SPLITSCREEN_NEVER &&
		CameraIsSingleScreen() &&
		GetNumPlayers(true, true, true) > 1)
	{
		const int w = gGraphicsDevice.cachedConfig.Res.x;
		const int h = gGraphicsDevice.cachedConfig.Res.y;
		const Vec2i screen = Vec2iAdd(
			PlayersGetMidpoint(), Vec2iNew(-w / 2, -h / 2));
		for (int i = 0; i < (int)gPlayerDatas.size; i++)
		{
			const PlayerData *pd = CArrayGet(&gPlayerDatas, i);
			if (!pd->IsLocal || !IsPlayerAlive(pd))
			{
				continue;
			}
			const TActor *p = ActorGetByUID(pd->ActorUID);
			const int pad = CAMERA_SPLIT_PADDING;
			GameEvent ei = GameEventNew(GAME_EVENT_ACTOR_IMPULSE);
			ei.u.ActorImpulse.Id = p->tileItem.id;
			ei.u.ActorImpulse.Vel = p->Vel;
			if (screen.x + pad > p->tileItem.x && p->Vel.x < 256)
			{
				ei.u.ActorImpulse.Vel.x = 256 - p->Vel.x;
			}
			else if (screen.x + w - pad < p->tileItem.x && p->Vel.x > -256)
			{
				ei.u.ActorImpulse.Vel.x = -256 - p->Vel.x;
			}
			if (screen.y + pad > p->tileItem.y && p->Vel.y < 256)
			{
				ei.u.ActorImpulse.Vel.y = 256 - p->Vel.y;
			}
			else if (screen.y + h - pad < p->tileItem.y && p->Vel.y > -256)
			{
				ei.u.ActorImpulse.Vel.y = -256 - p->Vel.y;
			}
			if (!Vec2iEqual(ei.u.ActorImpulse.Vel, p->Vel))
			{
				GameEventsEnqueue(&gGameEvents, ei);
			}
		}
	}

	UpdateAllActors(ticksPerFrame);
	UpdateObjects(ticksPerFrame);
	UpdateMobileObjects(ticksPerFrame);
	ParticlesUpdate(&gParticles, ticksPerFrame);

	UpdateWatches(&rData->map->triggers, ticksPerFrame);

	PowerupSpawnerUpdate(&rData->healthSpawner, ticksPerFrame);
	for (int i = 0; i < (int)rData->ammoSpawners.size; i++)
	{
		PowerupSpawnerUpdate(CArrayGet(&rData->ammoSpawners, i), ticksPerFrame);
	}

	if (!gCampaign.IsClient)
	{
		CheckMissionCompletion(rData->m);
	}
	else if (!NetClientIsConnected(&gNetClient))
	{
		// Check if disconnected from server; end mission
		rData->m->isDone = true;
	}

	HandleGameEvents(
		&gGameEvents, &rData->Camera,
		&rData->healthSpawner, &rData->ammoSpawners);

	rData->m->time += ticksPerFrame;

	CameraUpdate(
		&rData->Camera, rData->cmds[0], ticksPerFrame, 1000 / rData->loop.FPS);

	return UPDATE_RESULT_DRAW;
}
static void CheckMissionCompletion(const struct MissionOptions *mo)
{
	// Check if we need to update explore objectives
	for (int i = 0; i < (int)mo->missionData->Objectives.size; i++)
	{
		const MissionObjective *mobj =
			CArrayGet(&mo->missionData->Objectives, i);
		if (mobj->Type != OBJECTIVE_INVESTIGATE) continue;
		const ObjectiveDef *o = CArrayGet(&mo->Objectives, i);
		const int update = MapGetExploredPercentage(&gMap) - o->done;
		if (update > 0 && !gCampaign.IsClient)
		{
			GameEvent e = GameEventNew(GAME_EVENT_OBJECTIVE_UPDATE);
			e.u.ObjectiveUpdate.ObjectiveId = i;
			e.u.ObjectiveUpdate.Count = update;
			GameEventsEnqueue(&gGameEvents, e);
		}
	}

	const bool isMissionComplete =
		GetNumPlayers(PLAYER_ALIVE_OR_DYING, false, false) > 0 && IsMissionComplete(mo);
	if (mo->state == MISSION_STATE_PLAY && isMissionComplete)
	{
		GameEvent e = GameEventNew(GAME_EVENT_MISSION_PICKUP);
		GameEventsEnqueue(&gGameEvents, e);
	}
	if (mo->state == MISSION_STATE_PICKUP && !isMissionComplete)
	{
		GameEvent e = GameEventNew(GAME_EVENT_MISSION_INCOMPLETE);
		GameEventsEnqueue(&gGameEvents, e);
	}
	if (mo->state == MISSION_STATE_PICKUP &&
		mo->pickupTime + PICKUP_LIMIT <= mo->time)
	{
		GameEvent e = GameEventNew(GAME_EVENT_MISSION_END);
		GameEventsEnqueue(&gGameEvents, e);
	}

	// Check that all players have been destroyed
	// Note: there's a period of time where players are dying
	// Wait until after this period before ending the game
	bool allPlayersDestroyed = true;
	for (int i = 0; i < (int)gPlayerDatas.size; i++)
	{
		const PlayerData *p = CArrayGet(&gPlayerDatas, i);
		if (p->ActorUID != -1)
		{
			allPlayersDestroyed = false;
			break;
		}
	}
	if (allPlayersDestroyed && AreAllPlayersDeadAndNoLives())
	{
		GameEvent e = GameEventNew(GAME_EVENT_MISSION_END);
		GameEventsEnqueue(&gGameEvents, e);
	}
}
static void RunGameDraw(void *data)
{
	RunGameData *rData = data;

	// Draw everything
	CameraDraw(&rData->Camera, rData->pausingDevice);

	if (GameIsMouseUsed())
	{
		MouseDraw(&gEventHandlers.mouse);
	}

	// Draw automap if enabled
	if (rData->isMap)
	{
		AutomapDraw(0, rData->Camera.HUD.showExit);
	}
}
