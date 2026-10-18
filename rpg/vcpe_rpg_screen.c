/* VCPE RPG screen: Game_Screen (fades, tone, flash, shake, weather, pictures) and its drawing, the animation
 * player of Sprite_Base (RPG::Animation cells, 4 frames per cell frame, timings with sounds and flashes) and the
 * balloon icons of Sprite_Character. Arithmetic as VX Ace (float easing by duration). */
#include "vcpe_rpg.h"
#include "vcpe_spr.h"
#include "vcpe_gfx.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ Game_Screen */
void rpg_screen_clear(RpgScreen *s)
{
    memset(s, 0, sizeof *s);
    s->brightness = 255;
    s->shake_direction = 1;
    for (int i = 0; i <= RPG_MAX_PICTURES; i++) {
        s->pictures[i].number = i;
        s->pictures[i].zoom_x = s->pictures[i].zoom_y = 100;
        s->pictures[i].opacity = 255;
        s->pictures[i].blend_type = 1;
    }
}

void rpg_screen_flash_start(RpgScreen *s) { (void)s; }

void rpg_screen_flash_damage(RpgScreen *s)
{
    s->flash_color[0] = 255;
    s->flash_color[1] = 0;
    s->flash_color[2] = 0;
    s->flash_color[3] = 30;
    s->flash_duration = 8;
}

static void ease(float *v, float target, int d) { *v = (*v * (d - 1) + target) / d; }

static void picture_update(RpgPicture *p)
{
    if (p->duration > 0) {
        int d = p->duration;
        ease(&p->x, p->target_x, d);
        ease(&p->y, p->target_y, d);
        ease(&p->zoom_x, p->target_zoom_x, d);
        ease(&p->zoom_y, p->target_zoom_y, d);
        ease(&p->opacity, p->target_opacity, d);
        p->duration--;
    }
    if (p->tone_duration > 0) {
        int d = p->tone_duration;
        for (int i = 0; i < 4; i++)
            ease(&p->tone[i], p->tone_target[i], d);
        p->tone_duration--;
    }
    if (p->rotate_speed != 0) {
        p->angle += p->rotate_speed / 2.0f;
        while (p->angle < 0)
            p->angle += 360;
        p->angle = fmodf(p->angle, 360);
    }
}

void rpg_screen_update(RpgScreen *s)
{
    if (s->fadeout_duration > 0) {
        int d = s->fadeout_duration;
        s->brightness = (s->brightness * (d - 1)) / d;
        s->fadeout_duration--;
    }
    if (s->fadein_duration > 0) {
        int d = s->fadein_duration;
        s->brightness = (s->brightness * (d - 1) + 255) / d;
        s->fadein_duration--;
    }
    if (s->tone_duration > 0) {
        int d = s->tone_duration;
        for (int i = 0; i < 4; i++)
            ease(&s->tone[i], s->tone_target[i], d);
        s->tone_duration--;
    }
    if (s->flash_duration > 0) {
        int d = s->flash_duration;
        s->flash_color[3] = s->flash_color[3] * (d - 1) / d;
        s->flash_duration--;
    }
    if (s->shake_duration > 0 || s->shake != 0) {
        float delta = (s->shake_power * s->shake_speed * s->shake_direction) / 10.0f;
        if (s->shake_duration <= 1 && s->shake * (s->shake + delta) < 0)
            s->shake = 0;
        else
            s->shake += delta;
        if (s->shake > s->shake_power * 2)
            s->shake_direction = -1;
        if (s->shake < -s->shake_power * 2)
            s->shake_direction = 1;
        s->shake_duration--;
    }
    if (s->weather_duration > 0) {
        int d = s->weather_duration;
        ease(&s->weather_power, s->weather_power_target, d);
        s->weather_duration--;
        if (s->weather_duration == 0 && s->weather_power_target == 0)
            s->weather_type = 0;
    }
    for (int i = 1; i <= RPG_MAX_PICTURES; i++)
        picture_update(&s->pictures[i]);
}

void rpg_picture_show(RpgPicture *p, const char *name, int origin, float x, float y, float zx, float zy, float op,
                      int blend)
{
    int n = p->number;
    memset(p, 0, sizeof *p);
    p->number = n;
    snprintf(p->name, sizeof p->name, "%s", name);
    p->origin = origin;
    p->x = p->target_x = x;
    p->y = p->target_y = y;
    p->zoom_x = p->target_zoom_x = zx;
    p->zoom_y = p->target_zoom_y = zy;
    p->opacity = p->target_opacity = op;
    p->blend_type = blend;
}

void rpg_picture_move(RpgPicture *p, int origin, float x, float y, float zx, float zy, float op, int blend, int dur)
{
    p->origin = origin;
    p->target_x = x;
    p->target_y = y;
    p->target_zoom_x = zx;
    p->target_zoom_y = zy;
    p->target_opacity = op;
    p->blend_type = blend;
    p->duration = dur;
}

void rpg_picture_erase(RpgPicture *p) { p->name[0] = 0; }

/* ------------------------------------------------------------------ drawing */
void (*rpg_picture_draw_hook)(const RpgPicture *p, int img);   /* the game draws it itself (return via name) */

void rpg_screen_draw_pictures(const RpgScreen *s)
{
    for (int i = 1; i <= RPG_MAX_PICTURES; i++) {
        const RpgPicture *p = &s->pictures[i];
        if (!p->name[0] || p->opacity <= 0)
            continue;
        int img = rpg_image("pictures", p->name);
        if (img < 0)
            continue;
        int w = rpg_image_w(img), h = rpg_image_h(img);
        float zx = p->zoom_x / 100.f, zy = p->zoom_y / 100.f;
        float ox = p->origin == 0 ? 0 : w / 2.f, oy = p->origin == 0 ? 0 : h / 2.f;
        int a = (int)p->opacity;
        uint32_t c = rpg_rgba(255, 255, 255, a);
        vcpe_spr_filter(0);
        rpg_draw_part_x(&rpg_picture_xform, img, 0, 0, w, h, p->x - ox * zx, p->y - oy * zy, zx, zy, p->angle, c,
                        p->blend_type);
        vcpe_spr_filter(1);
        if (p->tone[0] || p->tone[1] || p->tone[2] || p->tone[3]) {
            /* a picture tone darkens / brightens its rectangle (approximation without per-pixel masks) */
        }
    }
}

void rpg_screen_draw_tone_flash(const RpgScreen *s)
{
    if (s->tone[0] || s->tone[1] || s->tone[2] || s->tone[3])
        rpg_screen_tone(s->tone, 0, 0, rpg_screen_w, rpg_screen_h);
}

void rpg_screen_draw_flash(const RpgScreen *s)
{
    if (s->flash_duration > 0 && s->flash_color[3] > 0)
        rpg_fill(0, 0, rpg_screen_w, rpg_screen_h,
                 rpg_rgba((int)s->flash_color[0], (int)s->flash_color[1], (int)s->flash_color[2], (int)s->flash_color[3]));
}

void rpg_screen_draw_brightness(const RpgScreen *s)
{
    if (s->brightness < 255)
        rpg_fill(0, 0, rpg_screen_w, rpg_screen_h, rpg_rgba(0, 0, 0, 255 - (int)s->brightness));
}

void rpg_screen_draw_over(const RpgScreen *s)
{
    rpg_screen_draw_flash(s);
    rpg_screen_draw_brightness(s);
}

/* weather: Spriteset_Weather's sprites as simple particles (rain streaks, storm streaks, snow flakes) */
#define WEATHER_MAX 120
static struct { float x, y, op; } drops[WEATHER_MAX];
static int drops_init;

void rpg_screen_draw_weather(const RpgScreen *s)
{
    if (!s->weather_type || s->weather_power <= 0)
        return;
    if (!drops_init) {
        drops_init = 1;
        for (int i = 0; i < WEATHER_MAX; i++) {
            drops[i].x = rpg_rand(rpg_screen_w + 100) - 100;
            drops[i].y = rpg_rand(rpg_screen_h + 200) - 200;
            drops[i].op = 160 + rpg_rand(96);
        }
    }
    int n = (int)(s->weather_power * 4);
    if (n > WEATHER_MAX)
        n = WEATHER_MAX;
    for (int i = 0; i < n; i++) {
        float *x = &drops[i].x, *y = &drops[i].y;
        switch (s->weather_type) {
        case 1: *x -= 1; *y += 6; drops[i].op -= 12; break;
        case 2: *x -= 3; *y += 6; drops[i].op -= 12; break;
        case 3: *x -= 1; *y += 3; drops[i].op -= 12; break;
        }
        if (drops[i].op < 64) {
            *x = rpg_rand(rpg_screen_w + 100) - 50 + rpg.display_x * 0;
            *y = rpg_rand(rpg_screen_h + 200) - 200;
            drops[i].op = 160 + rpg_rand(96);
        }
        int a = (int)drops[i].op;
        if (s->weather_type == 3)
            rpg_fill(*x, *y, 4, 4, rpg_rgba(255, 255, 255, a));
        else
            for (int k = 0; k < 8; k++)
                rpg_fill(*x + (s->weather_type == 2 ? -k / 2.f : -k / 8.f), *y + k * 4, 2, 4, rpg_rgba(255, 255, 255, a / 2));
    }
}

/* ------------------------------------------------------------------ animations */
static RpgAnimPlay anims[RPG_MAX_ANIMS];
static int ani_dup[RPG_MAX_ANIMS];
int rpg_anim_rate = 4;

int rpg_anim_start(int id, float x, float y, float h, int mirror, void *target)
{
    const RpgAnimation *a = rpg_anim_data(id);
    if (!a->id)
        return -1;
    int slot = -1;
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].target == target && anims[i].id && target) {
            slot = i;   /* dispose_animation: a new one replaces the target's current one */
            break;
        }
    for (int i = 0; slot < 0 && i < RPG_MAX_ANIMS; i++)
        if (!anims[i].id)
            slot = i;
    if (slot < 0)
        return -1;
    RpgAnimPlay *p = &anims[slot];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->rate_count = a->frame_max * rpg_anim_rate + 1;   /* @ani_duration */
    p->mirror = mirror;
    p->target = target;
    p->screen = a->position == 3;
    /* set_animation_origin: the target's centre, head (0) or foot (2) */
    p->x = p->screen ? rpg_screen_w / 2 : x;
    p->y = p->screen ? rpg_screen_h / 2 : (a->position == 0 ? y - h : a->position == 2 ? y : y - h / 2);
    p->target_h = h;
    /* a screen animation shown on several targets at once plays its sounds once */
    ani_dup[slot] = 0;
    if (p->screen)
        for (int i = 0; i < RPG_MAX_ANIMS; i++)
            if (i != slot && anims[i].id == id && anims[i].screen && anims[i].rate_count == p->rate_count)
                ani_dup[slot] = 1;
    return slot;
}

int rpg_anim_remaining(void *target)
{
    int r = 0;
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].id && anims[i].target == target && anims[i].rate_count > r)
            r = anims[i].rate_count;
    return r;
}

/* fast-forward: skip to the next cell frame (LNX battle speed style) */
void rpg_anims_fast(void *target)
{
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].id && (!target || anims[i].target == target) && anims[i].rate_count % rpg_anim_rate > 1)
            anims[i].rate_count -= anims[i].rate_count % rpg_anim_rate - 1;
}

int rpg_anim_any_of(int a, int b)
{
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].id && (anims[i].id == a || anims[i].id == b))
            return 1;
    return 0;
}

int rpg_anim_playing(void *target)
{
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].id && anims[i].target == target)
            return 1;
    return 0;
}

void rpg_anims_clear(void) { memset(anims, 0, sizeof anims); }

void (*rpg_anim_flash_hook)(void *target, const float color[4], int duration);

static void process_timing(RpgAnimPlay *p, int slot, const RpgTiming *t)
{
    if (!ani_dup[slot])
        rpg_se_play(RPG_STR(t->se.name), t->se.volume, t->se.pitch);
    int dur = t->flash_duration * rpg_anim_rate;
    switch (t->flash_scope) {
    case 1:   /* target */
        memcpy(p->flash, t->flash_color, sizeof p->flash);
        p->flash_duration = dur;
        if (rpg_anim_flash_hook)
            rpg_anim_flash_hook(p->target, t->flash_color, dur);
        break;
    case 2:   /* screen */
        if (!ani_dup[slot]) {
            RpgScreen *s = rpg.party.in_battle ? &rpg_battle_screen : &rpg.screen;
            memcpy(s->flash_color, t->flash_color, sizeof s->flash_color);
            s->flash_duration = dur;
        }
        break;
    case 3:   /* hide the target */
        p->hide_duration = dur;
        if (rpg_anim_flash_hook) {
            float none[4] = {0, 0, 0, -1};
            rpg_anim_flash_hook(p->target, none, dur);
        }
        break;
    }
}

static int frame_index(const RpgAnimPlay *p, const RpgAnimation *a)
{
    return a->frame_max - (p->rate_count + rpg_anim_rate - 1) / rpg_anim_rate;
}

void rpg_anims_update(void)
{
    for (int i = 0; i < RPG_MAX_ANIMS; i++) {
        RpgAnimPlay *p = &anims[i];
        if (!p->id)
            continue;
        const RpgAnimation *a = rpg_anim_data(p->id);
        if (p->flash_duration > 0)
            p->flash_duration--;
        if (p->hide_duration > 0)
            p->hide_duration--;
        p->rate_count--;
        if (p->rate_count % rpg_anim_rate == 0) {
            if (p->rate_count > 0) {
                int fi = frame_index(p, a);
                p->frame = fi;
                for (int k = 0; k < a->timings.count; k++) {
                    const RpgTiming *t = rpg_at(&rpg_db, RPG_T_TIMING, a->timings, k);
                    if (t->frame == fi)
                        process_timing(p, i, t);
                }
            } else {
                /* end_animation: the character's animation_id goes back to 0 */
                for (int e = 0; e <= rpg.nevents; e++) {
                    RpgChar *c = e < rpg.nevents ? &rpg.events[e] : &rpg.player.ch;
                    if (p->target == c)
                        c->animation_id = 0;
                }
                p->id = 0;
            }
        }
    }
}

void rpg_anim_target_flash(void *target, float out[4])
{
    out[3] = 0;
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].id && anims[i].target == target && anims[i].flash_duration > 0) {
            memcpy(out, anims[i].flash, sizeof anims[i].flash);
            out[3] = anims[i].flash[3] * anims[i].flash_duration / (float)(anims[i].flash_duration + 1);
        }
}

int rpg_anim_target_hidden(void *target)
{
    for (int i = 0; i < RPG_MAX_ANIMS; i++)
        if (anims[i].id && anims[i].target == target && anims[i].hide_duration > 0)
            return 1;
    return 0;
}

void rpg_anims_draw(void)
{
    for (int i = 0; i < RPG_MAX_ANIMS; i++) {
        RpgAnimPlay *p = &anims[i];
        if (!p->id || p->rate_count <= 0)
            continue;
        const RpgAnimation *a = rpg_anim_data(p->id);
        const RpgAnimFrame *f = rpg_at(&rpg_db, RPG_T_ANIMFRAME, a->frames, p->frame);
        if (!f)
            continue;
        int img1 = rpg_image_hue("animations", RPG_STR(a->animation1_name), a->animation1_hue);
        int img2 = rpg_image_hue("animations", RPG_STR(a->animation2_name), a->animation2_hue);
        for (int c = 0; c < f->cell_data.xsize && c < 16; c++) {
            int pattern = rpg_t16(&rpg_db, &f->cell_data, c, 0, 0);
            if (pattern < 0)
                continue;
            int cx = rpg_t16(&rpg_db, &f->cell_data, c, 1, 0), cy = rpg_t16(&rpg_db, &f->cell_data, c, 2, 0);
            int zoom = rpg_t16(&rpg_db, &f->cell_data, c, 3, 0), ang = rpg_t16(&rpg_db, &f->cell_data, c, 4, 0);
            int mir = rpg_t16(&rpg_db, &f->cell_data, c, 5, 0), op = rpg_t16(&rpg_db, &f->cell_data, c, 6, 0);
            int blend = rpg_t16(&rpg_db, &f->cell_data, c, 7, 0);
            int img = pattern < 100 ? img1 : img2;
            if (img < 0)
                continue;
            float x = p->mirror ? p->x - cx : p->x + cx, y = p->y + cy;
            float angle = p->mirror ? 360 - ang : ang;
            int mirror = p->mirror ? mir == 0 : mir == 1;
            float z = zoom / 100.f;
            /* ox = oy = 96: the cell's centre at (x, y) */
            float zx = mirror ? -z : z;
            int sx = pattern % 5 * 192, sy = pattern % 100 / 5 * 192;
            uint32_t col = rpg_rgba(255, 255, 255, op);
            if (angle == 0 || angle == 360)
                rpg_draw_part(img, sx, sy, 192, 192, x - 96 * zx, y - 96 * z, zx, z, 0, col, blend);
            else
                rpg_draw_part(img, sx, sy, 192, 192, x - 96 * zx, y - 96 * z, zx, z, angle, col, blend);
        }
    }
}

/* ------------------------------------------------------------------ balloons */
static void *balloon_targets[RPG_MAX_EVENTS + 1];

void rpg_balloons_update(void)
{
    for (int e = 0; e <= rpg.nevents; e++) {
        RpgChar *c = e < rpg.nevents ? &rpg.events[e] : &rpg.player.ch;
        if (c->balloon_id > 0 && c->balloon_duration == 0 && !balloon_targets[e]) {
            c->balloon_duration = 8 * 8 + 12;
            balloon_targets[e] = c;
        }
        if (c->balloon_duration > 0) {
            c->balloon_duration--;
            if (c->balloon_duration == 0) {
                c->balloon_id = 0;
                balloon_targets[e] = 0;
            }
        }
        /* a new map animation for the character */
        if (c->animation_id > 0 && !rpg_anim_playing(c)) {
            int h = 32;
            if (c->character_name[0]) {
                int img = rpg_image("characters", c->character_name);
                const char *n = c->character_name;
                int single = n[0] == '$' || (n[0] == '!' && n[1] == '$');
                if (img >= 0)
                    h = single ? rpg_image_h(img) / 4 : rpg_image_h(img) / 8;
            }
            if (rpg_anim_start(c->animation_id, rpg_char_screen_x(c), rpg_char_screen_y(c), h, 0, c) < 0)
                c->animation_id = 0;
        }
    }
}

void rpg_balloons_reset(void) { memset(balloon_targets, 0, sizeof balloon_targets); }

void rpg_balloon_draw(const RpgChar *c)
{
    static int img = -2;
    if (img == -2)
        img = rpg_image("system", "Balloon");
    if (img < 0 || c->balloon_duration <= 0)
        return;
    int f = (c->balloon_duration - 12) / 8;
    f = 7 - (f > 0 ? f : 0);
    int h = 32;
    if (c->character_name[0]) {
        int ci = rpg_image("characters", c->character_name);
        const char *n = c->character_name;
        int single = n[0] == '$' || (n[0] == '!' && n[1] == '$');
        if (ci >= 0)
            h = single ? rpg_image_h(ci) / 4 : rpg_image_h(ci) / 8;
    }
    float x = rpg_char_screen_x(c), y = rpg_char_screen_y(c) - h;
    rpg_draw_part(img, f * 32, (c->balloon_id - 1) * 32, 32, 32, x - 16, y - 32, 1, 1, 0, 0xffffffff, 0);
}
