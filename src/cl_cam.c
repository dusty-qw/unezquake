/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the included (GNU.txt) GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/* ZOID
 *
 * Player camera tracking in Spectator mode
 *
 * This takes over player controls for spectator automatic camera.
 * Player moves as a spectator, but the camera tracks and enemy player
 */

#include "quakedef.h"
#include "gl_model.h"
#include "teamplay.h"
#include "pmove.h"
#include "utils.h"
#include "sbar.h"
#include "qtv.h"

static int Cam_MainTrackNum(void);

static vec3_t desired_position; // where the camera wants to be.
static int oldbuttons;
static qbool cmddown, olddown;
extern cvar_t cam_thirdperson, cl_camera_tpp;

cvar_t cl_hightrack = {"cl_hightrack", "0" };	// track high fragger 
cvar_t cl_chasecam = {"cl_chasecam", "1"};		// "through the eyes" view

vec3_t cam_viewangles;
double cam_lastviewtime;

void CL_TrackMV1_f(void);
void CL_TrackMV2_f(void);
void CL_TrackMV3_f(void);
void CL_TrackMV4_f(void);
void CL_TrackTeam_f(void); 

void CL_Track_f(void);
void CL_Trackkiller_f(void);
void CL_Autotrack_f(void);

double last_lock = 0;			// Last time Cam_Lock() successful
static double auto_retrack_time = 0;	// Time when we should try to retrack
static int auto_retrack_target = -1;	// Player we're trying to retrack after map change

static int	killer = -1;		// id of the player who killed the player we are now tracking

void CL_Cam_SetKiller(int killernum, int victimnum) {
	if (victimnum != cl.viewplayernum) return;
	if (killernum < 0 || killernum >= MAX_CLIENTS) return;

	killer = killernum;
}

void vectoangles(vec3_t vec, vec3_t ang) {
	float forward, yaw, pitch;
	
	if (vec[1] == 0 && vec[0] == 0) {
		yaw = 0;
		pitch = (vec[2] > 0) ? 90 : 270;
	} else {
		yaw = /*(int)*/ (atan2(vec[1], vec[0]) * 180 / M_PI);
		if (yaw < 0)
			yaw += 360;

		forward = sqrt (vec[0]*vec[0] + vec[1]*vec[1]);
		pitch = /*(int)*/ (atan2(vec[2], forward) * 180 / M_PI);
		if (pitch < 0)
			pitch += 360;
	}

	ang[0] = pitch;
	ang[1] = yaw;
	ang[2] = 0;
}

void Cam_SetViewPlayer (void)
{
	int new_track;

	if (cl.spectator && cl.autocam && cl.spec_locked && cl_chasecam.value) {
		new_track = cl.spec_track;
	}
	else {
		new_track = cl.playernum;
	}

	if (new_track != cl.viewplayernum) {
		memset(cl.antilag_positions, 0, sizeof(cl.antilag_positions));
	}
	cl.viewplayernum = new_track;
}

// returns true if weapon model should be drawn in camera mode
qbool Cam_DrawViewModel(void) {
	if (!cl.spectator)
		return true;

	if (cl.autocam && cl.spec_locked && cl_chasecam.value)
		return true;
	return false;
}

static qbool Cam_FirstPersonMode(void)
{
	return cl_chasecam.value && !cam_thirdperson.integer && !cl_camera_tpp.integer;
}

// returns true if we should draw this player, we don't if we are chase camming
qbool Cam_DrawPlayer(int playernum)
{
	if (cl.spectator && cl.autocam && cl.spec_locked && cl.spec_track == playernum && Cam_FirstPersonMode())
		return false;
	return true;
}

void Cam_Unlock(void) 
{
	if (Cmd_FindAlias("f_freeflyspectate"))
	{
		Cbuf_AddTextEx (&cbuf_main, "f_freeflyspectate\n");
	}

	if (!cl.autocam) {
		return;
	}

	if (cls.mvdplayback == QTV_PLAYBACK)
	{
		// its not setinfo extension, but adding new extension just for this is stupid IMO
		QTV_Cmd_Printf(QTV_EZQUAKE_EXT_SETINFO, "ptrack");
	}
	else
	{
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message, "ptrack");
	}

	cl.autocam = CAM_NONE;
	cl.spec_locked = false;
	Sbar_Changed();

	if (cls.mvdplayback && cl.teamfortress) 
	{
		V_TF_ClearGrenadeEffects ();
	}

	if (TP_NeedRefreshSkins())
	{
		TP_RefreshSkins();
	}
}

void Cam_Lock(int playernum) 
{
	char st[32];

	if (Cmd_FindAlias("f_trackspectate")) {
		Cbuf_AddTextEx (&cbuf_main, "f_trackspectate\n");
	}

	snprintf(st, sizeof (st), "ptrack %i", playernum);
	if (cls.mvdplayback == QTV_PLAYBACK) {
		// its not setinfo extension, but adding new extension just for this is stupid IMO
		QTV_Cmd_Printf(QTV_EZQUAKE_EXT_SETINFO, st);
	}
	else {
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		MSG_WriteString (&cls.netchan.message, st);
	}

	if (CL_MultiviewEnabled ()) {
		CL_MultiviewSetTrackSlot (-1, playernum);
		if (!cls.findtrack) {
			return;
		}
	}

	if (cls.mvdplayback) {
		memcpy(cl.stats, cl.players[playernum].stats, sizeof(cl.stats));
		cl.ideal_track = playernum;
		cl.mvd_time_offset = 0;
	}
	last_lock = cls.realtime;

	cl.spec_track = playernum;
	cl.spec_locked = false;
	Sbar_Changed();

	if (TP_NeedRefreshSkins())
		TP_RefreshSkins();
}

// Initialize auto-retrack after map change
void Cam_InitAutoRetrack(void)
{
	// If we were tracking someone before the map change, set up auto-retrack
	if (cl.spectator && cl.spec_track >= 0 && cl.spec_track < MAX_CLIENTS) {
		auto_retrack_target = cl.spec_track;
		auto_retrack_time = cls.realtime + 2.0; // Wait 2 seconds before first attempt
	} else {
		// No tracking to restore
		auto_retrack_target = -1;
	}
}

// Checks if we need to auto-retrack a player after map change
static void Cam_AutoRetrack(void)
{
	frame_t *frame;
	player_state_t *player;
	static double auto_retrack_start_time = 0;
	
	// Check if we have a pending auto-retrack
	if (auto_retrack_target < 0 || auto_retrack_target >= MAX_CLIENTS)
		return;
		
	// Don't try too frequently
	if (cls.realtime < auto_retrack_time)
		return;
		
	// Check if we're already tracking someone (any player)
	if (cl.spec_track >= 0 && cl.spec_locked) {
		// We're successfully tracking someone, cancel auto-retrack
		auto_retrack_target = -1;
		auto_retrack_start_time = 0;
		return;
	}
	
	frame = &cl.frames[cl.validsequence & UPDATE_MASK];
	player = &frame->playerstate[auto_retrack_target];
	
	// Check if the player we want to track has reconnected
	if (player->messagenum == cl.parsecount || player->messagenum == cl.oldparsecount) {
		// Player is back, try to lock onto them
		Cam_Lock(auto_retrack_target);
		auto_retrack_target = -1; // Clear the auto-retrack
		auto_retrack_start_time = 0; // Clear the start time
	} else {
		// Try again in 0.5 seconds
		auto_retrack_time = cls.realtime + 0.5;
		
		// Initialize start time on first attempt
		if (auto_retrack_start_time == 0) {
			auto_retrack_start_time = cls.realtime;
		}
		
		// Give up after 10 seconds from initial attempt
		if (cls.realtime - auto_retrack_start_time > 10) {
			int i;
			player_info_t *s;
			
			// Player didn't reconnect during timeout period
			// Try to find first available player to track
			for (i = 0; i < MAX_CLIENTS; i++) {
				s = &cl.players[i];
				if (s->name[0] && !s->spectator) {
					Cam_Lock(i);
					cl.ideal_track = i;
					break;
				}
			}
			
			auto_retrack_target = -1;
			auto_retrack_start_time = 0;
		}
	}
}

trace_t Cam_DoTrace(vec3_t vec1, vec3_t vec2) {
	VectorCopy (vec1, pmove.origin);
	return PM_PlayerTrace(pmove.origin, vec2);
}
	
// Returns distance or 9999 if invalid for some reason
static float Cam_TryFlyby(player_state_t *self, player_state_t *player, vec3_t vec, qbool checkvis) {
	vec3_t v;
	trace_t trace;
	float len;

	vectoangles(vec, v);
	VectorCopy (v, pmove.angles);
	VectorNormalizeFast(vec);
	VectorMA(player->origin, 800, vec, v);
	// v is endpos
	// fake a player move
	trace = Cam_DoTrace(player->origin, v);
	if (trace.inwater)
		return 9999;
	VectorCopy(trace.endpos, vec);
	VectorSubtract(trace.endpos, player->origin, v);
	len = VectorLength(v);
	if (len < 32 || len > 800)
		return 9999;
	if (checkvis) {
		VectorSubtract(trace.endpos, self->origin, v);
		len = VectorLength(v);

		trace = Cam_DoTrace(self->origin, vec);
		if (trace.fraction != 1 || trace.inwater)
			return 9999;
	}
	return len;
}

// Is player visible?
static qbool Cam_IsVisible(player_state_t *player, vec3_t vec) {
	trace_t trace;
	vec3_t v;

	trace = Cam_DoTrace(player->origin, vec);
	if (trace.fraction != 1 || /*trace.inopen ||*/ trace.inwater)
		return false;
	// check distance, don't let the player get too far away or too close
	VectorSubtract(player->origin, vec, v);

	return ((v[0]*v[0]+v[1]*v[1]+v[2]*v[2]) >= 256);
}

static qbool InitFlyby(player_state_t *self, player_state_t *player, int checkvis) {
    float f, max;
    vec3_t vec, vec2;
	vec3_t forward, right, up;

	VectorCopy(player->viewangles, vec);
    vec[0] = 0;
	AngleVectors (vec, forward, right, up);

    max = 1000;
	VectorAdd(forward, up, vec2);
	VectorAdd(vec2, right, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorAdd(forward, up, vec2);
	VectorSubtract(vec2, right, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorAdd(forward, right, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorSubtract(forward, right, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorAdd(forward, up, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorSubtract(forward, up, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorAdd(up, right, vec2);
	VectorSubtract(vec2, forward, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorSubtract(up, right, vec2);
	VectorSubtract(vec2, forward, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	// invert
	VectorSubtract(vec3_origin, forward, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorCopy(forward, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	// invert
	VectorSubtract(vec3_origin, right, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }
	VectorCopy(right, vec2);
    if ((f = Cam_TryFlyby(self, player, vec2, checkvis)) < max) {
        max = f;
		VectorCopy(vec2, vec);
    }

	// ack, can't find him
    if (max >= 1000)
		return false;
	cl.spec_locked = true;
	VectorCopy(vec, desired_position); 
	return true;
}

// cl_hightrack 
static void Cam_CheckHighTarget(void)
{
	int i, j, max;
	player_info_t	*s;

	j = -1;
	for (i = 0, max = -9999; i < MAX_CLIENTS; i++) {
		s = &cl.players[i];
		if (s->name[0] && !s->spectator && s->frags > max) {
			max = s->frags;
			j = i;
		}
	}
	if (j >= 0) {
		if (!cl.spec_locked || cl.players[j].frags > cl.players[cl.spec_track].frags) {
			Cam_Lock(j);
			cl.ideal_track = cl.spec_track;
		}
	} else
		Cam_Unlock();
} 

// Take over the user controls and track a player.
// We find a nice position to watch the player and move there
void Cam_Track(usercmd_t *cmd) 
{
	player_state_t *player, *self;
	frame_t *frame;
	vec3_t vec;

	if (!cl.spectator) {
		return;
	}

	// Check for auto-retrack after map change
	Cam_AutoRetrack();

	// hack: save +movedown command
	cmddown = cmd->upmove < 0;

	// cl_hightrack 
	if (cl_hightrack.value && !cl.spec_locked)
	{
		Cam_CheckHighTarget(); 
	}
	
	if (!cl.autocam || cls.state != ca_active)
	{
		return;
	}

	if (cl.spec_locked && (!cl.players[cl.spec_track].name[0] || cl.players[cl.spec_track].spectator))
	{
		cl.spec_locked = false;

		// cl_hightrack 
		if (cl_hightrack.value)
		{
			Cam_CheckHighTarget();
		}
		else 
		{
			Cam_Unlock();
		}
		return;
	}

	frame = &cl.frames[cl.validsequence & UPDATE_MASK];

	if (cl.autocam && cls.mvdplayback) {
		if (cl.ideal_track != cl.spec_track && cls.realtime - last_lock > 0.1 && frame->playerstate[cl.ideal_track].messagenum == cl.parsecount) {
			Cam_Lock(cl.ideal_track);
		}

		if ((frame->playerstate[cl.spec_track].messagenum != cl.parsecount && frame->playerstate[cl.spec_track].messagenum != cl.oldparsecount) || Cam_MainTrackNum() != cl.ideal_track) {
			int i;

			for (i = 0; i < MAX_CLIENTS - 1; i++) {
				if ((frame->playerstate[i].messagenum == cl.parsecount || frame->playerstate[i].messagenum == cl.oldparsecount)) {
					break;
				}
			}

			Cam_Lock(i);
			cls.findtrack = (i >= MAX_CLIENTS - 1);
		}
	}

	player = frame->playerstate + cl.spec_track;
	self = frame->playerstate + cl.playernum;

	if (!cl.spec_locked || !Cam_IsVisible(player, desired_position))
	{
		if (!cl.spec_locked || cls.realtime - cam_lastviewtime > 0.1)
		{
			if (!InitFlyby(self, player, true))
				InitFlyby(self, player, false);
			cam_lastviewtime = cls.realtime;
		}
	} 
	else 
	{
		cam_lastviewtime = cls.realtime;
	}
	
	// couldn't track for some reason
	if (!cl.spec_locked || !cl.autocam)
		return;

	if (cl_chasecam.value) 
	{
		cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;

		#ifdef JSS_CAM
		if (!cam_thirdperson.integer)
		#endif
		{
			VectorCopy(player->viewangles, cl.viewangles);
		}
		VectorCopy(player->origin, desired_position);
		if (memcmp(&desired_position, &self->origin, sizeof(desired_position)) != 0) {
			MSG_WriteByte (&cls.netchan.message, clc_tmove);
			MSG_WriteCoord (&cls.netchan.message, desired_position[0]);
			MSG_WriteCoord (&cls.netchan.message, desired_position[1]);
			MSG_WriteCoord (&cls.netchan.message, desired_position[2]);
			// move there locally immediately
			VectorCopy(desired_position, self->origin);
		}
	} 
	else 
	{
		// Ok, move to our desired position and set our angles to view
		// the player
		VectorSubtract(desired_position, self->origin, vec);
		cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;
		if (VectorLength(vec) > 16) 
		{ 
			// close enough?
			MSG_WriteByte (&cls.netchan.message, clc_tmove);
			MSG_WriteCoord (&cls.netchan.message, desired_position[0]);
			MSG_WriteCoord (&cls.netchan.message, desired_position[1]);
			MSG_WriteCoord (&cls.netchan.message, desired_position[2]);
		}

		// move there locally immediately
		VectorCopy(desired_position, self->origin);
										 
		VectorSubtract(player->origin, desired_position, vec);
		vectoangles(vec, cl.viewangles);
		cl.viewangles[0] = -cl.viewangles[0];
	}
}

// Returns true if last new button command was jump
qbool Cam_JumpCheck(usercmd_t *cmd)
{
	if ((cmd->buttons & BUTTON_JUMP) && (oldbuttons & BUTTON_JUMP))
		return false;		// don't pogo stick

	if (!(cmd->buttons & BUTTON_JUMP))
	{
		oldbuttons &= ~BUTTON_JUMP;
		return false;
	}
	oldbuttons |= BUTTON_JUMP;	// don't jump again until released

	return true;
}

// Returns true if last new button command was jump
qbool Cam_MoveDownCheck(usercmd_t *cmd)
{
	if (cmddown && olddown)
		return false;

	if (!cmddown)
	{
		olddown = false;
		return false;
	}
	olddown = true;

	return true;
}

void Cam_FinishMove(usercmd_t *cmd) 
{
	int i, end, inc;
	player_info_t *s;

	if (cls.state != ca_active)
		return;

	if (!cl.spectator) // only in spectator mode
		return;

	if (cmd->buttons & BUTTON_ATTACK) {
		if (!(oldbuttons & BUTTON_ATTACK)) {

			oldbuttons |= BUTTON_ATTACK;
			cl.autocam++;

			if (cl.autocam > CAM_TRACK) {
				Cam_Unlock();
				VectorCopy(cl.viewangles, cmd->angles);
				return;
			}
		} else
			return;
	} else {
		oldbuttons &= ~BUTTON_ATTACK;
		if (!cl.autocam)
			return;
	}

	// cl_hightrack 
	if (cl.autocam && cl_hightrack.value)
	{
		Cam_CheckHighTarget();
		if (Cam_JumpCheck(cmd))
		{
			Com_Printf_State(PRINT_FAIL,"cl_hightrack enabled. Unable to switch POV.\n");
		}
		return;
	}

	if (Cam_MoveDownCheck(cmd)) {
		inc = -1;
	} else {
		inc = 1;
	}

	if (cl.spec_locked) {
		if (!Cam_JumpCheck(cmd) && inc == 1) {
			return;
		}
		// Swap the Multiview mvinset/main view pov when jump button is pressed.
		CL_MultiviewTrackingAdjustment (inc);
	}

	
	if (cl.spec_locked && cl.autocam) {
		end = (cl.ideal_track + MAX_CLIENTS + inc) % MAX_CLIENTS;
	}
	else {
		end = cl.ideal_track;
	}

	i = end;
	do {
		s = &cl.players[i];
		if (s->name[0] && !s->spectator && (!cls.mvdplayback || cl.frames[cl.parsecount & UPDATE_MASK].playerstate[i].messagenum == cl.parsecount)) {
			if (cls.mvdplayback && cl.teamfortress) {
				V_TF_ClearGrenadeEffects(); // BorisU
			}
			Cam_Lock(i);
			cl.ideal_track = i;
			return;
		}
		i = (i + MAX_CLIENTS + inc) % MAX_CLIENTS;
	} while (i != end);

	// stay on same guy?
	i = cl.ideal_track;
	s = &cl.players[i];
	if (s->name[0] && !s->spectator) {
		Cam_Lock(i);
		return;
	}
	Com_Printf ("No target found ...\n");
	cl.autocam = cl.spec_locked = false;
}

void Cam_Reset(void)
{
	cl.autocam = CAM_NONE;
	cl.spec_track = 0;
	cl.ideal_track = 0;
	cl.spec_locked = false;
}

//Fixes spectator chasecam demos
void Cam_TryLock (void) {
	int i, j, old_autocam, old_spec_track;
	player_state_t *state;
	static float cam_lastlocktime;

	if (!cl.validsequence)
		return;

	if (!cl.autocam)
		cam_lastlocktime = 0;

	old_autocam = cl.autocam;
	old_spec_track = cl.spec_track;

	state = cl.frames[cl.validsequence & UPDATE_MASK].playerstate;
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (!cl.players[i].name[0] || cl.players[i].spectator ||
			state[i].messagenum != cl.parsecount)
			continue;
		if (fabs(state[i].command.angles[0] - cl.viewangles[0]) < 2 && fabs(state[i].command.angles[1] - cl.viewangles[1]) < 2) {
			for (j = 0; j < 3; j++)
				if (fabs(state[i].origin[j] - state[cl.playernum].origin[j]) > 200)
					break;	// too far
			if (j < 3)
				continue;
			cl.autocam = CAM_TRACK;
			cl.spec_track = i;
			cl.spec_locked = true;
			cam_lastlocktime = cls.realtime;
			break;
		}
	}

	if (cls.realtime - cam_lastlocktime > 0.3) {
		// Couldn't lock to any player for 0.3 seconds, so assume
		// the spectator switched to free spectator mode
		cl.autocam = CAM_NONE;
		cl.spec_track = 0;
		cl.spec_locked = false;
	}

	if (cl.autocam != old_autocam || cl.spec_track != old_spec_track) {
		Sbar_Changed ();

		if (TP_NeedRefreshSkins())
			TP_RefreshSkins();
	}
}


#ifdef JSS_CAM
static char *myftos (float f)
{
#define MAX_VAL 128
	static char buf[4][MAX_VAL];
	static char idx = 0;
	char *val;
	int	i;

	if (!cls.demoplayback && !cl.spectator)
		return "?";

	val = buf[(idx++) & 3];

	snprintf (val, MAX_VAL, "%f", f);

	// strip trailing zeroes
	for (i = strlen(val) - 1; i > 0 && val[i] == '0'; i--)
		val[i] = 0;
	if (val[i] == '.')
		val[i] = 0;
		
	return val;
}

void Cam_Pos_Set(float x, float y, float z)
{
	extern qbool clpred_newpos;

	cl.simorg[0] = x;
	cl.simorg[1] = y;
	cl.simorg[2] = z;
	clpred_newpos = true;
	
	VectorCopy (cl.simorg, cl.frames[cl.validsequence & UPDATE_MASK].playerstate[cl.playernum].origin);
	
	if (cls.state >= ca_active && !cls.demoplayback) {
		MSG_WriteByte (&cls.netchan.message, clc_tmove);
		MSG_WriteCoord (&cls.netchan.message, cl.simorg[0]);
		MSG_WriteCoord (&cls.netchan.message, cl.simorg[1]);
		MSG_WriteCoord (&cls.netchan.message, cl.simorg[2]);
	}
}

static void Cam_Pos_f (void)
{
	if (Cmd_Argc() == 1)
	{
		Com_Printf ("\"%s %s %s\"\n", myftos(cl.simorg[0]), myftos(cl.simorg[1]), myftos(cl.simorg[2]));
		return;
	}

	if (Cmd_Argc() == 2) {
		// cam_pos "x y z"  -->  cam_pos x y z
		Cmd_TokenizeString (va("cam_pos %s", Cmd_Argv(1)));
	}

	if (Cmd_Argc() != 4) {
		Com_Printf("usage:\n"
			"cam_pos - show current coordinates\n"
			"cam pos x y z - set new coordinates\n");
		return;
	}

	if (!cls.demoplayback && !cl.spectator)
		return;

	Cam_Reset();
	Cam_Pos_Set(Q_atof(Cmd_Argv(1)), Q_atof(Cmd_Argv(2)), Q_atof(Cmd_Argv(3)));
}

void Cam_Angles_Set(float pitch, float yaw, float roll)
{
	cl.simangles[0] = pitch;
	cl.simangles[1] = yaw;
	cl.simangles[2] = roll;
	VectorCopy (cl.simangles, cl.viewangles);
}

static void Cam_Angles_f (void)
{
	if (Cmd_Argc() == 1)
	{
		Com_Printf ("\"%s %s %s\"\n", myftos(cl.viewangles[0]), myftos(cl.viewangles[1]), myftos(cl.viewangles[2]));
		return;
	}

	if (Cmd_Argc() == 2) {
		// cam_angles "pitch yaw roll"  -->  cam_pos pitch yaw roll
		Cmd_TokenizeString (va("cam_angles %s", Cmd_Argv(1)));
	}

	if (Cmd_Argc() != 4 && Cmd_Argc() != 3) {
		Com_Printf("usage:\n"
			"cam_pos - show current angles\n"
			"cam pos pitch yaw [roll] - set new angles\n");
		return;
	}
	
	if (!cls.demoplayback && !cl.spectator)
		return;

	Cam_Angles_Set(Q_atof(Cmd_Argv(1)), Q_atof(Cmd_Argv(2)), Q_atof(Cmd_Argv(3)));
}

static char *Macro_Cam_Pos_X (void) { return myftos(cl.simorg[0]); }
static char *Macro_Cam_Pos_Y (void) { return myftos(cl.simorg[1]); }
static char *Macro_Cam_Pos_Z (void) { return myftos(cl.simorg[2]); }
static char *Macro_Cam_Pos (void) {	return va("\"%s %s %s\"", myftos(cl.simorg[0]), myftos(cl.simorg[1]), myftos(cl.simorg[2])); }

static char *Macro_Cam_Angles_Pitch (void) { return myftos(cl.viewangles[0]); }
static char *Macro_Cam_Angles_Yaw (void) { return myftos(cl.viewangles[1]); }
static char *Macro_Cam_Angles_Roll (void) { return myftos(cl.viewangles[2]); }
static char *Macro_Cam_Angles (void) { return va("\"%s %s %s\"", myftos(cl.viewangles[0]), myftos(cl.viewangles[1]), myftos(cl.viewangles[2])); }
#endif // JSS_CAM


void CL_InitCam(void) 
{
	Cvar_SetCurrentGroup(CVAR_GROUP_SPECTATOR);

	// cl_hightrack 
	Cvar_Register (&cl_hightrack); 

	Cvar_Register (&cl_chasecam);

	Cvar_ResetCurrentGroup();
	Cmd_AddCommand ("track", CL_Track_f);
	Cmd_AddCommand ("autotrack", CL_Autotrack_f);
	Cmd_AddCommand ("trackkiller", CL_Trackkiller_f);

	// Multivew tracking.
	Cmd_AddCommand ("track1", CL_TrackMV1_f);	
	Cmd_AddCommand ("track2", CL_TrackMV2_f);	
	Cmd_AddCommand ("track3", CL_TrackMV3_f);	
	Cmd_AddCommand ("track4", CL_TrackMV4_f);
	Cmd_AddCommand ("trackteam", CL_TrackTeam_f);	
 
 #ifdef JSS_CAM
	Cmd_AddCommand ("cam_pos", Cam_Pos_f);
	Cmd_AddCommand ("cam_angles", Cam_Angles_f);
	Cmd_AddMacro (macro_cam_pos_x, Macro_Cam_Pos_X);
	Cmd_AddMacro (macro_cam_pos_y, Macro_Cam_Pos_Y);
	Cmd_AddMacro (macro_cam_pos_z, Macro_Cam_Pos_Z);
	Cmd_AddMacro (macro_cam_pos, Macro_Cam_Pos);
	Cmd_AddMacro (macro_cam_angles_pitch, Macro_Cam_Angles_Pitch);
	Cmd_AddMacro (macro_cam_angles_yaw, Macro_Cam_Angles_Yaw);
	Cmd_AddMacro (macro_cam_angles_roll, Macro_Cam_Angles_Roll);
	Cmd_AddMacro (macro_cam_angles, Macro_Cam_Angles);
 #endif
 
	// Multiview tracking.	
	CL_MultiviewInitialise ();
}

static int Cam_MainTrackNum(void)
{
	if (CL_MultiviewInsetEnabled()) {
		return CL_MultiviewMainView();
	}
	return Cam_TrackNum();
}

//
// Change what player we are tracking.
//
// trackview:
// - Should be < 0 if we're in normal mode.
// - Between 0-3 if we're in multiview.
//
void CL_Track (int trackview)
{
	int slot;
	char *arg;

	if (cls.state < ca_connected) 
	{
		Com_Printf("You must be connected to track\n", Cmd_Argv(0));
		return;
	}

	if (!cl.spectator) 
	{
		Com_Printf("You can only track in spectator mode\n", Cmd_Argv(0));
		return;
	}

	// Don't go outside of the mv_trackslots array bounds.
	trackview = min(trackview, 3);

	// Allow resetting to default tracking for multiview.
	if (trackview >= 0 && !strcmp(Cmd_Args(), "off")) 
	{
		Com_Printf("Track %d resetting to default\n", trackview);
		CL_MultiviewSetTrackSlot (trackview, -1);
		return;
	}

	if (Cmd_Argc() != 2) 
	{
		if (trackview < 0)
		{
			// Normal track.
			Com_Printf("Usage: %s <userid> | <name>\n", Cmd_Argv(0));
		}
		else
		{
			// Multiview track.
			Com_Printf("Usage: %s <userid> | <name> | <off>\n", Cmd_Argv(0));
		}
		return;
	}

	slot = Player_GetSlot(arg = Cmd_Argv(1), true);

	//
	// The specified player wasn't found.
	//
	if (slot == PLAYER_NAME_NOMATCH) 
	{
		Com_Printf("%s : no such player %s\n", Cmd_Argv(0), arg);
		return;
	} 
	else if (slot == PLAYER_ID_NOMATCH) 
	{
		Com_Printf("%s : no player with userid %d\n", Cmd_Argv(0), Q_atoi(arg));
		return;
	} 
	else if (slot < 0 || slot >= MAX_CLIENTS) 
	{	
		// PLAYER_NUM_MISMATCH covered by this
		Com_Printf("%s : no such player\n", Cmd_Argv(0));
		return;
	}

	// A player has been found that we want to track.
	if (cl.players[slot].spectator) 
	{
		Com_Printf("You cannot track a spectator\n", Cmd_Argv(0));
	} 
	else if (Cam_MainTrackNum() != slot || trackview >= 0)
	{
		// If we're not already tracking the found slot
		// set the camera to track mode and lock it to the selected slot.
		// (Locked as in "not free flying", not "cannot change tracked player")
		cl.autocam = CAM_TRACK;
		Cam_Lock(slot);
		cl.ideal_track = slot;
		
		// Multiview tracking:
		// Set the specified track view to track the specified player.
		CL_MultiviewSetTrackSlot(trackview, slot);

		cl.spec_locked = true;

		if (cls.mvdplayback && cl.teamfortress)
		{
			V_TF_ClearGrenadeEffects();
		}
	}
}

void CL_Track_f(void) 
{
	CL_Track(-1);	
}

void CL_Trackkiller_f(void)
{
	if (killer >= 0 && killer < MAX_CLIENTS) {
		char buf[16];
		snprintf(buf, sizeof(buf), "track %d\n", cl.players[killer].userid);
		Cbuf_AddText(buf);
	}
}

// auto-tracking is a feature implemented in three different places in QW
// - server side, client side (for a demo), recorded in a demo
// this command will choose which feature is available
// at the moment and will toggle it (on/off)
void CL_Autotrack_f(void)
{
	cmd_alias_t* at;
	extern cvar_t mvd_autotrack;
	extern cvar_t demo_autotrack;
	qbool mvda = mvd_autotrack.integer ? true : false;
	qbool demoa = demo_autotrack.integer ? true : false;

	if (cls.demoplayback) {
		if (cls.mvdplayback) {
			if (cl_hightrack.integer) {
				Cvar_SetValue(&cl_hightrack, 0);
			}

			if (!mvda && !demoa) {
				// we will turn on both features but if demo_autotrack info is found
				// it will turn off mvd_autotrack
				Cvar_SetValue(&mvd_autotrack, 4);
				Cvar_SetValue(&demo_autotrack, 1);
				Com_Printf("MVD Autotracking on\n");
			} else if (mvda && !demoa) {
				Com_Printf("MVD Autotracking off\n");
				Cvar_SetValue(&mvd_autotrack, 0);
			} else if (!mvda && demoa) {
				Com_Printf("Demo Autotracking off\n");
				Cvar_SetValue(&demo_autotrack, 0);
			} else { // mvda && demoa
				Com_Printf("Autotracking off\n");
				Cvar_SetValue(&mvd_autotrack, 0);
				Cvar_SetValue(&demo_autotrack, 0);
			}
		} else {
			Com_Printf("Only one point of view is recorded in this demo\n");
		}
	}
	else { // not playing a demo
		if (cl.spectator) {
			if ((at = Cmd_FindAlias("autotrack")) != NULL) {
				// not very "clean" way to execute an alias, but sufficient for this purpose
				Cbuf_AddText(va("%s\n", at->value)); // note KTX this is cmd 154, but we want to be compatible with other mods/versions

				/* Bugfix: When setting autotrack ON, make sure to set cl_hightrack 0.
				If player hits autotrack bind before KTX had a chance to stuff the impulse, then ezQuake would set cl_hightrack to 1.
				Then, if player hits autotrack again after KTX has finished stuffing, both autotrack and cl_hightrack would be on, creating chaos.
				
				HOWEVER, this creates a different, albeit less frustrating bug: if you have autotrack on first, then set cl_hightrack 1, then turn off autotrack, cl_hightrack gets set to 0.
				Currently there is no better way to solve this as autotrack is simply a command sent to the server.*/				
				if (cl_hightrack.integer) {
					Cvar_SetValue(&cl_hightrack, 0);
					Com_Printf("Hightrack off\n");
				}		

			}
			else {
				if (!cl_hightrack.integer) {
					Com_Printf("Autotrack not supported here, tracking top fragger (Hightrack on)\n");
					Cvar_SetValue(&cl_hightrack, 1);
				}
				else {
					Com_Printf("Hightrack off\n");
					Cvar_SetValue(&cl_hightrack, 0);
				}
			}
		}
	}
}

//
// Returns the player id of the currently tracked player (not free flying).
//
int Cam_TrackNum(void) 
{
	static int mvlatch;

	// If we're free-flying, temporarily turn cl_multiview off
	if (cl_multiview.value && !cl.spec_locked)
	{
		mvlatch = cl_multiview.value;
		cl_multiview.value = 0;
	}
	else if (!cl_multiview.value && mvlatch && cl.spec_locked)
	{
		cl_multiview.value = mvlatch;
		mvlatch = 0;
	}

	if (!cl.autocam) {
		return -1;
	}
	
	return cl.spec_track;
}

int WhoIsSpectated (void)
{
	if (cl.spectator && cl.autocam == CAM_TRACK && cl.players[cl.spec_track].name[0]) {
		return cl.spec_track;
	}

    return -1;
}
