/*
Copyright (C) 2001-2002 A Nourai
Copyright (C) 2015 ezQuake team

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

#include "quakedef.h"
#include "gl_model.h"
#include "rulesets.h"
#include "input.h"

extern cvar_t allow_triggers;

/* FIXME: Figure out a nicer way to do all this */

typedef struct rulesetDef_s {
	ruleset_t ruleset;
	float maxfps;
	qbool restrictTriggers;
	qbool restrictPacket;
	qbool restrictParticles;
	qbool restrictPlay;
	qbool restrictLogging;
	qbool restrictInlay;
	qbool restrictPogo;
	qbool restrictRollAngle;
	qbool restrictIPC;
	qbool restrictExec;
	qbool restrictSetCalc;
	qbool restrictSetEval;
	qbool restrictSetEx;
} rulesetDef_t;

static rulesetDef_t rulesetDef = {
	rs_modern2020, 	// ruleset
	77.0,			// maxfps
	false,			// restrict triggers
	true,			// restrict /packet command
	false,			// restrict particles
	true,			// restrict play
	true,			// restrict logging
	false,			// restrict inlay
	false,			// retrict pogo
	true,			// restrict rollangle
	true,			// restrict IPC
	true,			// restrict exec
	true,			// restrict setCalc
	true,			// restrict setEval
	true			// restrict setEx
};

qbool RuleSets_DisallowExternalTexture(struct model_s *mod)
{
	switch (mod->modhint) {
		case MOD_EYES:
			return true;
		case MOD_BACKPACK:
			return rulesetDef.ruleset == rs_smackdown || rulesetDef.ruleset == rs_qcon || rulesetDef.ruleset == rs_smackdrive;
		default:
			return false;
	}
}

qbool RuleSets_DisallowSimpleTexture(model_t* mod)
{
	switch (mod->modhint) {
		case MOD_EYES:
		case MOD_PLAYER:
		case MOD_SENTRYGUN: // tf
		case MOD_DETPACK:   // tf
			return true; // no replacement allowed

		case MOD_BACKPACK:
			// Now allowed in Thunderdome...
			return rulesetDef.ruleset == rs_smackdown || rulesetDef.ruleset == rs_qcon || rulesetDef.ruleset == rs_smackdrive;

		default:
			return false; // replacement always allowed
	}
}

// for models (gl_outline 1 and 3)
qbool RuleSets_DisallowModelOutline(struct model_s *mod)
{
	if (mod == NULL) {
		// World model - only allow in default ruleset, cheats enabled
		return !(r_refdef2.allow_cheats && rulesetDef.ruleset == rs_default);
	}

	switch (mod->modhint) {
		case MOD_EYES:
		case MOD_THUNDERBOLT:
			return true;
		case MOD_BACKPACK:
			return !cls.demoplayback && (rulesetDef.ruleset == rs_qcon || rulesetDef.ruleset == rs_smackdown || rulesetDef.ruleset == rs_smackdrive);
		default:
			// return to just rs_qcon once backface outlining tested
//			return !cls.demoplayback && (rulesetDef.ruleset == rs_qcon || rulesetDef.ruleset == rs_smackdown || rulesetDef.ruleset == rs_smackdrive);
			return !cls.demoplayback && (rulesetDef.ruleset == rs_qcon);
	}
}

// gl_outline_scale_model
// 0-1 for smackdown and qcon, 0-5 for others
float RuleSets_ModelOutlineScale(void) {
	extern cvar_t gl_outline_scale_model;
	switch(rulesetDef.ruleset) {
		case rs_smackdown:
		case rs_smackdrive:
		case rs_qcon:
			return bound(0.0f, gl_outline_scale_model.value, 1.0f);
		default:
			return bound(0.0f, gl_outline_scale_model.value, 1.0f);
	}
}

// for edges (gl_outline 2 and 3)
qbool RuleSets_AllowEdgeOutline(void)
{
	switch(rulesetDef.ruleset) {
		case rs_qcon:
			return false;
		default:
			return true;
	}
}

qbool Rulesets_AllowTimerefresh(void)
{
	switch(rulesetDef.ruleset) {
		case rs_smackdown:
		case rs_smackdrive:
		case rs_thunderdome:
		case rs_modern2020:
		case rs_qcon:
			return (cl.standby || cl.spectator || cls.demoplayback);
		default:
			return true;
	}
}

qbool Rulesets_AllowNoShadows(void)
{
	switch(rulesetDef.ruleset) {
		case rs_mtfl:
		case rs_smackdown:
		case rs_smackdrive:
		case rs_thunderdome:
		case rs_modern2020:
		case rs_qcon:
			return false;
		default:
			return true;
	}
}

qbool Rulesets_AllowAlternateModel (const char* modelName)
{
	switch(rulesetDef.ruleset) {
	case rs_qcon:
		if (! strcmp (modelName, "progs/player.mdl"))
			return false;
		return true;
	default:
		return true;
	}
}

float Rulesets_MaxFPS(void)
{
	return rulesetDef.maxfps;
}

qbool Rulesets_RestrictTriggers(void)
{
	return !allow_triggers.integer;
}

qbool Rulesets_RestrictPlay(const char* name)
{
	if (!rulesetDef.restrictPlay) {
		return false;
	}

	if (cls.state == ca_active && (cl.spectator || cls.demoplayback || cl.standby)) {
		return false;
	}

	if (name == NULL || cbuf_current != &cbuf_svc) {
		return true;
	}

	if (strstr(name, "ktsound")) {
		return true;
	}

	return false;
}

qbool Rulesets_RestrictPacket(void)
{
	return cls.state == ca_active && !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictPacket;
}

qbool Rulesets_RestrictParticles(void)
{
	return !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictParticles && !r_refdef2.allow_cheats;
}

qbool Rulesets_RestrictInlay(void)
{
	return rulesetDef.restrictInlay;
}

qbool Rulesets_RestrictPogo(void)
{
	return rulesetDef.restrictPogo;
}

qbool Rulesets_RestrictIPC(void)
{
	return cls.state == ca_active && !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictIPC;
}

qbool Rulesets_RestrictExec(void)
{
	return cls.state == ca_active && !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictExec && cls.server_adr.type != NA_LOOPBACK;
}

qbool Rulesets_RestrictSetCalc(void)
{
	return cls.state == ca_active && !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictSetCalc;
}

qbool Rulesets_RestrictSetEval(void)
{
	return cls.state == ca_active && !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictSetEval;
}

qbool Rulesets_RestrictSetEx(void)
{
	return cls.state == ca_active && !cl.spectator && !cls.demoplayback && !cl.standby && rulesetDef.restrictSetEx;
}

qbool Rulesets_RestrictTCL(void)
{
	switch(rulesetDef.ruleset) {
		case rs_smackdown:
		case rs_smackdrive:
		case rs_thunderdome:
		case rs_modern2020:
		case rs_qcon:
			return true;
		case rs_mtfl:
		default:
			return false;
	}
}

const char *Rulesets_Ruleset(void)
{
	switch(rulesetDef.ruleset) {
		case rs_mtfl:
			return "MTFL";
		case rs_smackdown:
			return "smackdown";
		case rs_thunderdome:
			return "thunderdome";
		case rs_qcon:
			return "qcon";
		case rs_modern2020:
			return "modern2020";
		case rs_smackdrive:
			return "smackdrive";
		default:
			return "default";
	}
}

void Rulesets_OnChange_indphys(cvar_t *var, char *value, qbool *cancel)
{
	if (cls.state != ca_disconnected) {
		Com_Printf("%s can be changed only when disconnected\n", var->name);
		*cancel = true;
	}
	else *cancel = false;
}

void Rulesets_OnChange_r_fullbrightSkins(cvar_t *var, char *value, qbool *cancel)
{
	char *fbs;
	qbool fbskins_policy = (cls.demoplayback || cl.spectator) ? 1 :
		*(fbs = Info_ValueForKey(cl.serverinfo, "fbskins")) ? bound(0, Q_atof(fbs), 1) :
		cl.teamfortress ? 0 : 1;
	float fbskins = bound(0.0, Q_atof (value), fbskins_policy);

	if (!cl.spectator && cls.state != ca_disconnected) {
		if (fbskins > 0.0) {
			Cbuf_AddText(va("say all skins %d%% fullbright\n", (int) (fbskins * 100.0)));
		} else {
			Cbuf_AddText(va("say not using fullbright skins\n"));
		}
	}
}

void Rulesets_OnChange_allow_scripts (cvar_t *var, char *value, qbool *cancel)
{
	char *p;
	qbool progress;
	int val;

	p = Info_ValueForKey(cl.serverinfo, "status");
	progress = (strstr (p, "left")) ? true : false;
	val = Q_atoi(value);

	if (cls.state >= ca_connected && progress && !cl.spectator) {
		Com_Printf ("%s changes are not allowed during the match.\n", var->name);
		*cancel = true;
		return;
	}

	IN_ClearProtectedKeys ();

	if (!cl.spectator && cls.state != ca_disconnected) {
		if (val < 1) {
			Cbuf_AddText("say not using scripts\n");
		} else if (val < 2) {
			Cbuf_AddText("say using simple scripts\n");
		} else {
			Cbuf_AddText("say using advanced scripts\n");
		}
	}
}

void Rulesets_OnChange_cl_delay_packet(cvar_t *var, char *value, qbool *cancel)
{
	int ival = Q_atoi(value);

	if (ival == var->integer) {
		// no change
		return;
	}

	if (var == &cl_delay_packet && (ival < 0 || ival > CL_MAX_PACKET_DELAY * 2)) {
		Com_Printf("%s must be between 0 and %d\n", var->name, CL_MAX_PACKET_DELAY * 2);
		*cancel = true;
		return;
	}

	if (var == &cl_delay_packet_dev && (ival < 0 || ival > CL_MAX_PACKET_DELAY_DEVIATION)) {
		Com_Printf("%s must be between 0 and %d\n", var->name, CL_MAX_PACKET_DELAY_DEVIATION);
		*cancel = true;
		return;
	}

	if (var == &cl_delay_packet_target && (ival < 0 || ival > CL_MAX_PACKET_DELAY_TARGET)) {
		Com_Printf("%s must be between 0 and %d\n", var->name, CL_MAX_PACKET_DELAY_TARGET);
		*cancel = true;
		return;
	}

	if (cls.state == ca_active) {
		if ((cl.standby) || (cl.teamfortress)) {
			char announce[128];
			int delay_target_ms = (var == &cl_delay_packet_target ? ival : cl_delay_packet_target.integer);
			int delay_deviation = (var == &cl_delay_packet_dev ? ival : cl_delay_packet_dev.integer);
			int delay_constant = (var == &cl_delay_packet ? ival : cl_delay_packet.integer);

			if (delay_target_ms) {
				snprintf(announce, sizeof(announce), "say delay packet: target ping %d ms (%dms dev)\n", delay_target_ms, delay_deviation);
			}
			else if (delay_constant) {
				snprintf(announce, sizeof(announce), "say delay packet: adding %d ms (%dms dev)\n", delay_constant, delay_deviation);
			}
			else {
				snprintf(announce, sizeof(announce), "say delay packet: off\n");
			}

			// allow in standby or teamfortress. For teamfortress, more often than not
			// People 1on1 without "match mode" and they may want to sync pings.
			Cbuf_AddText(announce);
		}
		else {
			// disallow during the match
			Com_Printf("%s changes are not allowed during the match\n", var->name);
			*cancel = true;
		}
	}
	else {
		// allow in not fully connected state
	}
}

void Rulesets_OnChange_cl_iDrive(cvar_t *var, char *value, qbool *cancel)
{
	int ival = Q_atoi(value);	// this is used in the code
	float fval = Q_atof(value); // this is used to check value validity

	if (ival == var->integer && fval == var->value) {
		// no change
		return;
	}

	if (fval != 0 && fval != 1 && fval != 2 && fval != 3) {
		Com_Printf("Invalid value for %s, use 0 or 1 or 2 or 3.\n", var->name);
		*cancel = true;
		return;
	}

	if (cls.state == ca_active) {
		if (cl.standby) {
			// allow in standby
			Cbuf_AddText(va("say side step aid (strafescript): %s\n", ival ? "on" : "off"));
		}
		else {
			// disallow during the match
			Com_Printf("%s changes are not allowed during the match\n", var->name);
			*cancel = true;
		}
	} else {
		// allow in not fully connected state
	}
}

void Rulesets_OnChange_cl_autohop(cvar_t *var, char *value, qbool *cancel)
{
	int ival = Q_atoi(value);	// this is used in the code
	float fval = Q_atof(value); // this is used to check value validity

	if (ival == var->integer && fval == var->value) {
		// no change
		return;
	}

	if (fval != 0 && fval != 1) {
		Com_Printf("Invalid value for %s, use 0 or 1.\n", var->name);
		*cancel = true;
		return;
	}

	if (cls.state == ca_active) {
		if (cl.standby) {
			// allow in standby
			Cbuf_AddText(va("say cl_autohop: %s\n", ival ? "on" : "off"));
		}
		else {
			// disallow during the match
			Com_Printf("%s changes are not allowed during the match\n", var->name);
			*cancel = true;
		}
	} else {
		// allow in not fully connected state
	}
}

void Rulesets_OnChange_cl_hud(cvar_t *var, char *value, qbool *cancel)
{
	int ival = Q_atoi(value);	// this is used in the code
	float fval = Q_atof(value); // this is used to check value validity

	if (ival == var->integer && fval == var->value) {
		// no change
		return;
	}

	if (fval != 0 && fval != 1) {
		Com_Printf("Invalid value for %s, use 0 or 1.\n", var->name);
		*cancel = true;
		return;
	}

	if (cls.state == ca_active) {
		if (cl.standby) {
			// allow in standby
			Cbuf_AddText(va("say qw262 hud: %s\n", ival ? "enabled" : "disabled"));
		}
		else {
			// disallow during the match
			Com_Printf("%s changes are not allowed during the match\n", var->name);
			*cancel = true;
		}
	} else {
		// allow in not fully connected state
	}
}

void Rulesets_OnChange_inlay(cvar_t *var, char *value, qbool *cancel)
{
	int ival = Q_atoi(value);	// this is used in the code
	float fval = Q_atof(value); // this is used to check value validity

	if (ival == var->integer && fval == var->value) {
		// no change
		return;
	}

	if (fval != 0 && fval != 1) {
		Com_Printf("Invalid value for %s, use 0 or 1.\n", var->name);
		*cancel = true;
		return;
	}

	if (cls.state == ca_active) {
		if (cl.standby) {
			// allow in standby
			Cbuf_AddText(va("say teaminlay: %s\n", ival ? "enabled" : "disabled"));
		}
		else {
			// disallow during the match
			Com_Printf("%s changes are not allowed during the match\n", var->name);
			*cancel = true;
		}
	} else {
		// allow in not fully connected state
	}
}

void Rulesets_OnChange_cl_fakeshaft(cvar_t *var, char *value, qbool *cancel)
{
	float fakeshaft = Q_atof(value);

	if (!cl.spectator && cls.state != ca_disconnected) {
		if (fakeshaft == 2)
			Cbuf_AddText("say fakeshaft 2 (emulation of fakeshaft 0 for servers with antilag feature)\n");
		else if (fakeshaft > 0.999)
			Cbuf_AddText("say fakeshaft on\n");
		else if (fakeshaft < 0.001)
			Cbuf_AddText("say fakeshaft off\n");
		else
			Cbuf_AddText(va("say fakeshaft %.1f%%\n", fakeshaft * 100.0));
	}
}

void Rulesets_OnChange_cl_rollalpha(cvar_t *var, char *value, qbool *cancel)
{
	float fval = Q_atof(value); // this is used to check value validity

	if (!cl.spectator && cls.state != ca_disconnected) {
		if (fval == 20)
			Cbuf_AddText(va("say rollalpha: %s\n", "disabled"));
		else if (fval == 0)
			Cbuf_AddText(va("say rollalpha: %s\n", "enabled"));
		else
			Cbuf_AddText(va("say rollalpha: %s\n", value));
	}
}

void Rulesets_OnChange_allow_triggers(cvar_t *var, char *value, qbool *cancel)
{
	int ival = Q_atoi(value);	// this is used in the code
	float fval = Q_atof(value); // this is used to check value validity

	if (ival == var->integer && fval == var->value) {
		// no change
		return;
	}

	if (fval != 0 && fval != 1) {
		Com_Printf("Invalid value for %s, use 0 or 1.\n", var->name);
		*cancel = true;
		return;
	}

	if (!cl.spectator && cls.state != ca_disconnected) {
		if (cls.state == ca_active) {
			if (cl.standby) {
				if (ival)
					Cbuf_AddText("say triggers enabled\n");
				else
					Cbuf_AddText("say triggers disabled\n");
			}
			else {
				// disallow during the match
				Com_Printf("%s changes are not allowed during the match\n", var->name);
				*cancel = true;
			}
		}
	}
}

int Rulesets_MaxSequentialWaitCommands(void)
{
	switch (rulesetDef.ruleset) {
	case rs_qcon:
		return 10;
	default:
		return 32768;
	}
}

qbool Ruleset_BlockHudPicChange(void)
{
	switch (rulesetDef.ruleset) {
	case rs_qcon:
	case rs_smackdown:
	case rs_smackdrive:
	case rs_thunderdome:
	case rs_modern2020:
		return cls.state != ca_disconnected && !(cl.standby || cl.spectator || cls.demoplayback);
	default:
		return false;
	}
}

qbool Ruleset_AllowPolygonOffset(entity_t* ent)
{
	switch (rulesetDef.ruleset) {
	case rs_qcon:
		return false;
	case rs_default:
		return true;
	default:
		return ent->model && ent->model->isworldmodel;
	}
}

// Not technically ruleset-based but limits functionaly for similar reasons...
qbool Ruleset_IsLumaAllowed(struct model_s *mod)
{
	switch (mod->modhint)
	{
	case MOD_EYES:
	case MOD_BACKPACK:
	case MOD_PLAYER:

	case MOD_SENTRYGUN: // tf
	case MOD_DETPACK:   // tf

		return false; // no luma for such models

	default:

		return true; // luma allowed
	}
}

qbool Rulesets_ToggleWhenFlashed(void)
{
	return rulesetDef.ruleset == rs_mtfl;
}

qbool Rulesets_FullbrightModel(struct model_s* model)
{
	extern cvar_t gl_fb_models;
	qbool protected_model = (model->modhint == MOD_EYES || model->modhint == MOD_BACKPACK) && rulesetDef.ruleset != rs_default;
	qbool fb_requested = gl_fb_models.integer == 1 && model->modhint != MOD_GIB && model->modhint != MOD_VMODEL && !Ruleset_IsLocalSinglePlayerGame();

	return !protected_model && fb_requested;
}

const char* Ruleset_BlockPlayerCountMacros(void)
{
	if (rulesetDef.ruleset == rs_mtfl) {
		return BANNED_BY_MTFL;
	}
	return NULL;
}

qbool Ruleset_AllowPowerupShell(model_t* model)
{
	// always allow powerupshells for specs or demos.
	// do not allow powerupshells for eyes in other cases
	extern cvar_t gl_powerupshells;

	return (bound(0, gl_powerupshells.value, 1) && ((cls.demoplayback || cl.spectator) || model->modhint != MOD_EYES));
}

qbool Ruleset_CanLogConsole(void)
{
	return cls.demoplayback || cls.state != ca_active || cl.standby || cl.countdown || !rulesetDef.restrictLogging;
}

qbool Ruleset_AllowNoHardwareGamma(void)
{
	return rulesetDef.ruleset != rs_mtfl;
}

float Ruleset_RollAngle(void)
{
	extern cvar_t cl_rollangle;

	if (cls.demoplayback || cl.spectator || !rulesetDef.restrictRollAngle) {
		return fabs(cl_rollangle.value);
	}

	return bound(0.0f, cl_rollangle.value, 5.0f);
}

#ifndef CLIENTONLY
extern cvar_t     maxclients;
qbool Ruleset_IsLocalSinglePlayerGame(void)
{
	return com_serveractive && cls.state == ca_active && !cl.deathmatch && maxclients.integer == 1;
}
#endif
