#include "quakedef.h"

#include <time.h>

#include "teamplay.h"
#include "utils.h"

#include "discord_register.h"
#include "discord_rpc.h"

#define DISCORD_APPLICATION_ID "1529499621968052466"

// Rich Presence art asset key for the app logo, uploaded under
// Discord Developer Portal -> Rich Presence -> Art Assets.
#define DISCORD_LOGO_IMAGE_KEY "unezquake"

// How often we recheck whether the presence needs updating; Discord itself
// throttles actual presence pushes to about once every 15 seconds.
#define DISCORD_UPDATE_INTERVAL_MS 1000

cvar_t cl_discord_rpc = {"cl_discord_rpc", "1"};
cvar_t cl_discord_errors = {"cl_discord_errors", "0"};

typedef struct presence_s {
	char state[128];
	char details[128];
	int64_t startTimestamp;
	int64_t endTimestamp;
	char largeImageKey[32];
	char largeImageText[128];
	char smallImageKey[32];
	char smallImageText[128];
	char partyId[128];
	int partySize;
	int partyMax;
	int partyPrivacy;
	char joinSecret[128];
} presence_t;

typedef struct {
	qbool initialized;
	qbool was_active;
	qbool warned_not_connected;
	unsigned int next_update;
	int64_t match_start;
	int64_t init_time;
	presence_t old_presence;
} cl_presence_state_t;

static cl_presence_state_t cl_presence_state;

/*
 * CL_DiscordDisconnected
 */
static void CL_DiscordDisconnected(int errorCode, const char *message)
{
	Com_Printf("Discord RPC disconnected (%d): %s\n", errorCode, message);
}

/*
 * CL_DiscordErrored
 */
static void CL_DiscordErrored(int errorCode, const char *message)
{
	if (cl_discord_errors.integer) {
		Com_Printf("Discord RPC error %d: %s\n", errorCode, message);
	}
}

/*
 * CL_DiscordReady
 */
static void CL_DiscordReady(const DiscordUser *user)
{
	Com_Printf("Discord RPC connected as %s#%s\n", user->username, user->discriminator);
	cl_presence_state.initialized = true;
}

/*
 * CL_DiscordJoinGame
 */
static void CL_DiscordJoinGame(const char *secret)
{
	Cbuf_AddText(va("connect %s\n", secret));
}

/*
 * CL_PresenceStatusKey
 *
 * Short, space-free asset name matching an image uploaded to the Discord
 * application's Rich Presence art assets.
 */
static const char *CL_PresenceStatusKey(void)
{
	if (cls.demoplayback) {
		return cls.mvdplayback == 2 ? "qtv" : cls.mvdplayback == 1 ? "mvd" : "demo";
	}
	if (cl.spectator) {
		return "spectating";
	}
	if (cl.standby || cl.countdown) {
		return "warmup";
	}
	if (cl.paused) {
		return "paused";
	}
	if (cl.intermission) {
		return "gameover";
	}
	return "playing";
}

/*
 * CL_PresenceStatusText
 *
 * Human readable counterpart of CL_PresenceStatusKey(), shown as the small
 * image's tooltip.
 */
static const char *CL_PresenceStatusText(void)
{
	if (cls.demoplayback) {
		return cls.mvdplayback == 2 ? "Watching QTV" : cls.mvdplayback == 1 ? "Watching an MVD" : "Watching a demo";
	}
	if (cl.spectator) {
		return "Spectating";
	}
	if (cl.standby || cl.countdown) {
		return "Warmup";
	}
	if (cl.paused) {
		return "Paused";
	}
	return "Playing";
}

/*
 * CL_PresenceModeText
 */
static const char *CL_PresenceModeText(void)
{
	extern char *Macro_MatchType(void);
	const char *matchtype = Macro_MatchType();

	if (!strncmp(matchtype, "tfduel", 6)) {
		return "TF Duel";
	}
	if (cl.teamfortress) {
		return "Team Fortress";
	}
	if (!strncmp(matchtype, "duel", 4)) {
		return "Duel";
	}
	if (cl.teamplay) {
		return "Teamplay";
	}
	if (cl.gametype == GAME_COOP) {
		return "Coop";
	}
	if (cl.deathmatch) {
		return "Deathmatch";
	}
	return "Free For All";
}

/*
 * CL_BuildPresence
 */
static void CL_BuildPresence(presence_t *presence)
{
	memset(presence, 0, sizeof(*presence));

	if (cls.state == ca_active) {
		char hostname[64] = {0};
		char mapname[64] = {0};
		char mapshot[32] = {0};
		const char *status;
		int maxclients = Q_atoi(Info_ValueForKey(cl.serverinfo, "maxclients"));

		strlcpy(hostname, Info_ValueForKey(cl.serverinfo, "hostname"), sizeof(hostname));
		if (!hostname[0]) {
			strlcpy(hostname, cls.servername, sizeof(hostname));
		}
		RemoveColors(hostname, sizeof(hostname));

		strlcpy(mapname, Info_ValueForKey(cl.serverinfo, "map"), sizeof(mapname));
		strlcpy(mapshot, mapname, sizeof(mapshot));
		Q_strlwr(mapshot);

		strlcpy(presence->largeImageKey, mapshot[0] ? mapshot : DISCORD_LOGO_IMAGE_KEY, sizeof(presence->largeImageKey));
		strlcpy(presence->largeImageText, hostname[0] ? hostname : "Unknown server", sizeof(presence->largeImageText));
		strlcpy(presence->smallImageKey, CL_PresenceStatusKey(), sizeof(presence->smallImageKey));
		strlcpy(presence->smallImageText, CL_PresenceStatusText(), sizeof(presence->smallImageText));

		strlcpy(presence->state, mapname[0] ? mapname : "Unknown map", sizeof(presence->state));

		status = CL_PresenceStatusText();
		if (!strcmp(status, "Playing")) {
			snprintf(presence->details, sizeof(presence->details), "%s (%d/%d)", CL_PresenceModeText(), TP_CountPlayers(), maxclients);
		} else {
			snprintf(presence->details, sizeof(presence->details), "%s - %s (%d/%d)", status, CL_PresenceModeText(), TP_CountPlayers(), maxclients);
		}

		presence->partySize = TP_CountPlayers();
		presence->partyMax = maxclients;

		if (cls.server_adr.type != NA_LOOPBACK) {
			strlcpy(presence->partyId, hostname[0] ? hostname : "server", sizeof(presence->partyId));
			presence->partyPrivacy = DISCORD_PARTY_PUBLIC;

			if (!cls.demoplayback && maxclients > 0) {
				strlcpy(presence->joinSecret, NET_AdrToString(cls.server_adr), sizeof(presence->joinSecret));
			}
		} else {
			strlcpy(presence->partyId, "local", sizeof(presence->partyId));
			presence->partyPrivacy = DISCORD_PARTY_PRIVATE;
		}

		presence->startTimestamp = cl_presence_state.match_start;
	} else if (cls.state != ca_disconnected) {
		strlcpy(presence->largeImageKey, DISCORD_LOGO_IMAGE_KEY, sizeof(presence->largeImageKey));
		strlcpy(presence->state, "Connecting to a server", sizeof(presence->state));
		strlcpy(presence->details, "Connecting", sizeof(presence->details));
	} else {
		strlcpy(presence->largeImageKey, DISCORD_LOGO_IMAGE_KEY, sizeof(presence->largeImageKey));
		strlcpy(presence->state, "In the menus", sizeof(presence->state));
		strlcpy(presence->details, "Main Menu", sizeof(presence->details));
	}
}

/*
 * CL_UpdatePresence
 */
void CL_UpdatePresence(void)
{
	unsigned int now;
	presence_t presence;

	if (!cl_discord_rpc.integer) {
		return;
	}

	if (!cl_presence_state.initialized) {
		if (!cl_presence_state.warned_not_connected && cl_presence_state.init_time &&
			(int64_t)time(NULL) - cl_presence_state.init_time > 15) {
			Com_Printf("Discord: still not connected after 15 seconds - is the Discord desktop client running?\n");
			cl_presence_state.warned_not_connected = true;
		}
		Discord_RunCallbacks();
		return;
	}

	now = (unsigned int)(Sys_DoubleTime() * 1000.0);
	if (cl_presence_state.next_update > now) {
		Discord_RunCallbacks();
		return;
	}
	cl_presence_state.next_update = now + DISCORD_UPDATE_INTERVAL_MS;

	if (cls.state == ca_active && !cl_presence_state.was_active) {
		cl_presence_state.match_start = (int64_t)time(NULL);
	}
	cl_presence_state.was_active = (cls.state == ca_active);

	CL_BuildPresence(&presence);

	if (memcmp(&presence, &cl_presence_state.old_presence, sizeof(presence)) != 0) {
		DiscordRichPresence discord;

		memset(&discord, 0, sizeof(discord));
		discord.state = presence.state;
		discord.details = presence.details;
		discord.startTimestamp = presence.startTimestamp;
		discord.endTimestamp = presence.endTimestamp;
		discord.largeImageKey = presence.largeImageKey[0] ? presence.largeImageKey : NULL;
		discord.largeImageText = presence.largeImageText[0] ? presence.largeImageText : NULL;
		discord.smallImageKey = presence.smallImageKey;
		discord.smallImageText = presence.smallImageText;
		discord.partyId = presence.partyId[0] ? presence.partyId : NULL;
		discord.partySize = presence.partySize;
		discord.partyMax = presence.partyMax;
		discord.partyPrivacy = presence.partyPrivacy;
		discord.joinSecret = presence.joinSecret[0] ? presence.joinSecret : NULL;
		discord.instance = 1;

		Discord_UpdatePresence(&discord);
		cl_presence_state.old_presence = presence;
	}

	Discord_RunCallbacks();
}

/*
 * CL_InitDiscord
 */
void CL_InitDiscord(void)
{
	DiscordEventHandlers handlers;

	Cvar_SetCurrentGroup(CVAR_GROUP_DISCORD);
	Cvar_Register(&cl_discord_rpc);
	Cvar_Register(&cl_discord_errors);

	if (!cl_discord_rpc.integer) {
		return;
	}

	memset(&handlers, 0, sizeof(handlers));
	handlers.ready = CL_DiscordReady;
	handlers.disconnected = CL_DiscordDisconnected;
	handlers.errored = CL_DiscordErrored;
	handlers.joinGame = CL_DiscordJoinGame;

	Com_Printf("Discord: connecting to app id %s...\n", DISCORD_APPLICATION_ID);
	cl_presence_state.init_time = (int64_t)time(NULL);
	Discord_Initialize(DISCORD_APPLICATION_ID, &handlers, 1, NULL);
}

/*
 * CL_ShutdownDiscord
 */
void CL_ShutdownDiscord(void)
{
	if (cl_presence_state.initialized) {
		Discord_ClearPresence();
		Discord_Shutdown();
	}
	memset(&cl_presence_state, 0, sizeof(cl_presence_state));
}
