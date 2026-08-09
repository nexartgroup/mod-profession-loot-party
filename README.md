# mod-profession-loot-party

AzerothCore WotLK 3.3.5a module that gives eligible group members an **independent profession-loot roll** when another group member successfully gathers a Mining or Herbalism resource.

The module supports both:

* Normal AzerothCore gathering
* [`mod-auto-gather`](https://github.com/thanhtong89/mod-auto-gather)

Only this module needs to be modified for AutoGather compatibility. No changes to AzerothCore or `mod-auto-gather` are required.

---

## Features

* Supports **Mining**
* Supports **Herbalism**
* Supports normal 5-player groups
* Supports raid groups when enabled
* Gives eligible group members their **own independent loot roll**
* Uses the existing AzerothCore `gameobject_loot_template`
* Does not copy the original gatherer's loot
* The original gatherer keeps their normal AzerothCore loot
* Works with `mod-auto-gather`
* Does not require modifications to `mod-auto-gather`
* Does not require core modifications
* Does not require SQL changes
* Excludes dead players
* Excludes players outside the configured distance
* Requires the recipient to have the corresponding gathering profession
* Includes duplicate-processing protection
* Includes optional debug logging

---

# Requirements

* AzerothCore
* WotLK 3.3.5a
* A compiler supported by your AzerothCore installation

Optional:

* [`mod-auto-gather`](https://github.com/thanhtong89/mod-auto-gather)

The module should be installed as:

```text
azerothcore/
└── modules/
    └── mod-profession-loot-party/
        ├── CMakeLists.txt
        ├── conf/
        │   └── mod-profession-loot-party.conf.dist
        ├── README.md
        └── src/
            ├── ProfessionLootParty.cpp
            └── ProfessionLootParty.h
```

---

# Installation

From the AzerothCore root:

```bash
cd modules
git clone https://github.com/nexartgroup/mod-profession-loot-party.git
```

If you already have the module installed, replace its source files with the updated versions.

Then rebuild AzerothCore using your normal build procedure.

For example:

```bash
cd /path/to/azerothcore
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

The final `worldserver` executable must build successfully.

---

# Configuration

Copy the configuration file to your server's module configuration directory.

Example:

```bash
cp modules/mod-profession-loot-party/conf/mod-profession-loot-party.conf.dist \
   etc/modules/mod-profession-loot-party.conf
```

The exact destination can differ depending on your AzerothCore installation.

## Default configuration

```ini
ProfessionLootParty.Enable = 1
ProfessionLootParty.Mining = 1
ProfessionLootParty.Herbalism = 1
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
ProfessionLootParty.Distance = 100
ProfessionLootParty.Debug = 0
```

---

# Configuration Options

## Enable

```ini
ProfessionLootParty.Enable = 1
```

Enables or disables the entire module.

```ini
ProfessionLootParty.Enable = 0
```

disables all additional profession loot.

---

## Mining

```ini
ProfessionLootParty.Mining = 1
```

Controls additional loot rolls for Mining.

Set to:

```ini
ProfessionLootParty.Mining = 0
```

to disable Mining support while leaving Herbalism enabled.

---

## Herbalism

```ini
ProfessionLootParty.Herbalism = 1
```

Controls additional loot rolls for Herbalism.

Set to:

```ini
ProfessionLootParty.Herbalism = 0
```

to disable Herbalism support while leaving Mining enabled.

---

## Raid Groups

```ini
ProfessionLootParty.Raid = 1
```

When enabled, eligible members of raid groups can receive additional profession loot.

To restrict the module to normal groups:

```ini
ProfessionLootParty.Raid = 0
```

---

## RequireSkill

```ini
ProfessionLootParty.RequireSkill = 1
```

When enabled, the recipient must actually have the profession associated with the gathered resource.

For example:

```text
Mining node
    ↓
Only players with Mining can receive the additional roll
```

and:

```text
Herbalism node
    ↓
Only players with Herbalism can receive the additional roll
```

This option is enabled by default.

---

## Distance

```ini
ProfessionLootParty.Distance = 100
```

Maximum distance, in yards, between the resource node and an eligible recipient.

For example:

```ini
ProfessionLootParty.Distance = 50
```

means that a group member must be within 50 yards of the resource node to receive an additional roll.

The original gatherer is not affected by this setting.

---

## Debug

```ini
ProfessionLootParty.Debug = 0
```

Enable diagnostic logging:

```ini
ProfessionLootParty.Debug = 1
```

This is useful when testing the module or troubleshooting AutoGather compatibility.

Disable it for normal production operation:

```ini
ProfessionLootParty.Debug = 0
```

---

# How Loot Works

The module does **not** split the original loot between players.

Instead, every eligible recipient receives a completely separate loot roll.

For example, suppose a group contains:

```text
Alice  - Mining
Bob    - Mining
Carol  - Herbalism
```

Alice mines an ore node.

The result is:

```text
Alice  → normal AzerothCore Mining loot
Bob    → independent Mining loot roll
Carol  → nothing
```

If Bob gathers an herb:

```text
Bob    → normal AzerothCore Herbalism loot
Alice  → nothing
Carol  → independent Herbalism loot roll
```

The additional loot is rolled independently.

For example:

```text
Alice → 2 Copper Ore
Bob   → 3 Copper Ore
```

Bob's 3 ore are **not copied from Alice**.

They are generated by a new roll against the same GameObject loot template.

---

# Important: The Gatherer Does Not Get an Extra Roll

The player who actually gathers the resource receives the normal AzerothCore loot.

The module intentionally does not give the gatherer a second copy of the loot.

For example:

```text
Player A gathers
        ↓
Normal AzerothCore loot
        ↓
Player A receives normal loot

        +

Player B
        ↓
Independent loot roll

Player C
        ↓
Independent loot roll
```

Therefore the module adds loot for eligible group members without replacing the normal gathering system.

---

# Eligible Recipients

A group member must satisfy the applicable conditions.

The recipient must:

1. Not be the original gatherer
2. Be in the same group
3. Be in the same map
4. Be in the world
5. Be alive
6. Be within the configured distance
7. Have the corresponding profession enabled
8. Have the corresponding profession
9. Satisfy the `RequireSkill` setting
10. Be in an allowed group/raid configuration

The gatherer is intentionally excluded from the additional roll.

---

# Profession Matching

The module keeps Mining and Herbalism separate.

Example:

```text
Alice  = Mining
Bob    = Herbalism
Carol  = Mining
```

If Alice mines:

```text
Alice  → normal Mining loot
Bob    → nothing
Carol  → independent Mining loot
```

If Bob gathers an herb:

```text
Bob    → normal Herbalism loot
Alice  → nothing
Carol  → nothing
```

A Herbalist does not receive Mining loot.

A Miner does not receive Herbalism loot.

---

# AutoGather Compatibility

This version contains a dedicated compatibility path for:

[`mod-auto-gather`](https://github.com/thanhtong89/mod-auto-gather)

No modification to `mod-auto-gather` is required.

## Why compatibility is necessary

Normal AzerothCore gathering and AutoGather do not reach the profession-loot module through exactly the same GameObject lifecycle.

Normal gathering can expose the resource through the GameObject loot-state path.

`mod-auto-gather`, however, performs the gathering programmatically and calls the gathering-skill update while the resource is still available in its active state.

The compatibility code therefore does not depend exclusively on:

```text
GO_ACTIVATED
```

Instead, the module also watches the gathering-skill callback and identifies an AutoGather operation using the GameObject's skill-up list.

---

# AutoGather Detection

When AutoGather processes a Mining or Herbalism node, the module looks for a nearby resource GameObject that satisfies all of the following:

```text
GameObject exists
        ↓
GameObject is spawned
        ↓
GameObject is GO_READY
        ↓
Player is in the same map
        ↓
Player is in the same phase
        ↓
Player is within the configured search distance
        ↓
Player is present in the GameObject skill-up list
        ↓
GameObject matches the gathering profession
        ↓
AutoGather operation detected
```

The important compatibility signal is the GameObject skill-up list.

The module uses:

```cpp
gameObject->IsInSkillupList(player->GetGUID())
```

to identify the resource being processed by AutoGather.

---

# AutoGather Loot Flow

With AutoGather active, the intended flow is:

```text
AutoGather finds resource
        |
        v
Resource loot is generated normally
        |
        v
Gathering skill update
        |
        v
ProfessionLootParty detects the resource
        |
        v
Find eligible group members
        |
        v
Independent loot roll for each recipient
        |
        v
AutoGather continues its normal resource lifecycle
```

The module does **not** take ownership of the AutoGather resource lifecycle.

In particular, the compatibility code does not manually deactivate the GameObject.

AutoGather remains responsible for its own GameObject state.

---

# Normal Gathering Flow

Without AutoGather, the module supports the normal AzerothCore gathering flow:

```text
Player interacts with resource
        |
        v
AzerothCore gathering
        |
        v
GameObject gathering state
        |
        v
ProfessionLootParty records the resource
        |
        v
AzerothCore UpdateGatherSkill()
        |
        v
ProfessionLootParty confirms the gathering
        |
        v
Eligible group members are found
        |
        v
Independent loot rolls
```

The normal gathering path and AutoGather path both eventually use the same loot distribution function.

---

# Duplicate Loot Protection

The module contains protection against processing the same gathering operation twice.

For normal gathering, a short-lived pending record associates:

```text
Player
    +
GameObject
```

When the corresponding gathering-skill update is received, the pending operation is consumed.

This prevents the AutoGather fallback from processing the same operation again.

Pending records expire after:

```text
5000 milliseconds
```

This prevents stale player/GameObject associations from remaining indefinitely.

---

# Independent Loot Generation

Additional loot uses the existing AzerothCore GameObject loot system.

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
             GameObject loot template
                       |
          +------------+------------+
          |            |            |
          v            v            v
       Player A     Player B     Player C
        Roll #1      Roll #2      Roll #3
```

Each additional recipient therefore gets a new roll.

The module does not copy the gatherer's inventory result.

---

# Database Requirements

No database changes are required.

The module uses the existing AzerothCore:

```text
gameobject_loot_template
```

tables.

No custom SQL is required.

Existing GameObject loot IDs are reused.

---

# Supported Professions

Currently supported:

```text
Mining
Herbalism
```

Skinning is not handled by this module.

Skinning uses a different gathering/loot path and is outside the GameObject-based Mining/Herbalism implementation.

---

# Building

From the AzerothCore build directory:

```bash
cmake ..
make -j$(nproc)
```

Or use your normal AzerothCore build process.

The important source files are:

```text
src/ProfessionLootParty.cpp
src/ProfessionLootParty.h
```

The module loader is:

```cpp
void Addmod_profession_loot_partyScripts()
{
    new ProfessionLootParty::ConfigScript();
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}
```

The function name must remain exactly as shown because AzerothCore's generated module loader expects that symbol.

---

# Testing

For initial testing, use:

```ini
ProfessionLootParty.Enable = 1
ProfessionLootParty.Mining = 1
ProfessionLootParty.Herbalism = 1
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
ProfessionLootParty.Distance = 100
ProfessionLootParty.Debug = 1
```

Restart the worldserver after changing configuration.

---

## Test 1: Normal Mining

Create a group:

```text
Player A → Mining
Player B → Mining
```

Place both players near an ore node.

Have Player A mine it.

Expected:

```text
Player A → normal Mining loot
Player B → independent Mining loot
```

---

## Test 2: AutoGather Mining

Enable `mod-auto-gather`.

Use:

```text
Player A → Mining
Player B → Mining
```

Place both players near an ore node.

Allow AutoGather to gather the node.

Expected:

```text
Player A → normal AutoGather/AzerothCore loot
Player B → independent Mining loot
```

The gatherer should **not** receive a second profession-loot roll.

---

## Test 3: AutoGather Herbalism

Use:

```text
Player A → Herbalism
Player B → Herbalism
```

Allow AutoGather to gather an herb.

Expected:

```text
Player A → normal Herbalism loot
Player B → independent Herbalism loot
```

---

## Test 4: Different Professions

Use:

```text
Player A → Mining
Player B → Herbalism
```

When A mines:

```text
A → normal Mining loot
B → nothing
```

When B gathers an herb:

```text
B → normal Herbalism loot
A → nothing
```

---

## Test 5: Three Players

Use:

```text
Player A → Mining
Player B → Mining
Player C → Mining
```

A gathers an ore node.

Expected:

```text
A → normal Mining loot
B → independent Mining roll
C → independent Mining roll
```

---

## Test 6: Player Outside Distance

Set:

```ini
ProfessionLootParty.Distance = 50
```

Put one Miner close to the node and another Miner more than 50 yards away.

Expected:

```text
Nearby Miner → receives independent roll
Distant Miner → receives nothing
```

---

## Test 7: Player Without Profession

Use:

```text
Player A → Mining
Player B → no Mining
```

A gathers ore.

Expected:

```text
A → normal Mining loot
B → nothing
```

---

## Test 8: Dead Player

Use:

```text
Player A → Mining
Player B → Mining but dead
```

A gathers ore.

Expected:

```text
A → normal Mining loot
B → nothing
```

---

## Test 9: Raid

With:

```ini
ProfessionLootParty.Raid = 1
```

test two eligible Miners in a raid.

Expected:

```text
Gatherer → normal loot
Other eligible Miner → independent roll
```

Then test with:

```ini
ProfessionLootParty.Raid = 0
```

The additional raid-member roll should no longer occur.

---

# Debug Logging

Enable:

```ini
ProfessionLootParty.Debug = 1
```

The module can log events such as:

```text
[ProfessionLootParty] Gathering operation queued.
```

```text
[ProfessionLootParty] Confirmed successful profession gathering.
```

```text
[ProfessionLootParty] Detected mod-auto-gather operation.
```

```text
[ProfessionLootParty] Independent profession loot roll awarded.
```

```text
[ProfessionLootParty] Profession group loot processing completed.
```

For production:

```ini
ProfessionLootParty.Debug = 0
```

---

# Expected Production Behavior

With:

```ini
ProfessionLootParty.Enable = 1
ProfessionLootParty.Mining = 1
ProfessionLootParty.Herbalism = 1
ProfessionLootParty.Raid = 1
ProfessionLootParty.RequireSkill = 1
ProfessionLootParty.Distance = 100
ProfessionLootParty.Debug = 0
```

the module behaves as follows:

```text
                    RESOURCE GATHERED
                           |
              +------------+------------+
              |                         |
         Normal Gather              AutoGather
              |                         |
              +------------+------------+
                           |
                           v
                 Gathering skill update
                           |
                           v
                 Identify resource
                           |
                           v
                Identify profession
                           |
                           v
                 Find group members
                           |
                           v
                 Check eligibility
                           |
              +------------+------------+
              |                         |
         Not eligible                Eligible
              |                         |
              v                         v
           Nothing              Independent loot roll
```

The original gatherer retains the normal loot.

Every other eligible group member gets a separate loot roll.

---

# Limitations

The module intentionally adds independent loot rolls.

It does not split one loot result between players.

For example, a node might produce:

```text
Gatherer → 3 Ore
Miner #2 → 1 Ore
Miner #3 → 4 Ore
```

The total group loot can therefore be greater than the normal single-player result.

This is intentional.

---

# AutoGather Compatibility Scope

The compatibility implementation is designed specifically around the Mining/Herbalism GameObject gathering behavior used by `mod-auto-gather`.

It does not modify:

* `mod-auto-gather`
* AzerothCore core files
* Database tables
* AutoGather configuration

Only `mod-profession-loot-party` needs to contain the compatibility code.

---

# Troubleshooting

## Other group members receive nothing

Enable:

```ini
ProfessionLootParty.Debug = 1
```

Restart the worldserver and test again.

Check that:

```text
The players are grouped
The recipients have the correct profession
The recipients are alive
The recipients are nearby
The profession is enabled
The worldserver was rebuilt with the updated module
```

---

## Normal gathering works but AutoGather does not

Confirm that you are running the updated `ProfessionLootParty.cpp` and `ProfessionLootParty.h`.

The AutoGather compatibility depends on the additional gathering-skill detection path.

Also verify that the module was actually rebuilt and that the running `worldserver` is the newly compiled binary.

---

## AutoGather gives the gatherer normal loot but no group loot

Enable:

```ini
ProfessionLootParty.Debug = 1
```

Look for:

```text
Detected mod-auto-gather operation.
```

If that message never appears, verify that the resource is Mining or Herbalism and that the player is grouped.

---

## Duplicate additional loot

The module has a pending-operation mechanism specifically to prevent normal gathering and AutoGather detection from processing the same operation twice.

If duplicate loot is observed, enable debug logging and verify that the same gathering operation is not being reported multiple times by the surrounding server configuration.

---

# No Core Modifications

This module is implemented entirely through AzerothCore's module scripting interfaces.

It does not require modifications to:

```text
src/server/game/
src/server/scripts/
```

and it does not require modifying:

```text
mod-auto-gather
```

---

# Credits

Built for:

**AzerothCore WotLK 3.3.5a**

Original module:

**mod-profession-loot-party**

AutoGather compatibility:

**mod-auto-gather**

The module uses AzerothCore's existing:

* GameObject scripting
* Player scripting
* Group system
* GameObject loot templates
* Gathering skill system

No core modification is required.
