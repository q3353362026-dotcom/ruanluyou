"""Copy original local portraits and build the radar's HeroId catalog (development only)."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[2]
source = root / 'runtime/ErCore'
target = root / 'native/webui/radar'
portraits = target / 'portraits'
portraits.mkdir(exist_ok=True)
operators = json.loads((source / 'item_names_cn.json').read_text(encoding='utf-8'))['operators']
# Original liveview.HERO_NAME_OVERRIDE; retain its corrected display names.
overrides = {'88000000036': '佐娅', '88000000051': '旅人'}
result = {}
for hero_id in sorted(set(operators) | set(overrides)):
    name = overrides.get(hero_id) or operators[hero_id].get('Name')
    if not name:
        continue
    entry = {'name': name, 'portrait': None}
    image = source / 'web/avatars' / (name + '.png')
    if image.is_file():
        output = portraits / (hero_id + '.png')
        shutil.copyfile(image, output)
        assert hashlib.sha256(image.read_bytes()).digest() == hashlib.sha256(output.read_bytes()).digest()
        entry['portrait'] = '/radar/portraits/' + output.name
    result[hero_id] = entry
(target / 'operators.json').write_text(json.dumps({
    'source': 'Original item_names_cn.json operators.Name and liveview.HERO_NAME_OVERRIDE; original local PNGs',
    'operators': result
}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'{len(result)} names, {sum(bool(v["portrait"]) for v in result.values())} local portraits')
