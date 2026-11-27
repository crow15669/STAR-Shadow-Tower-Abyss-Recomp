# mapspots_cpp.py: writes the places of build/bin/logs/mapspots.txt (mapspots.py) into src/star/debug_menu.cpp, between
# the lines "//MAPSPOTS" and "//MAPSPOTS END".
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rows = [tuple(int(v) for v in line.split()) for line in open(os.path.join(ROOT, 'build', 'bin', 'logs', 'mapspots.txt')) if len(line.split()) == 4]
path = os.path.join(ROOT, 'src', 'star', 'debug_menu.cpp')
t = open(path, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in t else '\n'
a = t.index('//MAPSPOTS') + len('//MAPSPOTS')
b = t.index('//MAPSPOTS END')
pad = ' ' * 12
body = nl + ''.join('%s{%d, %d, %d, %d},%s' % (pad, *r, nl) for r in sorted(rows)) + pad
open(path, 'w', encoding='utf-8', newline='').write(t[:a] + body + t[b:])
print('%d places written' % len(rows))
