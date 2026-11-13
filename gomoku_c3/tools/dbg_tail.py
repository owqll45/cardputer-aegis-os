import re

path = r'c:\Users\awa\Documents\trae_projects\zd\awa\components\M5GFX\src\lgfx\Fonts\efont\lgfx_efont_cn.c'
text = open(path, 'r', encoding='utf-8', errors='replace').read()
m = re.search(r'lgfx_efont_cn_10_b\[(\d+)\]\s*=\s*', text)
pos = m.end()
seg_re = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
count = 0
while True:
    sm = seg_re.match(text, pos)
    if not sm:
        break
    count += 1
    pos = sm.end()
print('declared:', m.group(1))
print('segments:', count)
print('tail repr:', repr(text[pos:pos+300]))
