# mod-profession-loot-party

AzerothCore WotLK 3.3.5a module that gives eligible group/raid members
independent profession loot rolls when another member successfully gathers a
resource.

Supported professions:

* Mining
* Herbalism
* Skinning

Supported gathering implementations:

* Normal AzerothCore Mining / Herbalism / Skinning
* `mod-auto-gather` Mining / Herbalism / Skinning

## Features

### Independent loot

Each eligible player receives a fresh roll against the original profession loot
table. The module does not copy the original player's generated loot.

For example, a single Mining node may produce:

```text
Player A: 2 Ore
Player B: 3 Ore + rare item
Player C: 1 Ore
```

Each result was generated independently.

### Profession requirement

Only group members who have the relevant profession receive the additional loot
and skill attempts.

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

By default a receiver must also meet the node's own skill requirement, so a
member with Mining 1 cannot farm Titanium through a group mate. See
`RequireNodeSkill`.

### Independent loot and skill multipliers

Loot and profession leveling are configured separately per profession:

```text
Mining:     2x loot / 1x skill
Herbalism:  4x loot / 3x skill
Skinning:   5x loot / 2x skill
```

## How multipliers work

The multiplier is the **total number of rolls/attempts for an eligible
profession user**. The original gatherer's normal AzerothCore operation is
never replaced.

With `ProfessionLootParty.MiningLootMultiplier = 3`, when Alice mines:

```text
Alice:  normal AzerothCore loot + 2 additional independent rolls
Bob:    3 independent loot rolls
Carol:  no Mining -> nothing
```

With `ProfessionLootParty.MiningSkillMultiplier = 3`:

```text
Alice:  normal skill-up attempt + 2 additional attempts
Bob:    3 skill-up attempts
Carol:  no Mining -> nothing
```

Every simulated attempt goes through:

```cpp
Player::UpdateGatherSkill()
```

The module never writes the skill value directly, so AzerothCore stays
responsible for skill-up chance, skill caps, `SkillGain.*` configuration,
skill-up script hooks, profession rewards and the normal Skinning/Mining
modifiers.

An attempt can fail normally. `3x` means three chances, not a forced `+3`.

## Skinning

Both normal AzerothCore Skinning and `mod-auto-gather` are supported. The
module detects a successful operation from the resulting corpse state rather
than by modifying the Skinning implementation:

* dead creature
* valid `SkinLootId`
* `LOOT_SKINNING`
* `UNIT_FLAG_SKINNABLE` removed
* tapped by the skinner

The elite multiplier is preserved for simulated attempts:

```cpp
UpdateGatherSkill(SKILL_SKINNING, pureSkillValue, requiredSkill,
    creature->isElite() ? 2 : 1);
```

## Installation

```text
modules/
└── mod-profession-loot-party/
    ├── CMakeLists.txt
    ├── conf/
    │   └── mod-profession-loot-party.conf.dist
    └── src/
        ├── ProfessionLootParty.cpp
        └── ProfessionLootParty.h
```

Re-run CMake and rebuild AzerothCore, then copy
`mod-profession-loot-party.conf.dist` to your configuration directory as
`mod-profession-loot-party.conf`.

## Configuration

### General

```ini
ProfessionLootParty.Enable = 1
```

### Professions

```ini
ProfessionLootParty.Mining = 1
ProfessionLootParty.MiningLootMultiplier = 1
ProfessionLootParty.MiningSkillMultiplier = 1

ProfessionLootParty.Herbalism = 1
ProfessionLootParty.HerbalismLootMultiplier = 1
ProfessionLootParty.HerbalismSkillMultiplier = 1

ProfessionLootParty.Skinning = 1
ProfessionLootParty.SkinningLootMultiplier = 1
ProfessionLootParty.SkinningSkillMultiplier = 1
```

Multipliers are clamped to `1 – 100`.

### Eligibility

```ini
ProfessionLootParty.Distance = 100          ; 0 - 250
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
ProfessionLootParty.RequireNodeSkill = 1
ProfessionLootParty.RequireGroup = 1
ProfessionLootParty.ApplyToGatherer = 1
ProfessionLootParty.MaxRecipients = 0       ; 0 = unlimited
```

`RequireGroup = 0` also applies the multipliers to solo gatherers, which turns
the module into a personal gathering multiplier as well.

`ApplyToGatherer = 0` leaves the gatherer's own result untouched and only
rewards the rest of the group.

### Performance

```ini
ProfessionLootParty.SearchDistance = 10     ; 1 - 100
ProfessionLootParty.AutoGatherCompat = 1
```

`SearchDistance` is the radius used to locate the node or corpse the gatherer
just used — not the group distance. A gatherer always stands next to the
resource, so a small value is correct. The search cost grows with the square of
this radius and it runs on every gathering skill-up on the server, so leave it
low unless a gathering module moves the player away before the skill-up hook
fires.

If you do not run `mod-auto-gather`, set `AutoGatherCompat = 0` to remove the
fallback search entirely.

### Feedback

```ini
ProfessionLootParty.Announce = 0
ProfessionLootParty.Debug = 0
```

## Backwards compatibility

```ini
ProfessionLootParty.MiningMultiplier
ProfessionLootParty.HerbalismMultiplier
ProfessionLootParty.SkinningMultiplier
```

These are still read as deprecated fallbacks for the corresponding
**loot** multiplier. They never controlled skill-up attempts.

## Design

The module leaves the original gathering implementation alone.

Normal Mining/Herbalism:

```text
AzerothCore gathering
        |
        v
GameObject loot state (filtered: gathering nodes only)
        |
        v
OnPlayerUpdateGatheringSkill
        |
        +--> additional loot rolls
        +--> additional UpdateGatherSkill() attempts
```

`mod-auto-gather`:

```text
mod-auto-gather -> UpdateGatherSkill()
        |
        v
short-range node search (SearchDistance)
        |
        +--> additional loot rolls
        +--> additional UpdateGatherSkill() attempts
```

Skinning:

```text
Normal Skinning / AutoSkinCreature()
        |
        v
LOOT_SKINNING corpse state -> UpdateGatherSkill(SKILL_SKINNING)
        |
        +--> additional skinning loot rolls
        +--> additional UpdateGatherSkill() attempts
```

No changes to `SpellEffects.cpp`, `Player.cpp`, `GameObject.cpp` or
`mod-auto-gather` are required.

### Re-entrancy

`Player::UpdateGatherSkill()` fires `OnPlayerUpdateGatheringSkill` *before*
performing the skill-up calculation. Because the module calls
`UpdateGatherSkill()` itself to simulate additional attempts, a thread-local
RAII guard suppresses the nested hook. Without it, a simulated attempt would be
detected as a new gathering operation and distribute another round of loot.

### Thread safety

Map updates run on worker threads (`MapUpdate.Threads`). All state shared
between players is guarded by a mutex; the re-entrancy guard is thread-local
and lock free.

## Notes

`LootMultiplier` and `SkillMultiplier` are intentionally independent:

```ini
ProfessionLootParty.SkinningLootMultiplier = 2
ProfessionLootParty.SkinningSkillMultiplier = 4
```

means two loot rolls and four skill-up *chances* — not four guaranteed skill
points.

Ore, herbs and leather are the base of the crafting economy. On a populated
realm a high multiplier is an economic decision, not only a convenience
setting.

See [CHANGELOG.md](CHANGELOG.md) for the full list of fixes and behaviour
changes.

## License

This module follows the licensing terms of the project repository and its
AzerothCore integration.
