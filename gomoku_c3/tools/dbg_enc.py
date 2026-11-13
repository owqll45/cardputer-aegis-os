path = r'c:\Users\awa\Documents\trae_projects\zd\awa\components\M5GFX\src\lgfx\Fonts\efont\lgfx_efont_cn.c'
raw = open(path, 'rb').read()
u = raw.decode('utf-8', errors='ignore')
print('raw bytes:', len(raw), 'utf8 chars:', len(u), 'diff:', len(raw) - len(u))
high = sum(1 for b in raw if b > 127)
print('high bytes:', high)
