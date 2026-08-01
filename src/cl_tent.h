
#include "gl_model.h"

#define	MAX_BEAMS 32
typedef struct
{
	int entity;
	model_t *model;
	float endtime;
	vec3_t start, end;
} beam_t;

#define MAX_EXPLOSIONS 32
typedef struct explosion_s
{
	struct explosion_s *prev, *next;
	vec3_t origin;
	float start;
	model_t *model;
} explosion_t;

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
