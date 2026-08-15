/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Gives eligible group/raid members independent profession loot
 * rolls and simulated profession skill-up attempts when another
 * member successfully gathers a resource.
 *
 * Supported:
 *   - Normal AzerothCore Mining / Herbalism / Skinning
 *   - mod-auto-gather Mining / Herbalism / Skinning
 *
 * Loot and skill multipliers are independent.
 */

#include "ProfessionLootParty.h"

#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Unit.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ProfessionLootParty
{
    namespace
    {
        /* ==========================================================
         * Configuration
         * ========================================================== */

        ModuleSettings g_settings;

        constexpr uint32 MAX_PROFESSION_MULTIPLIER = 100;
        constexpr float  MAX_CONFIG_DISTANCE       = 250.0f;
        constexpr float  MAX_CONFIG_SEARCH         = 100.0f;

        /* ==========================================================
         * Shared runtime state
         *
         * AzerothCore updates maps in parallel worker threads
         * (MapUpdate.Threads > 1), so every container that is shared
         * between players on different maps must be guarded.
         * ========================================================== */

        struct PendingGatherEntry
        {
            ObjectGuid gameObject;
            uint32     createdAt = 0;
        };

        struct RecentSkinningEntry
        {
            ObjectGuid creature;
            uint32     createdAt = 0;
        };

        constexpr uint32 PENDING_TIMEOUT_MS          = 5000;
        constexpr uint32 SKINNING_RECENT_TIMEOUT_MS  = 60000;
        constexpr uint32 PRUNE_INTERVAL_MS           = 60000;
        constexpr std::size_t MAX_RECENT_PER_PLAYER  = 8;

        std::mutex g_stateMutex;
        std::unordered_map<uint64, PendingGatherEntry> g_pendingGathers;
        std::unordered_map<uint64, std::vector<RecentSkinningEntry>> g_recentSkinnings;
        uint32 g_lastPruneAt = 0;

        /*
         * Re-entrancy guard.
         *
         * SimulateGatherSkillAttempt() deliberately calls
         * Player::UpdateGatherSkill(), which fires
         * OnPlayerUpdateGatheringSkill() again. The nested call must
         * never be treated as a new gathering operation.
         *
         * The simulation is strictly synchronous and never leaves the
         * calling thread, so a thread-local flag is both correct and
         * lock free. It replaces the previous shared std::unordered_set,
         * which was a data race and could leak a stuck entry if
         * UpdateGatherSkill() ever threw.
         */
        thread_local bool t_simulating = false;

        class SimulationScope
        {
        public:
            SimulationScope() : _owned(!t_simulating)
            {
                if (_owned)
                    t_simulating = true;
            }

            ~SimulationScope()
            {
                if (_owned)
                    t_simulating = false;
            }

            SimulationScope(SimulationScope const&) = delete;
            SimulationScope& operator=(SimulationScope const&) = delete;

            bool Owned() const { return _owned; }

        private:
            bool _owned;
        };

        /* ==========================================================
         * Small helpers
         * ========================================================== */

        uint32 NowMS()
        {
            using namespace std::chrono;

            return static_cast<uint32>(
                duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        }

        bool IsExpired(uint32 createdAt, uint32 timeout)
        {
            return uint32(NowMS() - createdAt) > timeout;
        }

        void DebugLog(std::string const& message)
        {
            if (!g_settings.Debug)
                return;

            LOG_DEBUG("module", "[ProfessionLootParty] {}", message);
        }

        std::string DescribePlayer(Player* player)
        {
            return player ? player->GetName() : std::string("<null>");
        }

        int8 SlotForSkill(uint32 skillId)
        {
            switch (skillId)
            {
                case SKILL_MINING:    return PROFESSION_MINING;
                case SKILL_HERBALISM: return PROFESSION_HERBALISM;
                case SKILL_SKINNING:  return PROFESSION_SKINNING;
                default:              return -1;
            }
        }

        char const* SkillName(uint32 skillId)
        {
            switch (skillId)
            {
                case SKILL_MINING:    return "Mining";
                case SKILL_HERBALISM: return "Herbalism";
                case SKILL_SKINNING:  return "Skinning";
                default:              return "Profession";
            }
        }

        ProfessionSettings const* SettingsForSkill(uint32 skillId)
        {
            int8 const slot = SlotForSkill(skillId);

            return slot < 0 ? nullptr : &g_settings.Professions[slot];
        }

        bool IsGatheringSkill(uint32 skillId)
        {
            return skillId == SKILL_MINING || skillId == SKILL_HERBALISM;
        }

        bool IsSkinningSkill(uint32 skillId)
        {
            return skillId == SKILL_SKINNING;
        }

        /* ==========================================================
         * State bookkeeping
         * ========================================================== */

        /*
         * Drops timed-out entries. Called from inside the lock, at
         * most once per PRUNE_INTERVAL_MS, so nothing accumulates for
         * players that never complete a queued operation.
         */
        void PruneLocked()
        {
            uint32 const now = NowMS();

            if (uint32(now - g_lastPruneAt) < PRUNE_INTERVAL_MS)
                return;

            g_lastPruneAt = now;

            for (auto itr = g_pendingGathers.begin(); itr != g_pendingGathers.end();)
                itr = IsExpired(itr->second.createdAt, PENDING_TIMEOUT_MS)
                    ? g_pendingGathers.erase(itr)
                    : std::next(itr);

            for (auto itr = g_recentSkinnings.begin(); itr != g_recentSkinnings.end();)
            {
                auto& entries = itr->second;

                entries.erase(
                    std::remove_if(entries.begin(), entries.end(),
                        [](RecentSkinningEntry const& entry)
                        {
                            return IsExpired(entry.createdAt, SKINNING_RECENT_TIMEOUT_MS);
                        }),
                    entries.end());

                itr = entries.empty() ? g_recentSkinnings.erase(itr) : std::next(itr);
            }
        }

        bool WasSkinningCreatureRecentlyProcessed(Player* player, Creature* creature)
        {
            if (!player || !creature)
                return true;

            uint64 const key = player->GetGUID().GetRawValue();
            ObjectGuid const creatureGuid = creature->GetGUID();

            std::lock_guard<std::mutex> lock(g_stateMutex);

            auto itr = g_recentSkinnings.find(key);

            if (itr == g_recentSkinnings.end())
                return false;

            auto& entries = itr->second;

            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [](RecentSkinningEntry const& entry)
                    {
                        return IsExpired(entry.createdAt, SKINNING_RECENT_TIMEOUT_MS);
                    }),
                entries.end());

            if (entries.empty())
            {
                g_recentSkinnings.erase(itr);
                return false;
            }

            return std::any_of(entries.begin(), entries.end(),
                [&creatureGuid](RecentSkinningEntry const& entry)
                {
                    return entry.creature == creatureGuid;
                });
        }

        void MarkSkinningCreatureProcessed(Player* player, Creature* creature)
        {
            if (!player || !creature)
                return;

            RecentSkinningEntry entry;
            entry.creature  = creature->GetGUID();
            entry.createdAt = NowMS();

            std::lock_guard<std::mutex> lock(g_stateMutex);

            PruneLocked();

            auto& entries = g_recentSkinnings[player->GetGUID().GetRawValue()];

            /*
             * A ring of the last few corpses instead of a single slot.
             * With only one slot a player alternating between two
             * already-skinned corpses could be credited twice.
             */
            if (entries.size() >= MAX_RECENT_PER_PLAYER)
                entries.erase(entries.begin());

            entries.push_back(entry);
        }

        /* ==========================================================
         * Eligibility
         * ========================================================== */

        /*
         * Instance safe. Comparing only GetMapId() treats two separate
         * instances of the same map as the same place, which produced
         * meaningless distances across instance boundaries.
         */
        template<typename T>
        bool IsCloseEnough(Player* member, T* source)
        {
            if (!member || !source)
                return false;

            if (!member->IsInMap(source))
                return false;

            if (!member->InSamePhase(source))
                return false;

            return member->GetDistance(source) <= g_settings.MaxDistance;
        }

        bool IsSameGroup(Group* group, Player* member)
        {
            return group && member && member->GetGroup() == group;
        }

        bool HasRequiredProfession(Player* member, uint32 skillId, uint32 requiredSkill)
        {
            if (!member)
                return false;

            /*
             * RequireSkill = 0 now really means "do not require the
             * profession". Previously HasSkill() was enforced anyway,
             * which made the option almost a no-op.
             */
            if (!g_settings.RequireSkill)
                return true;

            if (!member->HasSkill(skillId))
                return false;

            uint32 const value = member->GetSkillValue(skillId);

            if (!value)
                return false;

            /*
             * Without this a member with Mining 1 would receive
             * Titanium Ore from a group mate's node.
             */
            if (g_settings.RequireNodeSkill && requiredSkill && value < requiredSkill)
                return false;

            return true;
        }

        template<typename T>
        bool IsEligibleMember(Player* gatherer, Group* group, Player* member,
            T* source, uint32 skillId, uint32 requiredSkill)
        {
            if (!gatherer || !member || !source)
                return false;

            /*
             * The gatherer is handled separately: the original
             * AzerothCore/AutoGather operation already granted one
             * normal loot roll and one normal skill-up attempt.
             */
            if (gatherer->GetGUID() == member->GetGUID())
                return false;

            if (!member->IsInWorld() || !member->IsAlive())
                return false;

            if (!IsSameGroup(group, member))
                return false;

            if (!IsCloseEnough(member, source))
                return false;

            if (!HasRequiredProfession(member, skillId, requiredSkill))
                return false;

            return true;
        }

        /* ==========================================================
         * Node / creature inspection
         * ========================================================== */

        /*
         * Resolve the skill requirement encoded in a gathering node's
         * lock. Pure lookup: it no longer mixes in "can this player
         * gather it", so the value can also be used to gate receivers.
         */
        bool GetNodeSkillRequirement(GameObject* gameObject, uint32 skillId, uint32& requiredSkill)
        {
            requiredSkill = 0;

            if (!gameObject || gameObject->GetGoType() != GAMEOBJECT_TYPE_CHEST)
                return false;

            GameObjectTemplate const* goInfo = gameObject->GetGOInfo();

            if (!goInfo)
                return false;

            uint32 const lockId = goInfo->GetLockId();

            if (!lockId)
                return false;

            LockEntry const* lockEntry = sLockStore.LookupEntry(lockId);

            if (!lockEntry)
                return false;

            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            {
                if (lockEntry->Type[i] != LOCK_KEY_SKILL)
                    continue;

                uint32 const lockType = lockEntry->Index[i];

                if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
                    continue;

                SkillType const resolvedSkill = SkillByLockType(LockType(lockType));

                /* skillId == 0 means "any gathering node". */
                if (skillId && resolvedSkill != skillId)
                    continue;

                requiredSkill = lockEntry->Skill[i];

                return true;
            }

            return false;
        }

        bool IsGatheringNode(GameObject* gameObject)
        {
            uint32 requiredSkill = 0;

            return GetNodeSkillRequirement(gameObject, 0, requiredSkill);
        }

        bool IsGatherableNodeForSkill(GameObject* gameObject, Player* player, uint32 skillId)
        {
            uint32 requiredSkill = 0;

            if (!GetNodeSkillRequirement(gameObject, skillId, requiredSkill))
                return false;

            return player && player->GetSkillValue(skillId) >= requiredSkill;
        }

        /*
         * Skinning requirement, mirroring the core formula used by
         * Spell::EffectSkinning and mod-auto-gather.
         */
        uint32 GetSkinningRequirement(Creature* creature)
        {
            if (!creature)
                return 0;

            int32 const targetLevel = creature->GetLevel();

            if (targetLevel < 10)
                return 0;

            if (targetLevel < 20)
                return (targetLevel - 10) * 10;

            return targetLevel * 5;
        }

        /* ==========================================================
         * Loot / skill granting
         * ========================================================== */

        /*
         * One real AzerothCore gathering skill attempt.
         *
         * This intentionally calls Player::UpdateGatherSkill() instead
         * of touching the skill value directly, so normal skill-up
         * chance, caps, skill gain configuration, script hooks and
         * profession rewards all keep working.
         */
        bool SimulateGatherSkillAttempt(Player* player, uint32 skillId,
            uint32 requiredSkill, uint32 multiplicator = 1)
        {
            if (!player)
                return false;

            uint32 const pureSkillValue = player->GetPureSkillValue(skillId);

            if (!pureSkillValue)
                return false;

            SimulationScope scope;

            if (!scope.Owned())
                return false;

            player->UpdateGatherSkill(skillId, pureSkillValue, requiredSkill, multiplicator);

            return true;
        }

        void GiveIndependentGatherLoot(Player* player, GameObject* gameObject)
        {
            if (!player || !gameObject)
                return;

            GameObjectTemplate const* goInfo = gameObject->GetGOInfo();

            if (!goInfo)
                return;

            uint32 const lootId = goInfo->GetLootId();

            if (!lootId)
                return;

            player->AutoStoreLoot(lootId, LootTemplates_Gameobject, true);
        }

        void GiveIndependentSkinningLoot(Player* player, Creature* creature)
        {
            if (!player || !creature)
                return;

            CreatureTemplate const* creatureInfo = creature->GetCreatureTemplate();

            if (!creatureInfo)
                return;

            uint32 const lootId = creatureInfo->SkinLootId;

            if (!lootId || !LootTemplates_Skinning.HaveLootFor(lootId))
                return;

            player->AutoStoreLoot(lootId, LootTemplates_Skinning, true);
        }

        void AnnounceToMember(Player* member, Player* gatherer, uint32 skillId)
        {
            if (!g_settings.Announce || !member || !gatherer || !member->GetSession())
                return;

            std::string const message =
                "|cff33ff99[Profession Loot Party]|r You received " +
                std::string(SkillName(skillId)) + " loot from " + gatherer->GetName() + ".";

            ChatHandler(member->GetSession()).SendSysMessage(message.c_str());
        }

        struct RollContext
        {
            uint32 skillId         = 0;
            uint32 requiredSkill   = 0;
            uint32 lootRolls       = 1;
            uint32 skillAttempts   = 1;
            uint32 skillMultiplier = 1;   /* elite skinning modifier */
        };

        template<typename LootFn>
        void ApplyRolls(Player* player, RollContext const& ctx,
            uint32 lootRolls, uint32 skillAttempts, LootFn&& giveLoot)
        {
            for (uint32 roll = 0; roll < lootRolls; ++roll)
                giveLoot(player);

            for (uint32 attempt = 0; attempt < skillAttempts; ++attempt)
                SimulateGatherSkillAttempt(player, ctx.skillId, ctx.requiredSkill, ctx.skillMultiplier);
        }

        /*
         * Shared distribution core for nodes and corpses.
         *
         * Gatherer : lootRolls - 1 additional rolls, skillAttempts - 1
         *            additional attempts (the core already gave one of
         *            each).
         * Others   : the full configured amounts.
         */
        template<typename SourceT, typename LootFn>
        uint32 Distribute(Player* gatherer, SourceT* source, RollContext const& ctx, LootFn&& giveLoot)
        {
            if (!gatherer || !source)
                return 0;

            if (g_settings.ApplyToGatherer)
            {
                ApplyRolls(gatherer, ctx,
                    ctx.lootRolls > 0 ? ctx.lootRolls - 1 : 0,
                    ctx.skillAttempts > 0 ? ctx.skillAttempts - 1 : 0,
                    giveLoot);
            }

            Group* group = gatherer->GetGroup();

            if (!group)
                return 0;

            uint32 recipients = 0;

            for (GroupReference* groupRef = group->GetFirstMember(); groupRef; groupRef = groupRef->next())
            {
                if (g_settings.MaxRecipients && recipients >= g_settings.MaxRecipients)
                    break;

                Player* member = groupRef->GetSource();

                if (!IsEligibleMember(gatherer, group, member, source, ctx.skillId, ctx.requiredSkill))
                    continue;

                ApplyRolls(member, ctx, ctx.lootRolls, ctx.skillAttempts, giveLoot);
                AnnounceToMember(member, gatherer, ctx.skillId);

                ++recipients;
            }

            return recipients;
        }

        /* ==========================================================
         * Cheap pre-checks
         *
         * These run before any grid search, which is by far the most
         * expensive part of the module. With the default 1x/1x setup
         * and no second group member present, nothing is searched at
         * all.
         * ========================================================== */

        bool ShouldProcess(Player* player, uint32 skillId)
        {
            if (!g_settings.Enabled || !player)
                return false;

            ProfessionSettings const* profession = SettingsForSkill(skillId);

            if (!profession || !profession->Enabled)
                return false;

            Group* group = player->GetGroup();

            if (!group)
                return !g_settings.RequireGroup &&
                       g_settings.ApplyToGatherer &&
                       (profession->LootMultiplier > 1 || profession->SkillMultiplier > 1);

            if (group->isRaidGroup() && !g_settings.IncludeRaid)
                return false;

            bool const gathererBonus = g_settings.ApplyToGatherer &&
                (profession->LootMultiplier > 1 || profession->SkillMultiplier > 1);

            bool const othersPossible = group->GetMembersCount() > 1;

            return gathererBonus || othersPossible;
        }

        float SearchRadius()
        {
            return std::max(1.0f, g_settings.SearchDistance);
        }

        /* ==========================================================
         * Distribution entry points
         * ========================================================== */

        void DistributeGatherLoot(Player* gatherer, GameObject* gameObject, uint32 skillId)
        {
            ProfessionSettings const* profession = SettingsForSkill(skillId);

            if (!gatherer || !gameObject || !profession || !profession->Enabled)
                return;

            RollContext ctx;
            ctx.skillId       = skillId;
            ctx.lootRolls     = profession->LootMultiplier;
            ctx.skillAttempts = profession->SkillMultiplier;

            GetNodeSkillRequirement(gameObject, skillId, ctx.requiredSkill);

            uint32 const recipients = Distribute(gatherer, gameObject, ctx,
                [gameObject](Player* target) { GiveIndependentGatherLoot(target, gameObject); });

            if (g_settings.Debug)
            {
                DebugLog("Gathering distributed. Player=" + DescribePlayer(gatherer) +
                    " Skill=" + SkillName(skillId) +
                    " Entry=" + std::to_string(gameObject->GetEntry()) +
                    " Recipients=" + std::to_string(recipients));
            }
        }

        void DistributeSkinningLoot(Player* skinner, Creature* creature)
        {
            ProfessionSettings const& profession = g_settings.Professions[PROFESSION_SKINNING];

            if (!skinner || !creature || !profession.Enabled)
                return;

            RollContext ctx;
            ctx.skillId         = SKILL_SKINNING;
            ctx.requiredSkill   = GetSkinningRequirement(creature);
            ctx.lootRolls       = profession.LootMultiplier;
            ctx.skillAttempts   = profession.SkillMultiplier;
            ctx.skillMultiplier = creature->isElite() ? 2 : 1;

            uint32 const recipients = Distribute(skinner, creature, ctx,
                [creature](Player* target) { GiveIndependentSkinningLoot(target, creature); });

            if (g_settings.Debug)
            {
                DebugLog("Skinning distributed. Player=" + DescribePlayer(skinner) +
                    " Entry=" + std::to_string(creature->GetEntry()) +
                    " Recipients=" + std::to_string(recipients));
            }
        }

        /* ==========================================================
         * mod-auto-gather detection
         * ========================================================== */

        class AutoGatherNodeCheck
        {
        public:
            AutoGatherNodeCheck(Player* player, uint32 skillId, float range)
                : _player(player), _skillId(skillId), _range(range) { }

            bool operator()(GameObject* gameObject)
            {
                if (!gameObject || !gameObject->isSpawned())
                    return false;

                /*
                 * Accept GO_READY and GO_ACTIVATED: depending on the
                 * gathering implementation the node may already have
                 * flipped state when the skill hook fires.
                 */
                uint32 const lootState = gameObject->getLootState();

                if (lootState != GO_READY && lootState != GO_ACTIVATED)
                    return false;

                if (!gameObject->IsInMap(_player) || !gameObject->InSamePhase(_player))
                    return false;

                if (!_player->IsWithinDist(gameObject, _range, false))
                    return false;

                /*
                 * mod-auto-gather adds the player to the skill-up list
                 * immediately before UpdateGatherSkill().
                 */
                if (!gameObject->IsInSkillupList(_player->GetGUID()))
                    return false;

                return IsGatherableNodeForSkill(gameObject, _player, _skillId);
            }

        private:
            Player* _player;
            uint32  _skillId;
            float   _range;
        };

        GameObject* FindAutoGatherNode(Player* player, uint32 skillId)
        {
            if (!player)
                return nullptr;

            float const range = SearchRadius();

            std::list<GameObject*> nodes;
            AutoGatherNodeCheck check(player, skillId, range);
            Acore::GameObjectListSearcher<AutoGatherNodeCheck> searcher(player, nodes, check);

            Cell::VisitObjects(player, searcher, range);

            if (nodes.empty())
                return nullptr;

            return *std::min_element(nodes.begin(), nodes.end(),
                [player](GameObject* left, GameObject* right)
                {
                    return player->GetDistance(left) < player->GetDistance(right);
                });
        }

        /*
         * Detects both normal AzerothCore skinning and
         * mod-auto-gather's AutoSkinCreature().
         */
        class SkinningCreatureCheck
        {
        public:
            SkinningCreatureCheck(Player* player, float range)
                : _player(player), _range(range) { }

            bool operator()(Creature* creature)
            {
                if (!creature || creature->IsAlive() || !creature->IsInWorld())
                    return false;

                if (!creature->IsInMap(_player) || !creature->InSamePhase(_player))
                    return false;

                if (!_player->IsWithinDist(creature, _range, false))
                    return false;

                if (creature->loot.loot_type != LOOT_SKINNING)
                    return false;

                CreatureTemplate const* creatureInfo = creature->GetCreatureTemplate();

                if (!creatureInfo || !creatureInfo->SkinLootId)
                    return false;

                if (creatureInfo->GetRequiredLootSkill() != SKILL_SKINNING)
                    return false;

                /*
                 * Both normal skinning and AutoSkinCreature() remove
                 * UNIT_FLAG_SKINNABLE on success.
                 */
                if (creature->HasUnitFlag(UNIT_FLAG_SKINNABLE))
                    return false;

                return creature->isTappedBy(_player);
            }

        private:
            Player* _player;
            float   _range;
        };

        Creature* FindSkinningCreature(Player* player)
        {
            if (!player)
                return nullptr;

            float const range = SearchRadius();

            std::list<Creature*> creatures;
            SkinningCreatureCheck check(player, range);
            Acore::CreatureListSearcher<SkinningCreatureCheck> searcher(player, creatures, check);

            Cell::VisitObjects(player, searcher, range);

            if (creatures.empty())
                return nullptr;

            return *std::min_element(creatures.begin(), creatures.end(),
                [player](Creature* left, Creature* right)
                {
                    return player->GetDistance(left) < player->GetDistance(right);
                });
        }

        /* ==========================================================
         * Configuration loading
         * ========================================================== */

        uint32 ClampMultiplier(uint32 value)
        {
            return std::max<uint32>(1, std::min(value, MAX_PROFESSION_MULTIPLIER));
        }

        uint32 ReadMultiplier(char const* key, char const* legacyKey)
        {
            uint32 const fallback = legacyKey
                ? sConfigMgr->GetOption<uint32>(legacyKey, 1)
                : 1;

            return ClampMultiplier(sConfigMgr->GetOption<uint32>(key, fallback));
        }

        void LoadConfig()
        {
            g_settings.Enabled = sConfigMgr->GetOption<bool>("ProfessionLootParty.Enable", true);

            ProfessionSettings& mining    = g_settings.Professions[PROFESSION_MINING];
            ProfessionSettings& herbalism = g_settings.Professions[PROFESSION_HERBALISM];
            ProfessionSettings& skinning  = g_settings.Professions[PROFESSION_SKINNING];

            mining.Enabled    = sConfigMgr->GetOption<bool>("ProfessionLootParty.Mining", true);
            herbalism.Enabled = sConfigMgr->GetOption<bool>("ProfessionLootParty.Herbalism", true);
            skinning.Enabled  = sConfigMgr->GetOption<bool>("ProfessionLootParty.Skinning", true);

            /* Deprecated *Multiplier keys stay valid as loot fallbacks. */
            mining.LootMultiplier = ReadMultiplier(
                "ProfessionLootParty.MiningLootMultiplier", "ProfessionLootParty.MiningMultiplier");
            herbalism.LootMultiplier = ReadMultiplier(
                "ProfessionLootParty.HerbalismLootMultiplier", "ProfessionLootParty.HerbalismMultiplier");
            skinning.LootMultiplier = ReadMultiplier(
                "ProfessionLootParty.SkinningLootMultiplier", "ProfessionLootParty.SkinningMultiplier");

            mining.SkillMultiplier = ReadMultiplier(
                "ProfessionLootParty.MiningSkillMultiplier", nullptr);
            herbalism.SkillMultiplier = ReadMultiplier(
                "ProfessionLootParty.HerbalismSkillMultiplier", nullptr);
            skinning.SkillMultiplier = ReadMultiplier(
                "ProfessionLootParty.SkinningSkillMultiplier", nullptr);

            g_settings.IncludeRaid      = sConfigMgr->GetOption<bool>("ProfessionLootParty.Raid", true);
            g_settings.RequireSkill     = sConfigMgr->GetOption<bool>("ProfessionLootParty.RequireSkill", true);
            g_settings.RequireNodeSkill = sConfigMgr->GetOption<bool>("ProfessionLootParty.RequireNodeSkill", true);
            g_settings.RequireGroup     = sConfigMgr->GetOption<bool>("ProfessionLootParty.RequireGroup", true);
            g_settings.ApplyToGatherer  = sConfigMgr->GetOption<bool>("ProfessionLootParty.ApplyToGatherer", true);
            g_settings.AutoGatherCompat = sConfigMgr->GetOption<bool>("ProfessionLootParty.AutoGatherCompat", true);
            g_settings.Announce         = sConfigMgr->GetOption<bool>("ProfessionLootParty.Announce", false);
            g_settings.Debug            = sConfigMgr->GetOption<bool>("ProfessionLootParty.Debug", false);

            g_settings.MaxRecipients    = sConfigMgr->GetOption<uint32>("ProfessionLootParty.MaxRecipients", 0);

            g_settings.MaxDistance = std::max(0.0f, std::min(
                sConfigMgr->GetOption<float>("ProfessionLootParty.Distance", 100.0f), MAX_CONFIG_DISTANCE));

            g_settings.SearchDistance = std::max(1.0f, std::min(
                sConfigMgr->GetOption<float>("ProfessionLootParty.SearchDistance", 10.0f), MAX_CONFIG_SEARCH));
        }

        void LogConfig()
        {
            LOG_INFO("server.loading", ">> ProfessionLootParty: {}",
                g_settings.Enabled ? "Enabled" : "Disabled");

            if (!g_settings.Enabled)
                return;

            static char const* const names[PROFESSION_MAX] = { "Mining", "Herbalism", "Skinning" };

            for (uint8 slot = 0; slot < PROFESSION_MAX; ++slot)
            {
                ProfessionSettings const& profession = g_settings.Professions[slot];

                LOG_INFO("server.loading", "   {}: {} Loot={}x Skill={}x",
                    names[slot], profession.Enabled ? "on" : "off",
                    profession.LootMultiplier, profession.SkillMultiplier);
            }

            std::string const recipients = g_settings.MaxRecipients
                ? std::to_string(g_settings.MaxRecipients)
                : std::string("unlimited");

            LOG_INFO("server.loading", "   Distance={} SearchDistance={} MaxRecipients={}",
                g_settings.MaxDistance, g_settings.SearchDistance, recipients);

            LOG_INFO("server.loading", "   Raid={} RequireGroup={} RequireSkill={} RequireNodeSkill={}",
                g_settings.IncludeRaid, g_settings.RequireGroup,
                g_settings.RequireSkill, g_settings.RequireNodeSkill);

            LOG_INFO("server.loading", "   ApplyToGatherer={} AutoGatherCompat={} Announce={}",
                g_settings.ApplyToGatherer, g_settings.AutoGatherCompat, g_settings.Announce);
        }
    } // anonymous namespace

    /* ==============================================================
     * Public API
     * ============================================================== */

    bool IsEnabled()
    {
        return g_settings.Enabled;
    }

    ModuleSettings const& GetSettings()
    {
        return g_settings;
    }

    bool IsProfessionEnabled(uint32 skillId)
    {
        if (!g_settings.Enabled)
            return false;

        ProfessionSettings const* profession = SettingsForSkill(skillId);

        return profession && profession->Enabled;
    }

    uint32 GetProfessionLootMultiplier(uint32 skillId)
    {
        ProfessionSettings const* profession = SettingsForSkill(skillId);

        return (g_settings.Enabled && profession) ? profession->LootMultiplier : 0;
    }

    uint32 GetProfessionSkillMultiplier(uint32 skillId)
    {
        ProfessionSettings const* profession = SettingsForSkill(skillId);

        return (g_settings.Enabled && profession) ? profession->SkillMultiplier : 0;
    }

    void AddPendingGather(Player* gatherer, GameObject* gameObject)
    {
        if (!gatherer || !gameObject)
            return;

        PendingGatherEntry entry;
        entry.gameObject = gameObject->GetGUID();
        entry.createdAt  = NowMS();

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            PruneLocked();

            g_pendingGathers[gatherer->GetGUID().GetRawValue()] = entry;
        }

        if (g_settings.Debug)
        {
            DebugLog("Gathering queued. Player=" + DescribePlayer(gatherer) +
                " Entry=" + std::to_string(gameObject->GetEntry()));
        }
    }

    void RemovePendingGather(ObjectGuid playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);

        g_pendingGathers.erase(playerGuid.GetRawValue());
    }

    void RemoveRecentSkinning(ObjectGuid playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);

        g_recentSkinnings.erase(playerGuid.GetRawValue());
    }

    void ForgetPlayer(ObjectGuid playerGuid)
    {
        uint64 const key = playerGuid.GetRawValue();

        std::lock_guard<std::mutex> lock(g_stateMutex);

        g_pendingGathers.erase(key);
        g_recentSkinnings.erase(key);
    }

    bool ProcessPendingGather(Player* gatherer, uint32 skillId)
    {
        if (!gatherer || !IsGatheringSkill(skillId))
            return false;

        PendingGatherEntry entry;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            auto itr = g_pendingGathers.find(gatherer->GetGUID().GetRawValue());

            if (itr == g_pendingGathers.end())
                return false;

            entry = itr->second;
            g_pendingGathers.erase(itr);
        }

        /*
         * From here on the operation is claimed: return true so the
         * caller does not fall through to the AutoGather path.
         */
        if (IsExpired(entry.createdAt, PENDING_TIMEOUT_MS))
            return true;

        if (!ShouldProcess(gatherer, skillId))
            return true;

        GameObject* gameObject = ObjectAccessor::GetGameObject(*gatherer, entry.gameObject);

        if (!gameObject)
            return true;

        /*
         * Core EffectOpenLock() adds the node to the skill-up list
         * before calling UpdateGatherSkill().
         */
        if (!gameObject->IsInSkillupList(gatherer->GetGUID()))
            return true;

        DistributeGatherLoot(gatherer, gameObject, skillId);

        return true;
    }

    void ProcessAutoGather(Player* gatherer, uint32 skillId)
    {
        if (!g_settings.AutoGatherCompat)
            return;

        if (!gatherer || !IsGatheringSkill(skillId))
            return;

        if (!ShouldProcess(gatherer, skillId))
            return;

        GameObject* gameObject = FindAutoGatherNode(gatherer, skillId);

        if (!gameObject)
            return;

        /* The GameObject lifecycle stays owned by the gathering implementation. */
        DistributeGatherLoot(gatherer, gameObject, skillId);
    }

    bool ProcessSkinning(Player* skinner)
    {
        if (!skinner || !ShouldProcess(skinner, SKILL_SKINNING))
            return false;

        Creature* creature = FindSkinningCreature(skinner);

        if (!creature)
            return false;

        if (WasSkinningCreatureRecentlyProcessed(skinner, creature))
            return true;

        MarkSkinningCreatureProcessed(skinner, creature);

        DistributeSkinningLoot(skinner, creature);

        return true;
    }

    /* ==============================================================
     * Scripts
     * ============================================================== */

    PlayerScript::PlayerScript()
        : ::PlayerScript("ProfessionLootParty_PlayerScript")
    {
    }

    void PlayerScript::OnPlayerUpdateGatheringSkill(Player* player, uint32 skillId,
        uint32 /*currentLevel*/, uint32 /*gray*/, uint32 /*green*/, uint32 /*yellow*/, uint32& /*gain*/)
    {
        if (!player || !g_settings.Enabled)
            return;

        /*
         * Critical re-entrancy protection: the module calls
         * UpdateGatherSkill() itself, which re-enters this hook.
         */
        if (t_simulating)
            return;

        if (IsSkinningSkill(skillId))
        {
            ProcessSkinning(player);
            return;
        }

        if (!IsGatheringSkill(skillId))
            return;

        /* Normal AzerothCore gathering first. */
        if (ProcessPendingGather(player, skillId))
            return;

        /* Otherwise this may be mod-auto-gather. */
        ProcessAutoGather(player, skillId);
    }

    void PlayerScript::OnPlayerLogout(Player* player)
    {
        if (!player)
            return;

        ForgetPlayer(player->GetGUID());
    }

    GameObjectScript::GameObjectScript()
        : ::AllGameObjectScript("ProfessionLootParty_GameObjectScript")
    {
    }

    void GameObjectScript::OnGameObjectLootStateChanged(GameObject* gameObject, uint32 state, Unit* unit)
    {
        if (!g_settings.Enabled || !gameObject || !unit)
            return;

        if (state != GO_ACTIVATED || !unit->IsPlayer())
            return;

        /*
         * Only queue actual gathering nodes.
         *
         * This hook fires for doors, chests, quest objects and every
         * other activated GameObject, so filtering here removes a very
         * large number of pointless map insertions and eliminates
         * false positives from non-gathering interactions.
         */
        if (!IsGatheringNode(gameObject))
            return;

        Player* gatherer = unit->ToPlayer();

        if (!gatherer)
            return;

        if (g_settings.RequireGroup && !gatherer->GetGroup())
            return;

        /*
         * Mining vs Herbalism is not guessed here; the real SkillType
         * arrives with OnPlayerUpdateGatheringSkill().
         */
        AddPendingGather(gatherer, gameObject);
    }

    ConfigScript::ConfigScript()
        : ::WorldScript("ProfessionLootParty_ConfigScript")
    {
    }

    /*
     * Both hooks are implemented on purpose: depending on the core
     * revision module .conf files are merged before or after
     * OnBeforeConfigLoad(). Loading twice is harmless and idempotent,
     * only the later pass logs the summary.
     */
    void ConfigScript::OnBeforeConfigLoad(bool /*reload*/)
    {
        LoadConfig();
    }

    void ConfigScript::OnAfterConfigLoad(bool /*reload*/)
    {
        LoadConfig();
        LogConfig();
    }
}

/*
 * AzerothCore module loader.
 */
void Addmod_profession_loot_partyScripts()
{
    new ProfessionLootParty::ConfigScript();
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}
