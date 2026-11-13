import re, sys

SRC = r"c:\Users\awa\Documents\trae_projects\zd\awa\components\M5GFX\src\lgfx\Fonts\efont\lgfx_efont_cn.c"
DST = r"c:\Users\awa\Documents\trae_projects\zd\gomoku_c3\main\efont_data.c"

text = open(SRC, "r", encoding="utf-8", errors="ignore").read()

want = ["lgfx_efont_cn_10_b", "lgfx_efont_cn_12", "lgfx_efont_cn_14"]

def parse_c_string(s):
    out = bytearray()
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '\\':
            # 转义
            if i + 1 < n and s[i+1] in '01234567':
                j = i + 1
                val = 0
                cnt = 0
                while j < n and cnt < 3 and s[j] in '01234567':
                    val = val * 8 + int(s[j], 8)
                    j += 1
                    cnt += 1
                out.append(val & 0xFF)
                i = j
            elif i + 1 < n and s[i+1] == 'x':
                j = i + 2
                val = 0
                while j < n and s[j] in '0123456789abcdefABCDEF':
                    val = val * 16 + int(s[j], 16)
                    j += 1
                out.append(val & 0xFF)
                i = j
            elif i + 1 < n:
                m = {'n': 10, 'r': 13, 't': 9, '\\': 92, '"': 34, "'": 39, '0': 0}
                ch = s[i+1]
                out.append(m.get(ch, ord(ch)))
                i += 2
            else:
                i += 1
        else:
            out.append(ord(c) & 0xFF)
            i += 1
    return bytes(out)

out = []
out.append("#include <stdint.h>\n")

for name in want:
    m = re.search(re.escape(name) + r"\[(\d+)\]\s*=\s*", text)
    if not m:
        print("NOT FOUND:", name)
        sys.exit(1)
    size = int(m.group(1))
    pos = m.end()
    # 逐段匹配字符串字面量,直到遇到分号
    data = bytearray()
    seg_re = re.compile(r'\s*"((?:[^"\\]|\\.)*)"', re.S)
    while True:
        sm = seg_re.match(text, pos)
        if not sm:
            break
        data += parse_c_string(sm.group(1))
        pos = sm.end()
    assert text[pos:].lstrip().startswith(';'), name + " terminator not found"
    # C 字符串字面量初始化数组时会带上隐式结尾 NUL,声明大小 = 内容 + 1
    if len(data) == size - 1:
        data.append(0)
    if len(data) != size:
        print(name, "size mismatch:", len(data), "!=", size)
        sys.exit(1)
    out.append("const uint8_t %s[%d] = {" % (name, size))
    for i in range(0, size, 16):
        out.append(",".join("0x%02x" % b for b in data[i:i+16]) + ",")
    out.append("};\n")
    print(name, "ok", size)

open(DST, "w", encoding="utf-8").write("\n".join(out))
print("written", DST)
