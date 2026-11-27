"""creaturesweep.py [kind ...]: asks for each kind of creature once in a hidden run from the user's quick save (the Debug
page's own way, STAR_DEBUG_TEST=30:creature:<kind>) and writes what became of it to build/bin/logs/creaturesweep.txt:
whether the game made it and whether the run went on to its time limit."""
import os, re, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, 'build', 'bin', 'logs')
kinds = [int(a) for a in sys.argv[1:]] or list(range(21, 74)) + [81, 82, 83, 91, 92, 94, 95, 98, 101, 102, 103, 104, 105, 107, 108, 109, 110, 118, 119]
with open(os.path.join(LOGS, 'creaturesweep.txt'), 'a') as out:
    for k in kinds:
        env = dict(os.environ, FROM='660', TO='660', SECONDS_LIMIT='13')
        subprocess.run(['bash', 'tools/ab.sh', 'sweep', 'q2', 'STAR_TEST_CARDS=1', 'PS2X_AUDIO_MUTE=1', 'STAR_MOUSE_KEYBOARD=1',
                        'STAR_DEBUG_TEST=30:creature:%d' % k], cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        text = open(os.path.join(LOGS, 'ab_sweep.log'), 'rb').read().decode('latin1')
        there = 'is there' in text
        fetched = re.search(r'fetched after (\d+) frames', text)
        limit = 'time limit of 13 s reached' in text
        died = 'STA_GO.ADX' in text
        line = '%d %s %s %s%s' % (k, 'made' if there else ('fetched' if fetched else 'not taken'), 'after %s frames' % fetched.group(1) if fetched else '',
                                 'ran on' if limit else 'STOPPED', ' (the player was killed)' if died else '')
        print(line, flush=True)
        out.write(line + '\n'); out.flush()
