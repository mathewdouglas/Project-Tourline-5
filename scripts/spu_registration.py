"""Remove unused diagnostic strings from the lifter's private SPU table.

Dispatch registers only the guest address and host function pointer. Keeping
the unused name duplicates thousands of linker symbols in the player binary.
The symbols themselves remain available to debuggers and profiling tools.
"""
import re


TYPE = 'typedef struct { uint32_t addr; void (*func)(spu_context*); const char* name; } spu_func_entry;'
COMPACT_TYPE = 'typedef struct { uint32_t addr; void (*func)(spu_context*); } spu_func_entry;'
TABLE = re.compile(r'(static const spu_func_entry spu_function_table\[\] = \{\n)(.*?)(\n\};)', re.S)
ROW = re.compile(r'    \{ (0x[0-9A-Fa-f]+u), (\w+), "(\w+)" \},')
COMPACT_ROW = re.compile(r'    \{ 0x[0-9A-Fa-f]+u, \w+ \},')


def compact_registration(source):
    """Preserve every registration pair, order and sentinel; reject format drift."""
    match = TABLE.search(source)
    compact = COMPACT_TYPE in source
    if not match or source.count(COMPACT_TYPE if compact else TYPE) != 1:
        raise ValueError('Unexpected SPU registration table layout')
    # The upstream lifter currently never reads the name. Do not silently drop
    # metadata if a future version starts using it.
    if re.search(r'\be\s*->\s*name\b', source):
        raise ValueError('SPU registration names are now consumed')
    lines = match[2].splitlines()
    sentinel = '    { 0, NULL }' if compact else '    { 0, NULL, NULL }'
    if not lines or lines[-1] != sentinel:
        raise ValueError('Unexpected SPU registration sentinel')
    rows = []
    for line in lines[:-1]:
        if compact:
            if not COMPACT_ROW.fullmatch(line):
                raise ValueError('Unexpected compact SPU registration row')
            rows.append(line)
        else:
            row = ROW.fullmatch(line)
            if not row or row[2] != row[3]:
                raise ValueError('Unexpected SPU registration name or row')
            rows.append(f'    {{ {row[1]}, {row[2]} }},')
    table = match[1] + '\n'.join(rows + ['    { 0, NULL }']) + match[3]
    return (source[:match.start()] + table + source[match.end():]).replace(TYPE, COMPACT_TYPE)
