# Changelog

## 2.0.0 — Stability, performance and balance pass

Behaviour with the default configuration is unchanged except where
noted under *Behaviour changes*.

---

### Fixed — crash and correctness

**Data races on shared state (crash risk).**
`PendingGathers`, `RecentSkinnings` and `SimulatedSkillUpdates` were plain
global `std::unordered_map`/`std::unordered_set` containers written from
`OnPlayerUpdateGatheringSkill` and `OnGameObjectLootStateChanged`. Those hooks
run on map update worker threads, so any server with `MapUpdate.Threads > 1`
had two players on different maps mutating the same buckets concurrently —
undefined behaviour, in practice a rehash-time crash.

The pending/recent containers are now guarded by a `std::mutex` with short
critical sections. The re-entrancy guard no longer needs the mutex at all: it
became a `thread_local` flag, because the simulated `UpdateGatherSkill()` call
is strictly synchronous and never leaves the calling thread.

**Leaked re-entrancy guard.**
The old guard inserted the player GUID into a set, called
`UpdateGatherSkill()`, then erased it. If anything below that call threw, the
GUID stayed in the set forever and that player was silently excluded from the
module for the rest of the session. The flag is now released by an RAII scope
object.

**Instances treated as the same place.**
Eligibility compared `GetMapId()`, which is identical for every instance of a
given map. Two players in different instances of the same dungeon passed the
map check and were then measured with a meaningless distance. Replaced with
`IsInMap()`, which compares map *and* instance id.

**Skinning could be credited twice.**
`RecentSkinnings` stored only the single most recently processed corpse per
player. A player alternating between two already-skinned corpses (A, B, A)
re-triggered distribution on A. It now keeps a small ring of the last eight
corpses per player, still expiring after 60 s.

**Unbounded state growth.**
Entries were only removed on successful processing or logout. Every
`GO_ACTIVATED` from a grouped player — doors, quest objects, ordinary chests —
inserted a pending entry that could sit until the next gather. Entries are now
also swept lazily, at most once per minute, from inside the existing lock.

**`RequireSkill = 0` did almost nothing.**
`HasSkill()` was enforced unconditionally, so switching the option off still
required the profession. It now genuinely means "do not require the
profession".

---

### Fixed — balance

**Skill requirement was not checked for receivers.**
Only the gatherer's skill was validated against the node's lock. A group member
with Mining 1 standing next to a level-450 node received Titanium Ore. The new
`ProfessionLootParty.RequireNodeSkill` (default `1`) requires each receiver to
meet the node's or creature's own requirement.

`GetGatheringRequirement()` was split: resolving the lock requirement and
deciding whether a given player may gather are now separate, so the value can
be reused for receiver gating.

**Inconsistent raid handling.**
With `Raid = 0` the group loop skipped everyone, but the *gatherer* still got
their bonus rolls. Raid groups are now skipped entirely, including the
gatherer.

---

### Performance

**Search radius decoupled from group distance.**
Both grid searches used `ProfessionLootParty.Distance` (default 100 yd) as
their radius. That option describes how far a *receiving member* may stand from
the resource; it has nothing to do with finding the node the gatherer is
standing on top of. A 100 yd `Cell` visit touches roughly sixteen times as many
cells as a 25 yd one, and it ran on every mining, herbalism and skinning
skill-up on the server.

The searches now use the new `ProfessionLootParty.SearchDistance`
(default 10 yd). Receiver eligibility still uses `Distance`.

**Searches skipped when they cannot produce anything.**
A cheap `ShouldProcess()` pre-check runs before any grid search: profession
enabled, group present, raid allowed, and either the gatherer can actually
receive a bonus (a multiplier above 1) or at least one other group member
exists. With the shipped default configuration and a player whose group mates
are offline, the module now does no work at all.

**`mod-auto-gather` detection can be switched off.**
`ProfessionLootParty.AutoGatherCompat = 0` removes the fallback grid search
completely for servers that do not run that module.

**Gathering nodes filtered at the source.**
`OnGameObjectLootStateChanged` fires for every activated GameObject. It now
checks the lock for a Mining/Herbalism entry before touching the pending map,
which removes both the lock contention and a class of false positives from
non-gathering interactions.

**Invariants hoisted out of the member loop.**
`GetGroup()` and `IsProfessionEnabled()` were re-evaluated per group member.
They are resolved once per operation.

---

### Added

| Option | Default | Purpose |
| --- | --- | --- |
| `ProfessionLootParty.SearchDistance` | `10` | Radius for locating the used node/corpse |
| `ProfessionLootParty.RequireNodeSkill` | `1` | Receiver must meet the node's skill requirement |
| `ProfessionLootParty.RequireGroup` | `1` | Set `0` to also multiply for solo gatherers |
| `ProfessionLootParty.ApplyToGatherer` | `1` | Set `0` to leave the gatherer's own result untouched |
| `ProfessionLootParty.MaxRecipients` | `0` | Cap receivers per operation (`0` = unlimited) |
| `ProfessionLootParty.AutoGatherCompat` | `1` | Toggle `mod-auto-gather` detection |
| `ProfessionLootParty.Announce` | `0` | Chat message to receiving members |

---

### Changed

**Config loaded from both `OnBeforeConfigLoad` and `OnAfterConfigLoad`.**
Depending on the core revision, module `.conf` files are merged at different
points relative to `OnBeforeConfigLoad`. Loading is idempotent, so both hooks
call it and only the later one logs the summary. This removes a silent failure
mode where multipliers stayed at their defaults after `.reload config`.

**`Distance` is clamped to `0 – 250`.**
Values beyond the server's visibility distance had no effect anyway, since the
objects are not in the grid.

**Node loot state accepts `GO_ACTIVATED` as well as `GO_READY`.**
Some gathering implementations flip the state before the skill hook fires.

**Structure.**
The six loose profession globals became a `ProfessionSettings` array indexed by
an internal slot enum, removing four parallel `switch` statements. Loot
distribution for nodes and corpses shares one templated `Distribute()` core
instead of two near-identical 90-line functions. Debug logging no longer passes
a runtime format string to `LOG_DEBUG`, which breaks on cores built against
newer fmt versions.

---

### Behaviour changes to be aware of

1. `RequireNodeSkill` defaults to `1`. Low-skill group members that previously
   received high-level materials no longer do. Set it to `0` for the old
   behaviour.
2. `RequireSkill = 0` now really distributes to members without the
   profession. Previously it was close to a no-op.
3. Raid groups with `Raid = 0` now also skip the gatherer's bonus rolls.
4. `SearchDistance` defaults to `10` yd. If a gathering module moves the player
   away from the node before the skill-up hook fires, raise it.

---

### Known limitations

* `Player::AutoStoreLoot()` sends an inventory-full error and drops the item
  when there is no bag space. The module does not mail overflow.
* Additional skill attempts for the gatherer resolve *before* the core's own
  attempt, because the hook fires ahead of the roll. The core's attempt
  therefore uses the gray/green/yellow thresholds captured before those
  attempts. The effect is negligible at one skill point per gather.
* The module multiplies materials. Ore, herbs and leather are the base of the
  crafting economy; a 4x multiplier on a populated realm is an economic
  decision, not just a convenience setting.
