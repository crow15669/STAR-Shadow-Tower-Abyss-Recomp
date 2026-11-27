"""flashcut.py <F1 folder> <out.png> [which]: finds the frames in which a muzzle flash is new (the model d149... in
frames.txt), prints their lines and cuts the pictures of the `which`-th one (default the first), with the picture
before, side by side."""
import re, os, sys, subprocess
d, out = sys.argv[1], sys.argv[2]
which = int(sys.argv[3]) if len(sys.argv) > 3 else 0
frames = []; cur = None
for line in open(os.path.join(d, 'frames.txt'), errors='replace'):
    if line.startswith('F '):
        cur = {'F': line.strip(), 'o': [], 'ids': [int(x) for x in re.findall(r'(\d+)@', line)]}; frames.append(cur)
    elif cur is not None: cur['o'].append(line.rstrip())
names = {int(re.search(r'k(\d+)', n).group(1)): n for n in os.listdir(d) if n.endswith('.png')}
shots = [i for i, fr in enumerate(frames) if any('d149121d5ef09855' in o for o in fr['o']) and not any('d149121d5ef09855' in o for o in frames[i-1]['o'])]
print(len(shots), 'frames with a new flash')
for i in shots[:4]:
    fr = frames[i]
    print(fr['F'][:60], '|', ' '.join(fr['F'].split('pictures:')[1].split()))
    for o in fr['o']:
        if 'd149121d5ef09855' in o or o[:2] in ('C ', 'L '): print('    ' + o[:130])
fr = frames[shots[which]]
files = [os.path.join(d, names[i]) for i in [min(fr['ids']) - 1] + fr['ids'] if i in names]
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pngcrop.py'), out, '2', '110', '170', '370', '330'] + files)
