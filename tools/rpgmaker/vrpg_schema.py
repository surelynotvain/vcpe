"""VRPG schema: the record layouts of the VCPE RPG data format, shared by the converter (vrpg.py) and the C header
it generates (rpg/vcpe_rpg_db.h). One place defines every record, so the Python writer and the C structs can never
disagree.

Every field is 32 bits:
    i      int32 (nil -> 0, true -> 1)
    f      float
    s      string: byte offset into the document's string pool (0 = "")
    a      audio file: three fields name(s), volume(i), pitch(i)  -> RpgAudio in C
    tone   4 floats (red, green, blue, gray / alpha)               -> float[4]
    i*N    N ints from a Ruby array (padded with 0)
    r:T    a range into table T: first(i), count(i)                -> RpgRange
    list   an event command list: a range into the cmds table     -> RpgRange
    route  a move route: index into the routes table (-1 none)
    t16    a Ruby Table: a range into the int16 pool plus its xsize, ysize, zsize -> RpgTable
    sub:T  an embedded object laid out as record T (no range, inline)

Record names follow RPG Maker VX Ace's RPG:: classes; field names are the attribute names without '@'.
"""

SCHEMA = [
    # name, ruby class (for documentation), fields [(field, kind)]
    ('Feature', 'RPG::BaseItem::Feature', [('code', 'i'), ('data_id', 'i'), ('value', 'f')]),
    ('Effect', 'RPG::UsableItem::Effect', [('code', 'i'), ('data_id', 'i'), ('value1', 'f'), ('value2', 'f')]),
    ('Damage', 'RPG::UsableItem::Damage', [('type', 'i'), ('element_id', 'i'), ('formula', 's'), ('variance', 'i'),
                                          ('critical', 'i')]),
    ('Actor', 'RPG::Actor', [('id', 'i'), ('name', 's'), ('nickname', 's'), ('class_id', 'i'), ('initial_level', 'i'),
                             ('max_level', 'i'), ('character_name', 's'), ('character_index', 'i'), ('face_name', 's'),
                             ('face_index', 'i'), ('equips', 'i*5'), ('description', 's'), ('icon_index', 'i'),
                             ('features', 'r:Feature'), ('note', 's')]),
    ('Learning', 'RPG::Class::Learning', [('level', 'i'), ('skill_id', 'i'), ('note', 's')]),
    ('Class', 'RPG::Class', [('id', 'i'), ('name', 's'), ('exp_params', 'i*4'), ('params', 't16'),
                             ('learnings', 'r:Learning'), ('icon_index', 'i'), ('description', 's'),
                             ('features', 'r:Feature'), ('note', 's')]),
    ('Skill', 'RPG::Skill', [('id', 'i'), ('name', 's'), ('icon_index', 'i'), ('description', 's'),
                             ('features', 'r:Feature'), ('note', 's'), ('scope', 'i'), ('occasion', 'i'),
                             ('speed', 'i'), ('success_rate', 'i'), ('repeats', 'i'), ('tp_gain', 'i'),
                             ('hit_type', 'i'), ('animation_id', 'i'), ('damage', 'sub:Damage'),
                             ('effects', 'r:Effect'), ('stype_id', 'i'), ('mp_cost', 'i'), ('tp_cost', 'i'),
                             ('message1', 's'), ('message2', 's'), ('required_wtype_id1', 'i'),
                             ('required_wtype_id2', 'i')]),
    ('Item', 'RPG::Item', [('id', 'i'), ('name', 's'), ('icon_index', 'i'), ('description', 's'),
                           ('features', 'r:Feature'), ('note', 's'), ('scope', 'i'), ('occasion', 'i'), ('speed', 'i'),
                           ('success_rate', 'i'), ('repeats', 'i'), ('tp_gain', 'i'), ('hit_type', 'i'),
                           ('animation_id', 'i'), ('damage', 'sub:Damage'), ('effects', 'r:Effect'),
                           ('itype_id', 'i'), ('price', 'i'), ('consumable', 'i')]),
    ('Weapon', 'RPG::Weapon', [('id', 'i'), ('name', 's'), ('icon_index', 'i'), ('description', 's'),
                               ('features', 'r:Feature'), ('note', 's'), ('price', 'i'), ('etype_id', 'i'),
                               ('params', 'i*8'), ('wtype_id', 'i'), ('animation_id', 'i')]),
    ('Armor', 'RPG::Armor', [('id', 'i'), ('name', 's'), ('icon_index', 'i'), ('description', 's'),
                             ('features', 'r:Feature'), ('note', 's'), ('price', 'i'), ('etype_id', 'i'),
                             ('params', 'i*8'), ('atype_id', 'i')]),
    ('DropItem', 'RPG::Enemy::DropItem', [('kind', 'i'), ('data_id', 'i'), ('denominator', 'i')]),
    ('Action', 'RPG::Enemy::Action', [('skill_id', 'i'), ('condition_type', 'i'), ('condition_param1', 'f'),
                                      ('condition_param2', 'f'), ('rating', 'i')]),
    ('Enemy', 'RPG::Enemy', [('id', 'i'), ('name', 's'), ('icon_index', 'i'), ('description', 's'),
                             ('features', 'r:Feature'), ('note', 's'), ('battler_name', 's'), ('battler_hue', 'i'),
                             ('params', 'i*8'), ('exp', 'i'), ('gold', 'i'), ('drop_items', 'r:DropItem'),
                             ('actions', 'r:Action')]),
    ('State', 'RPG::State', [('id', 'i'), ('name', 's'), ('icon_index', 'i'), ('description', 's'),
                             ('features', 'r:Feature'), ('note', 's'), ('restriction', 'i'), ('priority', 'i'),
                             ('remove_at_battle_end', 'i'), ('remove_by_restriction', 'i'),
                             ('auto_removal_timing', 'i'), ('min_turns', 'i'), ('max_turns', 'i'),
                             ('remove_by_damage', 'i'), ('chance_by_damage', 'i'), ('remove_by_walking', 'i'),
                             ('steps_to_remove', 'i'), ('message1', 's'), ('message2', 's'), ('message3', 's'),
                             ('message4', 's')]),
    ('Member', 'RPG::Troop::Member', [('enemy_id', 'i'), ('x', 'i'), ('y', 'i'), ('hidden', 'i')]),
    ('TroopCond', 'RPG::Troop::Page::Condition', [('turn_ending', 'i'), ('turn_valid', 'i'),
                                                  ('enemy_valid', 'i'), ('actor_valid', 'i'), ('switch_valid', 'i'),
                                                  ('turn_a', 'i'), ('turn_b', 'i'), ('enemy_index', 'i'),
                                                  ('enemy_hp', 'i'), ('actor_id', 'i'), ('actor_hp', 'i'),
                                                  ('switch_id', 'i')]),
    ('TroopPage', 'RPG::Troop::Page', [('condition', 'sub:TroopCond'), ('span', 'i'), ('list', 'list')]),
    ('Troop', 'RPG::Troop', [('id', 'i'), ('name', 's'), ('members', 'r:Member'), ('pages', 'r:TroopPage')]),
    ('AnimFrame', 'RPG::Animation::Frame', [('cell_max', 'i'), ('cell_data', 't16')]),
    ('Timing', 'RPG::Animation::Timing', [('frame', 'i'), ('se', 'a'), ('flash_scope', 'i'), ('flash_color', 'tone'),
                                         ('flash_duration', 'i')]),
    ('Animation', 'RPG::Animation', [('id', 'i'), ('name', 's'), ('animation1_name', 's'), ('animation1_hue', 'i'),
                                     ('animation2_name', 's'), ('animation2_hue', 'i'), ('position', 'i'),
                                     ('frame_max', 'i'), ('frames', 'r:AnimFrame'), ('timings', 'r:Timing')]),
    ('Tileset', 'RPG::Tileset', [('id', 'i'), ('mode', 'i'), ('name', 's'), ('tileset_names', 's*9'),
                                 ('flags', 't16'), ('note', 's')]),
    ('CommonEvent', 'RPG::CommonEvent', [('id', 'i'), ('name', 's'), ('trigger', 'i'), ('switch_id', 'i'),
                                         ('list', 'list')]),
    ('Vehicle', 'RPG::System::Vehicle', [('character_name', 's'), ('character_index', 'i'), ('bgm', 'a'),
                                         ('start_map_id', 'i'), ('start_x', 'i'), ('start_y', 'i')]),
    ('Terms', 'RPG::System::Terms', [('basic', 's*8'), ('params', 's*8'), ('etypes', 's*5'), ('commands', 's*23')]),
    ('System', 'RPG::System', [('game_title', 's'), ('version_id', 'i'), ('japanese', 'i'), ('party_members', 'r:Int'),
                               ('currency_unit', 's'), ('elements', 'r:Str'), ('skill_types', 'r:Str'),
                               ('weapon_types', 'r:Str'), ('armor_types', 'r:Str'), ('switches', 'r:Str'),
                               ('variables', 'r:Str'), ('boat', 'sub:Vehicle'), ('ship', 'sub:Vehicle'),
                               ('airship', 'sub:Vehicle'), ('title1_name', 's'), ('title2_name', 's'),
                               ('opt_draw_title', 'i'), ('opt_use_midi', 'i'), ('opt_transparent', 'i'),
                               ('opt_followers', 'i'), ('opt_slip_death', 'i'), ('opt_floor_death', 'i'),
                               ('opt_display_tp', 'i'), ('opt_extra_exp', 'i'), ('window_tone', 'tone'),
                               ('title_bgm', 'a'), ('battle_bgm', 'a'), ('battle_end_me', 'a'), ('gameover_me', 'a'),
                               ('sounds', 'r:Audio'), ('test_troop_id', 'i'), ('start_map_id', 'i'), ('start_x', 'i'),
                               ('start_y', 'i'), ('terms', 'sub:Terms'), ('battleback1_name', 's'),
                               ('battleback2_name', 's'), ('battler_name', 's'), ('battler_hue', 'i'),
                               ('edit_map_id', 'i')]),
    ('MapInfo', 'RPG::MapInfo', [('id', 'i'), ('name', 's'), ('parent_id', 'i'), ('order', 'i'),
                                 ('expanded', 'i'), ('scroll_x', 'i'), ('scroll_y', 'i')]),
    # maps (each map is its own document holding exactly one Map record)
    ('Encounter', 'RPG::Map::Encounter', [('troop_id', 'i'), ('weight', 'i'), ('region_set', 'r:Int')]),
    ('PageCond', 'RPG::Event::Page::Condition', [('switch1_valid', 'i'), ('switch2_valid', 'i'),
                                                 ('variable_valid', 'i'), ('self_switch_valid', 'i'),
                                                 ('item_valid', 'i'), ('actor_valid', 'i'), ('switch1_id', 'i'),
                                                 ('switch2_id', 'i'), ('variable_id', 'i'), ('variable_value', 'i'),
                                                 ('self_switch_ch', 's'), ('item_id', 'i'), ('actor_id', 'i')]),
    ('PageGraphic', 'RPG::Event::Page::Graphic', [('tile_id', 'i'), ('character_name', 's'),
                                                  ('character_index', 'i'), ('direction', 'i'), ('pattern', 'i')]),
    ('Page', 'RPG::Event::Page', [('condition', 'sub:PageCond'), ('graphic', 'sub:PageGraphic'), ('move_type', 'i'),
                                  ('move_speed', 'i'), ('move_frequency', 'i'), ('move_route', 'route'),
                                  ('walk_anime', 'i'), ('step_anime', 'i'), ('direction_fix', 'i'), ('through', 'i'),
                                  ('priority_type', 'i'), ('trigger', 'i'), ('list', 'list')]),
    ('Event', 'RPG::Event', [('id', 'i'), ('name', 's'), ('x', 'i'), ('y', 'i'), ('pages', 'r:Page')]),
    ('Map', 'RPG::Map', [('display_name', 's'), ('tileset_id', 'i'), ('width', 'i'), ('height', 'i'),
                         ('scroll_type', 'i'), ('specify_battleback', 'i'), ('battleback1_name', 's'),
                         ('battleback2_name', 's'), ('autoplay_bgm', 'i'), ('bgm', 'a'), ('autoplay_bgs', 'i'),
                         ('bgs', 'a'), ('disable_dashing', 'i'), ('encounter_list', 'r:Encounter'),
                         ('encounter_step', 'i'), ('parallax_name', 's'), ('parallax_loop_x', 'i'),
                         ('parallax_loop_y', 'i'), ('parallax_sx', 'i'), ('parallax_sy', 'i'),
                         ('parallax_show', 'i'), ('note', 's'), ('data', 't16'), ('events', 'r:Event')]),
    # command lists and move routes (any document)
    ('Cmd', 'RPG::EventCommand', [('code', 'i'), ('indent', 'i'), ('params', 'r:Param')]),
    ('Route', 'RPG::MoveRoute', [('repeat', 'i'), ('skippable', 'i'), ('wait', 'i'), ('list', 'r:MoveCmd')]),
    ('MoveCmd', 'RPG::MoveCommand', [('code', 'i'), ('params', 'r:Param')]),
    # scalar tables
    ('Int', None, [('v', 'i')]),
    ('Str', None, [('v', 's')]),
    ('Param', None, [('v', 'i')]),
    ('Audio', 'RPG::AudioFile', [('name', 's'), ('volume', 'i'), ('pitch', 'i')]),
]

# the order of tables in a document (the C enum RPG_T_*)
TABLES = [name for name, _, _ in SCHEMA]
DB_ROOTS = ['Actor', 'Class', 'Skill', 'Item', 'Weapon', 'Armor', 'Enemy', 'State', 'Troop', 'Animation', 'Tileset',
            'CommonEvent', 'System', 'MapInfo']


def field_slots(kind):
    """32-bit slots a field kind takes."""
    if kind == 'a':
        return 3
    if kind == 'tone':
        return 4
    if kind.startswith('r:') or kind == 'list':
        return 2
    if kind == 't16':
        return 5
    if '*' in kind:
        return int(kind.split('*')[1])
    if kind.startswith('sub:'):
        return record_slots(kind[4:])
    return 1


def fields(name):
    for n, _, f in SCHEMA:
        if n == name:
            return f
    raise KeyError(name)


def record_slots(name):
    return sum(field_slots(k) for _, k in fields(name))


def c_header():
    """The C header rpg/vcpe_rpg_db.h (generated; run `python3 vrpg_schema.py > ../../rpg/vcpe_rpg_db.h`)."""
    out = ['/* VCPE RPG data records. GENERATED by tools/rpgmaker/vrpg_schema.py: do not edit, change the schema. */',
           '#ifndef VCPE_RPG_DB_H', '#define VCPE_RPG_DB_H', '#include <stdint.h>', '',
           'typedef struct { int32_t first, count; } RpgRange;',
           'typedef struct { int32_t name, volume, pitch; } RpgAudio;',
           'typedef struct { int32_t first, count, xsize, ysize, zsize; } RpgTable;', '',
           'enum {']
    out += ['    RPG_T_%s,' % n.upper() for n in TABLES]
    out += ['    RPG_NTABLES', '};', '']
    for name, ruby, fl in SCHEMA:
        if name == 'Audio':
            continue   # the RpgAudio typedef above
        if ruby:
            out.append('/* %s */' % ruby)
        out.append('typedef struct {')
        for f, k in fl:
            if k == 'i' or k == 'route':
                out.append('    int32_t %s;' % f)
            elif k == 'f':
                out.append('    float %s;' % f)
            elif k == 's':
                out.append('    int32_t %s;   /* string */' % f)
            elif k == 'a':
                out.append('    RpgAudio %s;' % f)
            elif k == 'tone':
                out.append('    float %s[4];' % f)
            elif k.startswith('r:'):
                out.append('    RpgRange %s;   /* -> %s */' % (f, k[2:]))
            elif k == 'list':
                out.append('    RpgRange %s;   /* -> Cmd */' % f)
            elif k == 't16':
                out.append('    RpgTable %s;' % f)
            elif k.startswith('sub:'):
                out.append('    Rpg%s %s;' % (k[4:], f))
            elif '*' in k:
                t, n = k.split('*')
                out.append('    int32_t %s[%s];%s' % (f, n, '   /* strings */' if t == 's' else ''))
        out.append('} Rpg%s;' % name)
        out.append('')
    out += ['#define RPG_DB_SLOTS { %s }' % ', '.join(str(record_slots(n)) for n in TABLES), '', '#endif', '']
    return '\n'.join(out)


def c_schema():
    """The C field table rpg/vcpe_rpg_schema.h used by the on-device converter (rpg/vcpe_rpg_cook_db.c); run
    `python3 vrpg_schema.py schema > ../../rpg/vcpe_rpg_schema.h`."""
    out = ['/* VCPE RPG schema as data, for the on-device converter. GENERATED by tools/rpgmaker/vrpg_schema.py. */',
           '#ifndef VCPE_RPG_SCHEMA_H', '#define VCPE_RPG_SCHEMA_H',
           'enum { RS_I, RS_F, RS_S, RS_A, RS_TONE, RS_T16, RS_ROUTE, RS_LIST, RS_SUB, RS_RANGE, RS_IARR, RS_SARR };',
           'typedef struct { const char *name; unsigned char kind, arg; } RpgSchemaField;   /* arg: table or count */',
           'typedef struct { const char *name; const RpgSchemaField *fields; int nfields, slots; } RpgSchemaTable;', '']
    idx = {n: i for i, n in enumerate(TABLES)}
    for name, _, fl in SCHEMA:
        rows = []
        for f, k in fl:
            if k in ('i', 'f', 's', 'a', 'tone', 't16', 'route', 'list'):
                kind, arg = 'RS_' + {'i': 'I', 'f': 'F', 's': 'S', 'a': 'A', 'tone': 'TONE', 't16': 'T16',
                                     'route': 'ROUTE', 'list': 'LIST'}[k], 0
            elif k.startswith('sub:'):
                kind, arg = 'RS_SUB', idx[k[4:]]
            elif k.startswith('r:'):
                kind, arg = 'RS_RANGE', idx[k[2:]]
            else:
                t, n = k.split('*')
                kind, arg = ('RS_SARR' if t == 's' else 'RS_IARR'), int(n)
            rows.append('{"@%s", %s, %d}' % (f, kind, arg))
        out.append('static const RpgSchemaField rs_%s[] = {%s};' % (name.lower(), ', '.join(rows)))
    out.append('static const RpgSchemaTable rpg_schema[] = {')
    for name, _, fl in SCHEMA:
        out.append('    {"%s", rs_%s, %d, %d},' % (name, name.lower(), len(fl), record_slots(name)))
    out.append('};')
    out.append('static const char *const rpg_schema_db_roots[] = {%s};' % ', '.join('"%s"' % r for r in DB_ROOTS))
    out += ['#endif', '']
    return '\n'.join(out)


if __name__ == '__main__':
    import sys
    print(c_schema() if sys.argv[1:] == ['schema'] else c_header(), end='')
