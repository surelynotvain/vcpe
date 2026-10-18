/* VCPE RPG resources: RPG Maker names -> engine ids, and RPG Maker's audio rules on top of vcpe_stream / vcpe_audio.
 *
 * Names are looked up case-insensitively ("Graphics/Characters/Actor1" written as "characters/actor1" by the
 * converter), because projects made on Windows mix the case of file names freely.
 *
 * Audio follows RGSS3:
 * - BGM, BGS and ME are streams (channels 0, 1, 2); SE are resident effects on the mixer's voices.
 * - Playing the BGM / BGS that is already playing only changes its volume and pitch (no restart).
 * - An ME pauses the BGM; when the ME ends, the BGM continues where it was, fading back in.
 * - Volumes are percentages (linear), pitch is a percentage of the speed. */
#include "vcpe_rpg.h"
#include "vcpe_stream.h"
#include "vcpe_audio.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const RpgName *img_names, *se_names, *st_names;
static int img_n, se_n, st_n;
static int vol_bgm = 100, vol_bgs = 100, vol_me = 100, vol_se = 100;
static RpgSound cur_bgm, cur_bgs, cur_me;
static int bgm_track = -1, bgs_track = -1, me_track = -1, me_was_playing;
static int me_resume_fade;

void rpg_res_images(const RpgName *names, int n)
{
    img_names = names;
    img_n = n;
}

void rpg_res_sounds(const RpgName *se, int nse, const RpgName *streams, int nstreams)
{
    se_names = se;
    se_n = nse;
    st_names = streams;
    st_n = nstreams;
}

static void key(char *out, int cap, const char *folder, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", folder, name);
    if (n >= cap)
        n = cap - 1;
    for (int i = 0; i < n; i++) {
        unsigned char c = out[i];
        if (c == '\\')
            c = '/';
        out[i] = (c < 128) ? (char)tolower(c) : (char)c;
    }
    /* RPG Maker names never carry the extension, but scripts sometimes do */
    char *dot = strrchr(out, '.');
    if (dot && (!strcmp(dot, ".png") || !strcmp(dot, ".jpg") || !strcmp(dot, ".ogg") || !strcmp(dot, ".wav") ||
                !strcmp(dot, ".mp3") || !strcmp(dot, ".bmp")))
        *dot = 0;
}

static int find(const RpgName *t, int n, const char *k)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int m = (lo + hi) / 2, c = strcmp(k, t[m].name);
        if (!c)
            return t[m].id;
        if (c < 0)
            hi = m;
        else
            lo = m + 1;
    }
    return -1;
}

int rpg_image(const char *folder, const char *name)
{
    if (!name || !*name)
        return -1;
    char k[192];
    key(k, sizeof k, folder, name);
    int id = find(img_names, img_n, k);
    if (id < 0) {
        static char last[192];
        if (strcmp(last, k)) {
            snprintf(last, sizeof last, "%s", k);
            rpg_log("RPG missing image %s", k);
        }
    }
    return id;
}

int rpg_image_hue(const char *folder, const char *name, int hue)
{
    if (!hue)
        return rpg_image(folder, name);
    char n2[160];
    snprintf(n2, sizeof n2, "%s@%d", name, hue);
    char k[192];
    key(k, sizeof k, folder, n2);
    int id = find(img_names, img_n, k);
    return id >= 0 ? id : rpg_image(folder, name);
}

static int stream_id(const char *folder, const char *name)
{
    char k[192];
    key(k, sizeof k, folder, name);
    int id = find(st_names, st_n, k);
    if (id < 0)
        rpg_log("RPG missing audio %s", k);
    return id;
}

static int gain(int volume, int option) { return volume * option * 256 / 10000; }

static void set(RpgSound *s, const char *name, int volume, int pitch)
{
    snprintf(s->name, sizeof s->name, "%s", name);
    s->volume = volume;
    s->pitch = pitch;
}

void rpg_bgm_play_at(const char *name, int volume, int pitch, uint32_t pos)
{
    if (!name || !*name) {
        rpg_bgm_stop();
        return;
    }
    int t = stream_id("bgm", name);
    if (t >= 0 && t == bgm_track && vcpe_stream_track(0) == t && !pos) {
        vcpe_stream_gain(0, gain(volume, vol_bgm));
        vcpe_stream_pitch(0, pitch);
    } else if (t >= 0) {
        vcpe_stream_play(0, t, gain(volume, vol_bgm), pitch, 0, pos);
        if (me_track >= 0 && vcpe_stream_active(2))
            vcpe_stream_pause(0, 1);
    } else {
        vcpe_stream_stop(0, 0);
    }
    bgm_track = t;
    set(&cur_bgm, name, volume, pitch);
}

void rpg_bgm_play(const char *name, int volume, int pitch) { rpg_bgm_play_at(name, volume, pitch, 0); }

void rpg_bgm_stop(void)
{
    vcpe_stream_stop(0, 0);
    bgm_track = -1;
    cur_bgm.name[0] = 0;
}

void rpg_bgm_fade(int ms)
{
    vcpe_stream_stop(0, ms);
    bgm_track = -1;
    cur_bgm.name[0] = 0;
}

void rpg_bgs_play(const char *name, int volume, int pitch)
{
    if (!name || !*name) {
        rpg_bgs_stop();
        return;
    }
    int t = stream_id("bgs", name);
    if (t >= 0 && t == bgs_track && vcpe_stream_track(1) == t) {
        vcpe_stream_gain(1, gain(volume, vol_bgs));
        vcpe_stream_pitch(1, pitch);
    } else if (t >= 0) {
        vcpe_stream_play(1, t, gain(volume, vol_bgs), pitch, 0, 0);
    } else {
        vcpe_stream_stop(1, 0);
    }
    bgs_track = t;
    set(&cur_bgs, name, volume, pitch);
}

void rpg_bgs_stop(void)
{
    vcpe_stream_stop(1, 0);
    bgs_track = -1;
    cur_bgs.name[0] = 0;
}

void rpg_bgs_fade(int ms)
{
    vcpe_stream_stop(1, ms);
    bgs_track = -1;
    cur_bgs.name[0] = 0;
}

void rpg_me_play(const char *name, int volume, int pitch)
{
    if (!name || !*name) {
        rpg_me_stop();
        return;
    }
    int t = stream_id("me", name);
    if (t < 0)
        return;
    vcpe_stream_play(2, t, gain(volume, vol_me), pitch, 0, 0);
    vcpe_stream_pause(0, 1);
    me_track = t;
    me_was_playing = 1;
    set(&cur_me, name, volume, pitch);
}

void rpg_me_stop(void)
{
    vcpe_stream_stop(2, 0);
    me_track = -1;
}

int rpg_me_playing(void) { return me_track >= 0 && vcpe_stream_active(2); }

void rpg_audio_update(void)
{
    if (me_was_playing && !vcpe_stream_active(2)) {
        me_was_playing = 0;
        me_track = -1;
        /* RGSS3: the BGM comes back after the ME, fading in */
        if (bgm_track >= 0) {
            vcpe_stream_pause(0, 0);
            me_resume_fade = 1;
        }
    }
}

void rpg_se_play(const char *name, int volume, int pitch)
{
    if (!name || !*name)
        return;
    char k[192];
    key(k, sizeof k, "se", name);
    int id = find(se_names, se_n, k);
    if (id < 0) {
        rpg_log("RPG missing audio %s", k);
        return;
    }
    vcpe_sfx_play(id, pitch / 100.f, gain(volume, vol_se), 0, vcpe_voice_count() - 1);
}

void rpg_se_stop(void) { vcpe_voices_stop_all(); }

RpgSound rpg_bgm_current(void) { return cur_bgm; }
RpgSound rpg_bgs_current(void) { return cur_bgs; }
uint32_t rpg_bgm_pos(void) { return bgm_track >= 0 ? vcpe_stream_pos(0) : 0; }

void rpg_audio_volumes(int bgm, int bgs, int me, int se)
{
    vol_bgm = bgm;
    vol_bgs = bgs;
    vol_me = me;
    vol_se = se;
    if (bgm_track >= 0)
        vcpe_stream_gain(0, gain(cur_bgm.volume, vol_bgm));
    if (bgs_track >= 0)
        vcpe_stream_gain(1, gain(cur_bgs.volume, vol_bgs));
}

void rpg_play_system_se(int which)
{
    const RpgSystem *s = rpg_system();
    const RpgAudio *a = rpg_at(&rpg_db, RPG_T_AUDIO, s->sounds, which);
    if (a)
        rpg_se_play(RPG_STR(a->name), a->volume, a->pitch);
}
