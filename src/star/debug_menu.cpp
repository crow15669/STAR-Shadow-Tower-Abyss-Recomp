#include "star/debug_menu.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_game_menu.h"

#include "pcsx2gs_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// STAR 2026-11-25. What the page stands on (all read out of the game, `ghidra\decomp_dbg*.c`, `decomp_area*.c`):
//
// Items. The game keeps two lists. What the player CARRIES (the Item and Equipment menus): 4 bytes an item at the
// player's item record (*(0x201e20), FUN_001c9ba0) + 0xa0 + 4 x number (durability, where worn, how many, rounds
// loaded); FUN_001cbed0(item, how many, durability) adds to it, FUN_001cbcd0(item) is that for one armour piece or
// weapon (new, a gun loaded), FUN_001cc0c0(item, 1) answers "may the player take one more". And the STORE ("Store
// Item"): 8 bytes an item at save data + 0x5c00 + 8 x number (byte 0 durability, byte 2 how many, byte 5 = 3 once
// had), the save data at *(0x201dc0); FUN_001a95a0(item, how many) adds to it (the new-game set-up FUN_001aa760 /
// FUN_001aaae0 fills it so), FUN_001a9510(item) takes an item out of it. The page gives into what is carried (it gave
// into the store at first: "does not appear in my inventory", the user, 2026-11-25). An item's definition is 0x90 bytes at *(0x2022b0) + 0x90 x number: the half word at + 10 is 0 for
// things that stack (as many as the byte at + 0x2b allows), 1 for armour and the like, 2 for weapons (one of each); the half
// word at + 0x20 is the weapon's kind (1 sword, 2 great sword, 3 mace, 4 hammer, 5 axe, 6 great axe, 9 and 19 bows,
// the rest guns). Numbers: 1-80 things to use, 100- helms, 200- armour, 300- gloves, 400- boots, 500- bracelets,
// 600- amulets, 700- rings, 800-1009 weapons.
//
// Names. The game keeps its texts as tables in memory: "STA ", a count, 8 zero bytes, then count records of
// {number, offset of the name, offset of the description} and the texts themselves (offsets from the table's
// start). 605 items (*(*(0x202160)), menu/dbitem.txm); 68 creatures (*(*(0x202164)), menu/dbchar.txm); 15 areas
// (*(*(0x20215c)), menu/dbmap.txm): the holders FUN_00195c00 fills at start-up.
//
// Places. The save data holds where play begins again: the map's number in the byte at + 0x2f and the place as three
// 16-bit numbers at + 0x30 (x, y, z). The game's own "start play again from the saved place" is three words set to
// 1 (FUN_0012c0a0: 0x201ea4 = the save data is to be kept, 0x201e98 = leave play, 0x201e9c = and come straight back
// without the title): the main loop FUN_0012b350 ends, FUN_0012bf40 starts it again, and FUN_0012b120 puts the
// player where the save data says. The map the player is in is the word at 0x2022b8; the player's place is at
// inner + 0x1c0 (inner = *(*(0x2022e4) + 0x518)).
namespace star
{
    namespace
    {
        uint8_t *g_ram = nullptr;
        std::filesystem::path g_placesFile;

        uint32_t word(uint32_t address)
        {
            uint32_t value = 0;
            if (address < 0x01FFFFFCu)
                std::memcpy(&value, g_ram + address, 4);
            return value;
        }

        bool inRam(uint32_t address)
        {
            return address >= 0x00100000u && address < 0x01FFFF00u;
        }

        // ---- the game's text tables ----------------------------------------------------------------------------------
        struct Entry
        {
            uint32_t number;
            std::string name, note;
        };

        bool readTable(uint32_t at, std::vector<Entry> &out)
        {
            if (!inRam(at) || std::memcmp(g_ram + at, "STA ", 4) != 0)
                return false;
            const uint32_t count = word(at + 4);
            if (count == 0u || count > 4000u || word(at + 8) != 0u || word(at + 12) != 0u || !inRam(at + 16u + count * 12u))
                return false;
            const auto text = [&](uint32_t offset, std::string &into)
            {
                if (offset < 16u + count * 12u || offset > 0x80000u || !inRam(at + offset + 200u))
                    return false;
                const char *s = reinterpret_cast<const char *>(g_ram + at + offset);
                size_t n = 0;
                for (; n < 200u && s[n] != 0; ++n)
                    if (static_cast<unsigned char>(s[n]) < 0x20u && s[n] != '\n')
                        return false;
                if (n == 200u)
                    return false;
                into.assign(s, n);
                std::replace(into.begin(), into.end(), '\n', ' ');
                return true;
            };
            out.clear();
            out.reserve(count);
            for (uint32_t k = 0; k < count; ++k)
            {
                Entry entry;
                entry.number = word(at + 16u + k * 12u);
                if (entry.number > 0xFFFFu || !text(word(at + 20u + k * 12u), entry.name) || !text(word(at + 24u + k * 12u), entry.note))
                    return false;
                out.push_back(std::move(entry));
            }
            return true;
        }

        struct Tables
        {
            uint32_t itemsAt = 0;
            std::vector<Entry> items; // sorted by number
            std::vector<Entry> areas; // the map-data screen's names: an area is a map's number / 10 (FUN_00195460)
            std::vector<Entry> creatures; // the creature-data screen's names and texts (menu/dbchar.txm), by kind
        };
        Tables g_tables;

        // The items' texts: the table the game's own item screens read, *(*(0x202160)) (menu/dbitem.txm). Read anew
        // while the page is shown (605 short texts), so what is put right in the game's memory shows here too.
        void findTables()
        {
            std::vector<Entry> entries;
            const uint32_t itemsHolder = word(0x202160u);
            const uint32_t at = inRam(itemsHolder) ? word(itemsHolder) : 0u;
            if (inRam(at) && readTable(at, entries) && entries.size() >= 300u)
            {
                std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.number < b.number; });
                g_tables.itemsAt = at;
                g_tables.items = std::move(entries);
            }
            else
            {
                g_tables.itemsAt = 0u;
                g_tables.items.clear();
            }
            // the areas' names: the table the game's map-data screen reads (menu/dbmap.txm, its holder at 0x20215c)
            if (const uint32_t holder = word(0x20215Cu); inRam(holder))
                readTable(word(holder), g_tables.areas);
            if (const uint32_t holder = word(0x202164u); inRam(holder) && readTable(word(holder), g_tables.creatures))
                std::sort(g_tables.creatures.begin(), g_tables.creatures.end(), [](const Entry &a, const Entry &b) { return a.number < b.number; });
        }

        // ---- the game's maps ---------------------------------------------------------------------------------------------
        // The game's own list of its maps: 42 half words at 0x1f37b0. Each has a record of 0x1c0 bytes in the save data
        // (*(0x201dc4) + 0x1c0 x its place in the list); bit 0 of the byte at + 0x50 is "the player has been here"
        // (FUN_001aa440 reads it for the map-data screen; FUN_001aa4c0 sets it when the floor under the player
        // belongs to that map). The screen has no places in it: an area shows once one of its maps has that bit.
        constexpr uint32_t kMapList = 0x001F37B0u;
        constexpr int kMapCount = 42;

        int mapAt(int index)
        {
            int16_t map = 0;
            std::memcpy(&map, g_ram + kMapList + static_cast<uint32_t>(index) * 2u, 2);
            return map;
        }

        bool visited(int index)
        {
            const uint32_t records = word(0x201DC4u);
            return inRam(records) && (g_ram[records + static_cast<uint32_t>(index) * 0x1C0u + 0x50u] & 1u) != 0u;
        }

        std::string areaName(int map)
        {
            for (const Entry &area : g_tables.areas)
                if (area.number == static_cast<uint32_t>(map / 10) && !area.name.empty())
                    return area.name;
            return "Area " + std::to_string(map / 10);
        }

        // A place to stand in each map: where the map's own file puts one of its creatures or people, lifted to the
        // player's height (maps' files have a list of things, of creatures and of people with their places; a map's
        // places are counted from its own middle, and so is the player's while in it). Each one was tried in a hidden
        // run: the player lands, stays, and the game names that map as the one they are in.
        struct MapSpot
        {
            int map, x, y, z;
        };
        const MapSpot kMapSpots[] = {
            //MAPSPOTS
            {10, 320, -219, -30},
            {11, -5700, -266, -4350},
            {12, -3831, -619, -5368},
            {13, -4516, -1138, 7540},
            {22, 2264, -259, -16596},
            {33, -2003, -265, 8913},
            {40, 772, 2207, -18},
            {41, -2212, -146, 4019},
            {42, 563, 696, 585},
            {43, 5523, 1171, 3869},
            {50, -5772, -200, 2013},
            {51, -3281, -215, 6742},
            {52, -22, -219, -5005},
            {53, 7162, -630, 1900},
            {60, 5068, -1019, 7660},
            {61, -633, -220, -1937},
            {62, -900, 1181, -8800},
            {63, 200, -211, 4900},
            {64, 1003, -219, -18788},
            {70, 4963, 571, -3545},
            {71, -1600, -190, 1400},
            {80, 6649, -5979, -1348},
            {81, 1817, -6999, -2065},
            {82, -601, 8101, 1853},
            {83, -6166, -3099, -2942},
            {90, 4436, 156, 4935},
            {91, 7404, 341, 2279},
            {92, 1389, 281, -532},
            {100, -5698, 262, 515},
            {101, 2644, -19, -559},
            {110, -3863, -3657, 12083},
            {111, -12750, -3380, 7930},
            {112, -8525, -6367, 1825},
            {120, 194, -219, -9382},
            {121, 11826, -1199, -3692},
            {122, -218, -344, -18910},
            {123, 599, -177, -77},
            {130, -4600, -349, 11900},
            {131, -12800, -349, -2500},
            {132, 7500, -219, -18221},
            {140, 900, -42, -1024},
            {150, 0, -25944, 1018},
            //MAPSPOTS END
        };

        const MapSpot *spotOf(int map)
        {
            for (const MapSpot &spot : kMapSpots)
                if (spot.map == map)
                    return &spot;
            return nullptr;
        }

        const char *const kKinds[] = {"Things to use (potions, ammunition, keys)", "Helms", "Armour", "Gloves", "Boots", "Bracelets", "Amulets",
                                      "Rings", "Swords", "Great swords", "Maces", "Hammers", "Axes", "Great axes and polearms", "Bows", "Guns"};
        constexpr int kKindCount = 16;

        int kindOf(uint32_t number)
        {
            if (number < 100u)
                return 0;
            if (number < 800u)
                return static_cast<int>(number / 100u); // 1 helms .. 7 rings
            const uint32_t definitions = word(0x2022B0u);
            uint16_t weapon = 0;
            if (inRam(definitions) && inRam(definitions + number * 0x90u + 0x22u))
                std::memcpy(&weapon, g_ram + definitions + number * 0x90u + 0x20u, 2);
            switch (weapon)
            {
            case 1: return 8;
            case 2: return 9;
            case 3: return 10;
            case 4: return 11;
            case 5: return 12;
            case 6: return 13;
            case 9: case 19: return 14;
            default: return 15;
            }
        }

        bool stacks(uint32_t number)
        {
            const uint32_t definitions = word(0x2022B0u);
            uint16_t type = 1;
            if (inRam(definitions) && inRam(definitions + number * 0x90u + 12u))
                std::memcpy(&type, g_ram + definitions + number * 0x90u + 10u, 2);
            return type == 0u;
        }

        // Enchantments are not rolled when a weapon is found: every enchanted or glowing weapon and armour piece is an item
        // of its own, with its own number, values and text ("Infighting Sword" 803 "Has minor Element enchantment", 804
        // Solvent, 805 Spirit; the plain ones are 800-802). Those with a magic effect of their own to show have its
        // number in the half word at + 0xc of the definition (51 weapons, 47 armour pieces, the four lift keys).
        bool hasEffect(uint32_t number)
        {
            const uint32_t definitions = word(0x2022B0u);
            uint16_t effect = 0;
            if (inRam(definitions) && inRam(definitions + number * 0x90u + 0x10u))
                std::memcpy(&effect, g_ram + definitions + number * 0x90u + 0xCu, 2);
            return effect != 0u;
        }

        // What the player carries (the game's Item and Equipment menus): 4 bytes an item at the player's item record
        // (*(0x201e20), FUN_001c9ba0) + 0xa0 + 4 x number: durability, where it is worn, how many, rounds loaded.
        int carried(uint32_t number)
        {
            const uint32_t record = word(0x201E20u);
            return inRam(record) && number < 0x400u ? g_ram[record + 0xA0u + number * 4u + 2u] : 0;
        }

        // What is in the store (the game's "Store Item"): 8 bytes an item at save data + 0x5c00 + 8 x number.
        int stored(uint32_t number)
        {
            const uint32_t save = word(0x201DC0u);
            if (!inRam(save) || number >= 0x400u)
                return 0;
            const uint8_t *entry = g_ram + save + 0x5C00u + number * 8u;
            return entry[5] != 0u ? entry[2] : 0;
        }

        // ---- places ----------------------------------------------------------------------------------------------------
        struct Place
        {
            std::string name;
            int map = 0, x = 0, y = 0, z = 0;
            bool own = false; // the user's (kept in the file), not one of the list below
        };
        // Places I have stood at in tests; the names are mine, not the game's.
        const Place kPlaces[] = {
            {"First area: where a new game begins (map 10)", 10, -2450, -1675, 7400},
            {"First area: at the stairs (map 10)", 10, -1901, -1397, 5530},
            {"Hub: between the shrines (map 33)", 33, -2003, -265, 8913},
            {"Hub: at the save shrine (map 33)", 33, -1377, -265, 9153},
            {"Hub: at the stone tablet (map 33)", 33, -396, -215, 7376},
            {"Hub: before the bridge (map 33)", 33, 3567, -215, 5952},
            {"Repair shrine's area (map 60)", 60, 6807, -1037, 16849},
            {"Cave with the creatures (map 63)", 63, 1838, -296, 10207},
            {"Chasm Nexus: the walkway behind the door (map 140)", 140, -77, -415, -3465},
        };
        std::mutex g_mutex; // the user's places, the requests and the answer
        std::vector<Place> g_own;
        bool g_ownLoaded = false;

        void loadPlaces()
        {
            if (g_ownLoaded)
                return;
            g_ownLoaded = true;
            std::ifstream in(g_placesFile);
            std::string line;
            while (std::getline(in, line))
            {
                // name = map, x, y, z
                const size_t equals = line.rfind('=');
                if (equals == std::string::npos || line.empty() || line[0] == '#')
                    continue;
                Place place;
                place.own = true;
                place.name = line.substr(0, equals);
                while (!place.name.empty() && (place.name.back() == ' ' || place.name.back() == '\t'))
                    place.name.pop_back();
                if (!place.name.empty() && std::sscanf(line.c_str() + equals + 1, "%d , %d , %d , %d", &place.map, &place.x, &place.y, &place.z) == 4)
                    g_own.push_back(std::move(place));
            }
        }

        void savePlaces()
        {
            std::ofstream out(g_placesFile, std::ios::trunc);
            out << "# STAR: the places remembered on the F2 menu's Debug page. One a line: name = map, x, y, z\n"
                   "# The names can be changed here (not while the game runs: it writes this file again).\n";
            for (const Place &place : g_own)
                out << place.name << " = " << place.map << ", " << place.x << ", " << place.y << ", " << place.z << "\n";
        }

        bool whereNow(Place &place)
        {
            const uint32_t player = word(0x2022E4u);
            const uint32_t inner = inRam(player) ? word(player + 0x518u) & 0x1FFFFFFFu : 0u;
            if (!inRam(inner))
                return false;
            float at[3];
            std::memcpy(at, g_ram + inner + 0x1C0u, 12);
            place.map = static_cast<int>(word(0x2022B8u));
            place.x = static_cast<int>(at[0]);
            place.y = static_cast<int>(at[1]);
            place.z = static_cast<int>(at[2]);
            return place.map > 0 && place.map < 256 && std::abs(place.x) < 32000 && std::abs(place.y) < 32000 && std::abs(place.z) < 32000;
        }

        // ---- what the page asks of the game thread ------------------------------------------------------------------------
        struct Request
        {
            // (Creature: `item` is its kind, `count` its options: 1 with its own sounds, 2 it does not notice the player;
            //  Sounds: `item` is the area whose sounds are wanted, 0 = the area the player is in)
            enum class Kind { Give, Go, Creature, Sounds, Probe } kind = Kind::Give;
            uint32_t item = 0;
            int count = 0;
            Place place;
            std::string name;
        };
        std::vector<Request> g_requests;
        std::string g_answer;
        Place g_before; // where the player was before the last jump
        bool g_haveBefore = false;

        void ask(Request request)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_requests.push_back(std::move(request));
        }

        // A function of the game called from here: for small ones that come straight back (no waiting inside).
        uint32_t callGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t function, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t a3, uint32_t t0)
        {
            const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(function);
            if (!fn)
                return 0u;
            R5900Context scratch = *ctx;
            SET_GPR_U32(&scratch, 4, a0);
            SET_GPR_U32(&scratch, 5, a1);
            SET_GPR_U32(&scratch, 6, a2);
            SET_GPR_U32(&scratch, 7, a3);
            SET_GPR_U32(&scratch, 8, t0);
            SET_GPR_U32(&scratch, 29, getRegU32(ctx, 29) - 0x1000u); // below the caller's stack frame
            SET_GPR_U32(&scratch, 31, 0u);
            scratch.pc = function;
            // to its end: generated code returns early at the EE scheduler's safe points (every call, every backward
            // branch), leaving the rest for a resume that a host caller never does; the game's collision test (187
            // pieces a map) was cut off from its second call on and answered with whatever was in v0
            runtime->eeScheduler().beginHostCall();
            fn(rdram, &scratch, runtime);
            runtime->eeScheduler().endHostCall();
            return getRegU32(&scratch, 2);
        }

        // ---- creatures ---------------------------------------------------------------------------------------------------
        // What the game does (`ghidra\decomp_chr*.c`). A map's file has a list of its creatures: records of 0x54 bytes at
        // record + 0x13c of the map's record, how many in the half word at + 0x70:
        //    + 0x00 place (three floats, counted from the map's middle), + 0x0c turn (three half words, 1/4096 radian),
        //    + 0x12 size (three half words, 0x1000 = 1), + 0x18 kind (the number of the creature names table and of its
        //    files), + 0x1a its own number in the map (below 64), + 0x1e how far off it is fetched, + 0x26 how far off
        //    it is made, + 0x22 / + 0x24 (1000 mostly), + 0x2a chance in 100, + 0x2c how many times it may be killed,
        //    + 0x2d (31, 30, 29..), + 0x2e, + 0x30 bits (8 = not counted against the map's limit of creatures at a time,
        //    and never taken away for being far off).
        // Beside the list, 8 bytes a record at + 0x17c: word 0 the creature's holder once it is fetched. Bits by the
        // creature's own number: fetched at + 0x90, made-or-decided at + 0x98, set off by an event at + 0xa0.
        // FUN_001b43a0(map record), every frame (from FUN_001b4b50), is the keeper: a record whose creature has not been
        // killed as often as it may be (FUN_001aa1e0(map, number): a count of four bits in the save data, the map's
        // record + 0x20) and is within its distance is fetched: FUN_001a7d90(kind) takes a creature's place (0x750 bytes;
        // how many are free: the half word at 0x4a4774) and FUN_001a51e0 one of the ELEVEN kinds the game holds at a
        // time (0x28 bytes each at 0x4a43b0, word 0 the kind, -1 free), which loads the kind's files if need be. Once
        // that is ready (bit 0 of the byte at creature + 0x5a0) and the player is near enough, FUN_001a6c90 makes it at
        // the record's place (bit 2).
        // So a creature of any kind is asked for by writing a record: one of this map's own that is not fetched is
        // taken over (its place, kind, a number no other record has, the distances wide open), and the keeper does the
        // rest. The map's file is read anew when the player leaves and comes back, so the change lasts that long.
        uint16_t half(uint32_t address)
        {
            uint16_t value = 0;
            if (address < 0x01FFFFFCu)
                std::memcpy(&value, g_ram + address, 2);
            return value;
        }

        struct Awaited // a creature asked for, until it is there
        {
            uint32_t record, map, slots, index;
            std::string name;
            bool calm = false;
            int frames = 0;
            bool fetched = false;
        };
        std::vector<Awaited> g_awaited; // the game thread's

        // A creature that does not notice the player. The game's own switch: bit 4 of the byte at creature + 0x5b4
        // (FUN_0013e610), which FUN_00135990, the creature's "do I notice the player" (near enough, heard, or seen with a
        // clear line), ends with: set, the answer is no. Kept set every frame while the creature is in its place of
        // the list (a creature's own script may touch the byte).
        struct Calm
        {
            uint32_t record, slots, index, creature;
        };
        std::vector<Calm> g_calm; // the game thread's

        void keepCalm()
        {
            const uint32_t holder = word(0x2022BCu);
            const uint32_t record = inRam(holder) ? word(holder) : 0u;
            for (size_t i = 0; i < g_calm.size();)
            {
                const Calm &one = g_calm[i];
                const uint32_t slot = one.record == record ? word(one.slots + one.index * 8u) : 0u;
                if (!inRam(slot) || word(slot) != one.creature)
                {
                    g_calm.erase(g_calm.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                g_ram[one.creature + 0x5B4u] |= 0x10u;
                ++i;
            }
        }

        // ---- a creature's sounds ---------------------------------------------------------------------------------------------
        // The game has ONE bank of creature sounds at a time, the area's: se/m<area>s.mse, loaded when the player's
        // area (map / 10) is not the one in the word at 0x22ec18 (FUN_0012c4d0, every frame: it stops the sounds, lets
        // the old bank go and asks for the new one, FUN_001e1d30(map, 0); the loader FUN_001e19a0 ends with
        // FUN_0012dd10(map), which writes 0x22ec18 = map / 10). The table of sounds (four bytes a sound at 0x222bc0:
        // program, note, bank, bits) is filled from the bank: a creature of another area asks for sounds that are
        // marked "not there" (a Galp in the hub: sounds 4010, 4011, 6014, 6017 = -1, -1, bank 255), and is mute.
        // So a spawned creature's sounds are had by letting the game load ITS area's bank in place of this area's,
        // through the game's own steps with two arguments changed on the way:
        //   - the word at 0x22ec18 is set to a number no area has: the game's next frame does its whole change of banks;
        //   - where it asks for the bank (FUN_001e1d30) the map asked for becomes one of the wanted area;
        //   - where the loader reports the bank (FUN_0012dd10) the map becomes the player's again, so that the game
        //     takes the matter as settled until the player changes area (then it loads that area's bank as ever).
        // The price: this area's own creatures are mute meanwhile. "This area's sounds" loads them back the same way.
        PS2Runtime::RecompiledFunction g_askBank = nullptr, g_bankLoaded = nullptr;
        bool g_soundsReady = false; // both switches are in
        uint32_t g_wantArea = 0;    // asked for on the page, not yet asked of the game
        uint32_t g_loadingArea = 0; // asked of the game
        std::atomic<uint32_t> g_foreignArea{0}; // loaded in place of the player's area's (0 = the area's own)

        void askBankHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_wantArea != 0u)
            {
                // (the second argument stays the game's 0: a 1 is remembered at 0x4bc2d8 and every later request
                //  without it is turned down, the area's own bank never came back)
                SET_GPR_U32(ctx, 4, g_wantArea * 10u);
                g_loadingArea = g_wantArea;
                g_wantArea = 0u;
                std::printf("[STAR debug] the game is asked for the sounds of area %u%c", g_loadingArea, 10);
            }
            g_askBank(rdram, ctx, runtime); // nothing after the call
        }

        void bankLoadedHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_loadingArea != 0u && getRegU32(ctx, 4) == g_loadingArea * 10u)
            {
                SET_GPR_U32(ctx, 4, word(0x2022B8u));
                g_foreignArea.store(g_loadingArea);
                std::printf("[STAR debug] the sounds of area %u are loaded in place of this area's%c", g_loadingArea, 10);
            }
            else
                g_foreignArea.store(0u);
            g_loadingArea = 0u;
            g_bankLoaded(rdram, ctx, runtime); // nothing after the call
        }

        // the area whose creature sounds are loaded, or on their way
        uint32_t soundArea()
        {
            if (g_wantArea != 0u)
                return g_wantArea;
            if (g_loadingArea != 0u)
                return g_loadingArea;
            const uint32_t foreign = g_foreignArea.load();
            return foreign != 0u ? foreign : word(0x2022B8u) / 10u;
        }

        // (the game thread) area 0 = the player's own
        std::string wantSounds(uint8_t *rdram, uint32_t area)
        {
            const uint32_t own = word(0x2022B8u) / 10u;
            if (!g_soundsReady || own == 0u)
                return "The sounds can't be changed just now.";
            if (area == 0u)
                area = own;
            if (area == soundArea())
                return "Those sounds are already loaded.";
            g_wantArea = area == own ? 0u : area;
            g_loadingArea = 0u;
            const uint32_t none = 99u; // (no area has it; above 0, so that the game lets the loaded bank go)
            std::memcpy(rdram + 0x22EC18u, &none, 4);
            return area == own ? "Loading this area's creature sounds again." : "Loading the creature sounds of " + areaName(static_cast<int>(area * 10u)) + " in place of this area's.";
        }

        uint32_t callGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t function, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0,
                          uint32_t a3 = 0, uint32_t t0 = 0);

        // Where the middle of the view meets the ground (STAR 2026-11-26; the user: "placed the creature in the center of
        // where I'm looking", "checked for ground before placing a creature"), by the game's own line test against the
        // maps loaded (0x201e88, 0x201e8c): FUN_001b3240(map, result, segment {from, to: two quad words}, layers, 0)
        // runs a segment against the map's collision pieces (FUN_0014e230: bit i of the mask = layer i; layer 2 = the
        // floor, the player's floor probe; layers 0 and 1 = walls and slopes) and leaves the segment's end at the nearest
        // hit, the surface's direction at result + 0x10 (y grows downward: a floor faces (0, -1, 0)). callGame runs it
        // to its end (it was cut off at the EE scheduler's safe points before, and answered at random). The collision
        // code tests only one piece and triangle where the half words at 0x222a18 / 0x222a16 say so (0xffff = all): open
        // while we ask, as they were after.
        struct OpenCollision
        {
            uint8_t *ram;
            uint16_t saved[2];
            explicit OpenCollision(uint8_t *rdram) : ram(rdram)
            {
                std::memcpy(saved, ram + 0x222A16u, 4);
            }
            ~OpenCollision() { std::memcpy(ram + 0x222A16u, saved, 4); }
        };
        bool lineTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const float from[3], float to[3], uint32_t layers, float hitDir[3])
        {
            // two quad words for the segment and four for the result, on the guest stack below the caller's frame (the
            // game's function runs further down, callGame)
            const uint32_t sp = getRegU32(ctx, 29) & ~15u;
            const uint32_t segment = sp - 0x200u, result = sp - 0x180u;
            if (!inRam(segment) || !inRam(result + 0x40u))
                return false;
            const float seg[8] = {from[0], from[1], from[2], 1.0f, to[0], to[1], to[2], 1.0f};
            std::memcpy(rdram + segment, seg, sizeof(seg));
            std::memset(rdram + result, 0, 0x40);
            for (const uint32_t holder : {0x201E88u, 0x201E8Cu})
            {
                const uint32_t loaded = word(holder);
                const uint32_t map = inRam(loaded) ? word(loaded) : 0u;
                if (inRam(map))
                {
                    const uint16_t all[2] = {0xFFFFu, 0xFFFFu};
                    std::memcpy(rdram + 0x222A16u, all, 4);
                    callGame(rdram, ctx, runtime, 0x001B3240u, map, result, segment, layers, 0u);
                }
            }
            float end[3], dir[3];
            std::memcpy(end, rdram + segment + 0x10u, 12);
            std::memcpy(dir, rdram + result + 0x10u, 12);
            const float moved = std::fabs(end[0] - to[0]) + std::fabs(end[1] - to[1]) + std::fabs(end[2] - to[2]);
            const float dirLength = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (!(moved > 0.5f) || !(dirLength > 0.5f))
                return false; // (no hit: the end did not move, no surface)
            std::memcpy(to, end, sizeof(end));
            std::memcpy(hitDir, dir, sizeof(dir));
            return true;
        }
        // the floor straight down from a point, within that far below it
        bool floorBelow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const float at[3], float above, float below, float &floorY)
        {
            const float from[3] = {at[0], at[1] - above, at[2]};
            float to[3] = {at[0], at[1] + below, at[2]}, dir[3] = {};
            if (!lineTest(rdram, ctx, runtime, from, to, 4u, dir) || !(-dir[1] > 0.3f))
                return false;
            floorY = to[1];
            return true;
        }
        struct Ground
        {
            bool found = false;
            float point[3] = {};
            const char *why = "";
        };
        // Along the view from the eye, 6000 units: the floor and the walls. The floor first: there. A wall first: in front
        // of it, toward the eye, on floor at most a creature's height below; a slope that faces up (a curb, a step): the
        // floor right under the point looked at. Neither: no creature.
        Ground groundInView(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t inner)
        {
            Ground g;
            const OpenCollision open(rdram);
            float eye[3], view[16];
            std::memcpy(eye, rdram + inner, 12);
            std::memcpy(view, rdram + 0x2F16C0u, 64);
            float d[3] = {view[2], view[6], view[10]};
            const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (!(length > 0.01f))
            {
                g.why = "Can't tell where you are looking just now.";
                return g;
            }
            for (float &v : d)
                v /= length;
            float floorAt[3] = {eye[0] + d[0] * 6000.0f, eye[1] + d[1] * 6000.0f, eye[2] + d[2] * 6000.0f};
            float wallAt[3] = {floorAt[0], floorAt[1], floorAt[2]}, floorDir[3] = {}, wallDir[3] = {};
            const bool floor = lineTest(rdram, ctx, runtime, eye, floorAt, 4u, floorDir);
            const bool wall = lineTest(rdram, ctx, runtime, eye, wallAt, 3u, wallDir);
            const auto along = [&](const float p[3]) { return (p[0] - eye[0]) * d[0] + (p[1] - eye[1]) * d[1] + (p[2] - eye[2]) * d[2]; };
            std::printf("[STAR debug] the view from %.0f %.0f %.0f along %.2f %.2f %.2f: floor %s %.0f, wall %s %.0f (facing %.2f %.2f %.2f)%c", eye[0], eye[1],
                        eye[2], d[0], d[1], d[2], floor ? "at" : "none,", floor ? along(floorAt) : 0.0f, wall ? "at" : "none,", wall ? along(wallAt) : 0.0f,
                        wallDir[0], wallDir[1], wallDir[2], 10);
            if (floor && (!wall || along(floorAt) <= along(wallAt) + 1.0f))
            {
                g.found = true;
                std::memcpy(g.point, floorAt, sizeof(floorAt));
                return g;
            }
            if (!wall)
            {
                g.why = "Nothing in reach where you look: look at the ground.";
                return g;
            }
            // a slope that faces up: the floor right under the point looked at (a step's height at most); else, and for a
            // wall, a step in front of it, toward the eye, on the floor below (a wall looked at high up stands on floor
            // far below the point)
            const float across = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            const bool slope = -wallDir[1] > 0.5f;
            float y = 0.0f;
            if (slope && floorBelow(rdram, ctx, runtime, wallAt, 150.0f, 300.0f, y))
            {
                g.found = true;
                g.point[0] = wallAt[0], g.point[1] = y, g.point[2] = wallAt[2];
                return g;
            }
            float at[3] = {wallAt[0], wallAt[1], wallAt[2]};
            if (across > 0.01f)
            {
                at[0] -= d[0] / across * 80.0f;
                at[2] -= d[2] / across * 80.0f;
            }
            if (!floorBelow(rdram, ctx, runtime, at, 150.0f, slope ? 300.0f : 2000.0f, y))
            {
                g.why = slope ? "No ground there (a slope over a drop): look at the ground." : "No ground in front of that wall: look at the ground.";
                return g;
            }
            g.found = true;
            g.point[0] = at[0], g.point[1] = y, g.point[2] = at[2];
            return g;
        }

        uint16_t areasOfKind(uint32_t kind);

        std::string askCreature(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t kind, const std::string &name, int options)
        {
            const uint32_t holder = word(0x2022BCu);
            const uint32_t record = inRam(holder) ? word(holder) : 0u;
            const uint32_t player = word(0x2022E4u);
            const uint32_t inner = inRam(player) ? word(player + 0x518u) & 0x1FFFFFFFu : 0u;
            if (!inRam(record) || !inRam(inner) || word(record) != word(0x2022B8u))
                return "Not in play just now.";
            const uint32_t count = half(record + 0x70u), list = word(record + 0x13Cu), slots = word(record + 0x17Cu);
            if (count == 0u || count > 200u || !inRam(list) || !inRam(slots) || !inRam(list + count * 0x54u))
                return "This map has no creature list to use (no creatures of its own).";
            // a kind the game holds already, or room for one more
            bool held = false, room = false;
            for (uint32_t k = 0; k < 11u; ++k)
            {
                const uint32_t has = word(0x4A43B0u + k * 0x28u);
                held = held || has == kind;
                room = room || has == 0xFFFFFFFFu;
            }
            if (!held && !room)
                return "All 11 creature kinds the game can hold are in use here: " + name + " can't be loaded.";
            if (static_cast<int16_t>(half(0x4A4774u)) < 4)
                return "No room for another creature just now.";
            // where: on the ground where the middle of the view rests (groundInView), not on top of the player
            float at[3], offset[3];
            std::memcpy(at, rdram + inner + 0x1C0u, 12);
            std::memcpy(offset, rdram + record + 0x10u, 12);
            const Ground ground = groundInView(rdram, ctx, runtime, inner);
            if (!ground.found)
                return ground.why;
            {
                const float dx = ground.point[0] - at[0], dz = ground.point[2] - at[2];
                if (dx * dx + dz * dz < 150.0f * 150.0f)
                    return "Too close: look at the ground a little further away.";
            }
            const float place[3] = {ground.point[0] - offset[0], ground.point[1] - offset[1], ground.point[2] - offset[2]};
            std::printf("[STAR debug] the view rests on the ground at %.0f %.0f %.0f (the player at %.0f %.0f %.0f)%c", ground.point[0], ground.point[1],
                        ground.point[2], at[0], at[1], at[2], 10);
            // which record: one that is not fetched; of those, one whose own creature is used up, else the farthest away
            const uint32_t savedMaps = word(0x201DC4u);
            int mapIndex = -1;
            for (int k = 0; k < kMapCount; ++k)
                if (mapAt(k) == static_cast<int>(word(record)))
                    mapIndex = k;
            const auto kills = [&](uint32_t number) -> uint32_t
            {
                if (mapIndex < 0 || !inRam(savedMaps))
                    return 0u;
                return (word(savedMaps + static_cast<uint32_t>(mapIndex) * 0x1C0u + 0x20u + (number >> 3) * 4u) >> ((number & 7u) * 4u)) & 0xFu;
            };
            // Which record. Best one whose own creature is used up (killed as often as it may be); then the farthest of
            // those not fetched; and where all are fetched (a map with two or three creatures), one whose creature is
            // loaded but not there (dead, or not made): that creature is given back first, as the game does when it
            // lets a map go (FUN_001a6a10(holder), from FUN_001b5640). One that is there is never taken.
            bool numberUsed[64] = {};
            int best = -1;
            float bestScore = -1.0f;
            int counts[3] = {}; // not fetched, fetched and not there, there (for the log)
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t r = list + i * 0x54u;
                const uint32_t number = half(r + 0x1Au) % 100u;
                if (number >= 64u)
                    continue;
                numberUsed[number] = true;
                const uint32_t slot = word(slots + i * 8u);
                const uint32_t creature = inRam(slot) ? word(slot) : 0u;
                const bool fetched = slot != 0u || (word(record + 0x90u + (number >> 5) * 4u) & (1u << (number & 31u))) != 0u;
                bool there = false;
                if (fetched)
                {
                    // (still being loaded, bit 0 clear, counts as there: it is not ours to give back yet)
                    there = !inRam(creature) || (rdram[creature + 0x5A0u] & 1u) == 0u || (rdram[creature + 0x5A0u] & 4u) != 0u;
                    bool awaited = false;
                    for (const Awaited &one : g_awaited)
                        awaited = awaited || (one.record == record && one.index == i);
                    there = there || awaited;
                }
                ++counts[!fetched ? 0 : (there ? 2 : 1)];
                if (there)
                    continue;
                float theirs[3];
                std::memcpy(theirs, rdram + r, 12);
                const float dx = theirs[0] + offset[0] - at[0], dz = theirs[2] + offset[2] - at[2];
                const bool usedUp = kills(number) >= rdram[r + 0x2Cu];
                const float score = (usedUp ? 2e12f : 0.0f) + (fetched ? 0.0f : 1e12f) + std::min(dx * dx + dz * dz, 9e11f);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = static_cast<int>(i);
                }
            }
            if (best < 0)
                return "All " + std::to_string(counts[2]) + " creatures of this map are here: kill one, or try a larger map.";
            const uint32_t r = list + static_cast<uint32_t>(best) * 0x54u;
            if (const uint32_t slot = word(slots + static_cast<uint32_t>(best) * 8u); slot != 0u)
            {
                callGame(rdram, ctx, runtime, 0x001A6A10u, slot, 0u, 0u);
                const uint32_t zero = 0u;
                std::memcpy(rdram + slots + static_cast<uint32_t>(best) * 8u, &zero, 4);
                const uint32_t old = half(r + 0x1Au) % 100u;
                for (const uint32_t bits : {0x90u, 0x98u, 0xA0u})
                {
                    uint32_t value = word(record + bits + (old >> 5) * 4u) & ~(1u << (old & 31u));
                    std::memcpy(rdram + record + bits + (old >> 5) * 4u, &value, 4);
                }
                std::printf("[STAR debug] record %d's own creature (loaded, not there) was given back to make room%c", best, 10);
            }
            std::printf("[STAR debug] this map's creature list: %d not fetched, %d fetched and not there, %d there%c", counts[0], counts[1], counts[2], 10);
            numberUsed[half(r + 0x1Au) % 100u] = false; // (its own number becomes free)
            uint32_t number = 64u;
            for (uint32_t n = 63u; n > 0u && number == 64u; --n) // from the top: the map's own numbers start low
                if (!numberUsed[n])
                    number = n;
            if (number == 64u)
                return "This map's creature list is full.";
            // the rest of the record as another of the same kind in this map has it, else as most creatures have it
            uint8_t rest[0x54 - 0x1C] = {};
            const auto put16 = [](uint8_t *to, uint16_t value) { std::memcpy(to, &value, 2); };
            put16(rest + (0x22 - 0x1C), 1000);
            put16(rest + (0x24 - 0x1C), 1000);
            rest[0x2D - 0x1C] = 31;
            for (uint32_t i = 0; i < count; ++i)
                if (half(list + i * 0x54u + 0x18u) == kind && static_cast<int>(i) != best)
                {
                    std::memcpy(rest, rdram + list + i * 0x54u + 0x1Cu, sizeof(rest));
                    break;
                }
            put16(rest + (0x1E - 0x1C), 60000); // fetched from anywhere in the map
            put16(rest + (0x20 - 0x1C), 0);
            put16(rest + (0x26 - 0x1C), 60000); // and made at once
            put16(rest + (0x28 - 0x1C), 0);
            put16(rest + (0x2A - 0x1C), 100);
            rest[0x2C - 0x1C] = 15;             // may be killed as often as the count can tell
            rest[0x30 - 0x1C] = 8;              // beside the map's limit, and it stays
            uint8_t whole[0x54] = {};
            std::memcpy(whole, place, 12);
            const uint16_t size = 0x1000;
            for (int k = 0; k < 3; ++k)
                std::memcpy(whole + 0x12 + k * 2, &size, 2);
            put16(whole + 0x18, static_cast<uint16_t>(kind));
            put16(whole + 0x1A, static_cast<uint16_t>(number));
            std::memcpy(whole + 0x1C, rest, sizeof(rest));
            std::memcpy(rdram + r, whole, sizeof(whole));
            // a clean slate for that number: the bits, and the count of kills in the save data
            for (const uint32_t bits : {0x90u, 0x98u, 0xA0u})
            {
                uint32_t value = word(record + bits + (number >> 5) * 4u) & ~(1u << (number & 31u));
                std::memcpy(rdram + record + bits + (number >> 5) * 4u, &value, 4);
            }
            if (mapIndex >= 0 && inRam(savedMaps))
            {
                const uint32_t where = savedMaps + static_cast<uint32_t>(mapIndex) * 0x1C0u + 0x20u + (number >> 3) * 4u;
                uint32_t value = word(where) & ~(0xFu << ((number & 7u) * 4u));
                std::memcpy(rdram + where, &value, 4);
            }
            g_awaited.push_back({record, word(record), slots, static_cast<uint32_t>(best), name, (options & 2) != 0});
            // its sounds: its area's bank, unless the bank that is loaded has them
            std::string sounds;
            if ((options & 1) != 0)
            {
                const uint16_t lives = areasOfKind(kind);
                if (lives != 0u && (lives >> soundArea() & 1u) == 0u)
                {
                    uint32_t area = 1u;
                    while ((lives >> area & 1u) == 0u)
                        ++area;
                    wantSounds(rdram, area);
                    sounds = " Its sounds (" + areaName(static_cast<int>(area * 10u)) + ") replace this area's.";
                }
            }
            std::printf("[STAR debug] creature kind %u asked for: record %d of %u in map %u, number %u, at %.0f %.0f %.0f (%s)%c", kind, best, count,
                        word(record), number, place[0], place[1], place[2], held ? "its kind is loaded" : "its kind has to be loaded", 10);
            return "Spawning " + name + (held ? "..." : " (loading its kind first)...") + sounds;
        }

        // The game thread, every frame: what has become of the creatures asked for.
        void watchCreatures(std::string &answer)
        {
            for (size_t i = 0; i < g_awaited.size();)
            {
                Awaited &one = g_awaited[i];
                const uint32_t holder = word(0x2022BCu);
                bool done = false;
                if (!inRam(holder) || word(holder) != one.record || word(one.record) != one.map)
                {
                    answer = one.name + ": you left the map before it appeared.";
                    done = true;
                }
                else if (const uint32_t slot = word(one.slots + one.index * 8u); inRam(slot) && inRam(word(slot)))
                {
                    const uint32_t creature = word(slot);
                    if (!one.fetched)
                    {
                        one.fetched = true;
                        std::printf("[STAR debug] %s: fetched after %d frames (creature %08x)%c", one.name.c_str(), one.frames, creature, 10);
                    }
                    if (one.calm)
                        g_ram[creature + 0x5B4u] |= 0x10u; // (from the first: it shall not notice the player on arriving either)
                    if ((g_ram[creature + 0x5A0u] & 4u) != 0u)
                    {
                        answer = one.name + (one.calm ? " is here (it ignores you)." : " is here.");
                        {
                            const uint32_t theirs = word(creature + 0x518u) & 0x1FFFFFFFu;
                            if (inRam(theirs))
                            {
                                float at[3];
                                std::memcpy(at, g_ram + theirs + 0x1C0u, 12);
                                std::printf("[STAR debug] %s stands at %.0f %.0f %.0f%c", one.name.c_str(), at[0], at[1], at[2], 10);
                            }
                        }
                        if (one.calm)
                            g_calm.push_back({one.record, one.slots, one.index, creature});
                        done = true;
                    }
                }
                if (!done && ++one.frames > 600)
                {
                    answer = one.name + (one.fetched ? " was loaded but didn't appear within 20 seconds." : ": the game didn't take it within 20 seconds.");
                    done = true;
                }
                if (done)
                    g_awaited.erase(g_awaited.begin() + static_cast<std::ptrdiff_t>(i));
                else
                    ++i;
            }
        }

        // ---- the page ------------------------------------------------------------------------------------------------------
        void goTo(const Place &place, const std::string &name)
        {
            Request request;
            request.kind = Request::Kind::Go;
            request.place = place;
            request.name = name;
            ask(std::move(request));
        }

        void drawMaps(const Pcsx2GsUi *ui, const Place &here, bool known)
        {
            ui->text_note("Every map, by the names on the game's map-data screen. A jump puts you where the map places a creature or a "
                          "person, so something may be standing there. The map counts as visited; nothing else in the story changes, so "
                          "a door on the way back may still be locked.");
            ui->begin_disabled(known ? 0 : 1);
            if (ui->begin_child("maps", 0.0f, 0.0f))
            {
                int area = -1;
                for (int index = 0; index < kMapCount; ++index)
                {
                    const int map = mapAt(index);
                    if (map <= 0 || map > 255)
                        continue;
                    if (map / 10 != area)
                    {
                        area = map / 10;
                        ui->separator_text(areaName(map).c_str());
                    }
                    ui->push_id(index);
                    const MapSpot *spot = spotOf(map);
                    ui->begin_disabled(spot ? 0 : 1);
                    if (ui->small_button("Go") && spot)
                    {
                        Place place;
                        place.map = spot->map;
                        place.x = spot->x;
                        place.y = spot->y;
                        place.z = spot->z;
                        goTo(place, areaName(map) + ", map " + std::to_string(map));
                    }
                    ui->end_disabled();
                    ui->same_line();
                    char text[96];
                    std::snprintf(text, sizeof(text), "Map %d%s", map, known && here.map == map ? "  (you are here)" : "");
                    ui->text(text);
                    if (!visited(index))
                    {
                        ui->same_line();
                        ui->text_disabled(spot ? "not visited" : "not visited, no spot known");
                    }
                    else if (!spot)
                    {
                        ui->same_line();
                        ui->text_disabled("no spot known");
                    }
                    ui->pop_id();
                }
            }
            ui->end_child();
            ui->end_disabled();
        }

        void drawSpots(const Pcsx2GsUi *ui, Place here, bool known)
        {
            ui->begin_disabled(known ? 0 : 1);
            if (ui->button("Save this spot"))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                loadPlaces();
                here.own = true;
                here.name = "Spot " + std::to_string(g_own.size() + 1u) + " (map " + std::to_string(here.map) + ")";
                g_own.push_back(here);
                savePlaces();
                g_answer = "Saved as \"" + here.name + "\" (rename it in " + g_placesFile.filename().string() + ").";
            }
            std::vector<Place> places(std::begin(kPlaces), std::end(kPlaces));
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                loadPlaces();
                places.insert(places.end(), g_own.begin(), g_own.end());
            }
            int forget = -1, ownIndex = 0;
            if (ui->begin_child("places", 0.0f, 0.0f))
            {
                for (size_t i = 0; i < places.size(); ++i)
                {
                    const Place &place = places[i];
                    ui->push_id(static_cast<int>(i));
                    if (ui->small_button("Go"))
                        goTo(place, place.name);
                    ui->same_line();
                    ui->text(place.name.c_str());
                    if (place.own)
                    {
                        ui->same_line();
                        if (ui->small_button("Delete"))
                            forget = ownIndex;
                        ++ownIndex;
                    }
                    ui->pop_id();
                }
            }
            ui->end_child();
            ui->end_disabled();
            if (forget >= 0)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (static_cast<size_t>(forget) < g_own.size())
                {
                    g_own.erase(g_own.begin() + forget);
                    savePlaces();
                }
            }
        }

        void drawItems(const Pcsx2GsUi *ui, bool known)
        {
            if (g_tables.items.empty())
            {
                ui->text_disabled("The item names load with the first area.");
                return;
            }
            ui->text_note("Items go into your inventory (the Item and Equipment menus), as if picked up. The game allows one of each "
                          "weapon and armour piece; one in storage is taken out of it. Enchanted weapons are entries of their own (the "
                          "same name, a note beside it).");
            static int kind = 0, onlyEffects = 0;
            ui->set_next_width(380.0f);
            ui->combo("Type", &kind, kKinds, kKindCount);
            ui->checkbox("Only items with a magic effect (enchanted, glowing)", &onlyEffects);
            ui->begin_disabled(known ? 0 : 1);
            if (ui->begin_child("items", 0.0f, 0.0f))
            {
                for (const Entry &item : g_tables.items)
                {
                    if (item.number >= 0x400u || kindOf(item.number) != kind || (onlyEffects != 0 && !hasEffect(item.number)))
                        continue;
                    ui->push_id(static_cast<int>(item.number));
                    const bool many = stacks(item.number);
                    const int have = carried(item.number), kept = stored(item.number);
                    if (ui->small_button(many ? "+1" : "Give"))
                        ask(Request{Request::Kind::Give, item.number, 1, {}, item.name});
                    if (many)
                    {
                        ui->same_line();
                        if (ui->small_button("+10"))
                            ask(Request{Request::Kind::Give, item.number, 10, {}, item.name});
                    }
                    ui->same_line();
                    char text[128];
                    if (many && have > 0 && kept > 0)
                        std::snprintf(text, sizeof(text), "%s  (In Inventory: %d, in storage: %d)", item.name.c_str(), have, kept);
                    else if (many && have > 0)
                        std::snprintf(text, sizeof(text), "%s  (In Inventory: %d)", item.name.c_str(), have);
                    else if (many && kept > 0)
                        std::snprintf(text, sizeof(text), "%s  (in storage: %d)", item.name.c_str(), kept);
                    else if (have > 0)
                        std::snprintf(text, sizeof(text), "%s  (In Inventory)", item.name.c_str());
                    else if (kept > 0)
                        std::snprintf(text, sizeof(text), "%s  (in storage)", item.name.c_str());
                    else
                        std::snprintf(text, sizeof(text), "%s", item.name.c_str());
                    ui->text(text);
                    if (!item.note.empty())
                    {
                        ui->same_line();
                        ui->text_disabled(item.note.c_str());
                    }
                    ui->pop_id();
                }
            }
            ui->end_child();
            ui->end_disabled();
        }

        // Which areas a kind of creature lives in (bit a = area a, a map's number / 10; kinds 92, 94, 95 and 98 have no
        // name in the game's list): from the creature lists of the
        // game's 42 map files (every kind was asked for once in the hub in a hidden run, tools\creaturesweep.py: all
        // 72 were made and the game ran on; twelve killed the player, who stood still, within seconds) (tools\mapdump.py over the files written with STAR_DEBUG_MAPDUMP).
        struct KindAreas
        {
            uint16_t kind, areas;
        };
        const KindAreas kKindAreas[] = {
            {21, 0x0002}, {22, 0x0002}, {23, 0x0002}, {24, 0x0002}, {25, 0x0004}, {26, 0x0008}, {27, 0x0010}, {28, 0x0010}, {29, 0x0010},
            {30, 0x0010}, {31, 0x0010}, {32, 0x0010}, {33, 0x0020}, {34, 0x0020}, {35, 0x0020}, {36, 0x0020}, {37, 0x0040}, {38, 0x0040},
            {39, 0x0040}, {40, 0x0040}, {41, 0x0040}, {42, 0x0080}, {43, 0x0080}, {44, 0x0080}, {45, 0x0080}, {46, 0x0080}, {47, 0x0100},
            {48, 0x0200}, {49, 0x0200}, {50, 0x0200}, {51, 0x0200}, {52, 0x0200}, {53, 0x0200}, {54, 0x0200}, {55, 0x0200}, {56, 0x0200},
            {57, 0x0400}, {58, 0x0400}, {59, 0x0400}, {60, 0x0400}, {61, 0x0800}, {62, 0x0800}, {63, 0x0800}, {64, 0x0800}, {65, 0x1000},
            {66, 0x1000}, {67, 0x1000}, {68, 0x2000}, {69, 0x2000}, {70, 0x2000}, {71, 0x2000}, {72, 0x2000}, {73, 0x2000}, {81, 0x1000},
            {82, 0x1000}, {83, 0x1000}, {91, 0x8000}, {92, 0x0010}, {94, 0x0002}, {95, 0x0002}, {98, 0x0300}, {101, 0x0100}, {102, 0x0100}, {103, 0x0100}, {104, 0x0100}, {105, 0x0100},
            {107, 0x0010}, {108, 0x0100}, {109, 0x0400}, {110, 0x0800}, {118, 0x8000}, {119, 0x4028},
        };

        uint16_t areasOfKind(uint32_t kind)
        {
            for (const KindAreas &one : kKindAreas)
                if (one.kind == kind)
                    return one.areas;
            return 0u;
        }

        void drawCreatures(const Pcsx2GsUi *ui, const Place &here, bool known)
        {
            if (g_tables.creatures.empty())
            {
                ui->text_disabled("The creature names load with the first area.");
                return;
            }
            ui->text_note("A creature appears on the ground at the centre of your view (in front of a wall if you look at one); with "
                          "no ground there, none appears. Its kind is loaded if this map lacks it (11 kinds at a time). It takes the "
                          "place of one of this map's creatures that is not near you, until you leave the map and come back; killing "
                          "it does not count toward the map's own.");
            // the areas that have creatures, by the game's names; first "where you are"
            static int choice = 0;
            std::vector<int> areas{0};
            std::vector<std::string> labels{known ? "This area (" + areaName(here.map) + ")" : std::string("This area")};
            for (int area = 1; area <= 15; ++area)
            {
                bool any = false;
                for (const KindAreas &one : kKindAreas)
                    any = any || (one.areas >> area & 1) != 0;
                if (any)
                {
                    areas.push_back(area);
                    labels.push_back(areaName(area * 10));
                }
            }
            std::vector<const char *> texts;
            for (const std::string &label : labels)
                texts.push_back(label.c_str());
            choice = std::clamp(choice, 0, static_cast<int>(areas.size()) - 1);
            ui->set_next_width(380.0f);
            ui->combo("Area", &choice, texts.data(), static_cast<int>(texts.size()));
            const int area = areas[static_cast<size_t>(choice)] != 0 ? areas[static_cast<size_t>(choice)] : (known ? here.map / 10 : -1);
            static int withSounds = 1, calm = 0;
            ui->checkbox("Load its sounds", &withSounds);
            ui->text_note("The game holds one area's creature sounds at a time: a creature from another area brings its area's, and this "
                          "area's creatures and ambient sounds go quiet meanwhile.");
            ui->checkbox("Ignores you", &calm);
            if (known)
            {
                const uint32_t sounds = soundArea(), own = static_cast<uint32_t>(here.map) / 10u;
                const std::string line = "Creature sounds: " + areaName(static_cast<int>(sounds * 10u)) + (sounds == own ? " (this area's)." : ".");
                ui->text(line.c_str());
                if (sounds != own)
                {
                    ui->same_line();
                    if (ui->small_button("Back to this area's sounds"))
                        ask(Request{Request::Kind::Sounds, 0u, 0, {}, ""});
                }
            }
            ui->begin_disabled(known ? 0 : 1);
            if (ui->begin_child("creatures", 0.0f, 0.0f))
            {
                int shown = 0;
                for (const KindAreas &one : kKindAreas)
                {
                    if (area < 0 || (one.areas >> area & 1) == 0)
                        continue;
                    std::string name = "Kind " + std::to_string(one.kind) + " (no name in the game)", note;
                    for (const Entry &creature : g_tables.creatures)
                        if (creature.number == one.kind)
                        {
                            name = creature.name;
                            note = creature.note;
                        }
                    ++shown;
                    ui->push_id(static_cast<int>(one.kind));
                    if (ui->small_button("Spawn"))
                        ask(Request{Request::Kind::Creature, one.kind, (withSounds ? 1 : 0) | (calm ? 2 : 0), {}, name});
                    ui->same_line();
                    ui->text(name.c_str());
                    if (!note.empty())
                    {
                        ui->same_line();
                        ui->text_disabled(note.c_str());
                    }
                    ui->pop_id();
                }
                if (shown == 0)
                    ui->text_disabled("No creatures of its own here. Choose another area above: any kind can be spawned here.");
            }
            ui->end_child();
            ui->end_disabled();
        }

        void drawDebug(const Pcsx2GsUi *ui)
        {
            ui->text_note("For testing. These change the game's data: a jump restarts the area as loading a save does (its "
                          "creatures come back).");
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_answer.empty())
                    ui->text_colored(0xFF80FF80u, g_answer.c_str());
            }
            findTables();
            Place here;
            const bool known = whereNow(here);
            {
                char text[200];
                if (known)
                    std::snprintf(text, sizeof(text), "You are in %s, map %d, at %d, %d, %d.", areaName(here.map).c_str(), here.map, here.x, here.y, here.z);
                else
                    std::snprintf(text, sizeof(text), "Not in play just now.");
                ui->text(text);
            }
            bool goBack = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                goBack = g_haveBefore;
            }
            if (goBack && known)
            {
                ui->same_line();
                if (ui->small_button("Back to before the last jump"))
                {
                    Place before;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        before = g_before;
                    }
                    goTo(before, "where you were");
                }
            }
            if (ui->begin_tab_bar("##debug"))
            {
                // (STAR_DEBUG_PAGE_FIRST=2, tests: the Creatures tab first, so that a run which opens the menu draws it)
                static const bool creaturesFirst = [] { const char *v = std::getenv("STAR_DEBUG_PAGE_FIRST"); return v && *v == '2'; }();
                if (creaturesFirst && ui->begin_tab("Creatures "))
                {
                    drawCreatures(ui, here, known);
                    ui->end_tab();
                }
                if (ui->begin_tab("Maps"))
                {
                    drawMaps(ui, here, known);
                    ui->end_tab();
                }
                if (ui->begin_tab("Saved spots"))
                {
                    drawSpots(ui, here, known);
                    ui->end_tab();
                }
                if (ui->begin_tab("Items"))
                {
                    drawItems(ui, known);
                    ui->end_tab();
                }
                if (ui->begin_tab("Creatures"))
                {
                    drawCreatures(ui, here, known);
                    ui->end_tab();
                }
                ui->end_tab_bar();
            }
        }
    }

    void debugMenuGameThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint64_t frame)
    {
        if (!g_ram)
            return;
        if (static const char *const test = std::getenv("STAR_DEBUG_TEST"); test && *test)
        {
            // STAR_DEBUG_TEST=<frame>:give:<item>:<count>[,...] or <frame>:go:<map>:<x>:<y>:<z> (tests): what the
            // page's buttons ask for, at those frames of play
            static std::vector<std::string> steps = []
            {
                std::vector<std::string> list;
                std::string all(std::getenv("STAR_DEBUG_TEST"));
                size_t from = 0;
                while (from <= all.size())
                {
                    const size_t comma = all.find(',', from);
                    list.push_back(all.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
                    if (comma == std::string::npos)
                        break;
                    from = comma + 1;
                }
                return list;
            }();
            for (std::string &step : steps)
            {
                unsigned long long at = 0;
                unsigned item = 0;
                int count = 0;
                Place place;
                if (step.empty())
                    continue;
                if (const int got = std::sscanf(step.c_str(), "%llu:creature:%u:%d", &at, &item, &count); got >= 2 && frame >= at)
                {
                    // (<frame>:creature:<kind>[:<options>]: 1 with its sounds, 2 it does not notice the player)
                    ask(Request{Request::Kind::Creature, item, got == 3 ? count : 0, {}, "creature " + std::to_string(item)});
                    step.clear();
                }
                else if (std::sscanf(step.c_str(), "%llu:sounds:%u", &at, &item) == 2 && frame >= at)
                {
                    ask(Request{Request::Kind::Sounds, item, 0, {}, ""});
                    step.clear();
                }
                else if (std::sscanf(step.c_str(), "%llu:probe:%u", &at, &item) == 2 && frame >= at)
                {
                    // (<frame>:probe:0: the game's floor straight down under the player)
                    ask(Request{Request::Kind::Probe, item, 0, {}, ""});
                    step.clear();
                }
                else if (std::sscanf(step.c_str(), "%llu:have:%u", &at, &item) == 2 && frame >= at)
                {
                    // (<frame>:have:<item>: how many the player carries and how many are in the store, to the log)
                    std::printf("[STAR debug] item %u: carried %d, in the store %d, map %u (record %08x, save data %08x, player %08x)%c", item, carried(item), stored(item),
                                word(0x2022B8u), word(0x201E20u), word(0x201DC0u), inRam(word(0x2022E4u)) ? word(word(0x2022E4u) + 0x518u) & 0x1FFFFFFFu : 0u, 10);
                    step.clear();
                }
                else if (std::sscanf(step.c_str(), "%llu:give:%u:%d", &at, &item, &count) == 3 && frame >= at)
                {
                    ask(Request{Request::Kind::Give, item, count, {}, "item " + std::to_string(item)});
                    step.clear();
                }
                else if (std::sscanf(step.c_str(), "%llu:go:%d:%d:%d:%d", &at, &place.map, &place.x, &place.y, &place.z) == 5 && frame >= at)
                {
                    Request request;
                    request.kind = Request::Kind::Go;
                    request.place = place;
                    request.name = "map " + std::to_string(place.map);
                    ask(std::move(request));
                    step.clear();
                }
            }
            static uint32_t lastMap = ~0u;
            if (const uint32_t now = word(0x2022B8u); now != lastMap)
            {
                lastMap = now;
                Place here;
                whereNow(here);
                std::printf("[STAR debug] frame %llu: in map %u at %d %d %d%c", static_cast<unsigned long long>(frame), now, here.x, here.y, here.z, 10);
            }
            if (static const char *const every = std::getenv("STAR_DEBUG_WHERE"); every && std::atoi(every) > 0 && frame % static_cast<uint64_t>(std::atoi(every)) == 0u)
            {
                // STAR_DEBUG_WHERE=<n> (tests): where the player is, every n frames of play
                Place here;
                whereNow(here);
                const uint32_t player = word(0x2022E4u);
                const uint32_t inner = inRam(player) ? word(player + 0x518u) & 0x1FFFFFFFu : 0u;
                float y = 0.0f;
                if (inRam(inner))
                    std::memcpy(&y, g_ram + inner + 0x1C4u, 4);
                std::printf("[STAR debug] frame %llu: map %u at %d %.1f %d%c", static_cast<unsigned long long>(frame), word(0x2022B8u), here.x, y, here.z, 10);
            }
            if (static const char *const folder = std::getenv("STAR_DEBUG_MAPDUMP"); folder && *folder)
            {
                // STAR_DEBUG_MAPDUMP=<folder> (tests): each map the game has loaded, once: its record (0x1e0 bytes) and
                // the four lists at record + 0x134.. (parts of the map 0x30 bytes each, things 0x30, creatures 0x54,
                // people 0x34; how many: the half words at record + 0x6c..), as <folder>/map_<n>.bin
                static std::vector<uint32_t> done;
                for (const uint32_t slot : {0x201E88u, 0x201E8Cu})
                {
                    const uint32_t holder = word(slot);
                    const uint32_t record = inRam(holder) ? word(holder) : 0u;
                    if (!inRam(record) || !inRam(word(record + 0x134u)))
                        continue;
                    const uint32_t map = word(record);
                    if (map == 0u || map > 255u || std::find(done.begin(), done.end(), map) != done.end())
                        continue;
                    done.push_back(map);
                    std::error_code ignored;
                    std::filesystem::create_directories(folder, ignored);
                    std::ofstream out(std::filesystem::path(folder) / ("map_" + std::to_string(map) + ".bin"), std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char *>(g_ram + record), 0x1E0);
                    const uint32_t sizes[4] = {0x30u, 0x30u, 0x54u, 0x34u};
                    for (uint32_t k = 0; k < 4u; ++k)
                    {
                        uint16_t count = 0;
                        std::memcpy(&count, g_ram + record + 0x6Cu + k * 2u, 2);
                        const uint32_t at = word(record + 0x134u + k * 4u);
                        if (inRam(at) && inRam(at + count * sizes[k]))
                            out.write(reinterpret_cast<const char *>(g_ram + at), static_cast<std::streamsize>(count) * sizes[k]);
                    }
                    // the links to the maps next door: 8 bytes each from *(record + 0x158) + 0x36
                    if (const uint32_t links = word(record + 0x158u); inRam(links))
                        out.write(reinterpret_cast<const char *>(g_ram + links), 0x120);
                    std::printf("[STAR debug] frame %llu: map %u is loaded (written out)%c", static_cast<unsigned long long>(frame), map, 10);
                }
            }
        }
        std::vector<Request> requests;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            requests.swap(g_requests);
        }
        std::string answer;
        watchCreatures(answer);
        keepCalm();
        for (const Request &request : requests)
        {
            const uint32_t save = word(0x201DC0u);
            if (!inRam(save))
                continue;
            if (request.kind == Request::Kind::Probe)
            {
                const uint32_t player = word(0x2022E4u);
                const uint32_t inner = inRam(player) ? word(player + 0x518u) & 0x1FFFFFFFu : 0u;
                if (inRam(inner))
                {
                    const OpenCollision open(rdram);
                    float eye[3], y = 0.0f;
                    std::memcpy(eye, rdram + inner, 12);
                    const bool hit = floorBelow(rdram, ctx, runtime, eye, 0.0f, 2000.0f, y);
                    std::printf("[STAR debug] probe: the floor under the eye %s %.1f (eye %.1f %.1f %.1f)%c", hit ? "at" : "none,", y, eye[0], eye[1], eye[2], 10);
                }
                continue;
            }
            if (request.kind == Request::Kind::Creature)
                answer = askCreature(rdram, ctx, runtime, request.item, request.name, request.count);
            else if (request.kind == Request::Kind::Sounds)
                answer = wantSounds(rdram, request.item);
            else if (request.kind == Request::Kind::Give)
            {
                if (request.item == 0u || request.item >= 0x400u)
                    continue;
                // Into the player's hands, the way the game itself hands things over: FUN_001cc0c0(item, 1) is its "may
                // the player take one more" (things that stack: fewer than the item's limit, counting the store; the
                // rest: not carried, stored or lying somewhere already), FUN_001cbed0(item, how many, durability) adds
                // to what is carried, FUN_001cbcd0(item) is that for one armour piece or weapon, new and loaded.
                // (FUN_001a95a0, used here at first, fills the STORE: the user's "does not appear in my inventory")
                const int before = carried(request.item);
                bool fromStore = false;
                if (stacks(request.item))
                {
                    for (int k = 0; k < request.count && callGame(rdram, ctx, runtime, 0x001CC0C0u, request.item, 1u) == 1u; ++k)
                        callGame(rdram, ctx, runtime, 0x001CBED0u, request.item, 1u, 0u);
                }
                else if (before == 0)
                {
                    if (stored(request.item) > 0)
                    {
                        callGame(rdram, ctx, runtime, 0x001A9510u, request.item); // out of the store (the game's own)
                        fromStore = true;
                    }
                    if (callGame(rdram, ctx, runtime, 0x001CC0C0u, request.item, 1u) == 1u)
                        callGame(rdram, ctx, runtime, 0x001CBCD0u, request.item);
                }
                const int after = carried(request.item);
                if (after > before && stacks(request.item) && after - before < request.count)
                    answer = "Given: " + request.name + " x" + std::to_string(after - before) + " (the most the game allows).";
                else if (after > before)
                    answer = "Given: " + request.name + (after - before > 1 ? " x" + std::to_string(after - before) : "") +
                             (fromStore ? " (taken out of storage)." : ".");
                else if (!stacks(request.item) && before > 0)
                    answer = "You already have " + request.name + ".";
                else
                    answer = "The game refused " + request.name + (stacks(request.item) ? " (you have the most it allows)." : " (it is already somewhere in the world).");
            }
            else
            {
                Place here;
                const bool known = whereNow(here);
                // Keep what the player carries, wears and is (2026-11-26; the user: "teleporting to a different area
                // doesn't keep any spawned items after the level load"): the restart loads the player's record from
                // the save data (FUN_001c9bb0: 0x10a0 bytes from save data + 0x20 onto the record at *(0x201e20)), so
                // the record goes there first, the way the game keeps it. The place below lies inside that copy
                // (save data + 0x2f = record + 0x0f), so it is written after.
                if (const uint32_t record = word(0x201E20u); inRam(record) && inRam(save + 0x20u + 0x10A0u) && inRam(record + 0x10A0u))
                    std::memmove(rdram + save + 0x20u, rdram + record, 0x10A0u);
                // the game's own "start play again from the saved place" (FUN_0012c0a0), with the place set first
                rdram[save + 0x2Fu] = static_cast<uint8_t>(request.place.map);
                const int16_t at[3] = {static_cast<int16_t>(request.place.x), static_cast<int16_t>(request.place.y), static_cast<int16_t>(request.place.z)};
                std::memcpy(rdram + save + 0x30u, at, sizeof(at));
                const uint32_t one = 1u;
                std::memcpy(rdram + 0x201EA4u, &one, 4);
                std::memcpy(rdram + 0x201E9Cu, &one, 4);
                std::memcpy(rdram + 0x201E98u, &one, 4);
                answer = "Jumped to " + request.name + ".";
                std::lock_guard<std::mutex> lock(g_mutex);
                if (known)
                {
                    g_before = here;
                    g_haveBefore = true;
                }
                std::printf("[STAR debug] jump to map %d at %d %d %d%c", request.place.map, request.place.x, request.place.y, request.place.z, 10);
                break; // (one jump at a time; what else was asked is dropped)
            }
        }
        if (!answer.empty())
        {
            std::printf("[STAR debug] %s%c", answer.c_str(), 10);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_answer = std::move(answer);
        }
    }

    namespace
    {
        // STAR_SOUND_LOG=1 (tests): every sound the game starts (FUN_00131640: the number in *(0x201ed0) + 0x30; four
        // bytes a sound at 0x222bc0: program, note, bank, bits; a bank's "loaded" byte at 0x201ed4 + bank).
        PS2Runtime::RecompiledFunction g_startSound = nullptr;
        void startSoundHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t voice = word(0x201ED0u);
            const uint32_t sound = inRam(voice) ? word(voice + 0x30u) : ~0u;
            if (sound < 0x2800u)
            {
                const uint8_t *entry = rdram + 0x222BC0u + sound * 4u;
                static uint32_t last = ~0u;
                static int repeats = 0;
                if (sound != last || ++repeats < 3)
                {
                    if (sound != last)
                        repeats = 0;
                    last = sound;
                    std::printf("[sound] %u: program %d note %d bank %u (%s) bits %02x, from %08x%c", sound, static_cast<int8_t>(entry[0]),
                                static_cast<int8_t>(entry[1]), entry[2], entry[2] < 4u && rdram[0x201ED4u + entry[2]] ? "loaded" : "NOT loaded", entry[3],
                                getRegU32(ctx, 31), 10);
                }
            }
            g_startSound(rdram, ctx, runtime); // nothing after the call
        }
    }

    void installDebugMenu(PS2Runtime &runtime, const std::filesystem::path &settingsFile)
    {
        if (const char *log = std::getenv("STAR_SOUND_LOG"); log && *log == '1' && runtime.hasFunction(0x131640u))
        {
            g_ram = runtime.memory().getRDRAM();
            g_startSound = runtime.lookupFunction(0x131640u);
            if (!runtime.replaceFunction(0x131640u, &startSoundHook))
                g_startSound = nullptr;
        }
        g_placesFile = settingsFile.parent_path() / "STAR places.txt";
        const char *on = std::getenv("STAR_DEBUG_MENU");
        const bool enabled = on && *on == '1';
        // STAR_DEBUG_TEST=<frame of play>:give:<item>:<count> or <frame>:go:<map>:<x>:<y>:<z> (tests) need the game
        // thread's side also without the page
        if (!enabled && !std::getenv("STAR_DEBUG_TEST"))
            return;
        g_ram = runtime.memory().getRDRAM();
        if (runtime.hasFunction(0x1E1D30u) && runtime.hasFunction(0x12DD10u))
        {
            g_askBank = runtime.lookupFunction(0x1E1D30u);
            g_bankLoaded = runtime.lookupFunction(0x12DD10u);
            const bool first = g_askBank && runtime.replaceFunction(0x1E1D30u, &askBankHook);
            const bool second = g_bankLoaded && runtime.replaceFunction(0x12DD10u, &bankLoadedHook);
            g_soundsReady = first && second; // (otherwise the page says the sounds cannot be changed)
        }
        if (enabled)
        {
            ps2x::menu::addPanel("Debug", &drawDebug);
            // STAR_DEBUG_PAGE_FIRST=1 (tests): the page as the menu's first tab, so that a run which opens the menu
            // by itself (PS2X_GAME_MENU_OPEN_AT) draws it
            if (const char *first = std::getenv("STAR_DEBUG_PAGE_FIRST"); first && (*first == '1' || *first == '2'))
                ps2x::menu::setTabOrder({"Debug"});
            std::printf("[STAR option] debug page: on%c", 10);
        }
    }
}
