/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2007-2023 ezQuake team
Copyright (C) 2021-2023 Sam "Reki" Piper
Copyright (C) 2026 unezQuake team

Native parser/renderer for the small EZCSQC protocol used by KTX weapon and
projectile prediction. Authoritative game state remains server-side.

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
#include "ezcsqc.h"
#include "pmove.h"
#include "qsound.h"
#include "gl_model.h"
#include "vx_stuff.h"
#include "cl_tent.h"

#ifdef FTE_PEXT_CSQC

extern cvar_t cl_rocket2grenade;
extern cvar_t gl_no24bit;

#define MAX_PREDWEPS 16
#define WEPANIM(wep, frame) (&(wep)->anim_states[(frame)])
#define LENGTH2S(length) ((float)(length) / 1000.0f)
#define EF_NAILTRAIL 0x40000000
#define EZCSQC_LOCAL_PROJECTILE_BASE MAX_EDICTS

/*
 * Weapon prediction is stored in three rings:
 * - ws_server: last server-authored weapon state for each parsed network frame
 * - ws_saved:  local prediction snapshots for later comparison/replay
 * - ws_predicted: scratch state rebuilt each render frame from ws_server + usercmds
 *
 * The client only uses this to choose local viewmodel frames/sounds. KTX remains
 * authoritative for hits, ammo, projectiles, damage, and final entity state.
 */
ezcsqc_weapon_state_t ws_server[UPDATE_BACKUP];
ezcsqc_weapon_state_t ws_predicted;
ezcsqc_weapon_state_t ws_saved[UPDATE_BACKUP];

/*
 * EZCSQC entities are not normal packet entities. mvdsv sends a compact stream
 * of entity numbers and type-specific payloads through svc_fte_csqcentities.
 * ezcsqc_networkedents maps server entity numbers to reusable local slots.
 */
ezcsqc_entity_t ezcsqc_entities[MAX_EDICTS];
ezcsqc_entity_t *ezcsqc_networkedents[MAX_EDICTS];
ezcsqc_world_t ezcsqc;
weppredsound_t *predictionsoundlist;

/* Weapon definitions are server-authored tables keyed by KTX weapon_index. */
static weppreddef_t wpredict_definitions[MAX_PREDWEPS];
static ezcsqc_entity_t *viewweapon;
static int projectile_ringbufferseek;
static int is_effectframe;
static int last_effectframe;
static int current_predframe;
static float lg_twidth;

static qbool Predraw_Projectile(ezcsqc_entity_t *self);
static void CL_EZCSQC_ProjectileBounce(ezcsqc_entity_t *proj, float dt, qbool update_angles);

static ezcsqc_entity_t *CL_EZCSQC_Ent_Spawn(void)
{
	int i;
	double time_safety = Sys_DoubleTime() - 0.5;

	/* Avoid immediately reusing a slot that may still be referenced by render code. */
	for (i = 0; i < MAX_EDICTS; i++) {
		ezcsqc_entity_t *ent = &ezcsqc_entities[i];
		if (!ent->isfree || ent->freetime > time_safety) {
			continue;
		}
		memset(ent, 0, sizeof(*ent));
		return ent;
	}

	// Fall back to slot zero only under extreme pressure; callers overwrite it immediately.
	return &ezcsqc_entities[0];
}

static void CL_EZCSQC_Ent_Remove(ezcsqc_entity_t *ent)
{
	if (!ent || ent->isfree) {
		return;
	}

	if (cl_ezcsqc_debug.integer > 1) {
		Com_Printf("EZCSQC remove ent=%d drawmask=%x viewweapon=%d weapon_prediction=%d\n",
			ent->entnum, ent->drawmask, ent == viewweapon, ezcsqc.weapon_prediction);
	}

	// Removing the bound viewweapon disables native weapon prediction until KTX sends a new one.
	if (ent == viewweapon) {
		viewweapon = NULL;
		ezcsqc.weapon_prediction = false;
	}

	// Networked entities must be unhooked so a reused server entnum allocates cleanly.
	if (!ent->local_projectile && ent->entnum > 0 && ent->entnum < MAX_EDICTS && ezcsqc_networkedents[ent->entnum] == ent) {
		ezcsqc_networkedents[ent->entnum] = NULL;
	}

	// Detach packet-style trail follow state for server projectiles that used cl_entities[].
	if ((ent->drawmask & DRAWMASK_PROJECTILE) && !ent->local_projectile && ent->entnum > 0 && ent->entnum < CL_MAX_EDICTS) {
		centity_t *cent = &cl_entities[ent->entnum];
		int t;

		cent->current.modelindex = 0;
		cent->sequence = 0;
		for (t = 0; t < sizeof(cent->trails) / sizeof(cent->trails[0]); t++) {
			cent->trails[t].lasttime = 0;
		}
	}

	ent->drawmask = 0;
	ent->predraw = NULL;
	ent->modelindex = 0;
	ent->isfree = true;
	ent->freetime = Sys_DoubleTime();
}

void CL_EZCSQC_InitializeEntities(void)
{
	int i;
	weppredsound_t *snd;

	// Clear all server protocol state and local prediction snapshots on reconnect/map load.
	memset(&ezcsqc, 0, sizeof(ezcsqc));
	memset(ws_server, 0, sizeof(ws_server));
	memset(&ws_predicted, 0, sizeof(ws_predicted));
	memset(ws_saved, 0, sizeof(ws_saved));
	memset(wpredict_definitions, 0, sizeof(wpredict_definitions));

	viewweapon = NULL;
	last_effectframe = 0;
	current_predframe = 0;
	lg_twidth = 0;
	projectile_ringbufferseek = 0;

	// Mark every EZCSQC slot free; server entity mappings are rebuilt from CSQC updates.
	for (i = 0; i < MAX_EDICTS; i++) {
		memset(&ezcsqc_entities[i], 0, sizeof(ezcsqc_entities[i]));
		ezcsqc_entities[i].isfree = true;
		ezcsqc_networkedents[i] = NULL;
	}

	// Sound definitions are rebuilt from weapondef payloads, so drop the old echo list.
	snd = predictionsoundlist;
	while (snd) {
		weppredsound_t *next = snd->next;
		free(snd);
		snd = next;
	}
	predictionsoundlist = NULL;
}

qbool CL_EZCSQC_Active(void)
{
	return cl_pext_ezcsqc.integer && (cls.fteprotocolextensions & FTE_PEXT_CSQC) && ezcsqc.active;
}

static void CL_EZCSQC_InitProjectile(ezcsqc_entity_t *ent, int modelindex, int ownernum)
{
	float latency = cls.latency;
	model_t *model = cl.model_precache[modelindex];

	/*
	 * Local predicted projectiles are short-lived visual placeholders. They
	 * start immediately when weapon prediction fires; cl_predict_buffer already
	 * controls the intentional effect delay. Keep them alive roughly until KTX's
	 * authoritative CSQC projectile arrives and can take over the trail.
	 */
	ent->drawmask = DRAWMASK_PROJECTILE;
	ent->predraw = Predraw_Projectile;
	ent->modelindex = modelindex;
	ent->ownernum = ownernum;
	ent->modelflags = model->flags;
	ent->effects = model->flags;
	ent->starttime = cl.time;
	ent->simtime = cl.time;
	ent->endtime = cl.time + max(latency + 0.013f, 0.05f);
	ent->parttime = cl.time;
	ent->projectile_type = 0;
	ent->partcount = 0;

	if ((model->flags & EF_GRENADE) || model->modhint == MOD_GRENADE) {
		ent->projectile_type = 1;
		ent->endtime = cl.time + max(latency, 0.05f);
		ent->effects = EF_GRENADE;
	}
	else if (model->flags & EF_ROCKET) {
		ent->effects = EF_ROCKET;
	}
	else if (model->modhint == MOD_SPIKE) {
		ent->effects = 256;
		ent->modelflags |= EF_NAILTRAIL;
	}
	else {
		ent->effects = model->flags & (EF_ROCKET | EF_GRENADE | EF_TRACER | EF_TRACER2 | EF_TRACER3);
	}
}

static int CL_EZCSQC_ProjectileVisualEffects(ezcsqc_entity_t *ent)
{
	int effects;
	model_t *model;

	// Missing models cannot have reliable model flags or trail hints.
	if (!ent->modelindex || ent->modelindex >= MAX_MODELS || !cl.model_precache[ent->modelindex]) {
		return 0;
	}

	model = cl.model_precache[ent->modelindex];

	/*
	 * KTX may send zero effects when the model itself carries the visual
	 * behavior. Packet entities use model flags for trails/lights too, so merge
	 * the two sources here before deciding how to render the projectile.
	 */
	effects = ent->effects | model->flags;
	if (model->modhint == MOD_GRENADE) {
		effects |= EF_GRENADE;
	}
	if (model->modhint == MOD_SPIKE) {
		effects |= 256;
	}

	return effects;
}

static qbool CL_EZCSQC_ProjectilePointSolid(vec3_t point)
{
	return PM_PointContents_AllBSPs(point) == CONTENTS_SOLID;
}

static qbool CL_EZCSQC_ProjectileTraceBlocked(vec3_t start, vec3_t end)
{
	trace_t trace;
	vec3_t point;
	int i;

	// Point-blank shots can begin or end inside BSP solid before a line trace reports a hit.
	if (CL_EZCSQC_ProjectilePointSolid(start) || CL_EZCSQC_ProjectilePointSolid(end)) {
		return true;
	}

	// The normal trace catches world intersections along the simulated segment.
	trace = PM_TraceLine(start, end);
	if (trace.fraction < 1 || trace.startsolid || trace.allsolid) {
		return true;
	}

	// Sample interior points too, because short traces can miss some all-solid edge cases.
	for (i = 1; i < 4; i++) {
		VectorInterpolate(start, i * 0.25f, end, point);
		if (CL_EZCSQC_ProjectilePointSolid(point)) {
			return true;
		}
	}

	return false;
}

static qbool CL_EZCSQC_PredictRocketSpawnTouch(int modelindex, vec3_t origin, vec3_t velocity, double prediction_time)
{
	model_t *model;
	vec3_t end, lookahead_end;
	trace_t trace;
	float speed;

	// Only rockets get local close-start explosion prediction.
	if (!modelindex || modelindex >= MAX_MODELS) {
		return false;
	}

	model = cl.model_precache[modelindex];
	if (!model || !(model->flags & EF_ROCKET)) {
		return false;
	}

	// Degenerate projectile definitions should never synthesize explosions.
	speed = VectorLength(velocity);
	if (speed <= 1) {
		return false;
	}

	/* Server-authoritative classic newmis close-hit sweep. */
	VectorMA(origin, 0.05f, velocity, end);
	trace = PM_TraceLine(origin, end);
	if (trace.fraction < 1 || trace.startsolid || trace.allsolid) {
		if (trace.e.entnum == 0) {
			vec3_t predicted_origin, back;

			// KTX backs the explosion temp entity off the true impact point by 8 units.
			VectorScale(velocity, -8.0f / speed, back);
			VectorAdd(trace.endpos, back, predicted_origin);
			CL_PredictRocketExplosion(predicted_origin, trace.endpos, prediction_time);
		}
		if (cl_ezcsqc_debug.integer > 2) {
			Com_Printf("EZCSQC predicted rocket spawn-touch model=%d org=(%.1f %.1f %.1f) hit=(%.1f %.1f %.1f)\n",
				modelindex, origin[0], origin[1], origin[2],
				trace.endpos[0], trace.endpos[1], trace.endpos[2]);
		}
		return true;
	}

	/*
	 * MVDSV simple-projectile render lookahead is applied after the server-side
	 * newmis step has survived. Mirror that as a separate visual-only probe:
	 * hide the model, but wait for the server explosion.
	 */
	VectorMA(end, 0.02f, velocity, lookahead_end);
	trace = PM_TraceLine(end, lookahead_end);
	if (trace.fraction < 1 || trace.startsolid || trace.allsolid) {
		if (cl_ezcsqc_debug.integer > 2) {
			Com_Printf("EZCSQC predicted rocket spawn-lookahead model=%d org=(%.1f %.1f %.1f) hit=(%.1f %.1f %.1f)\n",
				modelindex, origin[0], origin[1], origin[2],
				trace.endpos[0], trace.endpos[1], trace.endpos[2]);
		}
		return true;
	}

	return false;
}

static qbool CL_EZCSQC_OwnerProjectile(ezcsqc_entity_t *self)
{
	return self->local_projectile || self->ownernum == cl.playernum + 1;
}

static qbool CL_EZCSQC_ProjectileOriginTooCloseToDraw(ezcsqc_entity_t *self, vec3_t origin)
{
	vec3_t diff;

	if (!CL_EZCSQC_OwnerProjectile(self)) {
		return false;
	}

	VectorSubtract(origin, r_refdef.vieworg, diff);

	// Avoid drawing owner projectiles that are effectively inside the camera.
	if (DotProduct(diff, diff) < 8.0f * 8.0f) {
		return true;
	}

	return false;
}

static qbool CL_EZCSQC_ProjectileTooCloseToDraw(ezcsqc_entity_t *self)
{
	return CL_EZCSQC_ProjectileOriginTooCloseToDraw(self, self->origin);
}

static void CL_EZCSQC_ProjectileDebugDistances(ezcsqc_entity_t *self, vec3_t origin, float *travel, float *camdist)
{
	vec3_t diff;

	VectorSubtract(origin, self->start, diff);
	*travel = VectorLength(diff);
	VectorSubtract(origin, r_refdef.vieworg, diff);
	*camdist = VectorLength(diff);
}

static void CL_EZCSQC_DebugLocalProjectile(ezcsqc_entity_t *self, const char *event, vec3_t origin)
{
	float travel, camdist;

	if (cl_ezcsqc_debug.integer <= 2 || !self->local_projectile) {
		return;
	}

	CL_EZCSQC_ProjectileDebugDistances(self, origin, &travel, &camdist);
	Com_Printf("EZCSQC local projectile %s ent=%d model=%d type=%d age=%.3f end=%.3f draw=%d parts=%d travel=%.1f cam=%.1f org=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f)\n",
		event, self->entnum, self->modelindex, self->projectile_type,
		cl.time - self->starttime, self->endtime - cl.time, self->drawcount,
		self->partcount, travel, camdist, origin[0], origin[1], origin[2],
		self->vel[0], self->vel[1], self->vel[2]);
}

static void CL_EZCSQC_SeedProjectileTrail(ezcsqc_entity_t *self, vec3_t origin)
{
	centity_t *cent = &self->cent;
	int t;

	if (!self->local_projectile && self->entnum > 0 && self->entnum < CL_MAX_EDICTS) {
		cent = &cl_entities[self->entnum];
	}

	// Reset every trail stop to the same point so the next particle segment starts cleanly.
	self->trail_seeded = true;
	VectorCopy(origin, self->partorg);
	VectorCopy(origin, cent->old_origin);
	VectorCopy(origin, cent->lerp_origin);
	VectorCopy(origin, cent->current.origin);
	cent->sequence = cl.validsequence;

	for (t = 0; t < sizeof(cent->trails) / sizeof(cent->trails[0]); t++) {
		cent->trails[t].lasttime = 0;
		VectorCopy(origin, cent->trails[t].stop);
	}
}

static centity_t *CL_EZCSQC_ProjectileCentity(ezcsqc_entity_t *self, entity_state_t *state)
{
	centity_t *cent = &self->cent;

	/*
	 * Server-authored projectiles with real entity numbers should use the same
	 * cl_entities[] storage as packet entities. QMB dynamic nail trails store a
	 * cl_entities[] reference and follow cent->lerp_origin between trail spawns;
	 * embedded centity_t storage cannot support that follow mode.
	 */
	if (!self->local_projectile && self->entnum > 0 && self->entnum < CL_MAX_EDICTS) {
		int t;

		cent = &cl_entities[self->entnum];
		// Large discontinuities need a new trail number so old QMB followers detach.
		if (cent->current.modelindex != state->modelindex ||
			!VectorL2Compare(cent->current.origin, state->origin, 200)) {
			/*
			 * Match packet entity behavior: only reset trail history when the
			 * model changes or the origin jumps far enough that continuing the
			 * old trail would draw a bogus long segment.
			 */
			cent->trail_number = (cent->trail_number + 1) % 1000;
			for (t = 0; t < sizeof(cent->trails) / sizeof(cent->trails[0]); t++) {
				cent->trails[t].lasttime = 0;
				VectorCopy(self->partorg, cent->trails[t].stop);
			}
			VectorCopy(self->partorg, cent->old_origin);
			VectorCopy(state->origin, cent->lerp_origin);
		}
	}

	// Keep the centity current for render interpolation and QMB dynamic trail follow.
	cent->current = *state;
	cent->sequence = cl.validsequence;
	VectorCopy(state->origin, cent->current.origin);
	VectorCopy(state->origin, cent->lerp_origin);

	return cent;
}

static ezcsqc_entity_t *CL_EZCSQC_AllocLocalProjectile(void)
{
	int i;
	double time_safety = Sys_DoubleTime() - 0.5;

	/*
	 * Allocate predicted projectiles from the high end so they do not collide
	 * with low-numbered server CSQC entities. The entnum is deliberately outside
	 * the packet-entity range so local placeholders never look like authoritative
	 * cl_entities[] entries.
	 */
	for (i = MAX_EDICTS - 1; i >= 0; i--) {
		ezcsqc_entity_t *ent = &ezcsqc_entities[i];
		if (!ent->isfree || ent->freetime > time_safety) {
			continue;
		}
		// Local placeholders live in the shared pool but outside the networked entnum range.
		memset(ent, 0, sizeof(*ent));
		ent->entnum = EZCSQC_LOCAL_PROJECTILE_BASE + i;
		ent->local_projectile = true;
		return ent;
	}

	return NULL;
}

static void CL_EZCSQC_RemoveMatchedLocalProjectile(ezcsqc_entity_t *server)
{
	int i;
	vec3_t server_origin;

	/*
	 * When the authoritative KTX projectile arrives, remove the matching local
	 * placeholder and transfer its trail start point. Compare current projected
	 * positions, not the last rendered local origin, because parsing can happen
	 * before the local projectile is drawn for this frame.
	 */
	if (CL_EZCSQC_ProjectileVisualEffects(server) & EF_GRENADE) {
		ezcsqc_entity_t projected = *server;
		float dt;

		// Project grenades with the bounce path so matching compares visible endpoints.
		VectorCopy(projected.sim_origin, projected.origin);
		dt = max(cl.servertime - projected.simtime, 0);
		if (dt > 0) {
			CL_EZCSQC_ProjectileBounce(&projected, dt, false);
		}
		VectorCopy(projected.origin, server_origin);
	}
	else {
		// Non-bouncing projectiles can be compared with simple linear extrapolation.
		VectorMA(server->s_origin, cl.servertime - server->s_time, server->s_velocity, server_origin);
	}

	for (i = 0; i < MAX_EDICTS; i++) {
		ezcsqc_entity_t *local = &ezcsqc_entities[i];
		centity_t *cent;
		vec3_t local_origin;
		vec3_t diff;
		vec3_t diff_parallel, diff_perp, local_dir;
		float local_speed, server_speed;
		float parallel = 0;
		float perpendicular;

		if (local->isfree || !local->local_projectile || local->ownernum != server->ownernum) {
			continue;
		}
		if (local->modelindex && server->modelindex && local->modelindex != server->modelindex) {
			continue;
		}
		if (cl.time > local->endtime + 0.1f) {
			continue;
		}

		// Reconstruct where the local placeholder should be at the current render time.
		if (local->projectile_type == 1) {
			/* Grenades are simulated incrementally because they can bounce. */
			VectorCopy(local->origin, local_origin);
		}
		else {
			vec3_t traveled;
			VectorCopy(local->start, local_origin);
			VectorScale(local->vel, cl.time - local->starttime, traveled);
			VectorAdd(local_origin, traveled, local_origin);
		}

		// Split the mismatch into along-trajectory and sideways error for safer matching.
		VectorSubtract(server_origin, local_origin, diff);
		VectorClear(diff_parallel);
		local_speed = VectorLength(local->vel);
		server_speed = VectorLength(server->s_velocity);
		if (local_speed > 1) {
			VectorScale(local->vel, 1.0f / local_speed, local_dir);
			parallel = DotProduct(diff, local_dir);
			VectorScale(local_dir, parallel, diff_parallel);
		}
		VectorSubtract(diff, diff_parallel, diff_perp);
		perpendicular = VectorLength(diff_perp);

		// Accept larger owner rocket/nail phase gaps only when they are nearly collinear.
		if (VectorLength(diff) > 64) {
			if (server->ownernum != cl.playernum + 1 || local->projectile_type == 1 || perpendicular >= 8 || fabs(parallel) > 128) {
				continue;
			}
		}

		if (cl_ezcsqc_debug.integer > 2) {
			float local_travel, local_camdist;

			CL_EZCSQC_ProjectileDebugDistances(local, local_origin, &local_travel, &local_camdist);
			Com_Printf("EZCSQC local projectile handoff local=%d server=%d model=%d age=%.3f draw=%d parts=%d seeded=%d started=%d diff=%.1f parallel=%.1f perp=%.1f speed=%.1f server_speed=%.1f travel=%.1f cam=%.1f local=(%.1f %.1f %.1f) server=(%.1f %.1f %.1f)\n",
				local->entnum, server->entnum, local->modelindex,
				cl.time - local->starttime, local->drawcount, local->partcount,
				local->trail_seeded, local->trail_started, VectorLength(diff),
				parallel, perpendicular, local_speed, server_speed,
				local_travel, local_camdist,
				local_origin[0], local_origin[1], local_origin[2],
				server_origin[0], server_origin[1], server_origin[2]);
		}

		if (server->ownernum == cl.playernum + 1 && perpendicular < 8) {
			vec3_t smooth_origin;

			// Owner handoff starts from the last visible local model when available.
			if (local->has_last_draw_origin) {
				VectorCopy(local->last_draw_origin, smooth_origin);
			}
			else {
				VectorCopy(local_origin, smooth_origin);
			}

			// Smooth out only the displayed phase gap; authority stays with the server entity.
			server->handoff_smoothing = true;
			server->handoff_starttime = cl.time;
			server->handoff_endtime = cl.time + (local->has_last_draw_origin
				? bound(0.150, fabs(parallel) / max(local_speed, 1) * 6.0f, 0.300)
				: bound(0.026, fabs(parallel) / max(local_speed, 1) * 0.5f, 0.050));
			VectorCopy(smooth_origin, server->handoff_origin);
			VectorSubtract(server_origin, smooth_origin, server->handoff_delta);
			VectorCopy(smooth_origin, server->oldorigin);
			if (cl_ezcsqc_debug.integer > 2) {
				Com_Printf("EZCSQC local projectile handoff-smooth server=%d start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) duration=%.3f\n",
					server->entnum,
					smooth_origin[0], smooth_origin[1], smooth_origin[2],
					server_origin[0], server_origin[1], server_origin[2],
					server->handoff_endtime - server->handoff_starttime);
			}
		}
		if (local->projectile_type == 1) {
			// Grenade spin is cosmetic, so preserve the local visual phase at handoff.
			VectorCopy(local->angles, server->angles);
			VectorCopy(local->angles, server->sim_angles);
			server->angle_simtime = cl.time;
		}

		if (local->partcount || local->trail_seeded) {
			cent = (server->entnum > 0 && server->entnum < CL_MAX_EDICTS) ? &cl_entities[server->entnum] : &server->cent;
			/*
			 * Seed the authoritative trail from the last local visual trail
			 * point. Close-range placeholders may seed this before any trail
			 * particles are emitted, so the server trail does not restart at
			 * the muzzle when prediction ends.
			 */
			server->trail_seeded = true;
			server->trail_started = local->trail_started;
			VectorCopy(local->partorg, server->trail_origin);
			VectorCopy(local->partorg, server->partorg);
			VectorCopy(local->partorg, cent->old_origin);
			VectorCopy(local->partorg, cent->trails[0].stop);
			VectorCopy(local->partorg, cent->trails[1].stop);
			VectorCopy(local->partorg, cent->trails[2].stop);
			VectorCopy(local->partorg, cent->trails[3].stop);
		}
		CL_EZCSQC_Ent_Remove(local);
		return;
	}
}

static void CL_EZCSQC_ProjectileClipVelocity(vec3_t vel, vec3_t norm, float f)
{
	vec3_t vel2;

	// Remove the component into the collision plane and scale it for Quake-style bounce.
	VectorScale(norm, DotProduct(vel, norm), vel2);
	VectorScale(vel2, f, vel2);
	VectorSubtract(vel, vel2, vel);

	// Snap tiny residual velocity to zero so grenades can come to rest.
	if (vel[0] > -0.1f && vel[0] < 0.1f) {
		vel[0] = 0;
	}
	if (vel[1] > -0.1f && vel[1] < 0.1f) {
		vel[1] = 0;
	}
	if (vel[2] > -0.1f && vel[2] < 0.1f) {
		vel[2] = 0;
	}
}

static void CL_EZCSQC_ProjectileAdvanceAngles(ezcsqc_entity_t *proj, float dt)
{
	if (proj->projectile_type == 1) {
		// Grenades keep a separate simulated angle phase across server corrections.
		VectorMA(proj->sim_angles, dt, proj->avel, proj->sim_angles);
		VectorCopy(proj->sim_angles, proj->angles);
	}
	else {
		// Non-grenade angular velocity is simple display state.
		VectorMA(proj->angles, dt, proj->avel, proj->angles);
	}
}

static void CL_EZCSQC_ProjectileBounce(ezcsqc_entity_t *proj, float dt, qbool update_angles)
{
	float gravity_value = movevars.gravity;
	float movetime, bump;

	// A grounded grenade stays still until the server gives it upward velocity again.
	if ((proj->flags & 512) && proj->vel[2] >= 1.0f / 32.0f) {
		proj->flags &= ~512;
	}
	else if (proj->flags & 512) {
		return;
	}

	// Apply half gravity before and after the move, matching the server integration shape.
	proj->vel[2] -= 0.5f * dt * gravity_value;
	if (update_angles) {
		CL_EZCSQC_ProjectileAdvanceAngles(proj, dt);
	}

	// Resolve a few bounces inside the render frame instead of tunneling through walls.
	movetime = dt;
	for (bump = 0; bump < 5 && movetime > 0; bump++) {
		vec3_t move, to;
		trace_t phytrace;
		float d, bouncefac, bouncestp;

		VectorScale(proj->vel, movetime, move);
		VectorAdd(proj->origin, move, to);
		phytrace = PM_TraceLine(proj->origin, to);
		VectorCopy(phytrace.endpos, proj->origin);

		if (phytrace.fraction == 1) {
			break;
		}

		// Continue with the unconsumed fraction after the collision.
		movetime *= 1 - min(1, phytrace.fraction);
		bouncefac = 0.5f;
		bouncestp = (60.0f / 800.0f) * gravity_value;

		CL_EZCSQC_ProjectileClipVelocity(proj->vel, phytrace.plane.normal, 1 + bouncefac);
		d = DotProduct(phytrace.plane.normal, proj->vel);
		// Shallow floor impacts below the stop threshold become a resting grenade.
		if (phytrace.plane.normal[2] > 0.7f && d < bouncestp && d > -bouncestp) {
			proj->flags |= 512;
			VectorClear(proj->vel);
			VectorClear(proj->avel);
		}
		else {
			proj->flags &= ~512;
		}
	}

	// Only airborne grenades receive the second half of gravity.
	if (!(proj->flags & 512)) {
		proj->vel[2] -= 0.5f * dt * gravity_value;
	}
}

static void CL_EZCSQC_SetGrenadeServerState(ezcsqc_entity_t *self, qbool reset_angles)
{
	// Position and velocity are authoritative; angle phase is client-owned unless reset.
	VectorCopy(self->s_origin, self->origin);
	VectorCopy(self->s_origin, self->sim_origin);
	if (reset_angles) {
		VectorCopy(self->angles, self->sim_angles);
		self->angle_simtime = cl.time;
	}
	else if (!self->angle_simtime) {
		self->angle_simtime = cl.time;
	}
	VectorCopy(self->s_velocity, self->vel);
	// A near-zero server velocity means the authoritative grenade is at rest.
	if (VectorLength(self->vel) < 1.0f) {
		VectorClear(self->vel);
		VectorClear(self->avel);
		self->flags |= 512;
	}
	else {
		VectorSet(self->avel, 300, 300, 300);
		self->flags &= ~512;
	}
	self->simtime = self->s_time;
	self->projectile_type = 1;
}

static void CL_EZCSQC_ApplyProjectileHandoffSmoothing(ezcsqc_entity_t *self, vec3_t authoritative_origin)
{
	if (self->handoff_smoothing) {
		if (cl.time < self->handoff_endtime) {
			float frac = (self->handoff_endtime > self->handoff_starttime)
				? (cl.time - self->handoff_starttime) / (self->handoff_endtime - self->handoff_starttime)
				: 1.0f;
			vec3_t correction;

			// Smoothstep the stored phase offset so the rendered projectile eases into authority.
			frac = bound(0, frac, 1);
			frac = frac * frac * (3 - 2 * frac);
			VectorScale(self->handoff_delta, 1 - frac, correction);
			VectorSubtract(authoritative_origin, correction, self->origin);
		}
		else {
			// Once the smoothing window ends, draw exactly at the authoritative endpoint.
			self->handoff_smoothing = false;
			VectorCopy(authoritative_origin, self->origin);
		}
	}
	else {
		VectorCopy(authoritative_origin, self->origin);
	}
}

static qbool Predraw_Projectile(ezcsqc_entity_t *self)
{
	float dt;
	int visual_effects;

	if (!cl_predict_projectiles.integer) {
		return false;
	}

	if (!self->modelindex || self->modelindex >= MAX_MODELS || !cl.model_precache[self->modelindex]) {
		return false;
	}

	visual_effects = CL_EZCSQC_ProjectileVisualEffects(self);

	if (self->local_projectile) {
		double projectile_time = cl.time;

		/*
		 * Local projectiles exist only until the server-authored CSQC entity is
		 * visible. This mirrors legacy antilag's "fake until server arrives"
		 * behavior without routing through the old fproj_t renderer.
		 */
		if (projectile_time < self->starttime) {
			return false;
		}
		if (projectile_time >= self->endtime) {
			CL_EZCSQC_DebugLocalProjectile(self, "expire", self->origin);
			CL_EZCSQC_Ent_Remove(self);
			return false;
		}

			if (self->projectile_type == 1) {
				double dt = max(projectile_time - self->simtime, 0);

				// Local grenades free-run with bounce physics until the server grenade arrives.
				self->simtime = projectile_time;
				if (dt > 0) {
					CL_EZCSQC_ProjectileBounce(self, dt, true);
				}
		}
		else {
			vec3_t traveled;
			vec3_t trace_end;

			// Rockets and nails are straight-line visuals phased from their local start time.
			VectorCopy(self->start, self->origin);
			VectorScale(self->vel, projectile_time - self->starttime, traveled);
			VectorAdd(self->origin, traveled, self->origin);
			VectorMA(self->origin, 0.02f, self->vel, trace_end);

			/*
			 * Hits are still server-authoritative. This trace only prevents the
			 * local visual placeholder from visibly flying through nearby walls
			 * while waiting for KTX's remove/update message.
			 */
			if (CL_EZCSQC_ProjectileTraceBlocked(self->start, trace_end)) {
				CL_EZCSQC_DebugLocalProjectile(self, "trace-remove", self->origin);
				CL_EZCSQC_Ent_Remove(self);
				return false;
			}
		}
		self->debug_predraw_count++;
		if (cl_ezcsqc_debug.integer > 2 && (self->debug_predraw_count <= 4 || cl.time >= self->debug_next_time)) {
			CL_EZCSQC_DebugLocalProjectile(self, "predraw", self->origin);
			self->debug_next_time = cl.time + 0.025;
		}
		return true;
	}

	{
		vec3_t previous_origin;

		// Remember the previous drawn point so extrapolated server rockets stop at walls.
		VectorCopy(self->oldorigin, previous_origin);
			dt = cl.servertime - self->s_time;
			if (visual_effects & EF_GRENADE) {
				vec3_t authoritative_origin;

				// Server grenades receive sparse corrections, then bounce locally between them.
				VectorCopy(self->sim_origin, self->origin);
				dt = max(cl.servertime - self->simtime, 0);
				self->simtime = cl.servertime;
				if (dt > 0) {
					CL_EZCSQC_ProjectileBounce(self, dt, false);
				}
				VectorCopy(self->origin, authoritative_origin);
				VectorCopy(authoritative_origin, self->sim_origin);

				// Spin advances on render time so packet jitter does not make grenades tumble wildly.
				dt = max(cl.time - self->angle_simtime, 0);
				self->angle_simtime = cl.time;
				if (dt > 0) {
					CL_EZCSQC_ProjectileAdvanceAngles(self, min(dt, 0.050f));
				}
				CL_EZCSQC_ApplyProjectileHandoffSmoothing(self, authoritative_origin);
			}
		else {
			vec3_t authoritative_origin;
			vec3_t trace_end;

			// Server rockets/nails are extrapolated from the latest authoritative origin/time.
			VectorMA(self->s_origin, dt, self->s_velocity, authoritative_origin);
			CL_EZCSQC_ApplyProjectileHandoffSmoothing(self, authoritative_origin);
			VectorMA(self->origin, 0.02f, self->s_velocity, trace_end);
			/*
			 * Server projectiles are extrapolated between CSQC updates for
			 * smooth display. Stop non-bouncing visuals at the first local wall
			 * hit so extrapolation does not draw through geometry.
			 */
			if (CL_EZCSQC_ProjectileTraceBlocked(previous_origin, trace_end)) {
				CL_EZCSQC_Ent_Remove(self);
				return false;
			}
		}
		VectorCopy(self->origin, self->oldorigin);
	}
	return true;
}

static void WeaponPred_SpawnProjectile(usercmd_t *u, player_state_t *ps, ezcsqc_weapon_state_t *ws, weppredanim_t *anim)
{
	vec3_t forward, right, up, velocity, origin;
	ezcsqc_entity_t *ent;
	int modelindex = anim->projectile_model;

	if (!cl_predict_projectiles.integer || !modelindex || modelindex >= MAX_MODELS || !cl.model_precache[modelindex]) {
		return;
	}

	ent = CL_EZCSQC_AllocLocalProjectile();
	if (!ent) {
		if (cl_ezcsqc_debug.integer > 2) {
			Com_Printf("EZCSQC local projectile alloc failed model=%d frame=%d\n", modelindex, current_predframe);
		}
		return;
	}

	/*
	 * This is the client-side visual counterpart to KTX's projectile spawn. It
	 * does not affect hits, collision, ammo, damage, or authoritative state.
	 */
	CL_EZCSQC_InitProjectile(ent, modelindex, cl.playernum + 1);

	/*
	 * KTX sends projectile vectors in weapon-local coordinates:
	 *   [0] right, [1] forward, [2] up for velocity
	 *   [0] right, [1] forward, [2] world-z for spawn offset
	 * Keep that convention here so the local visual projectile lines up with
	 * the authoritative CSQC projectile when the server update arrives.
	 */
	AngleVectors(u->angles, forward, right, up);
	VectorCopy(ps->origin, origin);
	VectorMA(origin, anim->projectile_offset[0], right, origin);
	VectorMA(origin, anim->projectile_offset[1], forward, origin);
	origin[2] += anim->projectile_offset[2];

	VectorClear(velocity);
	VectorMA(velocity, anim->projectile_velocity[0], right, velocity);
	VectorMA(velocity, anim->projectile_velocity[1], forward, velocity);
	VectorMA(velocity, anim->projectile_velocity[2], up, velocity);

	// Point-blank rocket impacts become a predicted explosion instead of a fake model.
	if (CL_EZCSQC_PredictRocketSpawnTouch(modelindex, origin, velocity, ps->state_time)) {
		CL_EZCSQC_Ent_Remove(ent);
		return;
	}

	// Initialize common projectile motion state from the predicted weapon definition.
	vectoangles(velocity, ent->angles);
	VectorCopy(origin, ent->origin);
	VectorCopy(origin, ent->start);
	VectorCopy(velocity, ent->vel);
	VectorCopy(origin, ent->partorg);
	VectorClear(ent->avel);
	if (ent->projectile_type == 1) {
		// Grenades keep native-looking spin and do not get the non-grenade newmis phase lead.
		VectorSet(ent->avel, 300, 300, 300);
		VectorCopy(ent->angles, ent->sim_angles);
	}
	else {
		float speed = VectorLength(velocity);
		if (speed > 1) {
			// Match KTX/native newmis: rockets and nails begin 0.05 seconds into flight.
			ent->starttime -= 0.05f;
			VectorMA(origin, 0.05f, velocity, ent->origin);
			VectorCopy(ent->origin, ent->partorg);
		}
	}

	CL_EZCSQC_DebugLocalProjectile(ent, "spawn", ent->origin);

	if (cl_ezcsqc_debug.integer > 3) {
		Com_Printf("EZCSQC predicted projectile model=%d org=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f)\n",
			modelindex, origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2]);
	}
}

static void CL_EZCSQC_AddPredictionSound(unsigned short index, unsigned short mask, byte chan)
{
	weppredsound_t *snd, *last = NULL;

	if (!index || index >= MAX_SOUNDS) {
		return;
	}

	// Keep one entry per predicted sound/mask/channel tuple for echo suppression.
	for (snd = predictionsoundlist; snd; snd = snd->next) {
		if (snd->index == index && snd->mask == mask && snd->chan == chan) {
			return;
		}
		last = snd;
	}

	/* This list lets CL_EZCSQC_Event_Sound suppress server echoes of predicted sounds. */
	snd = malloc(sizeof(*snd));
	if (!snd) {
		return;
	}

	snd->index = index;
	snd->mask = mask;
	snd->chan = chan;
	snd->next = NULL;
	if (last) {
		last->next = snd;
	}
	else {
		predictionsoundlist = snd;
	}
}

static void EntUpdate_WeaponDef(ezcsqc_entity_t *self, qbool is_new)
{
	int i, k;
	int raw;
	int sendflags = MSG_ReadByte();
	int raw_wep_index = MSG_ReadByte();
	int wep_index = bound(0, raw_wep_index, MAX_PREDWEPS - 1);
	weppreddef_t *wep = &wpredict_definitions[wep_index];
	qbool had_model = wep->modelindex != 0;

	(void)self;
	(void)is_new;

	// A corrupt weapon index means the remaining CSQC stream cannot be trusted.
	if (raw_wep_index < 0 || raw_wep_index >= MAX_PREDWEPS) {
		Host_Error("CL_EZCSQC_Ent_Update: bad weapondef index %d flags=%02x ent=%d msg_readcount=%d msg_size=%d",
			raw_wep_index, sendflags, self->entnum, msg_readcount, net_message.cursize);
	}

	/*
	 * KTX sends numeric precache indexes, not names. Keep them as indexes so
	 * viewmodel and sound lookup follows the engine's normal precache tables.
	 */
	if (sendflags & WEAPONDEF_INIT) {
		wep->attack_time = MSG_ReadShort();
		raw = MSG_ReadShort();
		wep->modelindex = bound(0, raw, MAX_MODELS - 1);
	}

	if (sendflags & WEAPONDEF_FLAGS) {
		// Weapon flags bind Quake impulses/items to compact weapon_index values.
		wep->impulse = MSG_ReadByte();
		wep->itemflag = MSG_ReadByte();
		if (wep->itemflag != 255) {
			/* KTX transmits the item bit number to keep the payload small. */
			wep->itemflag = 1 << wep->itemflag;
		}
	}

	if (sendflags & WEAPONDEF_ANIM) {
		// Animation records drive the native viewmodel FSM and predicted effects.
		raw = MSG_ReadByte();
		wep->anim_number = bound(0, raw, WEPPRED_MAXSTATES);
		for (i = 0; i < wep->anim_number; i++) {
			weppredanim_t *anim = &wep->anim_states[i];

			// mdlframe is biased on the wire so negative looping markers fit in a byte.
			anim->mdlframe = MSG_ReadByte() - 127;
			anim->flags = MSG_ReadByte();
			if (anim->flags & WEPPREDANIM_MOREBYTES) {
				anim->flags |= MSG_ReadByte() << 8;
			}
			if (anim->flags & WEPPREDANIM_SOUND) {
				raw = MSG_ReadShort();
				anim->sound = bound(0, raw, MAX_SOUNDS - 1);
				anim->soundmask = MSG_ReadShort();
				// Register predicted sounds before replay can emit them.
				CL_EZCSQC_AddPredictionSound(anim->sound, anim->soundmask, (anim->flags & WEPPREDANIM_SOUNDAUTO) ? 0 : 1);
			}
			if (anim->flags & WEPPREDANIM_PROJECTILE) {
				raw = MSG_ReadShort();
				anim->projectile_model = bound(0, raw, MAX_MODELS - 1);
				for (k = 0; k < 3; k++) {
					anim->projectile_velocity[k] = MSG_ReadShort();
				}
				// Offsets are signed bytes so alternating nails can spawn left or right.
				for (k = 0; k < 3; k++) {
					anim->projectile_offset[k] = (signed char)MSG_ReadByte();
				}
			}
			anim->nextanim = MSG_ReadByte();
			if (anim->flags & WEPPREDANIM_BRANCH) {
				anim->altanim = MSG_ReadByte();
			}
			anim->length = MSG_ReadByte() * 10;
		}
	}

	if (cl_ezcsqc_debug.integer > 3 || (cl_ezcsqc_debug.integer > 1 && !had_model && wep->modelindex)) {
		Com_Printf("EZCSQC weapondef %s index=%d flags=%02x model=%d attack_time=%d anims=%d impulse=%d item=%d\n",
			is_new ? "new" : "update", wep_index, sendflags,
			wep->modelindex, wep->attack_time, wep->anim_number, wep->impulse, wep->itemflag);
	}
}

static void WeaponPred_SetModel(ezcsqc_entity_t *self)
{
	weppreddef_t *wep = &wpredict_definitions[bound(0, ws_predicted.weapon_index, MAX_PREDWEPS - 1)];

	// The predicted weapon entity only draws after its definition has arrived.
	if (wep->modelindex) {
		self->modelindex = wep->modelindex;
		self->frame = ws_predicted.frame;
	}
}

static void WeaponPred_PlayEffects(usercmd_t *u, player_state_t *ps, ezcsqc_weapon_state_t *ws, weppredanim_t *anim)
{
	/*
	 * Prediction replays several historical usercmds every render frame. Effects
	 * must fire only once for newly-crossed frames, not every replay.
	 */
	if (!is_effectframe) {
		return;
	}

	if (cl_nopred_weapon.integer) {
		if (cl_ezcsqc_debug.integer > 1 && (anim->flags & (WEPPREDANIM_SOUND | WEPPREDANIM_PROJECTILE))) {
			Com_Printf("EZCSQC effect suppressed: cl_nopred_weapon frame=%d flags=%x\n", current_predframe, anim->flags);
		}
		return;
	}

	if (anim->flags & WEPPREDANIM_SOUND) {
		int chan = (anim->flags & WEPPREDANIM_SOUNDAUTO) ? 0 : 1;
		// Lightning uses a minimum local active window so replay does not stutter the loop.
		if ((anim->flags & WEPPREDANIM_LTIME) && ws->client_time < lg_twidth) {
			if (cl_ezcsqc_debug.integer > 1) {
				Com_Printf("EZCSQC sound suppressed: LG time gate frame=%d client_time=%.3f lg_twidth=%.3f\n",
					current_predframe, ws->client_time, lg_twidth);
			}
			return;
		}
		// The cvar can enable all sounds or mask off specific predicted sound classes.
		if (cl_predict_weaponsound.integer == 1 || (cl_predict_weaponsound.integer && !(cl_predict_weaponsound.integer & anim->soundmask))) {
			if (anim->sound > 0 && anim->sound < MAX_SOUNDS && cl.sound_precache[anim->sound]) {
				if (cl_ezcsqc_debug.integer > 1) {
					Com_Printf("EZCSQC predicted sound frame=%d sound=%d mask=%x client_time=%.3f\n",
						current_predframe, anim->sound, anim->soundmask, ws->client_time);
				}
				S_StartSound(cl.playernum + 1, chan, cl.sound_precache[anim->sound], pmove.origin, 1, 0);
			}
		}
		if (anim->flags & WEPPREDANIM_LTIME) {
			// Remember the predicted LG window so repeated replay frames stay quiet.
			lg_twidth = ws->client_time + 0.6f;
		}
	}

	if (anim->flags & WEPPREDANIM_PROJECTILE) {
		// Projectile effects create only the local visual bridge; KTX still owns authority.
		if (cl_ezcsqc_debug.integer > 1) {
			Com_Printf("EZCSQC predicted projectile effect frame=%d model=%d client_time=%.3f\n",
				current_predframe, anim->projectile_model, ws->client_time);
		}
		WeaponPred_SpawnProjectile(u, ps, ws, anim);
	}
}

static void WeaponPred_StartFrame(usercmd_t *u, player_state_t *ps, ezcsqc_weapon_state_t *ws, weppreddef_t *wep, int framenum)
{
	weppredanim_t *anim = WEPANIM(wep, bound(0, framenum, WEPPRED_MAXSTATES - 1));

	// Non-attack frames can emit sounds/projectiles when the FSM enters them.
	if (!(anim->flags & WEPPREDANIM_ATTACK && !(anim->flags & WEPPREDANIM_BRANCH))) {
		WeaponPred_PlayEffects(u, ps, ws, anim);
	}

	// Advance the predicted weapon FSM to this animation state.
	ws->client_thinkindex = framenum;
	ws->client_nextthink = anim->length ? ws->client_time + LENGTH2S(anim->length) : 0;

	if (anim->mdlframe >= 0) {
		ws->frame = anim->mdlframe;
	}
	else {
		/* Negative mdlframe encodes a simple looping range from the draft protocol. */
		ws->frame++;
		if (ws->frame > -anim->mdlframe) {
			ws->frame = 1;
		}
	}
}

static void WeaponPred_WAttack(usercmd_t *u, player_state_t *ps, ezcsqc_weapon_state_t *ws)
{
	int i;
	weppreddef_t *wep = &wpredict_definitions[bound(0, ws->weapon_index, MAX_PREDWEPS - 1)];
	weppredanim_t *anim = WEPANIM(wep, bound(0, ws->client_thinkindex, WEPPRED_MAXSTATES - 1));

	// Do not start a new predicted attack until the weapon is ready and +attack is held.
	if (ws->client_time < ws->attack_finished || !(u->buttons & BUTTON_ATTACK)) {
		if (cl_ezcsqc_debug.integer > 3 && (u->buttons & BUTTON_ATTACK)) {
			Com_Printf("EZCSQC attack not ready frame=%d weapon=%d client_time=%.3f attack_finished=%.3f diff=%.3f think=%d\n",
				current_predframe, ws->weapon_index, ws->client_time, ws->attack_finished,
				ws->attack_finished - ws->client_time, ws->client_thinkindex);
		}
		return;
	}

	// Some weapons mark the current animation itself as the attack entry.
	if (anim->flags & WEPPREDANIM_ATTACK && !(anim->flags & WEPPREDANIM_BRANCH)) {
		if (cl_ezcsqc_debug.integer > 3) {
			Com_Printf("EZCSQC attack start frame=%d weapon=%d think=%d animflags=%x client_time=%.3f\n",
				current_predframe, ws->weapon_index, ws->client_thinkindex, anim->flags, ws->client_time);
		}
		WeaponPred_PlayEffects(u, ps, ws, anim);
		ws->attack_finished = ws->client_time + LENGTH2S(wep->attack_time);
		WeaponPred_StartFrame(u, ps, ws, wep, anim->nextanim);
		return;
	}

	// Otherwise find the default attack animation for the current weapon definition.
	for (i = 0; i < WEPPRED_MAXSTATES; i++) {
		anim = WEPANIM(wep, i);
		if (!(anim->flags & WEPPREDANIM_DEFAULT)) {
			continue;
		}
		if (!(anim->flags & WEPPREDANIM_ATTACK)) {
			break;
		}
		if (cl_ezcsqc_debug.integer > 3) {
			Com_Printf("EZCSQC default attack frame=%d weapon=%d anim=%d animflags=%x client_time=%.3f\n",
				current_predframe, ws->weapon_index, i, anim->flags, ws->client_time);
		}
		WeaponPred_PlayEffects(u, ps, ws, anim);
		ws->attack_finished = ws->client_time + LENGTH2S(wep->attack_time);
		WeaponPred_StartFrame(u, ps, ws, wep, anim->nextanim);
		break;
	}
}

static void WeaponPred_Logic(usercmd_t *u, player_state_t *ps, ezcsqc_weapon_state_t *ws)
{
	float time_held;
	int frame_to_go;
	weppreddef_t *wep = &wpredict_definitions[bound(0, ws->weapon_index, MAX_PREDWEPS - 1)];
	weppredanim_t *anim = WEPANIM(wep, bound(0, ws->client_thinkindex, WEPPRED_MAXSTATES - 1));

	// Default states and not-yet-due states do not advance on their own.
	if ((anim->flags & WEPPREDANIM_DEFAULT) || ws->client_time < ws->client_nextthink) {
		return;
	}

	// Non-branch attack states are entered only by WeaponPred_WAttack().
	if ((anim->flags & WEPPREDANIM_ATTACK) && !(anim->flags & WEPPREDANIM_BRANCH)) {
		return;
	}

	// Temporarily evaluate the state at its scheduled think time.
	time_held = ws->client_time;
	frame_to_go = anim->nextanim;
	ws->client_time = ws->client_nextthink;

	/* Branching attack states keep looping while +attack remains held. */
	if (anim->flags & WEPPREDANIM_ATTACK) {
		if (!(u->buttons & BUTTON_ATTACK) || ws->impulse) {
			frame_to_go = anim->altanim;
		}
		else {
			ws->attack_finished = ws->client_time + LENGTH2S(wep->attack_time);
		}
	}

	// Restore the replay time after applying any scheduled state transition.
	WeaponPred_StartFrame(u, ps, ws, wep, frame_to_go);
	ws->client_time = time_held;
}

static qbool WeaponPred_SwitchWeapon(int impulse, ezcsqc_weapon_state_t *ws)
{
	int i, found = false;
	int items = cl.stats[STAT_ITEMS];

	// Weapon switches obey the same attack_finished gate as the server.
	if (ws->client_time < ws->attack_finished) {
		return false;
	}

	// Find the predicted weapon definition mapped to the requested impulse.
	for (i = 0; i < MAX_PREDWEPS; i++) {
		weppreddef_t *wep = &wpredict_definitions[i];
		if (wep->impulse != impulse) {
			continue;
		}
		if (ws->weapon_index == i) {
			found = true;
			continue;
		}
		if (items & wep->itemflag) {
			// Reset the predicted FSM to the new weapon's first frame.
			ws->weapon_index = i;
			ws->weapon = wep->itemflag;
			ws->client_thinkindex = 0;
			ws->client_nextthink = 0;
			ws->frame = WEPANIM(wep, 0)->mdlframe;
			return true;
		}
	}

	return found;
}

static void WeaponPred_Simulate(usercmd_t u, player_state_t ps, ezcsqc_weapon_state_t *ws)
{
	// KTX can force prediction off while still letting time advance.
	if (ws->client_predflags == PRDFL_FORCEOFF) {
		ws->client_time += u.msec * 0.001f;
		ws->impulse = 0;
		ws->attack_finished = max(ws->attack_finished, ws->client_time + 0.05f);
		return;
	}

	// Locked/non-playable movement states should not synthesize local attacks.
	if (ps.pm_type == PM_DEAD || ps.pm_type == PM_NONE || ps.pm_type == PM_LOCK) {
		ws->impulse = 0;
		ws->attack_finished = ws->client_time + 0.05f;
		return;
	}

	// Replay the usercmd's elapsed time before evaluating weapon logic.
	ws->client_time += u.msec * 0.001f;
	if (u.impulse) {
		ws->impulse = u.impulse;
	}

	// Run scheduled animation logic before accepting switches or attacks.
	WeaponPred_Logic(&u, &ps, ws);
	if (ws->impulse && WeaponPred_SwitchWeapon(ws->impulse, ws)) {
		ws->impulse = 0;
	}
	WeaponPred_WAttack(&u, &ps, ws);
}

static qbool WeaponPred_Predraw(ezcsqc_entity_t *self)
{
	int i = 1;
	int effect_threshold;

	/*
	 * Match CL_PredictMove(): start from the last confirmed server frame and
	 * replay only unconfirmed local commands. Replaying cl.validsequence again
	 * duplicates the already-acknowledged command; replaying older commands can
	 * move attack_finished forward before the current +attack is processed.
	 */
	ws_predicted = ws_server[cl.validsequence & UPDATE_MASK];
	effect_threshold = bound(
		cl.validsequence + 1,
		cls.netchan.outgoing_sequence - (cl_predict_buffer.integer + 1),
		cls.netchan.outgoing_sequence - 1);
	if (cl_ezcsqc_debug.integer > 3) {
		Com_Printf("EZCSQC pred range valid=%d outgoing=%d threshold=%d last_effect=%d buffer=%d\n",
			cl.validsequence, cls.netchan.outgoing_sequence, effect_threshold, last_effectframe, cl_predict_buffer.integer);
	}

	/*
	 * Rebuild the weapon state by replaying pending usercmds on top of the last
	 * server update. This mirrors movement prediction: the server supplies the
	 * baseline, local input makes the viewmodel responsive until correction.
	 */
	for (; i < UPDATE_BACKUP - 1 && cl.validsequence + i < cls.netchan.outgoing_sequence; i++) {
		frame_t *to = &cl.frames[(cl.validsequence + i) & UPDATE_MASK];
		int frame_num = cl.validsequence + i;
		current_predframe = frame_num;
		is_effectframe = false;
		// Only newly eligible frames may emit one-shot predicted effects.
		if (frame_num > last_effectframe && frame_num <= effect_threshold) {
			is_effectframe = true;
			last_effectframe = frame_num;
		}
		WeaponPred_Simulate(to->cmd, to->playerstate[cl.playernum], &ws_predicted);
		// Save the post-command state so later prediction starts from matching indices.
		ws_saved[(cl.validsequence + i + 1) & UPDATE_MASK] = ws_predicted;
	}

	WeaponPred_SetModel(self);
	return false;
}

static void EntUpdate_WeaponInfo(ezcsqc_entity_t *self, qbool is_new)
{
	int sendflags = MSG_ReadByte();
	ezcsqc_weapon_state_t *ws_current = &ws_server[cl.validsequence & UPDATE_MASK];
	qbool was_viewweapon = (ezcsqc.weapon_prediction && viewweapon == self);

	/*
	 * mvdsv writes CSQC entities after packetentities, so CL_ParsePacketEntities()
	 * has already advanced cl.validsequence for this server frame. Store the
	 * server-authored weapon state at that same frame index and delta it from
	 * the previous valid frame.
	 */
	*ws_current = ws_server[cl.oldvalidsequence & UPDATE_MASK];

	if (sendflags & WEAPONINFO_INDEX) {
		// Weapon index updates bind the server weapon to a previously sent definition.
		ws_current->impulse = MSG_ReadByte();
		ws_current->weapon_index = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_AMMO_SHELLS) {
		ws_current->ammo_shells = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_AMMO_NAILS) {
		ws_current->ammo_nails = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_AMMO_ROCKETS) {
		ws_current->ammo_rockets = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_AMMO_CELLS) {
		ws_current->ammo_cells = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_ATTACK) {
		// Attack timing is the server baseline that local replay predicts from.
		ws_current->attack_finished = MSG_ReadFloat();
		ws_current->client_nextthink = MSG_ReadFloat();
		ws_current->client_thinkindex = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_TIMING) {
		// Client time and frame keep the viewmodel FSM aligned to KTX.
		ws_current->client_time = MSG_ReadFloat();
		ws_current->frame = MSG_ReadByte();
	}
	if (sendflags & WEAPONINFO_PRED_PING) {
		// Pred flags include server-side permission to predict local attacks.
		ws_current->client_predflags = MSG_ReadByte();
		ws_current->client_ping = MSG_ReadByte() / 1000.0f;
	}

	/*
	 * Do this for every weapon-info update, not only newly allocated EZCSQC
	 * slots. Servers are allowed to keep reusing an existing CSQC entity number,
	 * and the first weapon-info payload seen by this client may therefore arrive
	 * with is_new == false.
	 */
	self->drawmask = DRAWMASK_VIEWMODEL;
	self->predraw = WeaponPred_Predraw;
	ezcsqc.weapon_prediction = true;
	viewweapon = self;

	if (cl_ezcsqc_debug.integer > 3 || (cl_ezcsqc_debug.integer > 1 && !was_viewweapon)) {
		Com_Printf("EZCSQC weaponinfo %s ent=%d flags=%02x weapon_index=%d frame=%d client_time=%.3f attack_finished=%.3f predflags=%d ping=%.3f\n",
			is_new ? "new" : "update", self->entnum, sendflags, ws_current->weapon_index, ws_current->frame,
			ws_current->client_time, ws_current->attack_finished, ws_current->client_predflags, ws_current->client_ping);
	}
}

static void EntUpdate_Projectile(ezcsqc_entity_t *self, qbool is_new)
{
	int sendflags = MSG_ReadByte();

	/*
	 * Match KTX's post-port payload exactly. PROJECTILE_OWNER contains only the
	 * owner entity; the old quantized client_time byte is intentionally gone.
	 */
	if (sendflags & PROJECTILE_ORIGIN) {
		// Origin packets carry the authoritative state used for extrapolation.
		self->s_origin[0] = MSG_ReadCoord();
		self->s_origin[1] = MSG_ReadCoord();
		self->s_origin[2] = MSG_ReadCoord();
		self->s_velocity[0] = MSG_ReadCoord();
		self->s_velocity[1] = MSG_ReadCoord();
		self->s_velocity[2] = MSG_ReadCoord();
		self->s_time = MSG_ReadFloat();
	}
	if (sendflags & PROJECTILE_MODEL) {
		model_t *model;
		// Model/effects determine the local trail, light, and grenade behavior.
		self->modelindex = MSG_ReadShort();
		self->effects = MSG_ReadShort();
		if (self->modelindex < 0 || self->modelindex >= MAX_MODELS || !cl.model_precache[self->modelindex]) {
			self->modelindex = 0;
			self->modelflags = 0;
		}
		else {
			model = cl.model_precache[self->modelindex];
			self->modelflags = model->flags;
			if ((self->effects & EF_GRENADE) || model->modhint == MOD_GRENADE) {
				// Grenades use bounce simulation between sparse server origin updates.
				self->projectile_type = 1;
				self->effects |= EF_GRENADE;
			}
			if (model->modhint == MOD_SPIKE) {
				// Spike models need an explicit nail-trail effect for the shared trail path.
				self->modelflags |= EF_NAILTRAIL;
				self->effects |= 256;
			}
		}
	}
	if (sendflags & PROJECTILE_ANGLES) {
		// Initial angles seed model orientation and grenade spin phase.
		self->angles[0] = MSG_ReadAngle();
		self->angles[1] = MSG_ReadAngle();
		self->angles[2] = MSG_ReadAngle();
	}
	if (sendflags & PROJECTILE_OWNER) {
		// Owner links server projectiles to matching local placeholders.
		self->ownernum = MSG_ReadShort();
	}
	if (sendflags & PROJECTILE_SPAWN_ORIGIN) {
		vec3_t spawn_origin, diff;
		// Spawn origin is a trail seed, not a later correction target.
		spawn_origin[0] = MSG_ReadCoord();
		spawn_origin[1] = MSG_ReadCoord();
		spawn_origin[2] = MSG_ReadCoord();
		VectorSubtract(spawn_origin, self->s_origin, diff);
		/*
		 * KTX may keep sending the original spawn point on later updates.
		 * It is only a seed for a new trail; applying it after creation snaps
		 * the trail origin backward and makes the trail appear to restart.
		 */
		if (is_new && VectorLength(diff) < 150) {
			VectorCopy(spawn_origin, self->trail_origin);
		}
	}

	if (is_new) {
		// New projectiles become renderable EZCSQC entities with packet-style trail state.
		self->drawmask = DRAWMASK_PROJECTILE;
		self->predraw = Predraw_Projectile;
		if (!self->trail_origin[0] && !self->trail_origin[1] && !self->trail_origin[2]) {
			VectorCopy(self->s_origin, self->trail_origin);
		}
		VectorCopy(self->s_origin, self->oldorigin);
		VectorCopy(self->trail_origin, self->partorg);
		VectorCopy(self->trail_origin, self->cent.old_origin);
		VectorCopy(self->trail_origin, self->cent.lerp_origin);
		VectorCopy(self->trail_origin, self->cent.trails[0].stop);
		VectorCopy(self->trail_origin, self->cent.trails[1].stop);
		VectorCopy(self->trail_origin, self->cent.trails[2].stop);
		VectorCopy(self->trail_origin, self->cent.trails[3].stop);
		if (CL_EZCSQC_ProjectileVisualEffects(self) & EF_GRENADE) {
			CL_EZCSQC_SetGrenadeServerState(self, true);
		}
	}
	else if ((sendflags & PROJECTILE_ORIGIN) && (CL_EZCSQC_ProjectileVisualEffects(self) & EF_GRENADE)) {
		// Live grenade corrections reset physics state but keep client-owned spin continuity.
		CL_EZCSQC_SetGrenadeServerState(self, false);
	}

	if (is_new) {
		// Replace any matching local fake now that the authoritative projectile exists.
		CL_EZCSQC_RemoveMatchedLocalProjectile(self);

		if (!(CL_EZCSQC_ProjectileVisualEffects(self) & EF_GRENADE)) {
			vec3_t origin;

			// Drop non-bouncing projectiles that already hit local geometry before first draw.
			VectorMA(self->s_origin, max(cl.servertime - self->s_time, 0), self->s_velocity, origin);
			if (CL_EZCSQC_ProjectileTraceBlocked(self->s_origin, origin)) {
				CL_EZCSQC_Ent_Remove(self);
				return;
			}
		}
	}

	if (cl_ezcsqc_debug.integer > 3) {
		Com_Printf("EZCSQC projectile %s ent=%d flags=%02x model=%d effects=%d org=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f)\n",
			is_new ? "new" : "update", self->entnum, sendflags, self->modelindex, self->effects,
			self->s_origin[0], self->s_origin[1], self->s_origin[2],
			self->s_velocity[0], self->s_velocity[1], self->s_velocity[2]);
	}
}

static void CL_EZCSQC_Ent_Update(ezcsqc_entity_t *self, qbool is_new)
{
	int type = MSG_ReadByte();

	// Seeing any EZCSQC payload means the negotiated extension is active in practice.
	ezcsqc.active = true;
	if (cl_ezcsqc_debug.integer > 3) {
		Com_Printf("EZCSQC entity %s ent=%d type=%d\n", is_new ? "new" : "update", self->entnum, type);
	}

	// Dispatch by the first byte of each entity payload.
	switch (type) {
	case EZCSQC_PROJECTILE:
		EntUpdate_Projectile(self, is_new);
		break;
	case EZCSQC_WEAPONINFO:
		EntUpdate_WeaponInfo(self, is_new);
		break;
	case EZCSQC_WEAPONDEF:
		EntUpdate_WeaponDef(self, is_new);
		break;
	default:
		Host_Error("CL_EZCSQC_Ent_Update: unknown entity type %d", type);
		break;
	}
}

void CL_EZCSQC_ParseEntities(void)
{
	int entnum;

	/*
	 * svc_fte_csqcentities is a sequence of:
	 *   short entnum, payload...
	 *   short entnum|0x8000 for removals
	 *   short 0 terminator
	 */
	while (!msg_badread) {
		qbool is_new = false;
		ezcsqc_entity_t *ent;

		// Entity words are unsigned on the wire; high bit marks removals.
		entnum = (unsigned short) MSG_ReadShort();
		if (!entnum) {
			return;
		}

		if (entnum & 0x8000) {
			ezcsqc_entity_t *removed;

			// Strip the remove bit before looking up the networked entity slot.
			entnum &= 0x7fff;
			if (entnum >= MAX_EDICTS) {
				Host_Error("CL_EZCSQC_ParseEntities: bad remove entity %d", entnum);
			}
			removed = ezcsqc_networkedents[entnum];
			if (cl_ezcsqc_debug.integer > 1) {
				Com_Printf("EZCSQC remove message ent=%d known=%d drawmask=%x model=%d projectile=%d local=%d\n",
					entnum, removed && !removed->isfree, removed ? removed->drawmask : 0,
					removed ? removed->modelindex : 0,
					removed ? !!(removed->drawmask & DRAWMASK_PROJECTILE) : 0,
					removed ? removed->local_projectile : 0);
			}
			CL_EZCSQC_Ent_Remove(ezcsqc_networkedents[entnum]);
			ezcsqc_networkedents[entnum] = NULL;
			continue;
		}

		if (entnum >= MAX_EDICTS) {
			Host_Error("CL_EZCSQC_ParseEntities: bad entity %d", entnum);
		}

		// Create a local slot on first sight of a server CSQC entity number.
		ent = ezcsqc_networkedents[entnum];
		if (!ent || ent->isfree) {
			ent = CL_EZCSQC_Ent_Spawn();
			ent->entnum = entnum;
			ezcsqc_networkedents[entnum] = ent;
			is_new = true;
		}

		CL_EZCSQC_Ent_Update(ent, is_new);
	}
}

qbool CL_EZCSQC_Event_Sound(int entnum, int channel, int soundnumber, float vol, float attenuation, vec3_t pos, float pitchmod, float flags)
{
	weppredsound_t *snd;

	(void)vol;
	(void)attenuation;
	(void)pos;
	(void)pitchmod;
	(void)flags;

	if (!ezcsqc.weapon_prediction || cl_nopred_weapon.integer || entnum != cl.playernum + 1 || !cl_predict_weaponsound.integer) {
		return false;
	}

	// Return true to tell normal sound parsing that prediction already played it.
	for (snd = predictionsoundlist; snd; snd = snd->next) {
		if (snd->chan == channel && snd->index == soundnumber && !(cl_predict_weaponsound.integer & snd->mask)) {
			return true;
		}
	}

	return false;
}

static void CL_EZCSQC_DebugViewWeaponSkip(const char *reason)
{
	static const char *last_reason;
	static qbool last_attack_down;
	static double next_print_time;
	qbool attack_down;
	double now;

	if (cl_ezcsqc_debug.integer <= 1) {
		return;
	}

	// Use the newest queued command to report whether the user is actively firing.
	attack_down = (cls.netchan.outgoing_sequence > 0) &&
		(cl.frames[(cls.netchan.outgoing_sequence - 1) & UPDATE_MASK].cmd.buttons & BUTTON_ATTACK);
	now = Sys_DoubleTime();

	/*
	 * At debug level 2, persistent skip reasons should explain why prediction
	 * is unavailable without printing forever while the player is idle. When
	 * +attack is down, repeat the line once per second so firing-delay tests
	 * still show the active gate.
	 */
	if (cl_ezcsqc_debug.integer == 2 && !attack_down && reason == last_reason && !last_attack_down) {
		return;
	}
	if (reason == last_reason && attack_down == last_attack_down && now < next_print_time) {
		return;
	}

	last_reason = reason;
	last_attack_down = attack_down;
	next_print_time = now + 1.0;

	Com_Printf("EZCSQC viewweapon prediction skipped: %s attack=%d active=%d weapon_prediction=%d pmove_nopred_weapon=%d cl_nopred_weapon=%d predflags=%d viewweapon=%d isfree=%d spectator=%d demoplayback=%d intermission=%d\n",
		reason,
		attack_down,
		CL_EZCSQC_Active(),
		ezcsqc.weapon_prediction,
		pmove_nopred_weapon,
		cl_nopred_weapon.integer,
		pmove.client_predflags,
		viewweapon ? viewweapon->entnum : -1,
		viewweapon ? viewweapon->isfree : -1,
		cl.spectator,
		cls.demoplayback,
		cl.intermission);
}

qbool CL_EZCSQC_UpdateViewWeapon(int *modelindex, int *weaponframe)
{
	if (!ezcsqc.weapon_prediction) {
		CL_EZCSQC_DebugViewWeaponSkip("no weapon prediction entity");
		return false;
	}
	if (cl_nopred_weapon.integer) {
		CL_EZCSQC_DebugViewWeaponSkip("cl_nopred_weapon");
		return false;
	}
	if (!viewweapon) {
		CL_EZCSQC_DebugViewWeaponSkip("missing viewweapon");
		return false;
	}
	if (viewweapon->isfree) {
		CL_EZCSQC_DebugViewWeaponSkip("viewweapon is free");
		return false;
	}
	if (cl.spectator) {
		CL_EZCSQC_DebugViewWeaponSkip("spectator");
		return false;
	}
	if (cls.demoplayback) {
		CL_EZCSQC_DebugViewWeaponSkip("demo playback");
		return false;
	}
	if (cl.intermission) {
		CL_EZCSQC_DebugViewWeaponSkip("intermission");
		return false;
	}

	// cl_view.c asks for the predicted model/frame just before drawing the gun.
	if (viewweapon->predraw) {
		viewweapon->predraw(viewweapon);
	}

	// Missing model usually means weapon info arrived before the matching weapondef.
	if (!viewweapon->modelindex) {
		if (cl_ezcsqc_debug.integer > 1) {
			static double next_no_model_print_time;
			double now = Sys_DoubleTime();
			weppreddef_t *wep = &wpredict_definitions[bound(0, ws_predicted.weapon_index, MAX_PREDWEPS - 1)];
			if (now >= next_no_model_print_time) {
				next_no_model_print_time = now + 1.0;
				Com_Printf("EZCSQC no predicted model: weapon_index=%d def_model=%d def_anims=%d def_attack=%d frame=%d client_time=%.3f attack_finished=%.3f\n",
					ws_predicted.weapon_index, wep->modelindex, wep->anim_number, wep->attack_time,
					ws_predicted.frame, ws_predicted.client_time, ws_predicted.attack_finished);
			}
		}
		CL_EZCSQC_DebugViewWeaponSkip("no predicted model");
		return false;
	}

	*modelindex = viewweapon->modelindex;
	*weaponframe = viewweapon->frame;
	return true;
}

static void CL_EZCSQC_RenderEntity(ezcsqc_entity_t *self)
{
	entity_t ent;
	entity_state_t state;
	customlight_t cst_lt = { 0 };
	centity_t *cent;
	qbool is_projectile = self->drawmask & DRAWMASK_PROJECTILE;
	int visual_effects = 0;

	if (!self->modelindex || self->modelindex >= MAX_MODELS || !cl.model_precache[self->modelindex]) {
		return;
	}

	if (is_projectile) {
		visual_effects = CL_EZCSQC_ProjectileVisualEffects(self);
		// Suppressed owner projectiles still seed trails so the first visible segment is clean.
		if (CL_EZCSQC_ProjectileTooCloseToDraw(self) && !(self->handoff_smoothing && self->trail_started)) {
			if (self->local_projectile && !self->debug_close_logged) {
				CL_EZCSQC_DebugLocalProjectile(self, "hidden-close", self->origin);
				self->debug_close_logged = true;
			}
			else if (!self->local_projectile && CL_EZCSQC_OwnerProjectile(self) && cl_ezcsqc_debug.integer > 2 && !self->debug_close_logged) {
				vec3_t diff;
				float camdist;
				VectorSubtract(self->origin, r_refdef.vieworg, diff);
				camdist = VectorLength(diff);
				Com_Printf("EZCSQC server projectile hidden-close ent=%d model=%d age=%.3f draw=%d parts=%d cam=%.1f org=(%.1f %.1f %.1f)\n",
					self->entnum, self->modelindex, cl.time - self->s_time,
					self->drawcount, self->partcount, camdist,
					self->origin[0], self->origin[1], self->origin[2]);
				self->debug_close_logged = true;
			}
			CL_EZCSQC_SeedProjectileTrail(self, self->origin);
			return;
		}
	}

	// Build a transient renderer entity from the EZCSQC state.
	memset(&ent, 0, sizeof(ent));
	memset(&state, 0, sizeof(state));
	ent.colormap = vid.colormap;
	VectorCopy(self->origin, ent.origin);
	VectorCopy(self->angles, ent.angles);
	ent.model = cl.model_precache[self->modelindex];
	ent.frame = self->frame;

	state.number = self->entnum;
	state.modelindex = self->modelindex;
	state.effects = visual_effects ? visual_effects : self->effects;
	state.frame = self->frame;
	VectorCopy(self->origin, state.origin);
	VectorCopy(self->angles, state.angles);

	/*
	 * Build the same transient entity/state pair CL_LinkPacketEntities() would
	 * pass to CL_AddParticleTrail(). Keeping this shape lets user trail cvars
	 * and model hints work the same way for EZCSQC and packet entities.
	 */
	cent = is_projectile ? CL_EZCSQC_ProjectileCentity(self, &state) : &self->cent;
	if (!is_projectile) {
		cent->current = state;
		VectorCopy(self->origin, cent->current.origin);
		VectorCopy(self->origin, cent->lerp_origin);
	}

	if (is_projectile && visual_effects && cl.time >= self->parttime) {
		// Throttle particle emission to the normal projectile trail cadence.
		self->parttime = cl.time + 0.013f;
		if (CL_EZCSQC_OwnerProjectile(self) && !self->trail_started) {
			CL_EZCSQC_SeedProjectileTrail(self, self->origin);
			self->trail_started = true;
		}
		if (!VectorLength(self->partorg)) {
			VectorCopy(self->origin, self->partorg);
		}

		self->partcount++;
		self->trail_seeded = true;
		self->trail_started = true;
		if (self->local_projectile && self->partcount == 1) {
			CL_EZCSQC_DebugLocalProjectile(self, "first-trail", self->origin);
		}
		if (self->local_projectile) {
			/*
			 * Local predicted projectiles use an embedded centity_t and do not
			 * have a stable cl_entities[] index for QMB to follow. Keep the
			 * legacy fake-projectile stop-point update here, but leave
			 * server-authored projectiles to the normal packet-entity trail
			 * path below.
			 */
			VectorCopy(self->partorg, cent->old_origin);
			VectorCopy(self->partorg, cent->trails[0].stop);
			VectorCopy(self->partorg, cent->trails[1].stop);
			VectorCopy(self->partorg, cent->trails[2].stop);
			VectorCopy(self->partorg, cent->trails[3].stop);
			VectorCopy(self->origin, cent->current.origin);
			VectorCopy(self->origin, cent->lerp_origin);
		}
		VectorCopy(self->origin, self->partorg);

		/*
		 * Let CL_AddParticleTrail() pick the actual trail implementation from
		 * model hints, effects, and cvars. In particular, non-plasma nail trails
		 * become QMB dynamic trails when that particle mode is enabled.
		 */
		if (visual_effects & 256) {
			// Optional plasma nails use the custom fireball trail instead of normal nail trail.
			if (amf_nailtrail.integer && !gl_no24bit.integer && amf_nailtrail_plasma.integer) {
				byte color[3];
				color[0] = 0;
				color[1] = 70;
				color[2] = 255;
				FireballTrail(cent, color, 0.6, 0.3);
			}
			else {
				CL_AddParticleTrail(&ent, cent, &cst_lt, &state);
			}
		}
		else {
			CL_AddParticleTrail(&ent, cent, &cst_lt, &state);
		}
	}

	/*
	 * Match packet entity ordering for cl_r2g: emit rocket trails/lights using
	 * the rocket model/effects first, then swap only the displayed model.
	 */
	if (is_projectile && (visual_effects & EF_ROCKET) && cl_rocket2grenade.integer && cl_modelindices[mi_grenade] != -1) {
		ent.model = cl.model_precache[cl_modelindices[mi_grenade]];
	}

	if (is_projectile && CL_EZCSQC_OwnerProjectile(self) && self->drawcount == 0 && cl_ezcsqc_debug.integer > 2) {
		if (self->local_projectile) {
			CL_EZCSQC_DebugLocalProjectile(self, "first-model-draw", self->origin);
		}
		else {
			vec3_t diff, base;
			float travel, camdist;

			if (VectorLength(self->trail_origin)) {
				VectorCopy(self->trail_origin, base);
			}
			else {
				VectorCopy(self->s_origin, base);
			}
			VectorSubtract(self->origin, base, diff);
			travel = VectorLength(diff);
			VectorSubtract(self->origin, r_refdef.vieworg, diff);
			camdist = VectorLength(diff);
			Com_Printf("EZCSQC server projectile first-model-draw ent=%d model=%d age=%.3f parts=%d travel=%.1f cam=%.1f org=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f)\n",
				self->entnum, self->modelindex, cl.time - self->s_time,
				self->partcount, travel, camdist,
				self->origin[0], self->origin[1], self->origin[2],
				self->s_velocity[0], self->s_velocity[1], self->s_velocity[2]);
		}
	}
	if (is_projectile) {
		// Save the last rendered model point for future local-to-server handoff smoothing.
		self->drawcount++;
		VectorCopy(self->origin, self->last_draw_origin);
		self->has_last_draw_origin = true;
	}

	// Submit the final model after trails/lights have been emitted.
	CL_AddEntity(&ent);
}

static void CL_EZCSQC_RenderEntities_Ring(int drawmask, int index)
{
	int i = index;

	/*
	 * Start at a moving point in the entity array so projectile trail particle
	 * emission is spread more evenly across entities when many are active.
	 */
	do {
		ezcsqc_entity_t *ent = &ezcsqc_entities[i];
		if (!ent->isfree && (ent->drawmask & drawmask)) {
			if (!ent->predraw || ent->predraw(ent)) {
				CL_EZCSQC_RenderEntity(ent);
			}
		}
		i = (i + 1) & (MAX_EDICTS - 1);
	} while (i != index);
}

void CL_EZCSQC_PrepareParticleFrame(void)
{
	int i;

	if (!ezcsqc.active) {
		return;
	}

	/*
	 * QMB advances existing dynamic trail particles before the view render path
	 * adds this frame's entities. Native packet entities already have their
	 * centity_t sequence/origin refreshed during packet parsing, but EZCSQC
	 * projectiles are extrapolated locally. Refresh only the follow state here
	 * so an existing p_nailtrail does not disconnect before UpdateView renders.
	 */
	for (i = 0; i < MAX_EDICTS; i++) {
		ezcsqc_entity_t *ent = &ezcsqc_entities[i];
		entity_state_t state;
		vec3_t origin;

		if (ent->isfree || ent->local_projectile || !(ent->drawmask & DRAWMASK_PROJECTILE)) {
			continue;
		}
		if (!ent->modelindex || ent->modelindex >= MAX_MODELS || !cl.model_precache[ent->modelindex]) {
			continue;
		}

		/*
		 * Do the same extrapolation used for rendering, but do not call predraw
		 * here. This pass must not remove entities, bounce grenades, or emit new
		 * particles; it only keeps the QMB follow target current.
		 */
		VectorMA(ent->s_origin, cl.servertime - ent->s_time, ent->s_velocity, origin);
		if (CL_EZCSQC_ProjectileOriginTooCloseToDraw(ent, origin)) {
			CL_EZCSQC_SeedProjectileTrail(ent, origin);
			continue;
		}

		memset(&state, 0, sizeof(state));
		state.number = ent->entnum;
		state.modelindex = ent->modelindex;
		state.effects = CL_EZCSQC_ProjectileVisualEffects(ent);
		state.frame = ent->frame;
		VectorCopy(origin, state.origin);
		VectorCopy(ent->angles, state.angles);

		CL_EZCSQC_ProjectileCentity(ent, &state);
	}
}

void CL_EZCSQC_UpdateView(void)
{
	if (!ezcsqc.active && !ezcsqc.weapon_prediction) {
		if (cl_ezcsqc_debug.integer > 1) {
			Com_DPrintf("EZCSQC UpdateView skipped: inactive\n");
		}
		return;
	}

	CL_EZCSQC_RenderEntities_Ring(DRAWMASK_PROJECTILE, projectile_ringbufferseek = (projectile_ringbufferseek + 7) & (MAX_EDICTS - 1));
}

#endif
