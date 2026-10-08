/*
Copyright (C) 2026 unezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

// Main menu background scene.
//
// While disconnected, the world model of menu_background_map is loaded
// client-side only (no server involved) the same way the client does it when
// connecting: CM_LoadMap + Mod_ForName + R_NewMap. Doors, platforms and pickup
// items are added as static entities so the map doesn't look empty, and the
// camera tours the deathmatch spawn points with a slow pan and fades between
// shots. Connecting anywhere clears the client state, which frees the scene
// along with everything else on the hunk.

#include "quakedef.h"
#include "gl_model.h"
#include "cmodel.h"
#include "vfs.h"
#include "menu.h"
#include "menu_scene.h"

#define SCENE_MAX_SHOTS       64
#define SCENE_SHOT_TIME       10.0
#define SCENE_FADE_TIME       0.9
#define SCENE_PAN_DEGREES     36.0f
#define SCENE_DOLLY_DISTANCE  64.0f
#define SCENE_EYE_HEIGHT      22.0f
#define SCENE_FOV_43          85.0f

static void OnChange_menu_background_map(cvar_t *var, char *string, qbool *cancel);

cvar_t menu_background     = {"menu_background", "1"};
// space separated list of maps, one of those found is picked at random
cvar_t menu_background_map = {"menu_background_map", "aerowalk ztndm3 dm4 dm6 dm2 dm3 schloss bravado", 0, OnChange_menu_background_map};

typedef struct scene_shot_s {
	vec3_t origin;
	vec3_t angles;
	float dolly;           // how far forward the camera may travel during the shot
} scene_shot_t;

static struct {
	qbool loaded;
	qbool failed;          // don't retry every frame if no map could be loaded
	model_t *world;
	char map_name[MAX_QPATH];
	scene_shot_t shots[SCENE_MAX_SHOTS];
	int shot_count;
	double start_time;
} scene;

typedef struct scene_item_model_s {
	const char *classname;
	const char *model;     // used when spawnflags & 1 is not set (or for everything if big_model is NULL)
	const char *big_model; // ammo boxes: spawnflags & 1, health: spawnflags & 2 (megahealth)
	int skin;
} scene_item_model_t;

static const scene_item_model_t scene_item_models[] = {
	{ "item_armor1",                    "progs/armor.mdl",    NULL, 0 },
	{ "item_armor2",                    "progs/armor.mdl",    NULL, 1 },
	{ "item_armorInv",                  "progs/armor.mdl",    NULL, 2 },
	{ "weapon_supershotgun",            "progs/g_shot.mdl",   NULL, 0 },
	{ "weapon_nailgun",                 "progs/g_nail.mdl",   NULL, 0 },
	{ "weapon_supernailgun",            "progs/g_nail2.mdl",  NULL, 0 },
	{ "weapon_grenadelauncher",         "progs/g_rock.mdl",   NULL, 0 },
	{ "weapon_rocketlauncher",          "progs/g_rock2.mdl",  NULL, 0 },
	{ "weapon_lightning",               "progs/g_light.mdl",  NULL, 0 },
	{ "item_artifact_super_damage",     "progs/quaddama.mdl", NULL, 0 },
	{ "item_artifact_invulnerability",  "progs/invulner.mdl", NULL, 0 },
	{ "item_artifact_invisibility",     "progs/invisibl.mdl", NULL, 0 },
	{ "item_artifact_envirosuit",       "progs/suit.mdl",     NULL, 0 },
	{ "item_health",                    "maps/b_bh25.bsp",    "maps/b_bh100.bsp", 0 },
	{ "item_shells",                    "maps/b_shell0.bsp",  "maps/b_shell1.bsp", 0 },
	{ "item_spikes",                    "maps/b_nail0.bsp",   "maps/b_nail1.bsp", 0 },
	{ "item_rockets",                   "maps/b_rock0.bsp",   "maps/b_rock1.bsp", 0 },
	{ "item_cells",                     "maps/b_batt0.bsp",   "maps/b_batt1.bsp", 0 },
};

typedef struct scene_entity_s {
	char classname[64];
	char model[64];
	vec3_t origin;
	vec3_t angles;
	qbool has_mangle;
	int spawnflags;
} scene_entity_t;

static void OnChange_menu_background_map(cvar_t *var, char *string, qbool *cancel)
{
	// let the next frame try again with the new list
	scene.failed = false;
}

static qbool MenuScene_FileExists(const char *path)
{
	vfsfile_t *f = FS_OpenVFS(path, "rb", FS_ANY);

	if (f) {
		VFS_CLOSE(f);
		return true;
	}
	return false;
}

// Picks one of the maps in menu_background_map at random, among those that exist.
static qbool MenuScene_FindMap(char *path, size_t path_size)
{
	char list[512];
	char candidate[MAX_QPATH];
	char *name, *next;
	int found = 0;

	strlcpy(list, menu_background_map.string, sizeof(list));
	for (name = list; name && *name; name = next) {
		while (*name == ' ') {
			++name;
		}
		next = strchr(name, ' ');
		if (next) {
			*next++ = '\0';
		}
		if (!*name || FS_UnsafeFilename(name)) {
			continue;
		}

		snprintf(candidate, sizeof(candidate), "maps/%s.bsp", name);
		// reservoir sampling: every existing map has the same chance
		if (MenuScene_FileExists(candidate) && ((unsigned int)rand() ^ (unsigned int)(Sys_DoubleTime() * 1000)) % (unsigned int)++found == 0) {
			strlcpy(path, candidate, path_size);
		}
	}

	return found > 0;
}

// Reads the next entity from the map's entity lump, returns NULL when done.
static const char *MenuScene_ParseEntity(const char *data, scene_entity_t *ent)
{
	char key[64];

	memset(ent, 0, sizeof(*ent));

	data = COM_Parse(data);
	if (!data || com_token[0] != '{') {
		return NULL;
	}

	while (true) {
		data = COM_Parse(data);
		if (!data) {
			return NULL;
		}
		if (com_token[0] == '}') {
			return data;
		}
		strlcpy(key, com_token, sizeof(key));

		data = COM_Parse(data);
		if (!data) {
			return NULL;
		}

		if (!strcmp(key, "classname")) {
			strlcpy(ent->classname, com_token, sizeof(ent->classname));
		}
		else if (!strcmp(key, "model")) {
			strlcpy(ent->model, com_token, sizeof(ent->model));
		}
		else if (!strcmp(key, "origin")) {
			sscanf(com_token, "%f %f %f", &ent->origin[0], &ent->origin[1], &ent->origin[2]);
		}
		else if (!strcmp(key, "angle")) {
			ent->angles[YAW] = atof(com_token);
		}
		else if (!strcmp(key, "mangle")) {
			sscanf(com_token, "%f %f %f", &ent->angles[PITCH], &ent->angles[YAW], &ent->angles[ROLL]);
			ent->has_mangle = true;
		}
		else if (!strcmp(key, "spawnflags")) {
			ent->spawnflags = atoi(com_token);
		}
	}
}

// Registers a model the same way the server's model list would, so the renderer
// (which walks cl.model_precache when building its texture arrays) knows about it.
static model_t *MenuScene_PrecacheModel(const char *name)
{
	int i;

	for (i = 1; i < MAX_MODELS && cl.model_name[i][0]; i++) {
		if (!strcmp(cl.model_name[i], name)) {
			return cl.model_precache[i];
		}
	}
	if (i >= MAX_MODELS) {
		return NULL;
	}

	if (name[0] != '*' && !MenuScene_FileExists(name)) {
		return NULL;
	}

	cl.model_precache[i] = Mod_ForName(name, false);
	if (!cl.model_precache[i]) {
		return NULL;
	}
	strlcpy(cl.model_name[i], name, sizeof(cl.model_name[i]));
	return cl.model_precache[i];
}

static void MenuScene_AddStatic(model_t *model, const vec3_t origin, const vec3_t angles, int skin)
{
	entity_t *ent;

	if (!model || cl.num_statics >= MAX_STATIC_ENTITIES) {
		return;
	}

	ent = &cl_static_entities[cl.num_statics++];
	ent->entity_id = cl.num_statics;
	ent->model = model;
	ent->colormap = vid.colormap;
	ent->skinnum = skin;
	VectorCopy(origin, ent->origin);
	VectorCopy(angles, ent->angles);
	R_AddEfrags(ent);
}

static qbool MenuScene_PointIsOpen(const vec3_t point)
{
	mleaf_t *leaf = Mod_PointInLeaf((float *)point, scene.world);

	return leaf && leaf->contents == CONTENTS_EMPTY;
}

// Distance the camera can see in a direction before hitting a wall.
static float MenuScene_ViewDepth(const vec3_t origin, float yaw, float pitch)
{
	vec3_t angles, forward, point;
	float distance;

	VectorSet(angles, pitch, yaw, 0);
	AngleVectors(angles, forward, NULL, NULL);
	for (distance = 16; distance < 2048; distance += 16) {
		VectorMA(origin, distance, forward, point);
		if (!MenuScene_PointIsOpen(point)) {
			break;
		}
	}
	return distance;
}

// Spawn points often face a wall; look for the yaw where the whole pan sees the most of the map.
static float MenuScene_BestYaw(const vec3_t origin, float preferred_yaw, float pitch)
{
	float best_yaw = preferred_yaw, best_score = -1;
	int step;

	for (step = 0; step < 24; step++) {
		float yaw = preferred_yaw + step * 15;
		float left = MenuScene_ViewDepth(origin, yaw - SCENE_PAN_DEGREES * 0.5f, pitch);
		float centre = MenuScene_ViewDepth(origin, yaw, pitch);
		float right = MenuScene_ViewDepth(origin, yaw + SCENE_PAN_DEGREES * 0.5f, pitch);
		// the closest wall during the pan matters most, slightly prefer the map's own direction
		float score = min(min(left, right), centre) * 2 + centre - (step ? 32 : 0);

		if (score > best_score) {
			best_score = score;
			best_yaw = yaw;
		}
	}
	return anglemod(best_yaw);
}

static void MenuScene_AddShot(const vec3_t origin, const vec3_t angles, qbool choose_yaw)
{
	scene_shot_t *shot;
	vec3_t forward, end;

	if (scene.shot_count >= SCENE_MAX_SHOTS || !MenuScene_PointIsOpen(origin)) {
		return;
	}

	shot = &scene.shots[scene.shot_count++];
	VectorCopy(origin, shot->origin);
	VectorCopy(angles, shot->angles);
	if (choose_yaw) {
		shot->angles[YAW] = MenuScene_BestYaw(shot->origin, angles[YAW], angles[PITCH]);
	}

	// only dolly forward if the camera would stay inside the map
	AngleVectors(shot->angles, forward, NULL, NULL);
	VectorMA(shot->origin, SCENE_DOLLY_DISTANCE, forward, end);
	shot->dolly = MenuScene_PointIsOpen(end) ? SCENE_DOLLY_DISTANCE : 0;
}

static void MenuScene_ShuffleShots(void)
{
	int i;

	for (i = scene.shot_count - 1; i > 0; i--) {
		int j = rand() % (i + 1);
		scene_shot_t tmp = scene.shots[i];

		scene.shots[i] = scene.shots[j];
		scene.shots[j] = tmp;
	}
}

static void MenuScene_PopulateFromEntities(void)
{
	const char *data = CM_EntityString();
	scene_entity_t ent;
	int i;

	while (data && (data = MenuScene_ParseEntity(data, &ent))) {
		if (!strcmp(ent.classname, "info_player_deathmatch") || !strcmp(ent.classname, "info_player_start")) {
			vec3_t eye, angles;

			VectorCopy(ent.origin, eye);
			eye[2] += SCENE_EYE_HEIGHT;
			VectorSet(angles, 6, ent.angles[YAW], 0);
			MenuScene_AddShot(eye, angles, true);
		}
		else if (!strcmp(ent.classname, "info_intermission")) {
			// intermission cameras are placed by the mapper, trust them unless they look straight up/down
			qbool steep = fabs(ent.angles[PITCH]) > 35;

			if (steep || !ent.has_mangle) {
				VectorSet(ent.angles, 8, ent.angles[YAW], 0);
			}
			MenuScene_AddShot(ent.origin, ent.angles, steep || !ent.has_mangle);
		}
		else if (ent.model[0] == '*') {
			// doors, plats, func_wall...: drawn where the map places them
			MenuScene_AddStatic(MenuScene_PrecacheModel(ent.model), ent.origin, ent.angles, 0);
		}
		else {
			for (i = 0; i < sizeof(scene_item_models) / sizeof(scene_item_models[0]); i++) {
				const scene_item_model_t *item = &scene_item_models[i];
				const char *model = item->model;

				if (strcmp(ent.classname, item->classname)) {
					continue;
				}
				if (item->big_model) {
					int big_flag = !strcmp(item->classname, "item_health") ? 2 : 1;

					if (ent.spawnflags & big_flag) {
						model = item->big_model;
					}
					else if (!strcmp(item->classname, "item_health") && (ent.spawnflags & 1)) {
						model = "maps/b_bh10.bsp";
					}
				}
				MenuScene_AddStatic(MenuScene_PrecacheModel(model), ent.origin, ent.angles, item->skin);
				break;
			}
		}
	}

	if (scene.shot_count == 0) {
		// no usable spawn points: look across the middle of the map
		vec3_t centre, angles = { 10, 45, 0 };

		VectorAdd(scene.world->mins, scene.world->maxs, centre);
		VectorScale(centre, 0.5f, centre);
		MenuScene_AddShot(centre, angles, true);
	}

	MenuScene_ShuffleShots();
}

static qbool MenuScene_Load(void)
{
	char path[MAX_QPATH];
	unsigned int checksum2;
	int i;

	if (!MenuScene_FindMap(path, sizeof(path))) {
		Com_DPrintf("menu_background: none of '%s' found\n", menu_background_map.string);
		return false;
	}

	// Same steps as connecting to a server and receiving its model list, minus the server.
	CL_ClearState();

	cl.clipmodels[1] = CM_LoadMap(path, true, NULL, &checksum2);
	R_NewMapPreLoad();

	// The map is only flagged as the world model if it matches the mapname a server would have
	// set. Otherwise the renderer sizes the brush model index buffer for MAX_STANDARD_ENTITIES
	// copies of the whole map (hundreds of MB), and as that buffer never shrinks, every map played
	// afterwards renders much slower. The mapname must not stay set while disconnected though.
	COM_StripExtension(COM_SkipPath(path), scene.map_name, sizeof(scene.map_name));
	Cvar_ForceSet(&host_mapname, scene.map_name);
	strlcpy(cl.model_name[1], path, sizeof(cl.model_name[1]));
	cl.model_precache[1] = Mod_ForName(path, false);
	Cvar_ForceSet(&host_mapname, "");
	if (!cl.model_precache[1] || cl.model_precache[1]->type != mod_brush) {
		Com_Printf("menu_background: couldn't load %s\n", path);
		CL_ClearState();
		return false;
	}
	cl.worldmodel = scene.world = cl.model_precache[1];

	// inline models first, so they keep the indexes a server would give them
	for (i = 1; i < scene.world->numsubmodels; i++) {
		MenuScene_PrecacheModel(va("*%d", i));
	}

	scene.shot_count = 0;
	MenuScene_PopulateFromEntities();

	R_NewMap(false);

	scene.start_time = cls.realtime;
	scene.loaded = true;
	Com_DPrintf("menu_background: %s loaded, %d camera positions, %d static entities\n", path, scene.shot_count, cl.num_statics);
	return true;
}

void MenuScene_Invalidate(void)
{
	scene.loaded = false;
	scene.world = NULL;
}

static qbool MenuScene_ShouldLoad(void)
{
	return menu_background.integer && !M_ClassicMenus() && host_initialized && cls.state == ca_disconnected
		&& !cls.demoplayback && !com_serveractive && !scene.failed;
}

void MenuScene_Frame(void)
{
	if (scene.loaded) {
		if (!menu_background.integer || cls.state != ca_disconnected || cl.worldmodel != scene.world) {
			MenuScene_Invalidate();
		}
		return;
	}

	if (MenuScene_ShouldLoad() && !MenuScene_Load()) {
		scene.failed = true;
	}
}

const char *MenuScene_MapName(void)
{
	return MenuScene_Active() ? scene.map_name : "";
}

qbool MenuScene_HasWorld(void)
{
	return scene.world && cl.worldmodel == scene.world;
}

qbool MenuScene_Active(void)
{
	return scene.loaded && cls.state == ca_disconnected && cl.worldmodel && cl.worldmodel == scene.world;
}

static float MenuScene_SmoothStep(float t)
{
	t = bound(0, t, 1);
	return t * t * (3 - 2 * t);
}

static void MenuScene_ShotParameters(int *index, float *progress, double *time_in_shot)
{
	double elapsed = max(0, cls.realtime - scene.start_time);
	int shot = (int)(elapsed / SCENE_SHOT_TIME);

	*time_in_shot = elapsed - shot * SCENE_SHOT_TIME;
	*progress = (float)(*time_in_shot / SCENE_SHOT_TIME);
	*index = shot % max(1, scene.shot_count);
}

qbool MenuScene_SetupView(void)
{
	scene_shot_t *shot;
	vec3_t forward;
	double time_in_shot;
	float progress, eased, fov_y;
	int index;

	if (!MenuScene_Active() || scene.shot_count <= 0) {
		return false;
	}

	MenuScene_ShotParameters(&index, &progress, &time_in_shot);
	shot = &scene.shots[index];
	eased = MenuScene_SmoothStep(progress);

	// slow pan across the spawn's view direction while drifting forward
	VectorCopy(shot->angles, r_refdef.viewangles);
	r_refdef.viewangles[YAW] += (eased - 0.5f) * SCENE_PAN_DEGREES;
	r_refdef.viewangles[PITCH] += sin(cls.realtime * 0.35) * 1.5;
	AngleVectors(shot->angles, forward, NULL, NULL);
	VectorMA(shot->origin, eased * shot->dolly, forward, r_refdef.vieworg);

	r_refdef.vrect.x = r_refdef.vrect.y = 0;
	r_refdef.vrect.width = vid.width;
	r_refdef.vrect.height = vid.height;
	r_refdef.viewheight_test = 4;

	// keep the framing of a 4:3 view with SCENE_FOV_43 at any aspect ratio
	fov_y = 2 * atan(tan(SCENE_FOV_43 * M_PI / 360.0) * 0.75) * 180 / M_PI;
	r_refdef.fov_y = fov_y;
	r_refdef.fov_x = 2 * atan(tan(fov_y * M_PI / 360.0) * ((float)vid.width / max(1, vid.height))) * 180 / M_PI;

	// nothing else uses the client clock while disconnected; animate water, sky and lights with it
	cl.time = cls.realtime;

	return true;
}

void MenuScene_Draw2D(void)
{
	double time_in_shot;
	float progress, alpha;
	int index;

	if (!MenuScene_Active() || scene.shot_count <= 0) {
		return;
	}

	MenuScene_ShotParameters(&index, &progress, &time_in_shot);
	if (time_in_shot < SCENE_FADE_TIME) {
		alpha = 1.0f - (float)(time_in_shot / SCENE_FADE_TIME);
	}
	else if (time_in_shot > SCENE_SHOT_TIME - SCENE_FADE_TIME) {
		alpha = 1.0f - (float)((SCENE_SHOT_TIME - time_in_shot) / SCENE_FADE_TIME);
	}
	else {
		return;
	}

	alpha = MenuScene_SmoothStep(alpha);
	Draw_AlphaFillRGB(0, 0, vid.width, vid.height, RGBA_TO_COLOR(0, 0, 0, (byte)(alpha * 255)));
}

void MenuScene_Init(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_MENU);
	Cvar_Register(&menu_background);
	Cvar_Register(&menu_background_map);
	Cvar_ResetCurrentGroup();
}
