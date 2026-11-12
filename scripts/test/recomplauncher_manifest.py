"""Check launcher/recomplauncher.json against the game's settings file, without running anything.

RecompLauncher edits TOML line by line (src-tauri/src/profile.rs in elripalda/RecompLauncher).
This reimplements that editor's key matching and insertion closely enough to check that:

- Play starts tourline5.exe, never gt5.exe directly;
- every key a setting writes is one gt5p_settings.cpp reads, in its section;
- each choice, applied to a file in the game's own saved layout, leaves valid TOML whose value
  the game accepts, changes only that line, and reads back as the same choice;
- `config.new` on a missing file writes exactly the launcher's first-run preset;
- the settings file paths follow the selected-storage link, then the default folder.
"""
import json
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'launcher/recomplauncher.json'

# Values gt5p_settings.cpp accepts per key (parse_into); anything else is a warning there.
ACCEPTED = {
    ('video', 'resolution'): {'720p', '1080p', '1440p', '2160p'},
    ('video', 'aspect'): {'16:9', '21:9'},
    ('video', 'window'): {'windowed', 'borderless'},
    ('video', 'msaa'): {1, 2, 4, 8},
    ('game', 'anisotropy'): {1, 4, 8, 16},
    ('input', 'button_prompts'): {'auto', 'playstation', 'xbox', 'nintendo', 'keyboard'},
    ('video', 'shader_compiler'): {'dxc', 'fxc'},
    ('input', 'trigger_deadzone'): set(range(21)),
    ('input', 'trigger_curve'): {0, 1, 2},
}
BOOLEAN = {True, False}

# The layout gt5p_settings_save() writes (comments shortened), with its own defaults.
GAME_FILE = """# Gran Turismo 5 Prologue (ps3recomp) settings.

[video]
# Display mode.
resolution = "1080p"
aspect = "16:9"
window = "windowed"
msaa = 4
vsync = true
shader_cache = true
shader_compiler = "dxc"
shader_indicator = true
preload_indicator = false
shader_precompile = true
shader_warmup = true
shader_warmup_before_start = false
pipeline_async = true
race_preload = true

[input]
sdl_controllers = true
keyboard = true
button_prompts = "auto"
trigger_deadzone = 0
trigger_curve = 0
adaptive_triggers = false
trigger_rumble = false
sdl_rumble = false

[keyboard]
cross = "Z"

[game]
skip_movie = false
smooth_colour = true
anisotropy = 16

[performance]
vblank_hz = 59.94

[debug]
verbose_log = "auto"

[meta]
defaults_version = 1
"""


def key_line(key):
    return re.compile(r'^(\s*' + re.escape(key) + r'\s*=\s*)((?:"[^"]*"|[^#\s])*)(.*)$')


class TomlFile:
    """profile.rs File for format "toml"."""

    def __init__(self, text=None):
        self.existed = text is not None
        self.lines = (text or '').splitlines()

    @staticmethod
    def header(line):
        t = line.strip()
        if t.startswith('[') and not t.startswith('[[') and ']' in t:
            return t[1:t.index(']')].strip()
        return None

    def range(self, sec):
        if sec is None:
            return 0, len(self.lines)
        starts = [i for i, l in enumerate(self.lines) if self.header(l) == sec]
        if not starts:
            return None
        a = starts[0] + 1
        b = next((i for i in range(a, len(self.lines)) if self.lines[i].strip().startswith('[')), len(self.lines))
        return a, b

    @staticmethod
    def split(key):
        sec, _, k = key.partition('/')
        return (sec.strip(), k.strip()) if k.strip() and sec.strip() else (None, key)

    def get(self, key):
        sec, k = self.split(key)
        r = self.range(sec)
        if r is None:
            return None
        for line in self.lines[r[0]:r[1]]:
            m = key_line(k).match(line)
            if m:
                return m[2]
        return None

    def set(self, key, value):
        sec, k = self.split(key)
        r = self.range(sec)
        if r is not None:
            a, b = r
            for i in range(a, b):
                m = key_line(k).match(self.lines[i])
                if m:
                    self.lines[i] = m[1] + value + m[3]
                    return
            off = re.compile(r'^\s*[;#]+\s*(' + re.escape(k) + r'\s*=\s*)')
            for i in range(a, b):
                m = off.match(self.lines[i])
                if m:
                    self.lines[i] = m[1] + value
                    return
            at = b
            if sec is None:
                at = next((i for i, l in enumerate(self.lines) if l.strip().startswith('[')), len(self.lines))
            at = next((i + 1 for i in range(at - 1, a - 1, -1) if self.lines[i].strip()), a)
            self.lines.insert(at, f'{k} = {value}')
            return
        if self.lines and self.lines[-1].strip():
            self.lines.append('')
        self.lines.append(f'[{sec or ""}]')
        self.lines.append(f'{k} = {value}')

    def text(self):
        return ''.join(l + '\n' for l in self.lines)


def same(a, b):
    un = lambda x: x.strip().strip('"\'').strip()
    a, b = un(a), un(b)
    try:
        return abs(float(a) - float(b)) < 1e-6
    except ValueError:
        return a.lower() == b.lower()


def current(setting, f):
    val = lambda k: f.get(k) if f.get(k) is not None else setting.get('defaults', {}).get(k, '')
    for i, c in enumerate(setting['choices']):
        if all(same(val(k), v) for k, v in c['set'].items()):
            return i
    return None


def known_keys():
    source = (ROOT / 'src/gt5p_settings.cpp').read_text(encoding='utf-8')
    block = source[source.index('kKnown[] = {'):source.index('};', source.index('kKnown[] = {'))]
    return set(re.findall(r'\{"(\w+)",\s*"(\w+)"\}', block))


def launcher_preset():
    source = (ROOT / 'launcher/launcher.cpp').read_text(encoding='utf-8')
    m = re.search(r'f << "(# Project Tourline 5 first-run preset[^;]*)";', source)
    assert m, 'first-run preset not found in launcher.cpp'
    return tomllib.loads(m[1].encode().decode('unicode_escape'))


def check_value(section, key, value):
    if (section, key) in ACCEPTED:
        assert value in ACCEPTED[(section, key)], f'{section}.{key} = {value!r} is not accepted by the game'
    else:
        assert value in BOOLEAN, f'{section}.{key} = {value!r}: expected true or false'


def main():
    doc = json.loads(MANIFEST.read_text(encoding='utf-8'))
    assert doc['format'] == 1
    assert doc['start']['program'] == ['tourline5.exe'], 'Play must go through the Tourline launcher'
    assert not doc['start'].get('args'), 'the Tourline launcher takes no arguments for a normal launch'
    assert 'game' not in doc, 'named inputs carry the disc and installed update separately'
    inputs = {item['id']: item for item in doc['inputs']}
    assert set(inputs) == {'disc', 'update', 'executable'}
    for name in ('disc', 'update'):
        assert not inputs[name].get('optional')
        assert inputs[name]['formats'] == [{'type': 'folder'}], 'ISO import is unsupported'
    assert inputs['executable']['optional']
    assert inputs['executable']['formats'] == [{'type': 'file', 'extensions': ['elf', 'exe']}]
    assert all('destination' not in item for item in inputs.values()), 'keep sources outside the install'
    assert doc['prepare']['program'] == ['tourline5.exe']
    assert doc['prepare']['args'] == ['--recomplauncher-prepare', '--game', '{input:disc}',
                                      '--game-data', '{input:update}', '--executable={input:executable}']
    assert doc['prepare']['timeout'] >= 3600
    print('Easy Setup: disc/update folders and optional ELF/RPCS3, noninteractive prepare')
    print('start: tourline5.exe')

    (config,) = doc['config']
    assert config['id'] == 'main' and config['format'] == 'toml'
    link = '{local}/ProjectTourline5/selected-storage'
    assert config['file'] == [{'path': link + '/gt5p_settings.toml', 'if': link},
                              {'path': '{local}/ProjectTourline5/gt5p_settings.toml'}]
    for name, places in doc['folders'].items():
        assert places[0]['if'] == link and places[0]['path'].startswith(link)
        assert 'if' not in places[-1] and places[-1]['path'].startswith('{local}/ProjectTourline5')
    assert not config.get('always') and not config.get('ensure') and not config.get('game_path')
    print('settings file: selected-storage link, then default storage')

    known = known_keys()
    written = set(config['new']) | {k for s in doc['settings'] for c in s['choices'] for k in c['set']}
    for key in written:
        section, _, name = key.partition('/')
        assert (section, name) in known, f'{key} is not a key gt5p_settings.cpp reads'
    print(f'{len(written)} keys: all read by the game')

    fresh = TomlFile()
    for k, v in config['new'].items():
        fresh.set(k, v)
    parsed = tomllib.loads(fresh.text())
    assert parsed == launcher_preset(), (parsed, launcher_preset())
    for section, values in parsed.items():
        for key, value in values.items():
            if (section, key) in ACCEPTED or isinstance(value, bool):
                check_value(section, key, value)
    print('config.new: identical to the launcher first-run preset')

    ids = set()
    for setting in doc['settings']:
        assert setting['id'] not in ids
        ids.add(setting['id'])
        assert current(setting, TomlFile(GAME_FILE)) is not None, f'{setting["id"]}: no choice matches the game file'
        assert current(setting, fresh) is not None, f'{setting["id"]}: no choice matches a new file'
        for i, choice in enumerate(setting['choices']):
            f = TomlFile(GAME_FILE)
            for k, v in choice['set'].items():
                f.set(k, v)
            after = f.text()
            changed = [(a, b) for a, b in zip(GAME_FILE.splitlines(), after.splitlines()) if a != b]
            assert len(after.splitlines()) == len(GAME_FILE.splitlines()), f'{setting["id"]}: lines added'
            assert len(changed) <= len(choice['set']), f'{setting["id"]}: unrelated lines changed'
            parsed = tomllib.loads(after)
            for k in choice['set']:
                section, _, name = k.partition('/')
                check_value(section, name, parsed[section][name])
            assert current(setting, f) == i, f'{setting["id"]}: choice {i} reads back as {current(setting, f)}'
        defaults = setting.get('defaults', {})
        assert set(defaults) == {k for c in setting['choices'] for k in c['set']}, setting['id']
    print(f'{len(ids)} settings: every choice round-trips and keeps the rest of the file')
    print('recomplauncher.json: passed')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as e:
        print('FAILED:', e)
        sys.exit(1)
