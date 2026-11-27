#pragma once

#include <cstdint>

// STAR 2026-11-25: Shadow Tower Abyss's creature shadow volumes, completed where the game's VU1 program drops them.
//
// The game draws a creature's shadow as shadow volumes (NOTES.md "How the game makes its shadows"): for each piece,
// FUN_00132a70 hands VU1 program 0x678 (micro memory byte 0x33c0) the piece's shadow mesh, and for every triangle that
// faces the light the program draws a closed prism (the triangle, its three sides swept along the light, the far end)
// into the count buffer, front faces adding and back faces subtracting. The program drops a triangle's whole prism
// when any corner of the triangle itself lies outside the view (beside the screen, behind or very near the camera),
// so a creature close to the camera loses its shadow. The hook here runs before each start of that program
// (ps2x::vu1hooks program-start hook) and draws those prisms itself, cut at the view's edges, with the cap where the
// camera's near plane cuts a prism.
namespace star
{
    void installShadowVolumes();
    // triangles made so far (and prisms, out)
    uint64_t shadowVolumesMade(uint64_t &prisms);
}
