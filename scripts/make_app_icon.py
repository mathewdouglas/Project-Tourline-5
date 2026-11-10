"""Convert the supplied PT5 logo to Windows icon sizes without changing the artwork.

Run with a Python environment containing Pillow:
    python scripts/make_app_icon.py
The checked-in ICO is used directly by the build; Pillow is not a build dependency.
"""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/branding'


def main():
    with Image.open(ASSETS / 'pt5-logo.png') as source:
        image = source.convert('RGBA')
        image.save(ASSETS / 'pt5.ico', format='ICO',
                   sizes=[(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
    print(ASSETS / 'pt5.ico')


if __name__ == '__main__':
    main()
