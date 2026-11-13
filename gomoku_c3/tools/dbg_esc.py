import re
from collections import Counter

path = r'c:\Users\awa\Documents\trae_projects\zd\awa\components\M5GFX\src\lgfx\Fonts\efont\lgfx_efont_cn.c'
text = open(path, 'r', encoding='utf-8', errors='replace').read()
m = re.search(r'lgfx_efont_cn_10_b\[(\d+)\]\s*=\s*', text)
pos = m.end()
seg_re = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
esc = Counter()
while True:
    sm = seg_re.match(text, pos)
    if not sm:
        break
    s = sm.group(1)
    for em in re.finditer(r'\\(?:x[0-9a-fA-F]+|[0-7]{1,3}|.)', s):
        esc[em.group(0)[1:2]] += 1
    pos = sm.end()
for k, v in sorted(esc.items()):
    print(repr(k), v)
