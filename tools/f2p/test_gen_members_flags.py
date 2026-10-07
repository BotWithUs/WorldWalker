"""Tests for gen_members_flags.py. Run: python -m unittest tools/f2p/test_gen_members_flags.py"""

import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_members_flags as gen  # noqa: E402

ITEMS = {9075}  # astral rune: members
STRUCTS = [
    {'id': 1, 'params': {'2794': 'Lassar Teleport', '2809': 1}},
    {'id': 2, 'params': {'2794': 'Varrock Teleport'}},
    {'id': 3, 'params': {'2794': 'Varrock Teleport', '2809': 0}},
]


def spell(name, *item_ids, **extra):
    row = {'name': name, 'dest_x': 1, 'dest_y': 1, 'dest_plane': 0}
    if item_ids:
        row['requirements'] = {'items': [{'id': i, 'count': 1} for i in item_ids]}
    row.update(extra)
    return row


class SpellTests(unittest.TestCase):
    def setUp(self):
        self.spells = gen.members_spells(STRUCTS)

    def test_ability_param_makes_a_spell_members(self):
        self.assertTrue(gen.spell_is_members(spell('Lassar Teleport', 555), self.spells, ITEMS)[0])

    def test_a_struct_without_2809_is_free(self):
        self.assertIs(self.spells['Varrock Teleport'], False)
        self.assertFalse(gen.spell_is_members(spell('Varrock Teleport', 556), self.spells, ITEMS)[0])

    def test_any_members_rune_makes_a_spell_members(self):
        self.assertTrue(gen.spell_is_members(spell('Unknown', 556, 9075), self.spells, ITEMS)[0])


class ItemTeleportTests(unittest.TestCase):
    def test_members_only_when_every_alternative_is(self):
        self.assertTrue(gen.item_teleport_is_members(spell('x', 9075), ITEMS)[0])
        self.assertFalse(gen.item_teleport_is_members(spell('x', 9075, 1), ITEMS)[0])

    def test_no_item_gate_is_not_members(self):
        self.assertFalse(gen.item_teleport_is_members(spell('x'), ITEMS)[0])


class LodestoneTests(unittest.TestCase):
    def test_enums_and_curated(self):
        self.assertEqual(gen.lodestone_is_members({'name': 'Lumbridge'})[0], False)
        self.assertEqual(gen.lodestone_is_members({'name': 'Karamja'})[0], True)
        self.assertEqual(gen.lodestone_is_members({'name': 'Fort Forinthry'})[0], True)

    def test_an_unknown_lodestone_stops_the_run(self):
        with self.assertRaises(SystemExit):
            gen.lodestone_is_members({'name': 'Nowhere'})


class FlagTests(unittest.TestCase):
    def test_flag_goes_after_name_and_is_replaced(self):
        row = {'name': 'a', 'members': False, 'dest_x': 1}
        self.assertEqual(list(gen.with_flag(row, True).items()),
                         [('name', 'a'), ('members', True), ('dest_x', 1)])

    def test_none_removes_the_flag(self):
        self.assertNotIn('members', gen.with_flag({'name': 'a', 'members': True}, None))

    def test_render_keeps_the_files_line_ending(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'x.json'
            doc = {'teleports': [{'name': 'a'}]}
            path.write_bytes(json.dumps(doc, indent=2).replace('\n', '\r\n').encode() + b'\r\n')
            self.assertEqual(gen.render(path, doc), path.read_bytes())


if __name__ == '__main__':
    unittest.main()
