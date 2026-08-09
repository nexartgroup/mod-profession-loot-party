# mod-profession-loot-party

AzerothCore WotLK 3.3.5a module that allows grouped players with the same gathering profession to receive **independent profession-loot rolls** when a group member successfully gathers a resource.

Designed for AzerothCore's module system and the 3.3.5a client.

## Features

* Supports **Mining**.
* Supports **Herbalism**.
* Works with normal 5-player groups.
* Optionally works with raid groups.
* Each eligible group member receives a **new independent loot roll**.
* Uses the existing AzerothCore `gameobject_loot_template`.
* Does not copy the original gatherer's loot.
* The player who actually gathers the node receives their normal AzerothCore loot.
* Other eligible profession holders receive their own rolls.
* Players without the required profession receive nothing.
* Players outside the configured distance receive nothing.
* Dead or offline players are excluded.
* Configurable through `mod-profession-loot-party.conf`.
* Debug logging is available.

## Example

A group contains:

| Player | Profession |
| ------ | ---------- |
| Alice  | Mining     |
| Bob    | Mining     |
| Carol  | Herbalism  |

Alice mines an ore node.

The result is:

```text
Alice  -> normal AzerothCore Mining loot
Bob    -> independent Mining loot roll
Carol  -> nothing
```

If Bob subsequently gathers a herb node:

```text
Bob    -> normal AzerothCore Herbalism loot
Alice  -> nothing
Carol  -> independent Herbalism loot roll
```

The additional loot rolls are independent.

For example, if the node's loot table can produce 1–3 ore:

```text
Alice -> 2 Ore
Bob   -> 3 Ore
```

Bob's result is not copied from Alice's result. The loot template is rolled again.

---

## Requirements

* AzerothCore
* WotLK 3.3.5a
* A compiler supported by your AzerothCore build
* The module must be installed under:

```text
modules/mod-profession-loot-party
```

The module uses AzerothCore's existing GameObject loot system and gathering-skill hooks.

---

## Installation

From your AzerothCore root:

```bash
cd modules
git clone https://github.com/nexartgroup/mod-profession-loot-party.git
```

Or copy the module directory manually:

```text
azerothcore/
└── modules/
    └── mod-profession-loot-party/
        ├── CMakeLists.txt
        ├── conf/
        │   └── mod-profession-loot-party.conf.dist
        └── src/
            ├── ProfessionLootParty.cpp
            └── ProfessionLootParty.h
```

If you already have the module directory, make sure there isn't an older copy elsewhere in `modules/`.

---

## Configuration

After building/installing the module, copy the configuration file into the server's modules configuration directory.

Example:

```bash
cp modules/mod-profession-loot-party/conf/mod-profession-loot-party.conf.dist \
   etc/modules/mod-profession-loot-party.conf
```

The exact destination may differ depending on your AzerothCore build layout.

### Configuration options

```ini
ProfessionLootParty.Enable = 1
ProfessionLootParty.Mining = 1
ProfessionLootParty.Herbalism = 1
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
ProfessionLootParty.Distance = 100
ProfessionLootParty.Debug = 0
```

### Enable module

```ini
ProfessionLootParty.Enable = 1
```

Set to `0` to disable the module.

### Mining

```ini
ProfessionLootParty.Mining = 1
```

Controls whether Mining can generate additional group loot.

### Herbalism

```ini
ProfessionLootParty.Herbalism = 1
```

Controls whether Herbalism can generate additional group loot.

### Raid groups

```ini
ProfessionLootParty.Raid = 1
```

When enabled, the module also processes eligible members of raid groups.

Set to:

```ini
ProfessionLootParty.Raid = 0
```

to restrict the feature to normal groups.

### Require profession

```ini
ProfessionLootParty.RequireSkill = 1
```

When enabled, a player must actually possess the gathering profession associated with the resource.

For example, a player must have Mining to receive an additional Mining roll.

### Distance

```ini
ProfessionLootParty.Distance = 100
```

Maximum distance from the resource node for an additional loot recipient.

The value is in yards.

For example:

```ini
ProfessionLootParty.Distance = 50
```

restricts additional loot to players within 50 yards.

### Debug logging

```ini
ProfessionLootParty.Debug = 1
```

Enable this while testing.

The module writes diagnostic messages using the `module` logging category.

Example:

```text
[ProfessionLootParty] Gathering operation queued.
[ProfessionLootParty] Confirmed successful profession gathering.
[ProfessionLootParty] Independent profession loot roll awarded.
[ProfessionLootParty] Profession group loot processing completed.
```

Disable it for normal production operation:

```ini
ProfessionLootParty.Debug = 0
```

---

## How it works

The module deliberately does not attempt to determine the gathering profession from the GameObject alone.

AzerothCore's gathering flow determines the actual skill used by the player.

The module listens for:

```cpp
OnGameObjectLootStateChanged()
```

and temporarily associates the gathering player with the GameObject.

The actual profession is subsequently obtained from:

```cpp
OnPlayerUpdateGatheringSkill()
```

For example:

```text
Player interacts with resource
        |
        v
EffectOpenLock()
        |
        v
CanOpenLock()
        |
        +---- SKILL_MINING
        |
        +---- SKILL_HERBALISM
        |
        v
Normal AzerothCore loot
        |
        v
UpdateGatherSkill()
        |
        v
mod-profession-loot-party
        |
        v
Find eligible group members
        |
        v
Independent loot roll for each member
```

This avoids guessing whether a GameObject is an ore node or herb node based solely on its template.

---

## Independent loot rolls

The module uses:

```cpp
Player::AutoStoreLoot()
```

with:

```cpp
LootTemplates_Gameobject
```

and the GameObject's own loot ID.

Conceptually:

```text
                 gameobject_loot_template
                         |
          +--------------+--------------+
          |              |              |
          v              v              v
       Player A       Player B       Player C
        Roll #1        Roll #2        Roll #3
```

Each recipient therefore gets a separate roll against the same loot table.

It does **not** do this:

```text
Alice gets:
2 Copper Ore

Bob gets:
copy of Alice's 2 Copper Ore
```

Instead:

```text
Alice:
2 Copper Ore

Bob:
3 Copper Ore + possible rare drop
```

The exact result depends on the existing AzerothCore loot template and its configured chances/quantities.

---

## Who receives additional loot?

A group member must satisfy all applicable conditions:

1. The player is not the original gatherer.
2. The player is in the same group.
3. The player is in the same map.
4. The player is alive.
5. The player is in the world.
6. The player is within the configured distance.
7. The corresponding profession is enabled.
8. The player has the corresponding profession.
9. `RequireSkill` requirements are satisfied.
10. The group/raid configuration permits the group type.

The original gatherer is intentionally excluded from the additional roll because AzerothCore already gives that player the normal gathering loot.

---

## Loot table behavior

The module does not require new loot tables.

Existing entries in:

```text
gameobject_loot_template
```

are reused.

For example, if a Mining node has:

```text
Loot ID: 1234
```

the additional Mining recipients independently roll against the loot table associated with loot ID `1234`.

This means server administrators can continue using the normal AzerothCore database loot configuration.

---

## Important behavior

### The original gatherer

The gatherer receives normal AzerothCore loot exactly as before.

The module does not replace or modify that loot.

### Other miners

Other nearby group members with Mining receive independent Mining rolls.

### Herbalists

Herbalists do not receive Mining loot.

Likewise, Miners do not receive Herbalism loot.

### Different professions

For example:

```text
Alice  = Mining
Bob    = Herbalism
Carol  = Mining
```

If Alice mines:

```text
Alice  -> normal Mining loot
Bob    -> nothing
Carol  -> independent Mining roll
```

If Bob gathers an herb:

```text
Bob    -> normal Herbalism loot
Alice  -> nothing
Carol  -> nothing
```

---

## Building

From the AzerothCore root:

```bash
mkdir -p build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

On Linux, for example:

```bash
make -j$(nproc)
```

Use the same CMake/build procedure as the rest of your AzerothCore installation if your server already has an established build directory.

---

## Verifying the module was compiled

During the build you should see the module source being compiled:

```text
mod-profession-loot-party/src/ProfessionLootParty.cpp
```

The final `worldserver` link must complete successfully.

If you see an error such as:

```text
Undefined symbols for architecture arm64:
"Addmod_profession_loot_partyScripts()"
```

the module's loader function name does not match the generated AzerothCore module loader.

The source must export exactly:

```cpp
void Addmod_profession_loot_partyScripts()
{
    new ProfessionLootParty::ConfigScript();
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}
```

The spelling and capitalization are significant.

---

## Testing

For initial testing, enable:

```ini
ProfessionLootParty.Enable = 1
ProfessionLootParty.Mining = 1
ProfessionLootParty.Herbalism = 1
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
ProfessionLootParty.Distance = 100
ProfessionLootParty.Debug = 1
```

Restart the worldserver.

Create a group with two characters that both have Mining.

Stand close to an ore node.

Have Player A gather the node.

Expected behavior:

```text
Player A -> normal Mining loot
Player B -> additional independent Mining roll
```

The server log should contain messages similar to:

```text
[ProfessionLootParty] Gathering operation queued.
[ProfessionLootParty] Confirmed successful profession gathering.
[ProfessionLootParty] Independent profession loot roll awarded.
[ProfessionLootParty] Profession group loot processing completed.
```

Then test:

* two Miners
* three Miners
* Mining + Herbalism
* player outside the configured distance
* player without Mining
* dead group member
* normal party
* raid group
* Mining disabled
* Herbalism disabled

---

## Safety / duplicate-loot protection

The module maintains a short-lived pending gathering record.

The record is consumed when the corresponding gathering-skill update is processed.

This prevents the same gathering operation from being processed repeatedly by the module.

Pending records also expire after:

```text
5000 ms
```

This prevents stale GameObject/player associations from remaining indefinitely.

---

## Database changes

**None required.**

The module uses the existing AzerothCore GameObject loot tables.

No custom SQL is required.

---

## Supported professions

Currently:

```text
Mining
Herbalism
```

Skinning is intentionally not handled by this module because skinning follows a different loot path from GameObject gathering.

---

## Limitations

This module intentionally provides **additional independent rolls**, rather than splitting one loot result among the group.

For example, if a node normally gives:

```text
2-4 Ore
```

and three eligible miners are present, the result can conceptually be:

```text
Gatherer -> 3 Ore
Miner #2 -> 1 Ore
Miner #3 -> 4 Ore
```

It is therefore possible for the total amount of material obtained by the group to be greater than the normal single-player result.

This is the intended behavior of the module.

---

## Credits

Built for:

**AzerothCore — WotLK 3.3.5a**

Module:

**mod-profession-loot-party**

The module uses AzerothCore's existing:

* GameObject scripting system
* Player scripting system
* Group system
* GameObject loot templates
* Gathering skill system

No core modification is required.