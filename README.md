# mod-profession-loot-party

AzerothCore WotLK 3.3.5a module that gives eligible group/raid members an **independent profession-loot roll** when another group member successfully gathers a resource.

The original player keeps their normal loot.

Every other eligible player with the corresponding profession receives a **new, independent roll** against the same loot template.

## Features

### Mining

Supported with:

* Normal AzerothCore gathering
* `mod-auto-gather`

Example:

```text
Alice - Mining
Bob   - Mining
Carol - Herbalism

Alice mines a node.

Alice -> normal AzerothCore Mining loot
Bob   -> independent Mining loot roll
Carol -> nothing
```

### Herbalism

Supported with:

* Normal AzerothCore gathering
* `mod-auto-gather`

Example:

```text
Alice - Herbalism
Bob   - Herbalism
Carol - Mining

Alice gathers an herb.

Alice -> normal Herbalism loot
Bob   -> independent Herbalism loot roll
Carol -> nothing
```

### Skinning

Supported with:

* Normal AzerothCore Skinning
* `mod-auto-gather` Skinning

Example:

```text
Alice - Skinning
Bob   - Skinning
Carol - Mining

Alice skins a corpse.

Alice -> normal Skinning loot
Bob   -> independent Skinning loot roll
Carol -> nothing
```

The Skinning implementation uses the creature's `SkinLootId` and `LootTemplates_Skinning`, so each recipient receives a fresh roll instead of a copy of the original skinner's result.

## Independent rolls

The module does **not** copy the original player's loot.

For GameObjects it generates a new result from:

```cpp
LootTemplates_Gameobject
```

For Skinning it generates a new result from:

```cpp
LootTemplates_Skinning
```

Therefore, if the original gatherer receives:

```text
2x Saronite Ore
```

the next group member might independently receive:

```text
3x Saronite Ore
```

and another member might receive:

```text
1x Saronite Ore
+ additional random loot
```

Each player is rolled separately.

## Eligibility

A recipient must:

* be in the same group/raid;
* be online and in the world;
* be alive;
* be within the configured distance;
* be in the same phase;
* have the relevant profession;
* pass the configured skill requirement.

The player who actually gathered/skinned the resource is never given a second roll.

## Raid support

Raid groups are supported.

Configuration:

```ini
ProfessionLootParty.Raid = 1
```

Set to:

```ini
ProfessionLootParty.Raid = 0
```

to disable distribution to raid members.

## Configuration

Copy the distributed configuration file:

```text
mod-profession-loot-party.conf.dist
```

to:

```text
mod-profession-loot-party.conf
```

and configure:

```ini
# Enable the module.
ProfessionLootParty.Enable = 1

# Enable Mining distribution.
ProfessionLootParty.Mining = 1

# Enable Herbalism distribution.
ProfessionLootParty.Herbalism = 1

# Enable Skinning distribution.
ProfessionLootParty.Skinning = 1

# Include raid members.
ProfessionLootParty.Raid = 1

# Require recipients to actually have the profession.
ProfessionLootParty.RequireSkill = 1

# Maximum distance between the resource/corpse and recipient.
ProfessionLootParty.Distance = 100.0

# Additional diagnostic logging.
ProfessionLootParty.Debug = 0
```

## Normal AzerothCore gathering

Normal Mining and Herbalism are detected through the GameObject loot-state transition.

AzerothCore performs the successful gathering operation and subsequently updates the gathering skill.

The module remembers the GameObject when it reaches:

```cpp
GO_ACTIVATED
```

and verifies the GameObject through the player's skill-up list when:

```cpp
OnPlayerUpdateGatheringSkill()
```

is called.

This prevents unrelated GameObject interactions from triggering party profession loot.

## mod-auto-gather compatibility

`mod-auto-gather` does not necessarily follow the same GameObject activation path as normal AzerothCore gathering.

The module therefore has a fallback detector for Mining and Herbalism.

It looks for the nearby resource node that:

* is still `GO_READY`;
* is spawned;
* is in the player's map;
* is in the same phase;
* is within the configured range;
* contains the player's GUID in its skill-up list;
* has a valid Mining or Herbalism lock.

This allows the module to coexist with `mod-auto-gather` without taking control of the node's lifecycle.

The module does **not** call `SetLootState()` for AutoGather resources.

## Normal Skinning compatibility

Normal AzerothCore Skinning ultimately updates:

```cpp
SKILL_SKINNING
```

through:

```cpp
UpdateGatherSkill()
```

The module handles that skill update and identifies the successfully skinned creature.

The player's selected creature is preferred when available because it provides the most precise target identification.

The original skinner keeps the normal Skinning result.

Other eligible Skinning players receive independent rolls.

## mod-auto-gather Skinning compatibility

The module is also designed for `mod-auto-gather`'s automatic Skinning behavior.

`mod-auto-gather` performs its Skinning operation and then updates:

```cpp
SKILL_SKINNING
```

The module detects the resulting nearby skinned corpse and distributes independent Skinning rolls to eligible group members.

The module does not modify the corpse's lifecycle and does not attempt to perform Skinning itself.

This means `mod-auto-gather` remains responsible for the original Skinning operation.

## Special creature skinning

AzerothCore supports special creature loot-skill cases where a creature can be skinned using another profession.

This module's Skinning distribution intentionally handles:

```cpp
SKILL_SKINNING
```

only.

Mining and Herbalism distribution continues to use the normal GameObject gathering path.

## Installation

Place the module in:

```text
azerothcore/modules/mod-profession-loot-party
```

For example:

```bash
cd /path/to/azerothcore/modules

git clone https://github.com/nexartgroup/mod-profession-loot-party.git
```

Then copy the configuration:

```bash
cp etc/modules/mod-profession-loot-party.conf.dist \
   /path/to/azerothcore/etc/modules/mod-profession-loot-party.conf
```

If the configuration file is installed automatically by your module build, simply copy/edit the resulting `.conf.dist` as appropriate for your server setup.

## Build

After installing or changing the module, re-run CMake and perform a clean build.

Example:

```bash
cd /path/to/azerothcore

mkdir -p build
cd build

cmake ..

make -j$(sysctl -n hw.ncpu)
```

On Linux, use the appropriate CPU-count command for your environment.

## Module loader

The module must expose the AzerothCore module-loader function:

```cpp
void Addmod_profession_loot_partyScripts()
{
    new ProfessionLootParty::ConfigScript();
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}
```

The exact spelling is important.

If the linker reports:

```text
Undefined symbols for architecture arm64:

"Addmod_profession_loot_partyScripts()"
```

verify that `ProfessionLootParty.cpp` contains the exact function above and that the source file is included by the module CMake configuration.

## Requirements

Required:

* AzerothCore WotLK 3.3.5a
* C++ module support
* AzerothCore GameObject/Player/World scripting hooks used by this module

Optional:

* `mod-auto-gather` for automatic Mining, Herbalism and Skinning compatibility

The normal profession functionality does not require `mod-auto-gather`.

## Important behavior

The module does not change the original player's loot.

It only adds additional independent rolls for eligible group members.

For example, with:

```text
Alice - Mining
Bob   - Mining
Carol - Mining
```

and Alice mining a node:

```text
Alice -> normal node loot
Bob   -> independent node loot roll
Carol -> independent node loot roll
```

If Bob and Carol have different skill levels, their eligibility is still determined independently.

## Debugging

Enable:

```ini
ProfessionLootParty.Debug = 1
```

The server log will then contain messages showing:

* detected gathering operations;
* detected AutoGather operations;
* detected Skinning operations;
* selected GameObjects/corpses;
* independent rolls;
* rejected or missing targets.

After testing, disable debug logging:

```ini
ProfessionLootParty.Debug = 0
```

## Design

The module deliberately avoids modifying AzerothCore's core gathering or Skinning implementation.

The flow is:

```text
Normal Mining/Herbalism
        |
        v
GameObject GO_ACTIVATED
        |
        v
PendingGather
        |
        v
UpdateGatherSkill()
        |
        v
Independent party rolls
```

For AutoGather:

```text
mod-auto-gather
        |
        v
UpdateGatherSkill()
        |
        v
Detect matching resource
        |
        v
Independent party rolls
```

For Skinning:

```text
Normal Skinning / AutoGather Skinning
        |
        v
UpdateGatherSkill(SKILL_SKINNING)
        |
        v
Detect resulting corpse
        |
        v
SkinLootId
        |
        v
LootTemplates_Skinning
        |
        v
Independent party rolls
```

## No core patch required

The module uses AzerothCore's existing script hooks.

It does not require changes to:

```text
SpellEffects.cpp
GameObject.cpp
Player.cpp
LootMgr.cpp
```

and it does not replace the original profession loot logic.

## License

This module is intended for use with AzerothCore and is distributed under the license included with the repository.