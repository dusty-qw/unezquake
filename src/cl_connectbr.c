/*
Copyright (C) 2024 unezQuake team

cl_connectbr.c - Smart route selection for connectbr/connectnext commands

Multi-hop logic:
  Batch-queries all proxy candidates simultaneously (one socket each).
  Collects all pingstatus replies in a single select() loop -- total wait
  is ONE timeout window regardless of how many proxies are tested.

  1-hop:  you -> P1 -> dest
  2-hop:  you -> P1 -> P2 -> dest
          P2 candidates are proxies (port 30000) found in P1's pingstatus.
          A second batch round queries all P2 candidates.

CVars:
  cl_connectbr_timeout_ms     - pingstatus reply timeout in ms (default 600)
  cl_connectbr_ping_green     - green ping threshold (default 40)
  cl_connectbr_ping_yellow    - yellow ping threshold (default 80)
  cl_connectbr_ping_orange    - orange/max playable threshold (default 200)
  cl_connectbr_weight_ping    - score weight for ping (default 1.0)
  cl_connectbr_weight_loss    - score weight for loss (default 0.4)
  cl_connectbr_test_packets   - number of pingstatus packets to send for loss (default 10)
  cl_connectbr_packet_delay   - delay between packets in ms (default 15)
  cl_connectbr_verbose        - 0=quiet 1=normal 2=debug (default 1)
*/

#include "quakedef.h"
#include "EX_browser.h"
#include "cl_connectbr.h"

// cl_proxyaddr is declared in cl_main.c
extern cvar_t cl_proxyaddr;

// --------------------------------------------
// CVars
// --------------------------------------------
cvar_t cl_connectbr_timeout_ms    = {"cl_connectbr_timeout_ms",    "600",  CVAR_ARCHIVE};
cvar_t cl_connectbr_ping_green    = {"cl_connectbr_ping_green",    "40",   CVAR_ARCHIVE};
cvar_t cl_connectbr_ping_yellow   = {"cl_connectbr_ping_yellow",   "80",   CVAR_ARCHIVE};
cvar_t cl_connectbr_ping_orange   = {"cl_connectbr_ping_orange",   "200",  CVAR_ARCHIVE};
cvar_t cl_connectbr_weight_ping   = {"cl_connectbr_weight_ping",   "1.0",  CVAR_ARCHIVE};
cvar_t cl_connectbr_weight_loss   = {"cl_connectbr_weight_loss",   "0.4",  CVAR_ARCHIVE};
cvar_t cl_connectbr_test_packets  = {"cl_connectbr_test_packets",  "10",   CVAR_ARCHIVE};
cvar_t cl_connectbr_packet_delay  = {"cl_connectbr_packet_delay",  "15",   CVAR_ARCHIVE};
cvar_t cl_connectbr_verbose       = {"cl_connectbr_verbose",       "1",    CVAR_ARCHIVE};
cvar_t cl_connectbr_debug         = {"cl_connectbr_debug",         "0",    CVAR_ARCHIVE};

// --------------------------------------------
// Constants
// --------------------------------------------
#define CONNECTBR_MAX_PROXIES     128  /* 91 proxies qwfwd ativos globalmente (Mar/2026) */
#define CONNECTBR_MAX_NEIGHBORS   64
/* max rotas = 64 proxies (1-hop) + combinacoes 2-hop + 1 direta.
 * 256 cobre o pior caso pratico com folga (route_t ~600 bytes, total ~150KB). */
#define CONNECTBR_MAX_ROUTES      256
#define MAX_PROXYLIST             256
#define MAX_ADDRESS_LENGTH        128

#define BR_Debug(...) \
	do { if (cl_connectbr_debug.value) Com_Printf("[connectbr] " __VA_ARGS__); } while(0)

// --------------------------------------------
// Types
// --------------------------------------------
typedef struct {
	char    proxylist[MAX_PROXYLIST];
	char    label[128];
	float   ping_ms;
	float   loss_pct;
	float   score;
	qbool   via_proxy;
	qbool   valid;
} route_t;

typedef struct {
	netadr_t addr;
	int      dist_ms;
} ps_entry_t;

typedef struct {
	ps_entry_t entries[CONNECTBR_MAX_NEIGHBORS];
	int        count;
	int        dest_ping;   /* proxy's ping to dest, -1 if not in reply */
} ps_reply_t;

typedef struct {
	netadr_t  addr;
	char      ip_str[64];
	char      name[128];
	int       you_ms;       /* you -> this proxy (from browser or measured) */
	float     loss_pct;     /* packet loss to destination (for pingstatus) */
	socket_t  sock;
	qbool     replied;      /* received at least one pingstatus reply? */
	ps_reply_t ps;
} proxy_t;

// --------------------------------------------
// State
// --------------------------------------------
static route_t   br_routes[CONNECTBR_MAX_ROUTES];
static int       br_route_count   = 0;
static int       br_current_route = 0;
static netadr_t  br_target_addr;
static qbool     br_active        = false;
static qbool     br_measuring     = false;
static qbool     br_done          = false;
static char      br_ranking_buf[4096];

// --------------------------------------------
// Colour helpers
// --------------------------------------------
static const char *CL_BR_PingColor(float ms)
{
	int g = (int)cl_connectbr_ping_green.value;
	int y = (int)cl_connectbr_ping_yellow.value;
	int o = (int)cl_connectbr_ping_orange.value;
	if (g <= 0) g = 40;
	if (y <= 0) y = 80;
	if (o <= 0) o = 200;
	if (ms <= g) return "&c0f0";
	if (ms <= y) return "&cff0";
	if (ms <= o) return "&cfa0";
	return "&cf00";
}

static const char *CL_BR_LossColor(float pct)
{
	if (pct <= 0) return "&c0f0";
	if (pct <= 5) return "&cff0";
	return "&cf00";
}

// --------------------------------------------
// Helpers
// --------------------------------------------
static void CL_BR_AdrToStr(const netadr_t *a, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "%d.%d.%d.%d:%d",
	         a->ip[0], a->ip[1], a->ip[2], a->ip[3], (int)ntohs(a->port));
}

static qbool CL_BR_IsTarget(const netadr_t *a)
{
	return (memcmp(a->ip, br_target_addr.ip, 4) == 0 &&
	        a->port == br_target_addr.port);
}

static qbool CL_BR_AdrEqual(const netadr_t *a, const netadr_t *b)
{
	return (memcmp(a->ip, b->ip, 4) == 0 && a->port == b->port);
}

static int CL_BR_GetBrowserPing(const netadr_t *dest)
{
	int i, best = -1;
	SB_ServerList_Lock();
	for (i = 0; i < serversn; i++) {
		if (NET_CompareAdr(servers[i]->address, *dest) && servers[i]->ping >= 0) {
			best = servers[i]->ping;
			break;
		}
	}
	SB_ServerList_Unlock();
	return best;
}

// --------------------------------------------
// Parse a raw pingstatus reply buffer.
// --------------------------------------------
static void CL_BR_ParsePingstatus(const byte *buf, int len,
                                   const netadr_t *dest, ps_reply_t *ps)
{
	const byte *p   = buf;
	const byte *end = buf + len;

	ps->count     = 0;
	ps->dest_ping = -1;

	while (p + 8 <= end) {
		unsigned short port_net;
		short dist_raw;
		int   dist_ms;

		memcpy(&port_net, p + 4, 2);
		memcpy(&dist_raw, p + 6, 2);
		dist_ms = (int)(short)LittleShort(dist_raw);

		if (dist_ms >= 0) {
			if (dest &&
			    memcmp(p, dest->ip, 4) == 0 &&
			    memcmp(p + 4, &dest->port, 2) == 0) {
				ps->dest_ping = dist_ms;
			}
			if (ps->count < CONNECTBR_MAX_NEIGHBORS) {
				ps_entry_t *e = &ps->entries[ps->count++];
				e->addr.type = NA_IP;
				memcpy(e->addr.ip, p, 4);
				e->addr.port = port_net;
				e->dist_ms   = dist_ms;
			}
		}
		p += 8;
	}
}

// --------------------------------------------
// Batch pingstatus -- sends to all proxies simultaneously,
// measures you->proxy RTT from the first reply timing,
// counts multiple replies to compute loss.
// Total blocking time = timeout_ms, regardless of proxy count.
// --------------------------------------------
static void CL_BR_BatchPingstatus(proxy_t *proxies, int count,
                                   const netadr_t *dest, int timeout_ms)
{
	const char *packet      = "\xff\xff\xff\xffpingstatus";
	int         packet_count = (int)cl_connectbr_test_packets.value;
	double     *send_times;
	int        *recv_count;
	int         i, pkt;
	double      deadline;
	struct timeval tv;

	if (packet_count < 1)  packet_count = 1;
	if (packet_count > 20) packet_count = 20;
	if (timeout_ms <= 0)   timeout_ms   = 600;

	send_times = (double *)Q_malloc(count * sizeof(double));
	recv_count = (int    *)Q_malloc(count * sizeof(int));
	if (!send_times || !recv_count) {
		Q_free(send_times);
		Q_free(recv_count);
		return;
	}
	memset(recv_count, 0, count * sizeof(int));

	/* open one socket per proxy, record send time, send all packets */
	for (i = 0; i < count; i++) {
		struct sockaddr_storage addr_to;
		proxies[i].replied      = false;
		proxies[i].loss_pct     = 100.0f;
		proxies[i].ps.count     = 0;
		proxies[i].ps.dest_ping = -1;
		send_times[i]           = 0;

		proxies[i].sock = UDP_OpenSocket(PORT_ANY);
		if (proxies[i].sock == INVALID_SOCKET) continue;

		NetadrToSockadr(&proxies[i].addr, &addr_to);
		send_times[i] = Sys_DoubleTime();
		for (pkt = 0; pkt < packet_count; pkt++) {
			sendto(proxies[i].sock, packet, strlen(packet), 0,
			       (struct sockaddr *)&addr_to, sizeof(struct sockaddr));
		}
	}

	deadline = Sys_DoubleTime() + timeout_ms / 1000.0;

	while (Sys_DoubleTime() < deadline) {
		fd_set fd;
		int    maxsock = 0;
		qbool  any     = false;

		FD_ZERO(&fd);
		for (i = 0; i < count; i++) {
			/* keep listening until we've received all expected replies or timed out */
			if (recv_count[i] < packet_count && proxies[i].sock != INVALID_SOCKET) {
				FD_SET(proxies[i].sock, &fd);
				if ((int)proxies[i].sock > maxsock)
					maxsock = (int)proxies[i].sock;
				any = true;
			}
		}
		if (!any) break;

		tv.tv_sec = 0; tv.tv_usec = 20000;
		if (select(maxsock + 1, &fd, NULL, NULL, &tv) <= 0) continue;

		for (i = 0; i < count; i++) {
			if (recv_count[i] < packet_count
			    && proxies[i].sock != INVALID_SOCKET
			    && FD_ISSET(proxies[i].sock, &fd)) {
				byte buf[8 * 512];
				struct sockaddr_storage from;
				socklen_t from_len = sizeof(from);
				double    now      = Sys_DoubleTime();
				int ret = recvfrom(proxies[i].sock, (char *)buf, sizeof(buf), 0,
				                   (struct sockaddr *)&from, &from_len);
				if (ret > 5 && memcmp(buf, "\xff\xff\xff\xffn", 5) == 0) {
					recv_count[i]++;
					if (!proxies[i].replied) {
						/* first reply: parse pingstatus data */
						CL_BR_ParsePingstatus(buf + 5, ret - 5, dest, &proxies[i].ps);
						proxies[i].replied = true;
						/* measure you->proxy RTT from pingstatus round-trip
						   (use this only if browser didn't already have a ping) */
						if (proxies[i].you_ms < 0 && send_times[i] > 0) {
							int ms = (int)((now - send_times[i]) * 1000.0);
							proxies[i].you_ms = (ms < 1) ? 1 : ms;
						}
					}
				}
			}
		}
	}

	/* compute packet loss for each proxy */
	for (i = 0; i < count; i++) {
		if (proxies[i].replied && recv_count[i] > 0) {
			float loss = 100.0f * (1.0f - (float)recv_count[i] / (float)packet_count);
			proxies[i].loss_pct = (loss < 0) ? 0 : loss;
		}
	}

	Q_free(send_times);
	Q_free(recv_count);

	for (i = 0; i < count; i++) {
		if (proxies[i].sock != INVALID_SOCKET) {
			closesocket(proxies[i].sock);
			proxies[i].sock = INVALID_SOCKET;
		}
	}
}

// Forward declaration used by the route-building helpers below.
static void CL_BR_AddRoute(const char *proxylist, const char *label,
                            float ping_ms, float loss_pct, qbool via_proxy);

// --------------------------------------------
// Score -- lower is better (includes loss).
// --------------------------------------------
static float CL_BR_Score(float ping_ms, float loss_pct)
{
	float o      = cl_connectbr_ping_orange.value > 0 ? cl_connectbr_ping_orange.value : 200.0f;
	float w_ping = cl_connectbr_weight_ping.value  > 0 ? cl_connectbr_weight_ping.value  : 1.0f;
	float w_loss = cl_connectbr_weight_loss.value  > 0 ? cl_connectbr_weight_loss.value  : 0.4f;
	float norm_ping = ping_ms / (o * 2.0f);
	if (norm_ping > 1.0f) norm_ping = 1.0f;
	float norm_loss = loss_pct / 100.0f;
	if (norm_loss > 1.0f) norm_loss = 1.0f;
	return w_ping * norm_ping + w_loss * norm_loss;
}

static int CL_BR_RouteCompare(const void *a, const void *b)
{
	const route_t *ra = (const route_t *)a;
	const route_t *rb = (const route_t *)b;
	if (ra->score < rb->score) return -1;
	if (ra->score > rb->score) return  1;
	return 0;
}

// --------------------------------------------
// Add a route (dedup by proxylist string).
// Rejects if proxylist would be truncated.
// --------------------------------------------
static void CL_BR_AddRoute(const char *proxylist, const char *label,
                            float ping_ms, float loss_pct, qbool via_proxy)
{
	int i;

	if (br_route_count >= CONNECTBR_MAX_ROUTES) return;

	if (strlen(proxylist) >= MAX_PROXYLIST) {
		BR_Debug("route '%s' skipped: proxylist too long (%d chars)\n",
		         label, (int)strlen(proxylist));
		return;
	}

	for (i = 0; i < br_route_count; i++)
		if (strcmp(br_routes[i].proxylist, proxylist) == 0) return;

	strlcpy(br_routes[br_route_count].proxylist, proxylist, MAX_PROXYLIST);
	strlcpy(br_routes[br_route_count].label,     label,     sizeof(br_routes[0].label));
	br_routes[br_route_count].ping_ms   = ping_ms;
	br_routes[br_route_count].loss_pct  = loss_pct;
	br_routes[br_route_count].score     = CL_BR_Score(ping_ms, loss_pct);
	br_routes[br_route_count].via_proxy = via_proxy;
	br_routes[br_route_count].valid     = true;
	br_route_count++;
}

// --------------------------------------------
// Apply route and connect
// --------------------------------------------
static void CL_BR_ApplyRoute(int idx)
{
	route_t *r = &br_routes[idx];

	Cvar_Set(&cl_proxyaddr, "");
	if (r->proxylist[0])
		Cvar_Set(&cl_proxyaddr, r->proxylist);

	Com_Printf("\n&cf80connectbr:&r route #%d - %s\n", idx + 1, r->label);
	Com_Printf("  ping: %s%.0fms&r  loss: %s%.1f%%&r\n",
	           CL_BR_PingColor(r->ping_ms), r->ping_ms,
	           CL_BR_LossColor(r->loss_pct), r->loss_pct);

	if (idx + 1 < br_route_count)
		Com_Printf("  type &cf80connectnext&r for route #%d (%s)\n",
		           idx + 2, br_routes[idx + 1].label);
	else
		Com_Printf("  no more routes available.\n");

	Cbuf_AddText(va("connect %s\n", NET_AdrToString(br_target_addr)));
}

// --------------------------------------------
// Thread proc -- todo o trabalho pesado roda aqui, fora da thread principal.
// A thread principal dispara isso e retorna imediatamente (sem freeze).
// --------------------------------------------
static int CL_BR_MeasureProc(void *ignored)
{
	int i, j;
	int timeout_ms = (int)cl_connectbr_timeout_ms.value;
	int verbose    = (int)cl_connectbr_verbose.value;

	proxy_t *p1       = NULL;
	int      p1_count = 0;
	proxy_t *p2       = NULL;
	int      p2_count = 0;

	if (timeout_ms <= 0) timeout_ms = 600;

	// ══════════════════════════════════════════════════════════════════
	// Step 1: ping tree (Dijkstra, se ja construido)
	// ══════════════════════════════════════════════════════════════════
	if (SB_PingTree_Built() && !SB_PingTree_IsBuilding()) {
		int pathlen = SB_PingTree_GetPathLen(&br_target_addr);
		if (pathlen > 0) {
			int  total_ping_ms = 0;
			char proxy_str[MAX_PROXYLIST];
			char target_str[64];
			proxy_str[0] = '\0';
			CL_BR_AdrToStr(&br_target_addr, target_str, sizeof(target_str));
			if (SB_PingTree_GetProxyString(&br_target_addr, proxy_str,
			                               sizeof(proxy_str), &total_ping_ms)
			    && proxy_str[0] && strcmp(proxy_str, target_str) != 0) {
				float ping = (total_ping_ms > 0) ? (float)total_ping_ms : 1.0f;
				char  lbl[128];
				snprintf(lbl, sizeof(lbl), "ping tree (%d hop%s)",
				         pathlen, pathlen > 1 ? "s" : "");
				CL_BR_AddRoute(proxy_str, lbl, ping, 0.0f, true);
			}
		}
	}

	// ══════════════════════════════════════════════════════════════════
	// Step 2: coletar proxies P1 do server browser
	// ══════════════════════════════════════════════════════════════════
	p1 = (proxy_t *)Q_malloc(CONNECTBR_MAX_PROXIES * sizeof(proxy_t));
	if (!p1) {
		Com_Printf("connectbr: failed to allocate proxy list.\n");
		goto step_direct;
	}
	memset(p1, 0, CONNECTBR_MAX_PROXIES * sizeof(proxy_t));

	SB_ServerList_Lock();
	for (i = 0; i < serversn && p1_count < CONNECTBR_MAX_PROXIES; i++) {
		server_data *s = servers[i];
		int k; qbool dup = false;
		if (!s || ntohs(s->address.port) != 30000) continue;
		if (CL_BR_IsTarget(&s->address)) continue;
		for (k = 0; k < p1_count && !dup; k++)
			if (CL_BR_AdrEqual(&p1[k].addr, &s->address)) dup = true;
		if (dup) continue;
		p1[p1_count].addr   = s->address;
		p1[p1_count].you_ms = (s->ping >= 0) ? s->ping : -1;
		p1[p1_count].sock   = INVALID_SOCKET;
		CL_BR_AdrToStr(&s->address, p1[p1_count].ip_str, sizeof(p1[0].ip_str));
		strlcpy(p1[p1_count].name,
		        s->display.name[0] ? s->display.name : p1[p1_count].ip_str,
		        sizeof(p1[0].name));
		p1_count++;
	}
	SB_ServerList_Unlock();

	if (p1_count == 0) {
		if (verbose >= 1)
			Com_Printf("  No proxies found in server browser.\n");
		goto step_direct;
	}

	if (verbose >= 1)
		Com_Printf("  Testing %d prox%s...\n", p1_count, p1_count == 1 ? "y" : "ies");

	// ══════════════════════════════════════════════════════════════════
	// Step 3: pingstatus em batch para todos os P1s
	// ══════════════════════════════════════════════════════════════════
	CL_BR_BatchPingstatus(p1, p1_count, &br_target_addr, timeout_ms);

	// ══════════════════════════════════════════════════════════════════
	// Step 4: coletar candidatos P2 das respostas P1, query em batch
	// ══════════════════════════════════════════════════════════════════
	p2 = (proxy_t *)Q_malloc(CONNECTBR_MAX_PROXIES * sizeof(proxy_t));
	if (!p2) goto build_routes;
	memset(p2, 0, CONNECTBR_MAX_PROXIES * sizeof(proxy_t));

	for (i = 0; i < p1_count; i++) {
		if (!p1[i].replied) continue;
		for (j = 0; j < p1[i].ps.count; j++) {
			ps_entry_t *nb = &p1[i].ps.entries[j];
			int k; qbool dup = false;

			if (p2_count >= CONNECTBR_MAX_PROXIES) break;

			if (ntohs(nb->addr.port) != 30000)           continue;
			if (CL_BR_IsTarget(&nb->addr))               continue;
			if (CL_BR_AdrEqual(&nb->addr, &p1[i].addr))  continue;
			if (nb->addr.ip[0] == 0 && nb->addr.ip[1] == 0 &&
			    nb->addr.ip[2] == 0 && nb->addr.ip[3] == 0) continue;

			for (k = 0; k < p2_count && !dup; k++)
				if (CL_BR_AdrEqual(&p2[k].addr, &nb->addr)) dup = true;
			if (dup) continue;

			p2[p2_count].addr   = nb->addr;
			p2[p2_count].you_ms = -1;
			p2[p2_count].sock   = INVALID_SOCKET;
			CL_BR_AdrToStr(&nb->addr, p2[p2_count].ip_str, sizeof(p2[0].ip_str));
			strlcpy(p2[p2_count].name, p2[p2_count].ip_str, sizeof(p2[0].name));
			p2_count++;
		}
	}

	if (p2_count > 0) {
		BR_Debug("  %d P2 candidate(s) for 2-hop\n", p2_count);
		CL_BR_BatchPingstatus(p2, p2_count, &br_target_addr, timeout_ms);
	}

	// ══════════════════════════════════════════════════════════════════
	// Step 5: construir rotas
	// ══════════════════════════════════════════════════════════════════
build_routes:
	{
		int direct_ping = CL_BR_GetBrowserPing(&br_target_addr);
		if (direct_ping <= 0) direct_ping = -1;

		for (i = 0; i < p1_count; i++) {
			char lbl[128];
			int  p1_dest_ping;

			if (!p1[i].replied) continue;

			p1_dest_ping = (p1[i].ps.dest_ping >= 0)
			               ? p1[i].ps.dest_ping : direct_ping;

			/* 1-hop: you -> P1 -> dest */
			if (p1_dest_ping >= 0) {
				int   total     = p1[i].you_ms + p1_dest_ping;
				qbool estimated = (p1[i].ps.dest_ping < 0);
				snprintf(lbl, sizeof(lbl), "via %s [1 hop%s]",
				         p1[i].name, estimated ? " ~est" : "");
				CL_BR_AddRoute(p1[i].ip_str, lbl, (float)total, p1[i].loss_pct, true);
				if (verbose >= 1)
					Com_Printf("  [%s] %s%dms&r loss=%.0f%% (you %dms + proxy->dest %dms%s)\n",
					           p1[i].name, CL_BR_PingColor((float)total), total,
					           p1[i].loss_pct, p1[i].you_ms, p1_dest_ping,
					           estimated ? " ~est" : "");
			}

			/* 2-hop: so usa P1 como intermediario se voce->P1 < ping direto.
			 * Se nao temos ping direto (direct_ping==-1), nao filtra -- tenta tudo. */
			if (!p2) continue;
			if (direct_ping > 0 && p1[i].you_ms >= direct_ping) continue;

			for (j = 0; j < p1[i].ps.count; j++) {
				ps_entry_t *nb = &p1[i].ps.entries[j];
				int k, total2, p2_dest_ping;
				qbool estimated2;
				char proxylist[MAX_PROXYLIST];

				if (ntohs(nb->addr.port) != 30000)           continue;
				if (CL_BR_IsTarget(&nb->addr))               continue;
				if (CL_BR_AdrEqual(&nb->addr, &p1[i].addr))  continue;

				for (k = 0; k < p2_count; k++)
					if (CL_BR_AdrEqual(&p2[k].addr, &nb->addr)) break;
				if (k >= p2_count || !p2[k].replied) continue;

				p2_dest_ping = (p2[k].ps.dest_ping >= 0)
				               ? p2[k].ps.dest_ping : direct_ping;
				if (p2_dest_ping < 0) continue;

				estimated2 = (p2[k].ps.dest_ping < 0);
				total2 = p1[i].you_ms + nb->dist_ms + p2_dest_ping;

				snprintf(proxylist, sizeof(proxylist), "%s@%s",
				         p1[i].ip_str, p2[k].ip_str);
				snprintf(lbl, sizeof(lbl), "via %s -> %s [2 hops%s]",
				         p1[i].name, p2[k].ip_str, estimated2 ? " ~est" : "");
				CL_BR_AddRoute(proxylist, lbl, (float)total2, p2[k].loss_pct, true);

				if (verbose >= 1)
					Com_Printf("  [%s -> %s] %s%dms&r loss=%.0f%%  "
					           "(you %dms + P1->P2 %dms + P2->dest %dms%s) &cf802 hops&r\n",
					           p1[i].name, p2[k].ip_str,
					           CL_BR_PingColor((float)total2), total2,
					           p2[k].loss_pct,
					           p1[i].you_ms, nb->dist_ms, p2_dest_ping,
					           estimated2 ? " ~est" : "");
			}
		}
	}

	// ══════════════════════════════════════════════════════════════════
	// Step 6: conexao direta
	// ══════════════════════════════════════════════════════════════════
step_direct:
	{
		int dp = CL_BR_GetBrowserPing(&br_target_addr);
		if (dp > 0) {
			if (verbose >= 1)
				Com_Printf("  [direct] %s%dms&r\n", CL_BR_PingColor((float)dp), dp);
			CL_BR_AddRoute("", "direct", (float)dp, 0.0f, false);
		}
	}

	Q_free(p1);
	Q_free(p2);
	br_measuring = false;

	if (br_route_count == 0) {
		Com_Printf("connectbr: no routes found.\n");
		return 0;
	}

	qsort(br_routes, br_route_count, sizeof(route_t), CL_BR_RouteCompare);

	if (br_route_count > 10)
		br_route_count = 10;

	/* Monta ranking no buffer -- sera impresso pela thread principal em CL_ConnectBR_Frame */
	{
		int  show = (br_route_count < 6) ? br_route_count : 6;
		char tmp[256];
		br_ranking_buf[0] = '\0';
		snprintf(tmp, sizeof(tmp), "\n&cf80connectbr: top %d routes&r\n", show);
		strlcat(br_ranking_buf, tmp, sizeof(br_ranking_buf));
		for (i = 0; i < show; i++) {
			snprintf(tmp, sizeof(tmp), "  &cf80#%d&r  %s%.0fms&r  %s%.1f%%&r  %s\n",
			         i + 1,
			         CL_BR_PingColor(br_routes[i].ping_ms), br_routes[i].ping_ms,
			         CL_BR_LossColor(br_routes[i].loss_pct), br_routes[i].loss_pct,
			         br_routes[i].label);
			strlcat(br_ranking_buf, tmp, sizeof(br_ranking_buf));
		}
	}

	br_current_route = 0;
	br_active        = true;
	br_done          = true;   /* sinaliza thread principal para imprimir e conectar */
	return 0;
}

// --------------------------------------------
// PUBLIC: connectbr <address>
// Valida argumentos, seta estado e dispara thread.
// Retorna imediatamente -- sem freeze.
// --------------------------------------------
void CL_Connect_BestRoute_f(void)
{
	const char *addr;

	if (br_measuring) {
		Com_Printf("connectbr: measurement already in progress.\n");
		return;
	}

	if (Cmd_Argc() != 2) {
		Com_Printf("Usage: connectbr <address>\n");
		Com_Printf("  Tests 1-hop and 2-hop proxy routes and connects via the best one.\n");
		Com_Printf("  Use connectnext to try the next route.\n");
		return;
	}

	addr = Cmd_Argv(1);
	if (!addr || !*addr)                    { Com_Printf("connectbr: empty address\n");    return; }
	if (strlen(addr) > MAX_ADDRESS_LENGTH)  { Com_Printf("connectbr: address too long\n"); return; }

	if (!NET_StringToAdr(addr, &br_target_addr)) {
		Com_Printf("connectbr: invalid address '%s'\n", addr);
		return;
	}
	if (br_target_addr.port == 0)
		br_target_addr.port = htons(27500);

	if (SB_PingTree_IsBuilding()) {
		Com_Printf("connectbr: ping tree still building -- retry later.\n");
		return;
	}

	br_measuring     = true;
	br_route_count   = 0;
	br_current_route = 0;
	br_active        = false;
	br_done          = false;
	br_ranking_buf[0] = '\0';
	Cvar_Set(&cl_proxyaddr, "");

	if ((int)cl_connectbr_verbose.value >= 1)
		Com_Printf("\n&cf80connectbr:&r testing routes to %s\n", addr);

	if (Sys_CreateDetachedThread(CL_BR_MeasureProc, NULL) < 0) {
		Com_Printf("connectbr: failed to create measurement thread.\n");
		br_measuring = false;
	}
}

// --------------------------------------------
// PUBLIC: connectnext
// --------------------------------------------
void CL_Connect_Next_f(void)
{
	if (!br_active || br_route_count == 0) {
		Com_Printf("connectnext: no active connectbr session.\n");
		Com_Printf("  Use 'connectbr <address>' first.\n");
		return;
	}

	br_current_route++;

	if (br_current_route >= br_route_count) {
		Com_Printf("connectnext: no more routes (tried all %d).\n", br_route_count);
		br_active = false;
		return;
	}

	Host_EndGame();
	CL_BR_ApplyRoute(br_current_route);
}

// --------------------------------------------
// PUBLIC: CL_ConnectBR_Frame
// Chamada pela thread principal a cada frame.
// Verifica se a thread de medicao terminou e,
// se sim, imprime o ranking e conecta -- tudo
// na thread principal, sem race condition.
// --------------------------------------------
void CL_ConnectBR_Frame(void)
{
	if (!br_done) return;
	br_done = false;

	if (br_route_count == 0) return;

	/* Imprime ranking aqui -- thread principal, Com_Printf seguro */
	if (br_ranking_buf[0])
		Com_Printf("%s", br_ranking_buf);

	CL_BR_ApplyRoute(0);
}

// --------------------------------------------
// PUBLIC: register cvars
// --------------------------------------------
void CL_ConnectBR_Init(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_NETWORK);
	Cvar_Register(&cl_connectbr_timeout_ms);
	Cvar_Register(&cl_connectbr_ping_green);
	Cvar_Register(&cl_connectbr_ping_yellow);
	Cvar_Register(&cl_connectbr_ping_orange);
	Cvar_Register(&cl_connectbr_weight_ping);
	Cvar_Register(&cl_connectbr_weight_loss);
	Cvar_Register(&cl_connectbr_test_packets);
	Cvar_Register(&cl_connectbr_packet_delay);
	Cvar_Register(&cl_connectbr_verbose);
	Cvar_Register(&cl_connectbr_debug);
	Cvar_ResetCurrentGroup();
}
