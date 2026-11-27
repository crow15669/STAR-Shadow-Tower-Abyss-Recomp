#pragma once

// Host-side sampler (Windows): pauses each of STAR's threads many times over a short window, reads the instruction
// pointer and names it from STAR.pdb. Recompiled functions carry their guest address in their name
// (FUN_00128e84_0x128e84), so a game thread spinning in guest code shows where. Test runs: STAR_HOST_SAMPLE=1.

namespace star
{
    // Samples every thread except the caller for `milliseconds`, then prints each busy thread's hottest functions.
    void sampleHostThreads(int milliseconds);
}
