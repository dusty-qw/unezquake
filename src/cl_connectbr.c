/*
Copyright (C) 2024 unezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

cl_connectbr.c - User-facing route selection commands.

Route discovery, measurements and graph calculation belong exclusively to
sb_findroutes (EX_browser_pathfind.c). connectbr selects the best cached
route and connectnext advances through the cached alternatives without
probing the worldwide proxy list a second time.
*/

#include "quakedef.h"
#include "EX_browser.h"
#include "cl_connectbr.h"

extern cvar_t cl_proxyaddr;

cvar_t cl_connectbr_verbose = {"cl_connectbr_verbose", "1", CVAR_ARCHIVE};
cvar_t cl_connectbr_debug   = {"cl_connectbr_debug",   "0", CVAR_ARCHIVE};

#define CONNECTBR_DISPLAY_ROUTES 5
#define MAX_ADDRESS_LENGTH 128

static sb_route_t br_routes[SB_ROUTE_MAX_ALTERNATIVES];
static int br_route_count;
static int br_current_route;
static netadr_t br_target_addr;
static qbool br_active;
static qbool br_pending;

static const char *CL_BR_RouteColor(int cost_ms, int best_cost_ms)
{
	if (best_cost_ms <= 0 || cost_ms <= best_cost_ms)
		return "&c0f0";
	if ((long long)cost_ms * 100 <= (long long)best_cost_ms * 115)
		return "&caf0";
	if ((long long)cost_ms * 100 <= (long long)best_cost_ms * 135)
		return "&cff0";
	return "&cf00";
}

static void CL_BR_ApplyRoute(int index)
{
	sb_route_t *route;

	if (index < 0 || index >= br_route_count)
		return;

	route = &br_routes[index];
	Cvar_Set(&cl_proxyaddr, route->proxylist);

	if (route->proxylist[0]) {
		Com_Printf("\n&cf80connectbr:&r route #%d - %d hop%s, estimated cost %dms\n",
		           index + 1, route->hops, route->hops == 1 ? "" : "s",
		           route->total_cost_ms);
		Com_Printf("  cl_proxyaddr %s\n", route->proxylist);
	}
	else {
		Com_Printf("\n&cf80connectbr:&r route #%d - direct, estimated cost %dms\n",
		           index + 1, route->total_cost_ms);
	}

	if (index + 1 < br_route_count)
		Com_Printf("  type &cf80connectnext&r to try route #%d\n", index + 2);
	else
		Com_Printf("  no more routes available.\n");

	Cbuf_AddText(va("connect %s\n", NET_AdrToString(br_target_addr)));
}

static void CL_BR_BuildRankingAndConnect(void)
{
	int i;
	int show;

	br_pending = false;
	br_route_count = SB_PingTree_GetRoutes(&br_target_addr, br_routes,
	                                       SB_ROUTE_MAX_ALTERNATIVES);
	br_current_route = 0;
	br_active = br_route_count > 0;

	if (!br_route_count) {
		Com_Printf("connectbr: sb_findroutes found no route to %s.\n",
		           NET_AdrToString(br_target_addr));
		return;
	}

	show = min(br_route_count, CONNECTBR_DISPLAY_ROUTES);
	Com_Printf("\n&cf80connectbr: top %d routes from sb_findroutes&r\n", show);
	for (i = 0; i < show; i++) {
		if (br_routes[i].proxylist[0])
			Com_Printf("  %s#%d&r  %dms  %d hop%s  %s\n", CL_BR_RouteColor(br_routes[i].total_cost_ms, br_routes[0].total_cost_ms), i + 1,
			           br_routes[i].total_cost_ms, br_routes[i].hops,
			           br_routes[i].hops == 1 ? "" : "s", br_routes[i].proxylist);
		else
			Com_Printf("  %s#%d&r  %dms  direct\n", CL_BR_RouteColor(br_routes[i].total_cost_ms, br_routes[0].total_cost_ms), i + 1,
			           br_routes[i].total_cost_ms);
	}

	if (cl_connectbr_debug.integer)
		Com_Printf("[connectbr] %d cached alternative(s); no additional proxy scan\n",
		           br_route_count);

	CL_BR_ApplyRoute(0);
}

void CL_Connect_BestRoute_f(void)
{
	const char *address;

	if (Cmd_Argc() != 2) {
		Com_Printf("Usage: connectbr <address>\n");
		Com_Printf("  Uses the routes already measured by sb_findroutes.\n");
		Com_Printf("  Use connectnext to try the next cached route.\n");
		return;
	}

	address = Cmd_Argv(1);
	if (!address || !address[0]) {
		Com_Printf("connectbr: empty address\n");
		return;
	}
	if (strlen(address) > MAX_ADDRESS_LENGTH) {
		Com_Printf("connectbr: address too long\n");
		return;
	}
	if (!NET_StringToAdr(address, &br_target_addr)) {
		Com_Printf("connectbr: invalid address '%s'\n", address);
		return;
	}
	if (!br_target_addr.port)
		br_target_addr.port = htons(27500);

	br_active = false;
	br_route_count = 0;
	br_current_route = 0;
	Cvar_Set(&cl_proxyaddr, "");

	if (SB_PingTree_IsBuilding()) {
		br_pending = true;
		Com_Printf("connectbr: waiting for sb_findroutes to finish...\n");
		return;
	}
	if (!SB_PingTree_Built()) {
		br_pending = true;
		Com_Printf("connectbr: building sb_findroutes graph...\n");
		SB_PingTree_Build();
		return;
	}

	CL_BR_BuildRankingAndConnect();
}

void CL_Connect_Next_f(void)
{
	if (!br_active || !br_route_count) {
		Com_Printf("connectnext: no cached connectbr ranking.\n");
		Com_Printf("  Use 'connectbr <address>' first.\n");
		return;
	}

	if (br_current_route + 1 >= br_route_count) {
		Com_Printf("connectnext: no more routes (tried all %d).\n", br_route_count);
		br_active = false;
		return;
	}

	br_current_route++;
	Host_EndGame();
	CL_BR_ApplyRoute(br_current_route);
}

void CL_ConnectBR_Frame(void)
{
	if (br_pending && !SB_PingTree_IsBuilding() && SB_PingTree_Built())
		CL_BR_BuildRankingAndConnect();
}

void CL_ConnectBR_Init(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_NETWORK);
	Cvar_Register(&cl_connectbr_verbose);
	Cvar_Register(&cl_connectbr_debug);
	Cvar_ResetCurrentGroup();
}
