"""Registration metadata removal preserves executable code and dispatch pairs."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from spu_registration import TYPE, COMPACT_TYPE, compact_registration


PREFIX = '/* unrelated diagnostic text */\nvoid a(spu_context* ctx) { ctx->pc = 4; }\n'
SUFFIX = '''
void register_image(void) {
    for (const spu_func_entry* e = spu_function_table; e->func; ++e)
        spu_register_function(e->addr, e->func);
}
'''
TABLE = '''
static const spu_func_entry spu_function_table[] = {
    { 0x00000004u, a, "a" },
    { 0x00000008u, b, "b" },
    { 0, NULL, NULL }
};'''


class Registration(unittest.TestCase):
    def test_only_unused_column_removed(self):
        source = PREFIX + TYPE + TABLE + SUFFIX
        expected = PREFIX + COMPACT_TYPE + TABLE.replace(', "a"', '').replace(', "b"', '').replace(
            '{ 0, NULL, NULL }', '{ 0, NULL }') + SUFFIX
        self.assertEqual(compact_registration(source), expected)
        self.assertEqual(compact_registration(expected), expected)

    def test_empty_table(self):
        source = TYPE + TABLE.replace('    { 0x00000004u, a, "a" },\n', '').replace(
            '    { 0x00000008u, b, "b" },\n', '') + SUFFIX
        self.assertIn('{ 0, NULL }', compact_registration(source))

    def test_format_changes_fail_closed(self):
        source = TYPE + TABLE + SUFFIX
        for changed in (source.replace('"a"', '"other"'), source.replace('NULL, NULL', 'NULL, "end"'),
                        source.replace('e->addr', 'e->name'), source.replace('spu_function_table[]', 'different[]')):
            with self.assertRaises(ValueError):
                compact_registration(changed)


if __name__ == '__main__':
    unittest.main()
