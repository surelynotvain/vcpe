/* VCPE streams (4.5): several music-like channels at once (background music, ambient loops, jingles), each streamed
 * from its own place in a PAK, with an intro + loop, any sample rate, mono or stereo, pitch, gain, fades, pause and
 * a resumable position. The mixer of vcpe_audio pulls them through vcpe_audio_mix_hook, so vcpe_audio_init must run
 * first (its own single music track keeps working and can be left unused).
 *
 * Track data (tools/vcpe_stream.py writes it): IMA-ADPCM blocks of 1024 samples, the same block layout as vcpe_audio
 * music (per channel a 4-byte header {int16 predictor, u8 index, u8 0}, then per channel 512 bytes of codes). A track
 * is an intro segment followed by an optional loop segment; both start on a block boundary and say how many samples
 * of their last block are real, so a loop point can sit on any sample. */
#ifndef VCPE_STREAM_H
#define VCPE_STREAM_H
#include <stdint.h>

#define VCPE_STREAM_MAX 4

typedef struct {
    uint32_t intro_off, intro_samples;   /* the part played once (samples 0: none) */
    uint32_t loop_off, loop_samples;     /* the part repeated after it (samples 0: the track ends after the intro) */
    uint16_t rate;                       /* samples per second */
    uint8_t channels, pad;               /* 1 or 2 */
} VcpeStreamTrack;

/* open the PAK holding the tracks; channels: how many streams can play at once (<= VCPE_STREAM_MAX) */
int vcpe_stream_init(const char *pak, const VcpeStreamTrack *tracks, int ntracks, int channels);
void vcpe_stream_shutdown(void);

/* start track on channel ch from sample `start` (0: the beginning; a position from vcpe_stream_pos resumes),
 * gain 0..256, pitch in percent (100 = as recorded; it changes speed, like a tape), fading in over fade_ms */
void vcpe_stream_play(int ch, int track, int gain256, int pitch_pct, int fade_ms, uint32_t start);
/* fade out over fade_ms and stop (0: now) */
void vcpe_stream_stop(int ch, int fade_ms);
void vcpe_stream_gain(int ch, int gain256);
void vcpe_stream_pitch(int ch, int pitch_pct);
void vcpe_stream_pause(int ch, int on);
int vcpe_stream_track(int ch);           /* playing (or fading) track, -1 none */
int vcpe_stream_active(int ch);          /* 1 while it still makes sound (a track without loop ends by itself) */
uint32_t vcpe_stream_pos(int ch);        /* sample position, for vcpe_stream_play(..., start) later */
void vcpe_stream_master(int gain256);    /* all streams */
extern int vcpe_stream_underruns;        /* blocks the mixer found missing (memory stick too slow) */

#endif
