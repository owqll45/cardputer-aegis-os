import re
from collections import Counter

path = r'c:\Users\awa\Documents\trae_projects\zd\awa\components\M5GFX\src\lgfx\Fonts\efont\lgfx_efont_cn.c'
text = open(path, 'r', encoding='ascii').read()
m = re.search(r'lgfx_efont_cn_10_b\[(\d+)\]\s*=\s*', text)
pos = m.end()
seg_re = re.compile(r'\s*"((?:[^"\\]|\\.)*)"', re.S)
raw_chars = 0
esc_len = Counter()
nseg = 0
while True:
    sm = seg_re.match(text, pos)
    if not sm:
        break
    s = sm.group(1)
    nseg += 1
    raw_chars += len(s)
    i = 0
    while i < len(s):
        if s[i] == '\\':
            j = i + 1
            if j < len(s) and s[j] in '01234567':
                k = j
                while k < len(s) and k - j < 3 and s[k] in '01234567':
                    k += 1
                esc_len[k - i] += 1
                i = k
            else:
                esc_len[2] += 1
                i += 2
        else:
            i += 1
    pos = sm.end()

decoded = raw_chars - sum((l - 1) * c for l, c in esc_len.items())
print('segments:', nseg, 'raw_chars:', raw_chars, 'decoded:', decoded, 'declared:', m.group(1))
print('escape lengths:', dict(esc_len))
# check for 8/9 right after a 3-digit octal escape would be literal; check \8 \9
weird = len(re.findall(r'\\[89]', text[m.end():pos]))
print('backslash-8/9 count:', weird)
