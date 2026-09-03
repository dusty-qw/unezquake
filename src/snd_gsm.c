/*
 * Raw GSM 06.10 audio bridge.
 *
 * The transmit side captures the microphone and connects to an ffplay-style
 * TCP listener.  The receive side accepts an ffmpeg-style TCP sender and mixes
 * decoded speech into the normal sound output.  libgsm is loaded at runtime so
 * builds which do not use this feature need no additional dependency.
 */

#include <SDL.h>

#include "quakedef.h"
#include "qsound.h"
#include "net.h"

#define GSM_SAMPLE_RATE 8000
#define GSM_FRAME_SAMPLES 160
#define GSM_FRAME_BYTES 33
#define GSM_PCM_BYTES (GSM_FRAME_SAMPLES * sizeof(short))
#define GSM_IO_BUFFER 8192
#define GSM_SEND_BUFFER (GSM_FRAME_BYTES * 25)
#define GSM_RECONNECT_SECONDS 2.0
#define GSM_CONNECT_TIMEOUT_SECONDS 5.0
#define GSM_UPDATE_INTERVAL_SECONDS 0.005

#if defined(MSG_NOSIGNAL)
#define GSM_SEND_FLAGS MSG_NOSIGNAL
#else
#define GSM_SEND_FLAGS 0
#endif

typedef void *gsm_handle_t;
typedef gsm_handle_t (*gsm_create_fn)(void);
typedef void (*gsm_destroy_fn)(gsm_handle_t);
typedef int (*gsm_encode_fn)(gsm_handle_t, short *, unsigned char *);
typedef int (*gsm_decode_fn)(gsm_handle_t, unsigned char *, short *);

// Empty send address and port 0 keep both halves of the bridge disabled.
static cvar_t s_gsm_send = { "s_gsm_send", "", CVAR_ARCHIVE };
static cvar_t s_gsm_listen_port = { "s_gsm_listen_port", "0", CVAR_ARCHIVE };
static cvar_t s_gsm_inputdevice = { "s_gsm_inputdevice", "0", CVAR_ARCHIVE };
static cvar_t s_gsm_mic_volume = { "s_gsm_mic_volume", "1", CVAR_ARCHIVE };
static cvar_t s_gsm_receive_volume = { "s_gsm_receive_volume", "1", CVAR_ARCHIVE };

static struct {
	DL_t library;
	gsm_create_fn create;
	gsm_destroy_fn destroy;
	gsm_encode_fn encode;
	gsm_decode_fn decode;
	gsm_handle_t encoder;
	gsm_handle_t decoder;

	socket_t send_socket;
	qbool send_connecting;
	qbool send_configured;
	qbool send_config_error_printed;
	double connect_started;
	double next_connect;
	unsigned char send_buffer[GSM_SEND_BUFFER];
	unsigned int send_length;

	socket_t listen_socket;
	socket_t receive_socket;
	int listen_port;
	double next_listen;
	unsigned char receive_frame[GSM_FRAME_BYTES];
	unsigned int receive_length;

	SDL_AudioDeviceID capture_device;
	SDL_AudioStream *capture_stream;
	unsigned int capture_frame_bytes;
	double next_capture;
	double next_update;
	qbool library_failed;
} gsm_tcp = { NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	INVALID_SOCKET, false, false, false, 0, 0, { 0 }, 0,
	INVALID_SOCKET, INVALID_SOCKET, 0, 0, { 0 }, 0,
	0, NULL, 0, 0, 0, false };

static qbool S_GSM_WouldBlock(int error)
{
	// Interrupted calls are harmless and can be retried on the next audio update.
#ifdef _WIN32
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
#else
	return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS || error == EINTR;
#endif
}

static qbool S_GSM_PrivateAddress(const netadr_t *address)
{
	const byte *ip = address->ip;

	// Raw GSM has no authentication, so restrict it to local/private IPv4 ranges.
	if (address->type == NA_LOOPBACK)
		return true;
	if (address->type != NA_IP)
		return false;

	return ip[0] == 10 || ip[0] == 127 ||
		(ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
		(ip[0] == 192 && ip[1] == 168) ||
		(ip[0] == 169 && ip[1] == 254);
}

static qbool S_GSM_ParseEndpoint(const char *text, netadr_t *address)
{
	const char *colon;
	char host[16];
	char *port_end;
	struct in_addr ip;
	long port;
	size_t host_length;

	// Accept only an unambiguous numeric IPv4 address followed by a valid port.
	colon = strrchr(text, ':');
	if (!colon)
		return false;
	host_length = colon - text;
	if (!host_length || host_length >= sizeof(host))
		return false;
	memcpy(host, text, host_length);
	host[host_length] = '\0';
	if (inet_pton(AF_INET, host, &ip) != 1)
		return false;

	errno = 0;
	port = strtol(colon + 1, &port_end, 10);
	if (errno || port_end == colon + 1 || *port_end || port < 1 || port > 65535)
		return false;

	memset(address, 0, sizeof(*address));
	address->type = NA_IP;
	memcpy(address->ip, &ip, sizeof(address->ip));
	address->port = htons((unsigned short)port);
	return true;
}

static void S_GSM_CloseSocket(socket_t *socket)
{
	// Keep closed sockets at INVALID_SOCKET on every supported platform.
	if (*socket != INVALID_SOCKET) {
		closesocket(*socket);
		*socket = INVALID_SOCKET;
	}
}

static qbool S_GSM_SetNonBlocking(socket_t socket)
{
	unsigned long enabled = true;

	// No network operation is allowed to hold up the render or audio update.
#ifndef _WIN32
	if (fcntl(socket, F_SETFL, O_NONBLOCK) == -1)
		return false;
#endif
	return ioctlsocket(socket, FIONBIO, &enabled) != SOCKET_ERROR;
}

static void S_GSM_SetLowLatency(socket_t socket)
{
	int enabled = 1;
	int send_buffer = GSM_SEND_BUFFER;

	// Voice favors short latency over batching or a large amount of queued data.
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled, sizeof(enabled));
	setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char *)&send_buffer, sizeof(send_buffer));
#ifdef TCP_NOTSENT_LOWAT
	{
		unsigned int low_water = GSM_SEND_BUFFER;
		setsockopt(socket, IPPROTO_TCP, TCP_NOTSENT_LOWAT,
			(const char *)&low_water, sizeof(low_water));
	}
#endif
#ifdef SO_NOSIGPIPE
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&enabled, sizeof(enabled));
#endif
}

static qbool S_GSM_LoadLibrary(void)
{
	static const char *names[] = {
#ifdef _WIN32
		"gsm.dll", "libgsm.dll", "libgsm-1.dll",
#elif defined(__APPLE__)
		"libgsm.1.dylib", "libgsm.dylib",
#else
		"libgsm.so.1", "libgsm.so",
#endif
		NULL
	};
	int i;

	// Delay loading libgsm until the bridge is actually enabled.
	if (gsm_tcp.library)
		return true;
	if (gsm_tcp.library_failed)
		return false;

	for (i = 0; names[i]; ++i) {
		gsm_tcp.library = Sys_DLOpen(names[i]);
		if (gsm_tcp.library)
			break;
	}
	if (!gsm_tcp.library)
		goto failed;

	// Resolve only the four libgsm entry points used by the bridge.
	gsm_tcp.create = (gsm_create_fn)Sys_DLProc(gsm_tcp.library, "gsm_create");
	gsm_tcp.destroy = (gsm_destroy_fn)Sys_DLProc(gsm_tcp.library, "gsm_destroy");
	gsm_tcp.encode = (gsm_encode_fn)Sys_DLProc(gsm_tcp.library, "gsm_encode");
	gsm_tcp.decode = (gsm_decode_fn)Sys_DLProc(gsm_tcp.library, "gsm_decode");
	if (!gsm_tcp.create || !gsm_tcp.destroy || !gsm_tcp.encode || !gsm_tcp.decode)
		goto failed;

	// Sending and receiving need independent codec history.
	gsm_tcp.encoder = gsm_tcp.create();
	gsm_tcp.decoder = gsm_tcp.create();
	if (!gsm_tcp.encoder || !gsm_tcp.decoder)
		goto failed;

	return true;

failed:
	// Leave a completely inert state if the library is missing or incompatible.
	if (gsm_tcp.encoder && gsm_tcp.destroy)
		gsm_tcp.destroy(gsm_tcp.encoder);
	if (gsm_tcp.decoder && gsm_tcp.destroy)
		gsm_tcp.destroy(gsm_tcp.decoder);
	gsm_tcp.encoder = gsm_tcp.decoder = NULL;
	if (gsm_tcp.library)
		Sys_DLClose(gsm_tcp.library);
	gsm_tcp.library = NULL;
	gsm_tcp.library_failed = true;
	Com_Printf("GSM TCP: libgsm could not be loaded; install the libgsm runtime library\n");
	return false;
}

static qbool S_GSM_ResetCodec(gsm_handle_t *codec)
{
	// A new TCP stream must not inherit prediction state from the previous peer.
	if (*codec)
		gsm_tcp.destroy(*codec);
	*codec = gsm_tcp.create();
	return *codec != NULL;
}

static void S_GSM_CloseCapture(void)
{
	// Close the device before freeing the converter fed by that device.
	if (gsm_tcp.capture_device)
		SDL_CloseAudioDevice(gsm_tcp.capture_device);
	if (gsm_tcp.capture_stream)
		SDL_FreeAudioStream(gsm_tcp.capture_stream);
	gsm_tcp.capture_device = 0;
	gsm_tcp.capture_stream = NULL;
	gsm_tcp.capture_frame_bytes = 0;
}

static qbool S_GSM_OpenCapture(void)
{
	SDL_AudioSpec desired, obtained;
	const char *device = NULL;

	// Failed devices are retried slowly instead of once per rendered frame.
	if (gsm_tcp.capture_device)
		return true;
	if (Sys_DoubleTime() < gsm_tcp.next_capture)
		return false;

	// Ask for a common device format and convert whatever SDL obtains to GSM.
	memset(&desired, 0, sizeof(desired));
	desired.freq = 48000;
	desired.format = AUDIO_S16SYS;
	desired.channels = 1;
	desired.samples = 480;

	if (s_gsm_inputdevice.integer > 0)
		device = SDL_GetAudioDeviceName(s_gsm_inputdevice.integer - 1, 1);
	s_gsm_inputdevice.modified = false;

	gsm_tcp.capture_device = SDL_OpenAudioDevice(device, 1, &desired, &obtained,
		SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
	if (!gsm_tcp.capture_device) {
		Com_Printf("GSM TCP: could not open microphone: %s\n", SDL_GetError());
		gsm_tcp.next_capture = Sys_DoubleTime() + GSM_RECONNECT_SECONDS;
		return false;
	}

	// GSM 06.10 consumes host-endian signed mono samples at exactly 8 kHz.
	gsm_tcp.capture_stream = SDL_NewAudioStream(obtained.format, obtained.channels,
		obtained.freq, AUDIO_S16SYS, 1, GSM_SAMPLE_RATE);
	if (!gsm_tcp.capture_stream) {
		Com_Printf("GSM TCP: could not create microphone converter: %s\n", SDL_GetError());
		S_GSM_CloseCapture();
		gsm_tcp.next_capture = Sys_DoubleTime() + GSM_RECONNECT_SECONDS;
		return false;
	}

	gsm_tcp.capture_frame_bytes = obtained.channels * sizeof(short);
	gsm_tcp.next_capture = 0;
	SDL_PauseAudioDevice(gsm_tcp.capture_device, 0);
	Com_Printf("GSM TCP: capturing %s at %d Hz/%d channel%s\n",
		device ? device : "default microphone", obtained.freq, obtained.channels,
		obtained.channels == 1 ? "" : "s");
	return true;
}

static void S_GSM_DisconnectSender(void)
{
	// Discard queued bytes so a new raw stream always starts on a frame boundary.
	if (gsm_tcp.send_socket != INVALID_SOCKET)
		Com_Printf("GSM TCP: send connection closed\n");
	S_GSM_CloseSocket(&gsm_tcp.send_socket);
	gsm_tcp.send_connecting = false;
	gsm_tcp.connect_started = 0;
	gsm_tcp.send_length = 0;
	gsm_tcp.next_connect = Sys_DoubleTime() + GSM_RECONNECT_SECONDS;
	S_GSM_CloseCapture();
	gsm_tcp.next_capture = 0;
}

static void S_GSM_StartSender(void)
{
	netadr_t address;
	struct sockaddr_storage socket_address;
	int result;

	// Resolve configuration before allocating a socket or opening the microphone.
	if (!s_gsm_send.string[0] || Sys_DoubleTime() < gsm_tcp.next_connect)
		return;
	if (!S_GSM_ParseEndpoint(s_gsm_send.string, &address)) {
		if (!gsm_tcp.send_config_error_printed)
			Com_Printf("GSM TCP: s_gsm_send must be a numeric private IPv4 address:port\n");
		gsm_tcp.send_config_error_printed = true;
		gsm_tcp.next_connect = Sys_DoubleTime() + GSM_RECONNECT_SECONDS;
		return;
	}
	if (!S_GSM_PrivateAddress(&address)) {
		if (!gsm_tcp.send_config_error_printed)
			Com_Printf("GSM TCP: refusing non-private destination %s\n", NET_AdrToString(address));
		gsm_tcp.send_config_error_printed = true;
		gsm_tcp.next_connect = Sys_DoubleTime() + GSM_RECONNECT_SECONDS;
		return;
	}

	// Start the connection asynchronously; completion is polled in later updates.
	NetadrToSockadr(&address, &socket_address);
	gsm_tcp.send_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (gsm_tcp.send_socket == INVALID_SOCKET || !S_GSM_SetNonBlocking(gsm_tcp.send_socket)) {
		S_GSM_DisconnectSender();
		return;
	}

	result = connect(gsm_tcp.send_socket, (struct sockaddr *)&socket_address, sizeof(struct sockaddr_in));
	if (result == 0) {
		if (!S_GSM_ResetCodec(&gsm_tcp.encoder)) {
			S_GSM_DisconnectSender();
			return;
		}
		S_GSM_SetLowLatency(gsm_tcp.send_socket);
		Com_Printf("GSM TCP: connected microphone to %s\n", NET_AdrToString(address));
		S_GSM_OpenCapture();
	}
	else if (S_GSM_WouldBlock(qerrno)) {
		gsm_tcp.send_connecting = true;
		gsm_tcp.connect_started = Sys_DoubleTime();
	}
	else {
		S_GSM_DisconnectSender();
	}
}

static void S_GSM_CheckSenderConnection(void)
{
	fd_set writable;
	struct timeval timeout = { 0, 0 };
	int error = 0;
	socklen_t error_length = sizeof(error);

	if (!gsm_tcp.send_connecting)
		return;
	if (Sys_DoubleTime() - gsm_tcp.connect_started >= GSM_CONNECT_TIMEOUT_SECONDS) {
		S_GSM_DisconnectSender();
		return;
	}
	// A writable connecting socket has either connected or recorded an error.
	FD_ZERO(&writable);
	FD_SET(gsm_tcp.send_socket, &writable);
	if (select((int)gsm_tcp.send_socket + 1, NULL, &writable, NULL, &timeout) <= 0)
		return;
	if (getsockopt(gsm_tcp.send_socket, SOL_SOCKET, SO_ERROR, (char *)&error, &error_length) || error) {
		S_GSM_DisconnectSender();
		return;
	}

	gsm_tcp.send_connecting = false;
	gsm_tcp.connect_started = 0;
	if (!S_GSM_ResetCodec(&gsm_tcp.encoder)) {
		S_GSM_DisconnectSender();
		return;
	}
	S_GSM_SetLowLatency(gsm_tcp.send_socket);
	Com_Printf("GSM TCP: microphone connection established\n");
	S_GSM_OpenCapture();
}

static void S_GSM_Send(void);

static void S_GSM_Capture(void)
{
	unsigned char input[GSM_IO_BUFFER];
	short pcm[GSM_FRAME_SAMPLES];
	unsigned char encoded[GSM_FRAME_BYTES];
	unsigned int available;
	unsigned int discard;
	unsigned int before_send;
	int bytes;
	int i;
	float volume = s_gsm_mic_volume.value;

	if (!isfinite(volume) || volume < 0)
		volume = 0;
	else if (volume > 16)
		volume = 16;

	// Apply microphone changes without restarting the game sound device.
	if (s_gsm_inputdevice.modified) {
		S_GSM_CloseCapture();
		gsm_tcp.next_capture = 0;
		s_gsm_inputdevice.modified = false;
	}

	if (!gsm_tcp.capture_device && !S_GSM_OpenCapture())
		return;

	// Drop old capture data after a long game hitch rather than transmitting it late.
	available = SDL_GetQueuedAudioSize(gsm_tcp.capture_device);
	if (available > sizeof(input) && gsm_tcp.capture_frame_bytes) {
		discard = available - sizeof(input);
		discard -= discard % gsm_tcp.capture_frame_bytes;
		while (discard) {
			bytes = SDL_DequeueAudio(gsm_tcp.capture_device, input, min(discard, sizeof(input)));
			if (bytes <= 0)
				break;
			discard -= bytes;
		}
		available = SDL_GetQueuedAudioSize(gsm_tcp.capture_device);
	}
	// Feed native device samples into SDL's stateful 8 kHz mono converter.
	while (available) {
		bytes = SDL_DequeueAudio(gsm_tcp.capture_device, input, min(available, sizeof(input)));
		if (bytes <= 0 || SDL_AudioStreamPut(gsm_tcp.capture_stream, input, bytes) < 0)
			break;
		available -= bytes;
	}

	// Encode every complete 20 ms PCM block into one 33-byte raw GSM frame.
	while (SDL_AudioStreamAvailable(gsm_tcp.capture_stream) >= GSM_PCM_BYTES) {
		// Flush a full application queue without ever dropping part of a raw frame.
		if (gsm_tcp.send_length + GSM_FRAME_BYTES > sizeof(gsm_tcp.send_buffer)) {
			before_send = gsm_tcp.send_length;
			S_GSM_Send();
			if (gsm_tcp.send_socket == INVALID_SOCKET)
				return;
			if (gsm_tcp.send_length + GSM_FRAME_BYTES > sizeof(gsm_tcp.send_buffer) &&
				gsm_tcp.send_length == before_send) {
				Com_Printf("GSM TCP: sender is not consuming audio; reconnecting\n");
				S_GSM_DisconnectSender();
				return;
			}
			continue;
		}
		if (SDL_AudioStreamGet(gsm_tcp.capture_stream, pcm, sizeof(pcm)) != sizeof(pcm))
			break;
		if (volume != 1) {
			for (i = 0; i < GSM_FRAME_SAMPLES; ++i) {
				float sample = pcm[i] * volume;
				pcm[i] = bound(-32768, sample, 32767);
			}
		}
		if (gsm_tcp.encode(gsm_tcp.encoder, pcm, encoded) < 0)
			continue;
		memcpy(gsm_tcp.send_buffer + gsm_tcp.send_length, encoded, sizeof(encoded));
		gsm_tcp.send_length += sizeof(encoded);
	}
}

static void S_GSM_Send(void)
{
	int sent;

	if (!gsm_tcp.send_length)
		return;
	// Preserve an unsent suffix because non-blocking TCP may accept only part of it.
	sent = send(gsm_tcp.send_socket, (const char *)gsm_tcp.send_buffer,
		gsm_tcp.send_length, GSM_SEND_FLAGS);
	if (sent > 0) {
		memmove(gsm_tcp.send_buffer, gsm_tcp.send_buffer + sent, gsm_tcp.send_length - sent);
		gsm_tcp.send_length -= sent;
	}
	else if (sent == 0 || !S_GSM_WouldBlock(qerrno)) {
		S_GSM_DisconnectSender();
	}
}

static void S_GSM_UpdateListener(void)
{
	int port = s_gsm_listen_port.integer >= 1 && s_gsm_listen_port.integer <= 65535 ?
		s_gsm_listen_port.integer : 0;
	struct sockaddr_storage peer_address;
	socklen_t peer_length;
	netadr_t peer;
	socket_t accepted;

	// Recreate the listener and stop old buffered audio when its port changes.
	if (port != gsm_tcp.listen_port) {
		if (gsm_tcp.listen_port)
			S_RawAudio(RAW_SOURCE_GSM_TCP, NULL, 0, 0, 0, 0);
		S_GSM_CloseSocket(&gsm_tcp.receive_socket);
		S_GSM_CloseSocket(&gsm_tcp.listen_socket);
		gsm_tcp.receive_length = 0;
		gsm_tcp.listen_port = port;
		gsm_tcp.next_listen = 0;
	}
	// Retry a temporarily unavailable port rather than requiring another cvar change.
	if (port && gsm_tcp.listen_socket == INVALID_SOCKET &&
		Sys_DoubleTime() >= gsm_tcp.next_listen) {
		gsm_tcp.listen_socket = TCP_OpenListenSocket((unsigned short)port);
		if (gsm_tcp.listen_socket != INVALID_SOCKET)
			Com_Printf("GSM TCP: listening for private-LAN audio on port %d\n", port);
		else
			gsm_tcp.next_listen = Sys_DoubleTime() + GSM_RECONNECT_SECONDS;
	}

	if (gsm_tcp.listen_socket == INVALID_SOCKET)
		return;

	// Accept at most one connection per update and retain only one active sender.
	peer_length = sizeof(peer_address);
	accepted = accept(gsm_tcp.listen_socket, (struct sockaddr *)&peer_address, &peer_length);
	if (accepted == INVALID_SOCKET)
		return;
	// Reject public peers before any audio bytes are read or decoded.
	SockadrToNetadr(&peer_address, &peer);
	if (!S_GSM_PrivateAddress(&peer) || gsm_tcp.receive_socket != INVALID_SOCKET ||
		!S_GSM_SetNonBlocking(accepted)) {
		Com_Printf("GSM TCP: rejected connection from %s\n", NET_AdrToString(peer));
		closesocket(accepted);
		return;
	}
	if (!S_GSM_ResetCodec(&gsm_tcp.decoder)) {
		closesocket(accepted);
		return;
	}

	gsm_tcp.receive_socket = accepted;
	gsm_tcp.receive_length = 0;
	Com_Printf("GSM TCP: receiving audio from %s\n", NET_AdrToString(peer));
}

static void S_GSM_Receive(void)
{
	short decoded[GSM_FRAME_SAMPLES * 32];
	unsigned int decoded_frames = 0;
	int received;
	int i;
	float volume = s_gsm_receive_volume.value;

	if (!isfinite(volume) || volume < 0)
		volume = 0;
	else if (volume > 16)
		volume = 16;

	// Reassemble frames explicitly because TCP reads do not preserve write boundaries.
	while (decoded_frames < 32) {
		received = recv(gsm_tcp.receive_socket,
			(char *)gsm_tcp.receive_frame + gsm_tcp.receive_length,
			GSM_FRAME_BYTES - gsm_tcp.receive_length, 0);
		if (received > 0) {
			gsm_tcp.receive_length += received;
			if (gsm_tcp.receive_length != GSM_FRAME_BYTES)
				continue;
			gsm_tcp.receive_length = 0;
			if (gsm_tcp.decode(gsm_tcp.decoder, gsm_tcp.receive_frame,
				decoded + decoded_frames * GSM_FRAME_SAMPLES) < 0) {
				Com_Printf("GSM TCP: invalid raw GSM frame; closing receive connection\n");
				S_GSM_CloseSocket(&gsm_tcp.receive_socket);
				break;
			}
			++decoded_frames;
		}
		else {
			if (received == 0 || !S_GSM_WouldBlock(qerrno)) {
				Com_Printf("GSM TCP: receive connection closed\n");
				S_GSM_CloseSocket(&gsm_tcp.receive_socket);
				gsm_tcp.receive_length = 0;
			}
			break;
		}
	}

	// Submit one decoded batch so the normal raw stream handles output resampling.
	if (!decoded_frames)
		return;
	if (volume != 1) {
		for (i = 0; i < (int)(decoded_frames * GSM_FRAME_SAMPLES); ++i) {
			float sample = decoded[i] * volume;
			decoded[i] = bound(-32768, sample, 32767);
		}
	}
	S_RawAudio(RAW_SOURCE_GSM_TCP, (byte *)decoded, GSM_SAMPLE_RATE,
		decoded_frames * GSM_FRAME_SAMPLES, 1, sizeof(short));
}

void S_GSM_Update(void)
{
	double now;
	qbool listen_enabled = s_gsm_listen_port.integer >= 1 && s_gsm_listen_port.integer <= 65535;

	// Keep the disabled path cheap and release every resource when both halves are off.
	if (!s_gsm_send.string[0] && !listen_enabled) {
		if (gsm_tcp.library || gsm_tcp.library_failed || gsm_tcp.capture_device ||
			gsm_tcp.send_socket != INVALID_SOCKET || gsm_tcp.listen_socket != INVALID_SOCKET ||
			gsm_tcp.receive_socket != INVALID_SOCKET)
			S_GSM_Shutdown();
		return;
	}
	// Cap polling at 200 Hz so uncapped rendering does not cause excess syscalls.
	now = Sys_DoubleTime();
	if (now < gsm_tcp.next_update)
		return;
	gsm_tcp.next_update = now + GSM_UPDATE_INTERVAL_SECONDS;
	if (!S_GSM_LoadLibrary())
		return;

	// A destination change starts a fresh TCP connection and raw codec stream.
	if (!gsm_tcp.send_configured || s_gsm_send.modified) {
		S_GSM_DisconnectSender();
		gsm_tcp.send_configured = true;
		gsm_tcp.send_config_error_printed = false;
		s_gsm_send.modified = false;
		gsm_tcp.next_connect = 0;
	}
	if (s_gsm_send.string[0]) {
		if (gsm_tcp.send_socket == INVALID_SOCKET)
			S_GSM_StartSender();
		else if (gsm_tcp.send_connecting)
			S_GSM_CheckSenderConnection();
		else {
			S_GSM_Capture();
			S_GSM_Send();
		}
	}
	else if (gsm_tcp.send_socket != INVALID_SOCKET) {
		S_GSM_DisconnectSender();
	}

	// Sending and receiving are independent and may be enabled in either combination.
	S_GSM_UpdateListener();
	if (gsm_tcp.receive_socket != INVALID_SOCKET)
		S_GSM_Receive();
}

void S_GSM_Shutdown(void)
{
	// Stop queued playback before releasing sockets, devices, and codec state.
	if (snd_initialized && snd_started)
		S_RawAudio(RAW_SOURCE_GSM_TCP, NULL, 0, 0, 0, 0);
	S_GSM_CloseCapture();
	S_GSM_CloseSocket(&gsm_tcp.send_socket);
	S_GSM_CloseSocket(&gsm_tcp.receive_socket);
	S_GSM_CloseSocket(&gsm_tcp.listen_socket);
	if (gsm_tcp.encoder && gsm_tcp.destroy)
		gsm_tcp.destroy(gsm_tcp.encoder);
	if (gsm_tcp.decoder && gsm_tcp.destroy)
		gsm_tcp.destroy(gsm_tcp.decoder);
	if (gsm_tcp.library)
		Sys_DLClose(gsm_tcp.library);
	memset(&gsm_tcp, 0, sizeof(gsm_tcp));
	gsm_tcp.send_socket = gsm_tcp.receive_socket = gsm_tcp.listen_socket = INVALID_SOCKET;
}

void S_GSM_RegisterCvars(void)
{
	// Keep bridge configuration with the rest of the sound controls.
	Cvar_SetCurrentGroup(CVAR_GROUP_SOUND);
	Cvar_Register(&s_gsm_send);
	Cvar_Register(&s_gsm_listen_port);
	Cvar_Register(&s_gsm_inputdevice);
	Cvar_Register(&s_gsm_mic_volume);
	Cvar_Register(&s_gsm_receive_volume);
	Cvar_ResetCurrentGroup();
}
