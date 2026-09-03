# Raw GSM TCP audio bridge

UnezQuake can send its microphone to another computer and mix a received raw
GSM 06.10 stream into normal game playback. This is a direct TCP side channel;
it does not pass through the QuakeWorld server or use QuakeWorld voice packets.

GSM 06.10 is always 8 kHz mono. UnezQuake accepts the microphone's normal
hardware format and converts it at the codec boundary, and converts received
audio to the configured game playback format. Each raw GSM frame represents 20
ms (160 samples) and occupies 33 bytes.

The codec is provided by `libgsm`, which UnezQuake loads at runtime only when
the bridge is enabled. Install the libgsm runtime library on the gaming
computer and make sure it is visible to the operating system's dynamic loader.

## Send the microphone

Start a raw GSM TCP listener on the Discord computer. For example:

```shell
ffplay -f gsm -i 'tcp://0.0.0.0:5000?listen'
```

Then configure UnezQuake with that computer's private LAN address:

```text
s_gsm_send 192.168.1.20:5000
```

Use `s_audiodevicelist` and `s_gsm_inputdevice` to select a microphone. The
default device is 0. `s_gsm_mic_volume` controls its pre-encoding gain.

## Receive and mix audio

Tell UnezQuake to listen on a different port:

```text
s_gsm_listen_port 5001
```

On the Discord computer, send audio from the appropriate PipeWire/PulseAudio
source. A file-based FFmpeg example is:

```shell
ffmpeg -re -i input.wav -ar 8000 -ac 1 -c:a libgsm -f gsm \
  'tcp://192.168.1.10:5001'
```

Received speech is mixed with normal gameplay and is controlled by
`s_gsm_receive_volume` and the existing `s_raw_volume` voice-stream control.
There is deliberately no local microphone monitoring and received audio is not
fed back to the send stream, avoiding an audio feedback loop.

Set `s_gsm_send` to an empty string and `s_gsm_listen_port` to 0 to disable the
bridge.

## Network restriction

Both directions are restricted to IPv4 loopback (`127.0.0.0/8`), RFC1918
private networks (`10.0.0.0/8`, `172.16.0.0/12`, and `192.168.0.0/16`), and
IPv4 link-local (`169.254.0.0/16`). UnezQuake refuses outbound public addresses
and rejects inbound public peers. Outbound endpoints must use numeric IPv4
addresses, avoiding DNS delays and address changes. Raw GSM has no
authentication or encryption, so it should still be used only on a trusted LAN.
