"""Write the row-level "members" flags of the runtime teleport datasets.

A free-to-play account plans with WW_RESTRICT_FREE_TO_PLAY set, and the planner
then refuses every transition format::isMembersOnly calls members. For a
teleport it can only see the landing tile, which datasets/f2p_zones.json
classifies. What it cannot see is a teleport that is members-only although it
lands on free-to-play land: an Ancient Magicks spell to Ice Mountain, a
members item, House Teleport into Rimmington. This script supplies those from
the cache, as `"members": true` on the row:

  spell_teleports.json   a spell is members when its ability struct (the struct
                         whose param 2794 is the spell's name) has param 2809
                         COMBATV2_ABILITY_IS_MEMBERS = 1, or when any item it
                         needs (all are consumed: AND) is a members item.
  item_teleports.json    an item teleport (`teleports[]`) is members when every
     teleports[]         item that can cast it (alternatives: OR) is a members
                         item. Item `isMembers` is item opcode 16.
     lodestones          a lodestone is free when cache enum 12260
                         CHEEVO_LODESTONE_FREE names it, members when enum 12261
                         CHEEVO_LODESTONE_MEMBERS does, and otherwise takes its
                         flag from CURATED_LODESTONES below. Lodestones get the
                         flag both ways, true and false.

A row that the cache does not make members gets no flag, so its landing decides
at plan time, against whatever f2p_zones.json says then. The script never
writes "members": false on a spell or item teleport: a false flag beats
geography (format::isMembersOnly), and nothing in the cache can vouch for the
landing. Each run rewrites the flags from scratch, so re-run it after editing
either file or after a game update, and commit the result.

The bake (wwbuild build) holds the lodestone flags to the same enums and fails
on a mismatch, so a stale enum list here cannot ship.

    python tools/f2p/gen_members_flags.py <item.json> <struct.json> [datasets dir] [--check]

<item.json> and <struct.json> are NXTCacheLibrary's dumper output for the item
and struct config types (the "RuneScape Data Dump" item/item.json and
struct/struct.json: {"entries": [{"id", ...}]}). --check rewrites nothing and
exits 1 when a file would change.
"""

import json
import pathlib
import sys

# Cache enum 12260 CHEEVO_LODESTONE_FREE / 12261 CHEEVO_LODESTONE_MEMBERS, by the
# destination names item_teleports.json uses. Read from the live cache by the
# research pass of 2026-10-07; the bake re-checks them (src/build/MembersReport.cpp).
FREE_LODESTONES = {
    'Al Kharid', 'Ashdale', 'Burthorpe', 'Draynor Village', 'Edgeville', 'Falador',
    'Lumbridge', 'Port Sarim', 'Taverley', 'Varrock', 'Wilderness Crater',
}
MEMBERS_LODESTONES = {
    'Ardougne', 'Bandit Camp', 'Canifis', 'Catherby', "Eagles' Peak", 'Fremennik Province',
    'Karamja', 'Lunar Isle', "Oo'glog", 'Prifddinas', "Seers' Village", 'Tirannwn',
    'Yanille', 'Menaphos', 'Anachronia',
}
# Lodestones in neither enum (added after the achievement that the enums back).
# Each value is (is_members, source). See datasets/README.md "Free-to-play".
CURATED_LODESTONES = {
    'Fort Forinthry': (True, 'https://runescape.wiki/w/Fort_Forinthry_lodestone: "the only '
                             'members-only lodestone to be located in the free-to-play part of '
                             'the world"'),
    'City of Um': (False, 'https://runescape.wiki/w/City_of_Um (members = No) and '
                          'https://runescape.wiki/w/Free-to-play (Necromancy, City of Um)'),
    'Wendlewick': (False, 'https://runescape.wiki/w/Wendlewick (members = No) and '
                          'https://runescape.wiki/w/Free-to-play (Havenhythe, 23 March 2026)'),
}

PARAM_ABILITY_NAME = '2794'
PARAM_ABILITY_IS_MEMBERS = '2809'


def load_entries(path):
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)
    return doc['entries'] if isinstance(doc, dict) else doc


def members_items(item_entries):
    return {e['id'] for e in item_entries if e.get('isMembers')}


def members_spells(struct_entries):
    """Ability name -> whether its struct marks it members (param 2809 = 1).
    A struct with the name and no 2809 is a free ability: the param is
    autoDisable with default 0."""
    out = {}
    for e in struct_entries:
        params = e.get('params') or {}
        name = params.get(PARAM_ABILITY_NAME)
        if isinstance(name, str):
            # Several structs can carry one name; members if any says so.
            out[name] = out.get(name, False) or params.get(PARAM_ABILITY_IS_MEMBERS) == 1
    return out


def item_ids(row):
    return [i['id'] for i in (row.get('requirements') or {}).get('items', [])]


def spell_is_members(row, spells, items):
    """(is_members, reason) for one spell_teleports row."""
    needed = item_ids(row)
    if spells.get(row.get('name')):
        return True, 'ability param 2809'
    members = [i for i in needed if i in items]
    if members:
        return True, 'members item %s' % members
    return False, ''


def item_teleport_is_members(row, items):
    alternatives = item_ids(row)
    if alternatives and all(i in items for i in alternatives):
        return True, 'every item is members %s' % alternatives
    return False, ''


def lodestone_is_members(row):
    name = row.get('name')
    if name in FREE_LODESTONES:
        return False, 'enum 12260'
    if name in MEMBERS_LODESTONES:
        return True, 'enum 12261'
    if name in CURATED_LODESTONES:
        return CURATED_LODESTONES[name][0], 'curated: ' + CURATED_LODESTONES[name][1]
    raise SystemExit('lodestone %r is in neither enum and not curated' % name)


def with_flag(row, flag):
    """`row` with "members" set to `flag` (None removes it), placed after "name"."""
    out = {}
    for key, value in row.items():
        if key == 'members':
            continue
        out[key] = value
        if key == 'name' and flag is not None:
            out['members'] = flag
    if flag is not None and 'members' not in out:
        out['members'] = flag
    return out


def apply_spells(doc, spells, items, log):
    rows = doc['teleports']
    for i, row in enumerate(rows):
        is_members, why = spell_is_members(row, spells, items)
        if row.get('name') not in spells:
            log.append('spell %r: no ability struct of that name; items only' % row.get('name'))
        rows[i] = with_flag(row, True if is_members else None)
        log.append('spell %-34s %s %s' % (row.get('name'), 'MEMBERS' if is_members else '-', why))


def apply_items(doc, items, log):
    rows = doc['teleports']
    for i, row in enumerate(rows):
        is_members, why = item_teleport_is_members(row, items)
        rows[i] = with_flag(row, True if is_members else None)
        log.append('item  %-34s %s %s' % (row.get('name'), 'MEMBERS' if is_members else '-', why))
    lodes = doc['lodestones']['destinations']
    for i, row in enumerate(lodes):
        is_members, why = lodestone_is_members(row)
        lodes[i] = with_flag(row, is_members)
        log.append('lode  %-34s %s %s' % (row.get('name'), 'MEMBERS' if is_members else 'free', why))


def render(path, doc):
    """Serialise the way the datasets are stored: two-space indent, the file's
    own line ending, and its own trailing newline (or none)."""
    raw = path.read_bytes()
    newline = '\r\n' if b'\r\n' in raw else '\n'
    text = json.dumps(doc, indent=2, ensure_ascii=False).replace('\n', newline)
    if raw.endswith(newline.encode()):
        text += newline
    return text.encode('utf-8')


def main(argv):
    args = [a for a in argv[1:] if a != '--check']
    is_check = '--check' in argv
    if len(args) < 2:
        print(__doc__)
        return 2
    items = members_items(load_entries(args[0]))
    spells = members_spells(load_entries(args[1]))
    datasets = pathlib.Path(args[2] if len(args) > 2 else 'datasets')

    log = []
    spell_path = datasets / 'spell_teleports.json'
    item_path = datasets / 'item_teleports.json'
    spell_doc = json.loads(spell_path.read_bytes())
    item_doc = json.loads(item_path.read_bytes())
    apply_spells(spell_doc, spells, items, log)
    apply_items(item_doc, items, log)
    print('\n'.join(log))

    changed = []
    for path, doc in ((spell_path, spell_doc), (item_path, item_doc)):
        out = render(path, doc)
        if out != path.read_bytes():
            changed.append(path)
            if not is_check:
                path.write_bytes(out)
    print('%s: %s' % ('would change' if is_check else 'rewrote',
                      ', '.join(str(p) for p in changed) or 'nothing'))
    return 1 if is_check and changed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
