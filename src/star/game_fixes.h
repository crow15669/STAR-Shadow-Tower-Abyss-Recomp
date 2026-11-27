#pragma once

// Shadow Tower Abyss-specific fixes, each documented where it is made (game_fixes.cpp). GHR's practice: the runtime
// stays generic, the game layer holds what this game needs.

class PS2Runtime;

namespace star
{
    // Installs every fix (function replacements); after loadELF, before run.
    void installGameFixes(PS2Runtime &runtime);
}
