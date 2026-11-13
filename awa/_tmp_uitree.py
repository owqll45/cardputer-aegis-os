import re, sys, subprocess, os

xml_path = sys.argv[1] if len(sys.argv) > 1 else r"c:\Users\awa\Documents\trae_projects\zd\awa\KernelSU-Style-UI-Kit\ui4.xml"
s = open(xml_path, encoding="utf-8").read()
for m in re.finditer(r'text="([^"]*)"[^>]*bounds="([^"]*)"', s):
    t = m.group(1)
    if t:
        print(t, "|", m.group(2))
print("--- clickable ---")
for m in re.finditer(r'clickable="true"[^>]*bounds="([^"]*)"', s):
    print("clickable", m.group(1))
