// SPDX-License-Identifier: MIT
// AI Driver Behaviour, in-game part. The options of this addon are database edits (db/racecraft.json), applied by Swofty
// while the game boots, so this plugin has nothing to run at the moment.
// The two "Pace" grip-layer options (AI pushes harder / drivers differ every race) were removed in 0.1.7: the AI planned its
// corner speeds from more grip than the car has and could not judge corner entries on tight tracks (Monaco barriers). The
// grip-layer code lives on in common/kizi_grip.hpp for a later, track-aware version.
#include <windows.h>

#include <swofty/mixin.hpp>

SWOFTY_ON_LOAD
{
    swofty::log("[ai-driver-behaviour] pace options removed (0.1.7): no grip layers");
}

SWOFTY_ADDON()
