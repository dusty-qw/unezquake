/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2007-2023 ezQuake team
Copyright (C) 2021-2023 Sam "Reki" Piper
Copyright (C) 2026 unezQuake team

Native EZCSQC protocol declarations for KTX weapon and projectile prediction.
Authoritative game state remains server-side.

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

#ifndef EZQUAKE_EZCSQC_HEADER
#define EZQUAKE_EZCSQC_HEADER

#define EZCSQC_WEAPONINFO		1
#define EZCSQC_PROJECTILE		2
#define EZCSQC_PLAYER			3
#define EZCSQC_WEAPONDEF		4
#define EZCSQC_HUDELEMENT		32

#define DRAWMASK_ENGINE			0x01
#define DRAWMASK_VIEWMODEL		0x02
#define DRAWMASK_PROJECTILE		0x04

#define WEAPONINFO_INDEX		(1 << 0)
#define WEAPONINFO_AMMO_SHELLS		(1 << 1)
#define WEAPONINFO_AMMO_NAILS		(1 << 2)
#define WEAPONINFO_AMMO_ROCKETS		(1 << 3)
#define WEAPONINFO_AMMO_CELLS		(1 << 4)
#define WEAPONINFO_ATTACK		(1 << 5)
#define WEAPONINFO_TIMING		(1 << 6)
#define WEAPONINFO_PRED_PING		(1 << 7)

#define PROJECTILE_ORIGIN		(1 << 0)
#define PROJECTILE_MODEL		(1 << 1)
#define PROJECTILE_ANGLES		(1 << 2)
#define PROJECTILE_OWNER		(1 << 3)
#define PROJECTILE_SPAWN_ORIGIN		(1 << 4)

#define WEAPONDEF_INIT			(1 << 0)
#define WEAPONDEF_FLAGS			(1 << 1)
#define WEAPONDEF_ANIM			(1 << 2)

typedef struct {
	qbool	active;
	double	time;

	int	weapon_prediction;
	int	weaponframe;
	int	weaponmodel;
} ezcsqc_world_t;

typedef struct ezcsqc_entity_s {
	qbool	isfree;
	double	freetime;

	int	entnum;
	int	drawmask;
	qbool	(*predraw)(struct ezcsqc_entity_s *self);

	int	frame;
	int	modelindex;
	float	alpha;

	int	flags;
	int	modelflags;
	int	effects;

	vec3_t	origin;
	vec3_t	angles;

	int	ownernum;

	vec3_t	trail_origin;
	vec3_t	oldorigin;
	centity_t cent;
	vec3_t	s_origin;
	vec3_t	s_velocity;
	double	s_time;

	qbool	local_projectile;
	int	projectile_type;
	int	partcount;
	int	drawcount;
	int	debug_predraw_count;
	qbool	trail_seeded;
	qbool	trail_started;
	qbool	debug_close_logged;
	qbool	handoff_smoothing;
	qbool	has_last_draw_origin;
	qbool	predicted_explosion;
	double	debug_next_time;
	double	handoff_starttime;
	double	handoff_endtime;
	double	simtime;
	double	angle_simtime;
	double	prediction_start_state_time;
	double	starttime;
	double	endtime;
	double	parttime;
	vec3_t	start;
	vec3_t	handoff_origin;
	vec3_t	handoff_delta;
	vec3_t	last_draw_origin;
	vec3_t	sim_origin;
	vec3_t	sim_angles;
	vec3_t	vel;
	vec3_t	avel;
	vec3_t	partorg;
} ezcsqc_entity_t;

typedef struct {
	byte	state;
	byte	impulse;
	int	weapon;
	int	items;
	int	frame;
	int	ammo_shells;
	int	ammo_nails;
	int	ammo_rockets;
	int	ammo_cells;
	float	attack_finished;
	float	client_time;
	float	client_nextthink;
	int	client_thinkindex;
	int	client_predflags;
	float	client_ping;
	int	current_ammo;
	int	weapon_index;
} ezcsqc_weapon_state_t;

#define WEPPRED_MAXSTATES		32
#define WEPPREDANIM_SOUND		0x0001
#define WEPPREDANIM_PROJECTILE		0x0002
#define WEPPREDANIM_LGBEAM		0x0004
#define WEPPREDANIM_MUZZLEFLASH		0x0008
#define WEPPREDANIM_DEFAULT		0x0010
#define WEPPREDANIM_ATTACK		0x0020
#define WEPPREDANIM_BRANCH		0x0040
#define WEPPREDANIM_MOREBYTES		0x0080
#define WEPPREDANIM_SOUNDAUTO		(0x0100 | WEPPREDANIM_MOREBYTES)
#define WEPPREDANIM_LTIME		(0x0200 | WEPPREDANIM_MOREBYTES)

typedef struct weppredanim_s {
	signed char	mdlframe;
	unsigned short	flags;
	unsigned short	sound;
	unsigned short	soundmask;
	unsigned short	projectile_model;
	short		projectile_velocity[3];
	signed char	projectile_offset[3];
	byte		nextanim;
	byte		altanim;
	short		length;
} weppredanim_t;

typedef struct weppreddef_s {
	unsigned short	modelindex;
	unsigned short	attack_time;
	byte		impulse;
	int		itemflag;
	byte		anim_number;
	weppredanim_t	anim_states[WEPPRED_MAXSTATES];
} weppreddef_t;

typedef struct weppredsound_s {
	unsigned short index;
	byte chan;
	unsigned short mask;
	struct weppredsound_s *next;
} weppredsound_t;

extern cvar_t cl_pext_ezcsqc;
extern cvar_t cl_ezcsqc_debug;
extern ezcsqc_world_t ezcsqc;
extern ezcsqc_entity_t ezcsqc_entities[MAX_EDICTS];
extern ezcsqc_entity_t *ezcsqc_networkedents[MAX_EDICTS];
extern weppredsound_t *predictionsoundlist;

void CL_EZCSQC_ParseEntities(void);
void CL_EZCSQC_ParseSetup(void);
void CL_EZCSQC_InitializeEntities(void);
qbool CL_EZCSQC_Active(void);
qbool CL_EZCSQC_Event_Sound(int entnum, int channel, int soundnumber, float vol, float attenuation, vec3_t pos, float pitchmod, float flags);
qbool CL_EZCSQC_UpdateViewWeapon(int *modelindex, int *weaponframe);
void CL_EZCSQC_PrepareParticleFrame(void);
void CL_EZCSQC_UpdateView(void);

#endif
