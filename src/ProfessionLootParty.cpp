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
 *   - Normal AzerothCore Mining
 *   - Normal AzerothCore Herbalism
 *   - Normal AzerothCore Skinning
 *   - mod-auto-gather Mining
 *   - mod-auto-gather Herbalism
 *   - mod-auto-gather Skinning
 *
 * Loot and skill multipliers are independent.
 */

#include "ProfessionLootParty.h"

#include "CellImpl.h"
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
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace ProfessionLootParty
{
    namespace
    {
        bool Enabled = true;

        bool MiningEnabled = true;
        bool HerbalismEnabled = true;
        bool SkinningEnabled = true;

        /*
         * Total loot rolls.
         *
         * 1 = normal behavior
         * 2 = two total loot rolls
         * 3 = three total loot rolls
         */
        uint32 MiningLootMultiplier = 1;
        uint32 HerbalismLootMultiplier = 1;
        uint32 SkinningLootMultiplier = 1;

        /*
         * Total skill-up attempts.
         *
         * 1 = normal/base attempt
         * 2 = base + 1 additional attempt
         * 3 = base + 2 additional attempts
         */
        uint32 MiningSkillMultiplier = 1;
        uint32 HerbalismSkillMultiplier = 1;
        uint32 SkinningSkillMultiplier = 1;

        constexpr uint32 MAX_PROFESSION_MULTIPLIER = 100;

        bool IncludeRaid = true;
        bool RequireSkill = true;
        bool Debug = false;

        float MaxDistance = 100.0f;

        constexpr uint32 PENDING_TIMEOUT_MS = 5000;
        constexpr uint32 SKINNING_RECENT_TIMEOUT_MS = 60000;

        std::unordered_map<uint64, PendingGather> PendingGathers;
        std::unordered_map<uint64, RecentSkinning> RecentSkinnings;

        /*
         * UpdateGatherSkill() invokes OnPlayerUpdateGatheringSkill()
         * itself before doing the actual skill-up roll.
         *
         * Therefore all module-generated UpdateGatherSkill() calls
         * must be protected from being interpreted as a NEW gathering
         * operation.
         */
        std::unordered_set<uint64> SimulatedSkillUpdates;

        uint32 GetMSTime()
        {
            using namespace std::chrono;

            return static_cast<uint32>(
                duration_cast<milliseconds>(
                    steady_clock::now().time_since_epoch()).count());
        }

        bool IsPendingExpired(
            PendingGather const& pending)
        {
            return uint32(GetMSTime() - pending.createdAt) >
                   PENDING_TIMEOUT_MS;
        }

        bool IsRecentSkinningExpired(
            RecentSkinning const& recent)
        {
            return uint32(GetMSTime() - recent.createdAt) >
                   SKINNING_RECENT_TIMEOUT_MS;
        }

        bool IsSimulatedSkillUpdate(
            Player* player)
        {
            if (!player)
                return false;

            return SimulatedSkillUpdates.find(
                       player->GetGUID().GetRawValue()) !=
                   SimulatedSkillUpdates.end();
        }

        void DebugLog(
            char const* message,
            Player* player = nullptr,
            GameObject* gameObject = nullptr,
            Creature* creature = nullptr)
        {
            if (!Debug)
                return;

            if (player && gameObject)
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {} Player={} GO={} Entry={}",
                    message,
                    player->GetName(),
                    gameObject->GetGUID().ToString(),
                    gameObject->GetEntry());

                return;
            }

            if (player && creature)
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {} Player={} Creature={} Entry={}",
                    message,
                    player->GetName(),
                    creature->GetGUID().ToString(),
                    creature->GetEntry());

                return;
            }

            if (player)
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {} Player={}",
                    message,
                    player->GetName());

                return;
            }

            LOG_DEBUG(
                "module",
                "[ProfessionLootParty] {}",
                message);
        }

        bool IsGatheringSkill(
            uint32 skillId)
        {
            return skillId == SKILL_MINING ||
                   skillId == SKILL_HERBALISM;
        }

        bool IsSkinningSkill(
            uint32 skillId)
        {
            return skillId == SKILL_SKINNING;
        }

        bool HasProfession(
            Player* player,
            uint32 skillId)
        {
            return player &&
                   player->HasSkill(skillId);
        }

        bool IsSameGroup(
            Player* source,
            Player* member)
        {
            if (!source || !member)
                return false;

            Group* group = source->GetGroup();

            if (!group)
                return false;

            if (member->GetGroup() != group)
                return false;

            if (group->isRaidGroup() && !IncludeRaid)
                return false;

            return true;
        }

        bool IsCloseEnough(
            Player* member,
            GameObject* gameObject)
        {
            if (!member || !gameObject)
                return false;

            if (member->GetMapId() != gameObject->GetMapId())
                return false;

            if (!member->InSamePhase(gameObject))
                return false;

            return member->GetDistance(gameObject) <= MaxDistance;
        }

        bool IsCloseEnough(
            Player* member,
            Creature* creature)
        {
            if (!member || !creature)
                return false;

            if (member->GetMapId() != creature->GetMapId())
                return false;

            if (!member->InSamePhase(creature))
                return false;

            return member->GetDistance(creature) <= MaxDistance;
        }

        bool IsEligibleGatherMember(
            Player* gatherer,
            Player* member,
            GameObject* gameObject,
            uint32 skillId)
        {
            if (!gatherer || !member || !gameObject)
                return false;

            /*
             * Gatherer is processed separately because the original
             * AzerothCore/AutoGather operation already gave them one
             * normal loot roll and one normal skill-up attempt.
             */
            if (gatherer->GetGUID() == member->GetGUID())
                return false;

            if (!member->IsInWorld())
                return false;

            if (!member->IsAlive())
                return false;

            if (!IsSameGroup(gatherer, member))
                return false;

            if (!IsCloseEnough(member, gameObject))
                return false;

            if (!IsProfessionEnabled(skillId))
                return false;

            if (!HasProfession(member, skillId))
                return false;

            if (RequireSkill &&
                member->GetSkillValue(skillId) == 0)
            {
                return false;
            }

            return true;
        }

        bool IsEligibleSkinningMember(
            Player* skinner,
            Player* member,
            Creature* creature)
        {
            if (!skinner || !member || !creature)
                return false;

            if (skinner->GetGUID() == member->GetGUID())
                return false;

            if (!member->IsInWorld())
                return false;

            if (!member->IsAlive())
                return false;

            if (!IsSameGroup(skinner, member))
                return false;

            if (!IsCloseEnough(member, creature))
                return false;

            if (!IsProfessionEnabled(SKILL_SKINNING))
                return false;

            if (!HasProfession(member, SKILL_SKINNING))
                return false;

            if (RequireSkill &&
                member->GetSkillValue(SKILL_SKINNING) == 0)
            {
                return false;
            }

            return true;
        }

        /*
         * Return the configured TOTAL loot multiplier.
         */
        uint32 GetLootMultiplierInternal(
            uint32 skillId)
        {
            switch (skillId)
            {
                case SKILL_MINING:
                    return MiningLootMultiplier;

                case SKILL_HERBALISM:
                    return HerbalismLootMultiplier;

                case SKILL_SKINNING:
                    return SkinningLootMultiplier;

                default:
                    return 1;
            }
        }

        /*
         * Return the configured TOTAL skill-attempt multiplier.
         */
        uint32 GetSkillMultiplierInternal(
            uint32 skillId)
        {
            switch (skillId)
            {
                case SKILL_MINING:
                    return MiningSkillMultiplier;

                case SKILL_HERBALISM:
                    return HerbalismSkillMultiplier;

                case SKILL_SKINNING:
                    return SkinningSkillMultiplier;

                default:
                    return 1;
            }
        }

        /*
         * Find the required skill level represented by a Mining/
         * Herbalism GameObject lock.
         */
        bool GetGatheringRequirement(
            GameObject* gameObject,
            Player* player,
            uint32 skillId,
            uint32& requiredSkill)
        {
            requiredSkill = 0;

            if (!gameObject || !player)
                return false;

            if (gameObject->GetGoType() !=
                GAMEOBJECT_TYPE_CHEST)
            {
                return false;
            }

            GameObjectTemplate const* goInfo =
                gameObject->GetGOInfo();

            if (!goInfo)
                return false;

            uint32 lockId = goInfo->GetLockId();

            if (!lockId)
                return false;

            LockEntry const* lockEntry =
                sLockStore.LookupEntry(lockId);

            if (!lockEntry)
                return false;

            for (uint8 i = 0;
                 i < MAX_LOCK_CASE;
                 ++i)
            {
                if (lockEntry->Type[i] !=
                    LOCK_KEY_SKILL)
                {
                    continue;
                }

                uint32 lockType =
                    lockEntry->Index[i];

                if (lockType != LOCKTYPE_HERBALISM &&
                    lockType != LOCKTYPE_MINING)
                {
                    continue;
                }

                SkillType resolvedSkill =
                    SkillByLockType(
                        LockType(lockType));

                if (resolvedSkill != skillId)
                    continue;

                requiredSkill =
                    lockEntry->Skill[i];

                /*
                 * The same check used when identifying an
                 * AutoGather node.
                 */
                if (player->GetSkillValue(resolvedSkill) <
                    requiredSkill)
                {
                    return false;
                }

                return true;
            }

            return false;
        }

        bool IsGatherableNodeForSkill(
            GameObject* gameObject,
            Player* player,
            uint32 skillId)
        {
            uint32 requiredSkill = 0;

            return GetGatheringRequirement(
                gameObject,
                player,
                skillId,
                requiredSkill);
        }

        /*
         * Perform one real AzerothCore gathering skill attempt.
         *
         * This intentionally calls Player::UpdateGatherSkill()
         * instead of manipulating the skill value directly.
         *
         * That means:
         *   - normal server skill-up chance applies
         *   - skill caps apply
         *   - server skill gain configuration applies
         *   - PlayerScript skill hooks apply
         *   - professions can unlock their normal rewards
         */
        bool SimulateGatherSkillAttempt(
            Player* player,
            uint32 skillId,
            uint32 requiredSkill,
            uint32 multiplicator = 1)
        {
            if (!player)
                return false;

            uint32 pureSkillValue =
                player->GetPureSkillValue(skillId);

            if (!pureSkillValue)
                return false;

            uint64 playerKey =
                player->GetGUID().GetRawValue();

            if (!SimulatedSkillUpdates.insert(playerKey).second)
                return false;

            player->UpdateGatherSkill(
                skillId,
                pureSkillValue,
                requiredSkill,
                multiplicator);

            SimulatedSkillUpdates.erase(playerKey);

            return true;
        }

        /*
         * Skinning requirement matching the AutoGather implementation
         * supplied for this module.
         */
        uint32 GetSkinningRequirement(
            Creature* creature)
        {
            if (!creature)
                return 0;

            int32 targetLevel =
                creature->GetLevel();

            if (targetLevel < 10)
                return 0;

            if (targetLevel < 20)
                return (targetLevel - 10) * 10;

            return targetLevel * 5;
        }

        /*
         * One independent Mining/Herbalism loot roll.
         */
        void GiveIndependentGatherLoot(
            Player* player,
            GameObject* gameObject)
        {
            if (!player || !gameObject)
                return;

            GameObjectTemplate const* goInfo =
                gameObject->GetGOInfo();

            if (!goInfo)
                return;

            uint32 lootId =
                goInfo->GetLootId();

            if (!lootId)
                return;

            player->AutoStoreLoot(
                lootId,
                LootTemplates_Gameobject,
                true);
        }

        /*
         * One independent Skinning loot roll.
         */
        void GiveIndependentSkinningLoot(
            Player* player,
            Creature* creature)
        {
            if (!player || !creature)
                return;

            CreatureTemplate const* creatureInfo =
                creature->GetCreatureTemplate();

            if (!creatureInfo)
                return;

            uint32 lootId =
                creatureInfo->SkinLootId;

            if (!lootId)
                return;

            if (!LootTemplates_Skinning.HaveLootFor(lootId))
                return;

            player->AutoStoreLoot(
                lootId,
                LootTemplates_Skinning,
                true);
        }

        /*
         * Resolve and distribute Mining/Herbalism.
         *
         * Loot:
         *   Gatherer = lootMultiplier - 1 additional rolls
         *   Others   = lootMultiplier rolls
         *
         * Skill:
         *   Gatherer = skillMultiplier - 1 additional attempts
         *   Others   = skillMultiplier attempts
         */
        void DistributeGatherLoot(
            Player* gatherer,
            GameObject* gameObject,
            uint32 skillId)
        {
            if (!gatherer || !gameObject)
                return;

            if (!IsProfessionEnabled(skillId))
                return;

            Group* group =
                gatherer->GetGroup();

            if (!group)
                return;

            uint32 lootMultiplier =
                GetLootMultiplierInternal(skillId);

            uint32 skillMultiplier =
                GetSkillMultiplierInternal(skillId);

            uint32 requiredSkill = 0;

            /*
             * If this is a valid gathering operation, the requirement
             * can be resolved from the node's lock.
             *
             * Failure here should not prevent loot distribution.
             */
            GetGatheringRequirement(
                gameObject,
                gatherer,
                skillId,
                requiredSkill);

            /*
             * The gatherer already received:
             *
             *   1 normal loot roll
             *   1 normal UpdateGatherSkill() attempt
             *
             * Only add the configured additional amounts.
             */
            for (uint32 roll = 1;
                 roll < lootMultiplier;
                 ++roll)
            {
                GiveIndependentGatherLoot(
                    gatherer,
                    gameObject);
            }

            for (uint32 attempt = 1;
                 attempt < skillMultiplier;
                 ++attempt)
            {
                SimulateGatherSkillAttempt(
                    gatherer,
                    skillId,
                    requiredSkill);
            }

            /*
             * Every other group member with the profession receives
             * the configured TOTAL number of loot rolls and skill
             * attempts.
             */
            for (GroupReference* groupRef =
                     group->GetFirstMember();
                 groupRef != nullptr;
                 groupRef = groupRef->next())
            {
                Player* member =
                    groupRef->GetSource();

                if (!member)
                    continue;

                if (!IsEligibleGatherMember(
                        gatherer,
                        member,
                        gameObject,
                        skillId))
                {
                    continue;
                }

                /*
                 * Loot.
                 */
                for (uint32 roll = 0;
                     roll < lootMultiplier;
                     ++roll)
                {
                    GiveIndependentGatherLoot(
                        member,
                        gameObject);
                }

                /*
                 * Skill.
                 */
                for (uint32 attempt = 0;
                     attempt < skillMultiplier;
                     ++attempt)
                {
                    SimulateGatherSkillAttempt(
                        member,
                        skillId,
                        requiredSkill);
                }
            }

            DebugLog(
                "Profession group gathering processing completed.",
                gatherer,
                gameObject);
        }

        /*
         * Distribute successful Skinning.
         *
         * The original skinner has already received:
         *   - normal skinning loot
         *   - normal UpdateGatherSkill() attempt
         *
         * Additional configured amounts are therefore added only
         * by this module.
         */
        void DistributeSkinningLoot(
            Player* skinner,
            Creature* creature)
        {
            if (!skinner || !creature)
                return;

            if (!IsProfessionEnabled(SKILL_SKINNING))
                return;

            Group* group =
                skinner->GetGroup();

            if (!group)
                return;

            uint32 lootMultiplier =
                GetLootMultiplierInternal(
                    SKILL_SKINNING);

            uint32 skillMultiplier =
                GetSkillMultiplierInternal(
                    SKILL_SKINNING);

            uint32 requiredSkill =
                GetSkinningRequirement(creature);

            uint32 skillMultiplicator =
                creature->isElite() ? 2 : 1;

            /*
             * Original skinner:
             *
             * normal loot + (lootMultiplier - 1)
             * normal skill attempt + (skillMultiplier - 1)
             */
            for (uint32 roll = 1;
                 roll < lootMultiplier;
                 ++roll)
            {
                GiveIndependentSkinningLoot(
                    skinner,
                    creature);
            }

            for (uint32 attempt = 1;
                 attempt < skillMultiplier;
                 ++attempt)
            {
                SimulateGatherSkillAttempt(
                    skinner,
                    SKILL_SKINNING,
                    requiredSkill,
                    skillMultiplicator);
            }

            /*
             * Other Skinning users:
             *
             * full configured loot multiplier
             * full configured skill multiplier
             */
            for (GroupReference* groupRef =
                     group->GetFirstMember();
                 groupRef != nullptr;
                 groupRef = groupRef->next())
            {
                Player* member =
                    groupRef->GetSource();

                if (!member)
                    continue;

                if (!IsEligibleSkinningMember(
                        skinner,
                        member,
                        creature))
                {
                    continue;
                }

                for (uint32 roll = 0;
                     roll < lootMultiplier;
                     ++roll)
                {
                    GiveIndependentSkinningLoot(
                        member,
                        creature);
                }

                for (uint32 attempt = 0;
                     attempt < skillMultiplier;
                     ++attempt)
                {
                    SimulateGatherSkillAttempt(
                        member,
                        SKILL_SKINNING,
                        requiredSkill,
                        skillMultiplicator);
                }
            }

            DebugLog(
                "Profession group skinning processing completed.",
                skinner,
                nullptr,
                creature);
        }

        /*
         * AutoGather Mining/Herbalism node search.
         */
        class AutoGatherNodeCheck
        {
        public:
            AutoGatherNodeCheck(
                Player* player,
                uint32 skillId,
                float range)
                : _player(player),
                  _skillId(skillId),
                  _range(range)
            {
            }

            bool operator()(
                GameObject* gameObject)
            {
                if (!gameObject)
                    return false;

                if (gameObject->getLootState() !=
                    GO_READY)
                {
                    return false;
                }

                if (!gameObject->isSpawned())
                    return false;

                if (!gameObject->IsInMap(_player))
                    return false;

                if (!gameObject->InSamePhase(_player))
                    return false;

                if (!_player->IsWithinDist(
                        gameObject,
                        _range,
                        false))
                {
                    return false;
                }

                /*
                 * mod-auto-gather adds the player to the skill-up
                 * list immediately before UpdateGatherSkill().
                 */
                if (!gameObject->IsInSkillupList(
                        _player->GetGUID()))
                {
                    return false;
                }

                return IsGatherableNodeForSkill(
                    gameObject,
                    _player,
                    _skillId);
            }

        private:
            Player* _player;
            uint32 _skillId;
            float _range;
        };

        GameObject* FindAutoGatherNode(
            Player* player,
            uint32 skillId)
        {
            if (!player || !player->GetGroup())
                return nullptr;

            float searchRange = MaxDistance;

            if (searchRange < 1.0f)
                searchRange = 1.0f;

            std::list<GameObject*> nodes;

            AutoGatherNodeCheck check(
                player,
                skillId,
                searchRange);

            Acore::GameObjectListSearcher<
                AutoGatherNodeCheck> searcher(
                    player,
                    nodes,
                    check);

            Cell::VisitObjects(
                player,
                searcher,
                searchRange);

            if (nodes.empty())
                return nullptr;

            auto itr =
                std::min_element(
                    nodes.begin(),
                    nodes.end(),
                    [player](
                        GameObject* left,
                        GameObject* right)
                    {
                        return player->GetDistance(left) <
                               player->GetDistance(right);
                    });

            return itr != nodes.end()
                ? *itr
                : nullptr;
        }

        /*
         * Detect both:
         *   - normal AzerothCore skinning
         *   - mod-auto-gather AutoSkinCreature()
         */
        class SkinningCreatureCheck
        {
        public:
            SkinningCreatureCheck(
                Player* player,
                float range)
                : _player(player),
                  _range(range)
            {
            }

            bool operator()(
                Creature* creature)
            {
                if (!creature)
                    return false;

                if (creature->IsAlive())
                    return false;

                if (!creature->IsInWorld())
                    return false;

                if (!creature->IsInMap(_player))
                    return false;

                if (!creature->InSamePhase(_player))
                    return false;

                if (!_player->IsWithinDist(
                        creature,
                        _range,
                        false))
                {
                    return false;
                }

                if (creature->loot.loot_type !=
                    LOOT_SKINNING)
                {
                    return false;
                }

                CreatureTemplate const* creatureInfo =
                    creature->GetCreatureTemplate();

                if (!creatureInfo)
                    return false;

                if (!creatureInfo->SkinLootId)
                    return false;

                if (creatureInfo->GetRequiredLootSkill() !=
                    SKILL_SKINNING)
                {
                    return false;
                }

                /*
                 * Both normal Skinning and AutoSkinCreature()
                 * remove UNIT_FLAG_SKINNABLE after success.
                 */
                if (creature->HasUnitFlag(
                        UNIT_FLAG_SKINNABLE))
                {
                    return false;
                }

                if (!creature->isTappedBy(_player))
                    return false;

                return true;
            }

        private:
            Player* _player;
            float _range;
        };

        Creature* FindSkinningCreature(
            Player* player)
        {
            if (!player || !player->GetGroup())
                return nullptr;

            float searchRange = MaxDistance;

            if (searchRange < 1.0f)
                searchRange = 1.0f;

            std::list<Creature*> creatures;

            SkinningCreatureCheck check(
                player,
                searchRange);

            Acore::CreatureListSearcher<
                SkinningCreatureCheck> searcher(
                    player,
                    creatures,
                    check);

            Cell::VisitObjects(
                player,
                searcher,
                searchRange);

            if (creatures.empty())
                return nullptr;

            auto itr =
                std::min_element(
                    creatures.begin(),
                    creatures.end(),
                    [player](
                        Creature* left,
                        Creature* right)
                    {
                        return player->GetDistance(left) <
                               player->GetDistance(right);
                    });

            return itr != creatures.end()
                ? *itr
                : nullptr;
        }

        bool WasSkinningCreatureRecentlyProcessed(
            Player* player,
            Creature* creature)
        {
            if (!player || !creature)
                return true;

            uint64 playerKey =
                player->GetGUID().GetRawValue();

            auto itr =
                RecentSkinnings.find(playerKey);

            if (itr == RecentSkinnings.end())
                return false;

            if (IsRecentSkinningExpired(
                    itr->second))
            {
                RecentSkinnings.erase(itr);
                return false;
            }

            return itr->second.creature ==
                   creature->GetGUID();
        }

        void MarkSkinningCreatureProcessed(
            Player* player,
            Creature* creature)
        {
            if (!player || !creature)
                return;

            RecentSkinning recent;

            recent.gatherer =
                player->GetGUID();

            recent.creature =
                creature->GetGUID();

            recent.createdAt =
                GetMSTime();

            RecentSkinnings[
                player->GetGUID().GetRawValue()
            ] = recent;
        }
    }

    bool IsEnabled()
    {
        return Enabled;
    }

    bool IsProfessionEnabled(
        uint32 skillId)
    {
        if (!Enabled)
            return false;

        switch (skillId)
        {
            case SKILL_MINING:
                return MiningEnabled;

            case SKILL_HERBALISM:
                return HerbalismEnabled;

            case SKILL_SKINNING:
                return SkinningEnabled;

            default:
                return false;
        }
    }

    uint32 GetProfessionLootMultiplier(
        uint32 skillId)
    {
        if (!Enabled)
            return 0;

        return GetLootMultiplierInternal(skillId);
    }

    uint32 GetProfessionSkillMultiplier(
        uint32 skillId)
    {
        if (!Enabled)
            return 0;

        return GetSkillMultiplierInternal(skillId);
    }

    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject)
    {
        if (!gatherer || !gameObject)
            return;

        if (!gatherer->GetGroup())
            return;

        PendingGather pending;

        pending.gatherer =
            gatherer->GetGUID();

        pending.gameObject =
            gameObject->GetGUID();

        pending.createdAt =
            GetMSTime();

        PendingGathers[
            gatherer->GetGUID().GetRawValue()
        ] = pending;

        DebugLog(
            "Gathering operation queued.",
            gatherer,
            gameObject);
    }

    void RemovePendingGather(
        ObjectGuid playerGuid)
    {
        PendingGathers.erase(
            playerGuid.GetRawValue());
    }

    bool ProcessPendingGather(
        Player* gatherer,
        uint32 skillId)
    {
        if (!gatherer)
            return false;

        if (!IsGatheringSkill(skillId))
            return false;

        auto itr =
            PendingGathers.find(
                gatherer->GetGUID().GetRawValue());

        if (itr == PendingGathers.end())
            return false;

        PendingGather pending =
            itr->second;

        PendingGathers.erase(itr);

        if (IsPendingExpired(pending))
            return true;

        if (!IsProfessionEnabled(skillId))
            return true;

        GameObject* gameObject =
            ObjectAccessor::GetGameObject(
                *gatherer,
                pending.gameObject);

        if (!gameObject)
            return true;

        /*
         * Normal AzerothCore EffectOpenLock() adds the node to the
         * skill-up list before calling UpdateGatherSkill().
         */
        if (!gameObject->IsInSkillupList(
                gatherer->GetGUID()))
        {
            return true;
        }

        DistributeGatherLoot(
            gatherer,
            gameObject,
            skillId);

        return true;
    }

    void ProcessAutoGather(
        Player* gatherer,
        uint32 skillId)
    {
        if (!gatherer)
            return;

        if (!IsGatheringSkill(skillId))
            return;

        if (!IsProfessionEnabled(skillId))
            return;

        if (!gatherer->GetGroup())
            return;

        GameObject* gameObject =
            FindAutoGatherNode(
                gatherer,
                skillId);

        if (!gameObject)
            return;

        /*
         * Do not touch the GameObject lifecycle.
         * mod-auto-gather owns it.
         */
        DistributeGatherLoot(
            gatherer,
            gameObject,
            skillId);
    }

    bool ProcessSkinning(
        Player* skinner)
    {
        if (!skinner)
            return false;

        if (!IsEnabled())
            return false;

        if (!IsProfessionEnabled(
                SKILL_SKINNING))
        {
            return false;
        }

        if (!skinner->GetGroup())
            return false;

        Creature* creature =
            FindSkinningCreature(
                skinner);

        if (!creature)
            return false;

        if (WasSkinningCreatureRecentlyProcessed(
                skinner,
                creature))
        {
            return true;
        }

        MarkSkinningCreatureProcessed(
            skinner,
            creature);

        DistributeSkinningLoot(
            skinner,
            creature);

        return true;
    }

    void RemoveRecentSkinning(
        ObjectGuid playerGuid)
    {
        RecentSkinnings.erase(
            playerGuid.GetRawValue());
    }

    PlayerScript::PlayerScript()
        : ::PlayerScript(
            "ProfessionLootParty_PlayerScript")
    {
    }

    void PlayerScript::OnPlayerUpdateGatheringSkill(
        Player* player,
        uint32 skillId,
        uint32 /*currentLevel*/,
        uint32 /*gray*/,
        uint32 /*green*/,
        uint32 /*yellow*/,
        uint32& /*gain*/)
    {
        if (!player || !IsEnabled())
            return;

        /*
         * Critical re-entrancy protection.
         *
         * SimulateGatherSkillAttempt() deliberately calls
         * UpdateGatherSkill(), which calls this hook again.
         *
         * The nested invocation must NOT be interpreted as another
         * real gathering operation.
         */
        if (IsSimulatedSkillUpdate(player))
            return;

        if (IsSkinningSkill(skillId))
        {
            ProcessSkinning(player);
            return;
        }

        if (!IsGatheringSkill(skillId))
            return;

        /*
         * First attempt to match normal AzerothCore gathering.
         */
        if (ProcessPendingGather(
                player,
                skillId))
        {
            return;
        }

        /*
         * Otherwise this can be mod-auto-gather.
         */
        ProcessAutoGather(
            player,
            skillId);
    }

    void PlayerScript::OnPlayerLogout(
        Player* player)
    {
        if (!player)
            return;

        RemovePendingGather(
            player->GetGUID());

        RemoveRecentSkinning(
            player->GetGUID());

        SimulatedSkillUpdates.erase(
            player->GetGUID().GetRawValue());
    }

    GameObjectScript::GameObjectScript()
        : ::AllGameObjectScript(
            "ProfessionLootParty_GameObjectScript")
    {
    }

    void GameObjectScript::OnGameObjectLootStateChanged(
        GameObject* gameObject,
        uint32 state,
        Unit* unit)
    {
        if (!IsEnabled())
            return;

        if (!gameObject || !unit)
            return;

        if (!unit->IsPlayer())
            return;

        if (state != GO_ACTIVATED)
            return;

        Player* gatherer =
            unit->ToPlayer();

        if (!gatherer)
            return;

        if (!gatherer->GetGroup())
            return;

        /*
         * Do not guess Mining vs Herbalism here.
         *
         * AzerothCore supplies the actual SkillType through
         * OnPlayerUpdateGatheringSkill().
         */
        AddPendingGather(
            gatherer,
            gameObject);
    }

    ConfigScript::ConfigScript()
        : ::WorldScript(
            "ProfessionLootParty_ConfigScript")
    {
    }

    void ConfigScript::OnBeforeConfigLoad(
        bool /*reload*/)
    {
        Enabled =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.Enable",
                true);

        MiningEnabled =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.Mining",
                true);

        HerbalismEnabled =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.Herbalism",
                true);

        SkinningEnabled =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.Skinning",
                true);

        /*
         * New independent loot multipliers.
         *
         * Old ProfessionLootParty.*Multiplier settings are accepted
         * as a backwards-compatible fallback for loot only.
         */
        MiningLootMultiplier =
            sConfigMgr->GetOption<uint32>(
                "ProfessionLootParty.MiningLootMultiplier",
                sConfigMgr->GetOption<uint32>(
                    "ProfessionLootParty.MiningMultiplier",
                    1));

        HerbalismLootMultiplier =
            sConfigMgr->GetOption<uint32>(
                "ProfessionLootParty.HerbalismLootMultiplier",
                sConfigMgr->GetOption<uint32>(
                    "ProfessionLootParty.HerbalismMultiplier",
                    1));

        SkinningLootMultiplier =
            sConfigMgr->GetOption<uint32>(
                "ProfessionLootParty.SkinningLootMultiplier",
                sConfigMgr->GetOption<uint32>(
                    "ProfessionLootParty.SkinningMultiplier",
                    1));

        /*
         * Independent skill-up multipliers.
         */
        MiningSkillMultiplier =
            sConfigMgr->GetOption<uint32>(
                "ProfessionLootParty.MiningSkillMultiplier",
                1);

        HerbalismSkillMultiplier =
            sConfigMgr->GetOption<uint32>(
                "ProfessionLootParty.HerbalismSkillMultiplier",
                1);

        SkinningSkillMultiplier =
            sConfigMgr->GetOption<uint32>(
                "ProfessionLootParty.SkinningSkillMultiplier",
                1);

        /*
         * Keep both multipliers >= 1.
         *
         * The module's basic behavior always gives eligible
         * profession users one base roll/attempt.
         */
        MiningLootMultiplier =
            std::max<uint32>(
                1,
                std::min(
                    MiningLootMultiplier,
                    MAX_PROFESSION_MULTIPLIER));

        HerbalismLootMultiplier =
            std::max<uint32>(
                1,
                std::min(
                    HerbalismLootMultiplier,
                    MAX_PROFESSION_MULTIPLIER));

        SkinningLootMultiplier =
            std::max<uint32>(
                1,
                std::min(
                    SkinningLootMultiplier,
                    MAX_PROFESSION_MULTIPLIER));

        MiningSkillMultiplier =
            std::max<uint32>(
                1,
                std::min(
                    MiningSkillMultiplier,
                    MAX_PROFESSION_MULTIPLIER));

        HerbalismSkillMultiplier =
            std::max<uint32>(
                1,
                std::min(
                    HerbalismSkillMultiplier,
                    MAX_PROFESSION_MULTIPLIER));

        SkinningSkillMultiplier =
            std::max<uint32>(
                1,
                std::min(
                    SkinningSkillMultiplier,
                    MAX_PROFESSION_MULTIPLIER));

        IncludeRaid =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.Raid",
                true);

        RequireSkill =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.RequireSkill",
                true);

        MaxDistance =
            sConfigMgr->GetOption<float>(
                "ProfessionLootParty.Distance",
                100.0f);

        Debug =
            sConfigMgr->GetOption<bool>(
                "ProfessionLootParty.Debug",
                false);

        if (MaxDistance < 0.0f)
            MaxDistance = 0.0f;

        LOG_INFO(
            "server.loading",
            ">> ProfessionLootParty: {}",
            Enabled ? "Enabled" : "Disabled");

        LOG_INFO(
            "server.loading",
            "   Mining: {} Loot={}x Skill={}x",
            MiningEnabled,
            MiningLootMultiplier,
            MiningSkillMultiplier);

        LOG_INFO(
            "server.loading",
            "   Herbalism: {} Loot={}x Skill={}x",
            HerbalismEnabled,
            HerbalismLootMultiplier,
            HerbalismSkillMultiplier);

        LOG_INFO(
            "server.loading",
            "   Skinning: {} Loot={}x Skill={}x",
            SkinningEnabled,
            SkinningLootMultiplier,
            SkinningSkillMultiplier);

        LOG_INFO(
            "server.loading",
            "   Raid: {}",
            IncludeRaid);

        LOG_INFO(
            "server.loading",
            "   RequireSkill: {}",
            RequireSkill);

        LOG_INFO(
            "server.loading",
            "   Distance: {}",
            MaxDistance);

        LOG_INFO(
            "server.loading",
            "   AutoGather compatibility: enabled");

        LOG_INFO(
            "server.loading",
            "   Skinning compatibility: normal + AutoGather");
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