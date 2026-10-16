#pragma once
#define PSP_AUDIO_NEXT_CHANNEL (-1)
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_VOLUME_MAX 0x8000
int sceAudioChReserve(int ch, int samples, int fmt);
int sceAudioOutputPannedBlocking(int ch, int l, int r, void *buf);
static inline int sceAudioChRelease(int ch) { (void)ch; return 0; }
