"""elfstrings.py [pattern]: the ASCII strings of the game's program (game/SLPS_252.17) with their file offsets;
with a pattern (a regular expression) only those that match. By default those that look like file names."""
import re, sys, os
d = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'SLPS_252.17'), 'rb').read()
pattern = re.compile(sys.argv[1]) if len(sys.argv) > 1 else re.compile(r'[/\\]|\.[a-zA-Z0-9]{2,4}$')
for m in re.finditer(rb'[\x20-\x7e]{4,}', d):
    s = m.group().decode()
    if len(s) < 80 and pattern.search(s):
        print('%06x %s' % (m.start(), s))
