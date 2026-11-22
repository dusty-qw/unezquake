
#include "gl_model.h"
#include "r_part_script.h"

#define	MAX_BEAMS 32
typedef struct
{
	int entity;
	model_t *model;
	float endtime;
	vec3_t start, end;
	// Scripted effects along the beam and where it hits, and their states.
	pscript_effect_t effect, impact_effect;
	trailkey_t trail, impact;
	int trailtype, impacttype; // the types trail and impact were last used for
	int trailframe;            // pscript_hostframe stamp of the trail
} beam_t;

#define MAX_EXPLOSIONS 32
typedef struct explosion_s
{
	struct explosion_s *prev, *next;
	vec3_t origin;
	float start;
	model_t *model;
} explosion_t;

// Temp entity effects without a network message; each runs the same
// scripted effect, particles, light and sound the message would.

// Position-only types and TE_LIGHTNINGBLOOD; false for an unknown type.
qbool CL_TempEntityEffect(int type, vec3_t pos);
// TE_GUNSHOT
void CL_TE_Gunshot(vec3_t pos, int count);
// TE_BLOOD
void CL_TE_Blood(vec3_t pos, int count);
// TE_EXPLOSION2 tinted with a palette range
void CL_TE_Explosion2(vec3_t pos, int color_start, int color_length);
// TEDP_BLOOD, TEDP_SPARK, TEDP_FLAMEJET, TEDP_SMOKE, TEDP_TEI_PLASMAHIT; false when nothing was drawn
qbool CL_TE_DirectedEffect(int type, vec3_t pos, vec3_t vel, int count);
// TEDP_BLOODSHOWER, TEDP_PARTICLECUBE, TEDP_PARTICLERAIN, TEDP_PARTICLESNOW; false when nothing was drawn
qbool CL_TE_ParticleBox(int type, vec3_t mins, vec3_t maxs, vec3_t dir, int count, int color, qbool gravity);
// TEDP_EXPLOSIONRGB
void CL_TE_ExplosionRGB(vec3_t pos, const byte *color);
// TEDP_CUSTOMFLASH; nothing for a lifetime that is not positive
void CL_TE_CustomFlash(vec3_t pos, float radius, float lifetime, const byte *color);
// TEQW_RAILTRAIL
void CL_TE_RailTrail(vec3_t start, vec3_t end);
// TEDP_TEI_G3
void CL_TE_TeiG3(vec3_t start, vec3_t end);
// TE_LIGHTNING1-3 and TEQW_BEAM as beam types 1-4
void CL_CreateBeam(int type, int ent, vec3_t start, vec3_t end);
void CL_ClearBeam(int ent);

#define MAX_PREDEXPLOSIONS 16
typedef struct
{
	float time;
	vec3_t origin;
	float radius;
	int damage;
	qbool self_damage;
} predexplosion_t;
void CL_CheckPredictedExplosions(player_state_t *from, player_state_t *to);
void CL_PredictRocketExplosion(vec3_t te_origin, vec3_t kick_origin, double prediction_time);

//r_part_trails.c
extern void R_MissileTrail(centity_t *cent, int trail_num);
extern int fix_trail_num_for_grens(int trail_num);
