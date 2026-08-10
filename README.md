# mod-profession-loot-party

AzerothCore WotLK 3.3.5a module that gives eligible group/raid members independent profession loot rolls when another member successfully gathers a resource.

Supported professions:

* Mining
* Herbalism
* Skinning

Supported gathering implementations:

* Normal AzerothCore Mining
* Normal AzerothCore Herbalism
* Normal AzerothCore Skinning
* `mod-auto-gather` Mining
* `mod-auto-gather` Herbalism
* `mod-auto-gather` Skinning

## Features

### Independent loot

Each eligible player receives a fresh roll against the original profession loot table.

The module does not copy the original player's generated loot.

For example, if a Mining node produces:

```text
Player A: 2 Ore
Player B: 3 Ore + rare item
Player C: 1 Ore
```

each result was independently generated.

### Profession requirement

Only group members who have the relevant profession receive the additional loot and skill attempts.

Example:

```text
Alice - Mining
Bob   - Mining
Carol - no Mining
```

When Alice mines:

```text
Alice -> normal Mining result
Bob   -> independent Mining result
Carol -> nothing
```

### Independent loot and skill multipliers

Loot and profession leveling are configured separately.

Each profession has:

```text
LootMultiplier
SkillMultiplier
```

This allows configurations such as:

```text
Mining:
    2x loot
    1x skill

Herbalism:
    4x loot
    3x skill

Skinning:
    5x loot
    2x skill
```

## How multipliers work

The multiplier represents the **total number of rolls/attempts for an eligible profession user**.

The original gatherer's normal AzerothCore operation is not replaced.

### Loot multiplier

With:

```ini
ProfessionLootParty.MiningLootMultiplier = 3
```

when Alice mines:

```text
Alice:
    normal AzerothCore loot
    + 2 additional independent loot rolls

Bob:
    3 independent loot rolls

Carol:
    no Mining -> nothing
```

### Skill multiplier

With:

```ini
ProfessionLootParty.MiningSkillMultiplier = 3
```

when Alice mines:

```text
Alice:
    normal AzerothCore skill-up attempt
    + 2 additional skill-up attempts

Bob:
    3 skill-up attempts

Carol:
    no Mining -> nothing
```

The module uses:

```cpp
Player::UpdateGatherSkill()
```

for every simulated skill-up attempt.

It does **not** directly increase the profession value.

Therefore the normal AzerothCore skill-up system remains responsible for:

* skill-up chance
* skill caps
* server skill gain configuration
* skill-up script hooks
* profession skill rewards
* the normal Skinning/Mining modifiers

A skill-up attempt can therefore fail normally. `3x` means three attempts, not a forced `+3` skill value.

## Example configuration

### 2x loot, normal leveling

```ini
ProfessionLootParty.MiningLootMultiplier = 2
ProfessionLootParty.MiningSkillMultiplier = 1
```

Result:

```text
Alice:
    normal loot + 1 additional loot
    normal skill attempt

Bob:
    2 loot rolls
    1 skill attempt
```

### 4x loot, 3x leveling

```ini
ProfessionLootParty.MiningLootMultiplier = 4
ProfessionLootParty.MiningSkillMultiplier = 3
```

Result:

```text
Alice:
    normal loot + 3 additional loot
    normal skill attempt + 2 additional attempts

Bob:
    4 loot rolls
    3 skill attempts

Carol:
    no Mining -> nothing
```

## Skinning

Skinning supports both normal AzerothCore Skinning and `mod-auto-gather`.

The module detects the successful Skinning operation through the resulting corpse state rather than changing the Skinning implementation itself.

The expected successful Skinning state is:

* dead creature
* valid `SkinLootId`
* `LOOT_SKINNING`
* `UNIT_FLAG_SKINNABLE` removed

This is compatible with both normal AzerothCore Skinning and `mod-auto-gather`'s `AutoSkinCreature()` behavior.

For Skinning, the normal elite multiplier is preserved when simulated skill attempts call:

```cpp
UpdateGatherSkill(
    SKILL_SKINNING,
    pureSkillValue,
    requiredSkill,
    creature->isElite() ? 2 : 1);
```

## Installation

Copy the module into the AzerothCore `modules` directory:

```text
modules/
└── mod-profession-loot-party/
    ├── conf/
    │   └── mod-profession-loot-party.conf.dist
    └── src/
        ├── ProfessionLootParty.cpp
        └── ProfessionLootParty.h
```

Re-run CMake and rebuild AzerothCore.

Then copy:

```text
mod-profession-loot-party.conf.dist
```

to your server configuration directory as:

```text
mod-profession-loot-party.conf
```

Enable the desired settings.

## Important implementation detail

`Player::UpdateGatherSkill()` calls the `OnPlayerUpdateGatheringSkill` script hook before performing the actual skill-up calculation.

Because this module uses `UpdateGatherSkill()` to simulate additional profession attempts, the module contains a re-entrancy guard.

Without that guard, a simulated skill-up could be incorrectly detected as a new gathering operation and distribute another round of loot.

## Maximum multiplier

For safety, each multiplier is clamped to:

```text
1 - 100
```

This prevents an accidental configuration such as:

```ini
ProfessionLootParty.MiningLootMultiplier = 1000000
```

from generating an extreme number of loot operations.

## Configuration

### General

```ini
ProfessionLootParty.Enable = 1
```

### Mining

```ini
ProfessionLootParty.Mining = 1
ProfessionLootParty.MiningLootMultiplier = 1
ProfessionLootParty.MiningSkillMultiplier = 1
```

### Herbalism

```ini
ProfessionLootParty.Herbalism = 1
ProfessionLootParty.HerbalismLootMultiplier = 1
ProfessionLootParty.HerbalismSkillMultiplier = 1
```

### Skinning

```ini
ProfessionLootParty.Skinning = 1
ProfessionLootParty.SkinningLootMultiplier = 1
ProfessionLootParty.SkinningSkillMultiplier = 1
```

### Group eligibility

```ini
ProfessionLootParty.Distance = 100
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
```

### Debugging

```ini
ProfessionLootParty.Debug = 0
```

Set to `1` for diagnostic logging.

## Backwards compatibility

The old:

```ini
ProfessionLootParty.MiningMultiplier
ProfessionLootParty.HerbalismMultiplier
ProfessionLootParty.SkinningMultiplier
```

settings are accepted as deprecated fallbacks for the **loot multiplier**.

The new configuration should use:

```ini
ProfessionLootParty.MiningLootMultiplier
ProfessionLootParty.MiningSkillMultiplier

ProfessionLootParty.HerbalismLootMultiplier
ProfessionLootParty.HerbalismSkillMultiplier

ProfessionLootParty.SkinningLootMultiplier
ProfessionLootParty.SkinningSkillMultiplier
```

The old settings do not control skill-up attempts.

## Design

The module intentionally leaves the original gathering implementation alone.

For normal Mining/Herbalism:

```text
AzerothCore gathering
        |
        v
GameObject loot state
        |
        v
ProfessionLootParty detects operation
        |
        v
OnPlayerUpdateGatheringSkill
        |
        +--> additional loot rolls
        |
        +--> additional UpdateGatherSkill() attempts
```

For `mod-auto-gather`:

```text
mod-auto-gather
        |
        v
UpdateGatherSkill()
        |
        v
ProfessionLootParty detects matching node
        |
        +--> additional loot rolls
        |
        +--> additional UpdateGatherSkill() attempts
```

For Skinning:

```text
Normal Skinning / AutoSkinCreature()
        |
        v
LOOT_SKINNING corpse state
        |
        v
UpdateGatherSkill(SKILL_SKINNING)
        |
        v
ProfessionLootParty
        |
        +--> additional skinning loot rolls
        |
        +--> additional UpdateGatherSkill() attempts
```

The module therefore does not need to modify `SpellEffects.cpp`, `Player.cpp`, `GameObject.cpp`, or `mod-auto-gather`.

## Notes

`LootMultiplier` and `SkillMultiplier` are intentionally independent.

For example:

```ini
ProfessionLootParty.SkinningLootMultiplier = 5
ProfessionLootParty.SkinningSkillMultiplier = 1
```

means:

```text
5 loot rolls
1 normal skill-up attempt
```

while:

```ini
ProfessionLootParty.SkinningLootMultiplier = 2
ProfessionLootParty.SkinningSkillMultiplier = 4
```

means:

```text
2 loot rolls
4 skill-up attempts
```

The latter does not force four skill points. It gives four chances through AzerothCore's normal `UpdateGatherSkill()` mechanism.

## License

This module follows the licensing terms of the project repository and its AzerothCore integration.