"""statepeek.py: helpers to read the game's memory out of a save state (file offset = address + 0x18)."""
import struct, sys, os

class State:
    def __init__(self, path):
        self.f = open(path, 'rb')
    def raw(self, addr, n):
        self.f.seek((addr & 0x1FFFFFFF) + 0x18)
        return self.f.read(n)
    def u32(self, addr):
        return struct.unpack('<I', self.raw(addr, 4))[0]
    def f32(self, addr, n=1):
        return struct.unpack('<%df' % n, self.raw(addr, 4 * n))
    def player(self):
        p = self.u32(0x2022E4)
        return p, self.u32(p + 0x518)

if __name__ == '__main__':
    D = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'bin', 'logs', 'state_test')
    for name in sys.argv[1:]:
        s = State(os.path.join(D, name + '.state'))
        p, inner = s.player()
        print(name, 'player %08x inner %08x' % (p, inner))
