/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Group profession loot with independent loot rolls.
 *
 * Supports:
 *
 *   - Normal AzerothCore Mining
 *   - Normal AzerothCore Herbalism
 *   - Normal AzerothCore Skinning
 *   - mod-auto-gather Mining
 *   - mod-auto-gather Herbalism
 *   - mod-auto-gather Skinning
 *
 * Multiplier semantics:
 *
 *   MiningMultiplier = 1
 *       Gatherer    -> normal core loot
 *       Other       -> 1 independent roll
 *
 *   MiningMultiplier = 3
 *       Gatherer    -> normal core loot + 2 independent rolls
 *       Other       -> 3 independent rolls
 *
 * The configured multiplier represents the TOTAL number of
 * profession loot rolls each eligible profession user receives.
 *
 * The original AzerothCore/AutoGather operation is never replaced
 * or modified. Additional rolls are generated independently through
 * AutoStoreLoot().
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

namespace ProfessionLootParty
{
    namespace
    {
        bool Enabled = true;

        bool MiningEnabled = true;
        bool HerbalismEnabled = true;
        bool SkinningEnabled = true;

        /*
         * TOTAL number of rolls per profession user.
         *
         * 1 = current behavior
         * 2 = two total rolls
         * 3 = three total rolls
         */
        uint32 MiningMultiplier = 1;
        uint32 HerbalismMultiplier = 1;
        uint32 SkinningMultiplier = 1;

        /*
         * Safety limit preventing accidental enormous numbers
         * of loot-generation calls.
         */
        constexpr uint32 MAX_PROFESSION_MULTIPLIER = 100;

        bool IncludeRaid = true;
        bool RequireSkill = true;
        bool Debug = false;

        float MaxDistance = 100.0f;

        /*
         * Normal AzerothCore Mining/Herbalism:
         *
         * EffectOpenLock() changes the GameObject loot state to
         * GO_ACTIVATED before UpdateGatherSkill() is called.
         */
        constexpr uint32 PENDING_TIMEOUT_MS = 5000;

        /*
         * Skinning has no equivalent GameObject callback.
         *
         * This timeout is used only for duplicate/stale corpse
         * protection.
         */
        constexpr uint32 SKINNING_RECENT_TIMEOUT_MS = 60000;

        std::unordered_map<uint64, PendingGather> PendingGathers;

        std::unordered_map<uint64, RecentSkinning> RecentSkinnings;

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
            uint32 now = GetMSTime();

            return uint32(now - pending.createdAt) >
                   PENDING_TIMEOUT_MS;
        }

        bool IsRecentSkinningExpired(
            RecentSkinning const& recent)
        {
            uint32 now = GetMSTime();

            return uint32(now - recent.createdAt) >
                   SKINNING_RECENT_TIMEOUT_MS;
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
            if (!player)
                return false;

            return player->HasSkill(skillId);
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
             * The gatherer is handled separately.
             *
             * They already received the normal AzerothCore/
             * AutoGather loot.
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

            /*
             * The actual skinner is handled separately.
             *
             * They already received the normal skinning loot.
             */
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
         * Generate ONE independent Mining/Herbalism roll.
         */
        void GiveIndependentGatherRoll(
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
            {
                DebugLog(
                    "GameObject has no loot ID; skipping independent gathering roll.",
                    player,
                    gameObject);

                return;
            }

            /*
             * AutoStoreLoot() generates a NEW result from the
             * GameObject loot template.
             *
             * It does not copy the gatherer's loot result.
             */
            player->AutoStoreLoot(
                lootId,
                LootTemplates_Gameobject,
                true);

            DebugLog(
                "Independent profession gathering roll awarded.",
                player,
                gameObject);
        }

        /*
         * Generate ONE independent Skinning roll.
         */
        void GiveIndependentSkinningRoll(
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
            {
                DebugLog(
                    "Creature has no SkinLootId; skipping independent skinning roll.",
                    player,
                    nullptr,
                    creature);

                return;
            }

            if (!LootTemplates_Skinning.HaveLootFor(lootId))
            {
                DebugLog(
                    "SkinLootId has no skinning loot template; skipping roll.",
                    player,
                    nullptr,
                    creature);

                return;
            }

            /*
             * Every call generates a fresh independent roll.
             */
            player->AutoStoreLoot(
                lootId,
                LootTemplates_Skinning,
                true);

            DebugLog(
                "Independent skinning roll awarded.",
                player,
                nullptr,
                creature);
        }

        /*
         * Distribute successful Mining/Herbalism.
         *
         * The original gatherer's normal loot has already happened.
         *
         * Therefore:
         *
         *   multiplier = 1
         *       gatherer -> 0 additional rolls
         *
         *   multiplier = 3
         *       gatherer -> 2 additional rolls
         *
         * Other eligible group members receive the FULL multiplier.
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

            uint32 multiplier =
                GetProfessionMultiplier(skillId);

            DebugLog(
                "Confirmed successful profession gathering.",
                gatherer,
                gameObject);

            /*
             * The gatherer already received one normal roll from
             * AzerothCore/mod-auto-gather.
             *
             * Add multiplier - 1 fresh rolls.
             */
            if (multiplier > 1)
            {
                uint32 additionalRolls =
                    multiplier - 1;

                DebugLog(
                    "Giving gatherer additional profession rolls.",
                    gatherer,
                    gameObject);

                for (uint32 roll = 0;
                     roll < additionalRolls;
                     ++roll)
                {
                    GiveIndependentGatherRoll(
                        gatherer,
                        gameObject);
                }
            }

            /*
             * Every other eligible profession user receives the
             * COMPLETE configured number of independent rolls.
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

                for (uint32 roll = 0;
                     roll < multiplier;
                     ++roll)
                {
                    GiveIndependentGatherRoll(
                        member,
                        gameObject);
                }
            }

            DebugLog(
                "Profession group gathering loot processing completed.",
                gatherer,
                gameObject);
        }

        /*
         * Distribute successful Skinning.
         *
         * The original skinner already received their normal
         * skinning loot.
         *
         * Therefore:
         *
         *   multiplier = 1
         *       skinner -> normal loot only
         *
         *   multiplier = 3
         *       skinner -> normal loot + 2 independent rolls
         *
         * Other eligible skinners receive 3 independent rolls.
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

            uint32 multiplier =
                GetProfessionMultiplier(SKILL_SKINNING);

            DebugLog(
                "Confirmed successful profession skinning.",
                skinner,
                nullptr,
                creature);

            /*
             * The original skinner already received their normal
             * skinning loot.
             */
            if (multiplier > 1)
            {
                uint32 additionalRolls =
                    multiplier - 1;

                for (uint32 roll = 0;
                     roll < additionalRolls;
                     ++roll)
                {
                    GiveIndependentSkinningRoll(
                        skinner,
                        creature);
                }
            }

            /*
             * Other eligible Skinning users receive the full
             * configured multiplier.
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
                     roll < multiplier;
                     ++roll)
                {
                    GiveIndependentSkinningRoll(
                        member,
                        creature);
                }
            }

            DebugLog(
                "Profession group skinning loot processing completed.",
                skinner,
                nullptr,
                creature);
        }

        /*
         * Resolve whether a GameObject is a Mining/Herbalism
         * gathering node for the specified skill.
         *
         * This follows the lock data rather than guessing from
         * the GameObject entry.
         */
        bool IsGatherableNodeForSkill(
            GameObject* gameObject,
            Player* player,
            uint32 skillId)
        {
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

            uint32 lockId =
                goInfo->GetLockId();

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

                if (resolvedSkill == SKILL_NONE)
                    continue;

                if (resolvedSkill != skillId)
                    continue;

                if (player->GetSkillValue(
                        resolvedSkill) <
                    lockEntry->Skill[i])
                {
                    continue;
                }

                return true;
            }

            return false;
        }

        /*
         * Search for the exact GameObject that mod-auto-gather
         * has just processed.
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

                /*
                 * mod-auto-gather calls UpdateGatherSkill()
                 * before setting the node to GO_JUST_DEACTIVATED.
                 */
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
                 * AutoGather adds the player to the skill-up list
                 * immediately before UpdateGatherSkill().
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
            if (!player)
                return nullptr;

            if (!player->GetGroup())
                return nullptr;

            float searchRange =
                MaxDistance;

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

            /*
             * Prefer the closest matching node.
             */
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
         * Check whether a creature currently represents a
         * successful Skinning operation.
         *
         * Both normal AzerothCore Skinning and
         * mod-auto-gather AutoSkinCreature() end up with:
         *
         *   - dead creature
         *   - SkinLootId
         *   - LOOT_SKINNING
         *   - SKINNABLE flag removed
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

                /*
                 * Only actual skinning loot states are accepted.
                 */
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
                 * Successful skinning removes this flag.
                 *
                 * This applies to both normal EffectSkinning()
                 * and mod-auto-gather AutoSkinCreature().
                 */
                if (creature->HasUnitFlag(
                        UNIT_FLAG_SKINNABLE))
                {
                    return false;
                }

                /*
                 * Restrict the event to a corpse tapped by the
                 * actual skinner.
                 */
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
            if (!player)
                return nullptr;

            if (!player->GetGroup())
                return nullptr;

            float searchRange =
                MaxDistance;

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

            /*
             * Prefer the closest valid corpse.
             */
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

        void RemoveRecentSkinningInternal(
            ObjectGuid playerGuid)
        {
            RecentSkinnings.erase(
                playerGuid.GetRawValue());
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

    uint32 GetProfessionMultiplier(
        uint32 skillId)
    {
        if (!Enabled)
            return 0;

        switch (skillId)
        {
            case SKILL_MINING:
                return MiningMultiplier;

            case SKILL_HERBALISM:
                return HerbalismMultiplier;

            case SKILL_SKINNING:
                return SkinningMultiplier;

            default:
                return 0;
        }
    }

    /*
     * Normal AzerothCore Mining/Herbalism.
     */
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

    /*
     * Normal AzerothCore Mining/Herbalism.
     */
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

        /*
         * Consume immediately.
         *
         * This prevents ProcessAutoGather() from processing
         * the same operation a second time.
         */
        PendingGathers.erase(itr);

        if (IsPendingExpired(pending))
        {
            DebugLog(
                "Pending gathering operation expired.",
                gatherer);

            return true;
        }

        if (!IsProfessionEnabled(skillId))
            return true;

        GameObject* gameObject =
            ObjectAccessor::GetGameObject(
                *gatherer,
                pending.gameObject);

        if (!gameObject)
        {
            DebugLog(
                "Pending gathering GameObject no longer exists.",
                gatherer);

            return true;
        }

        /*
         * EffectOpenLock() adds the GameObject to the skill-up
         * list before UpdateGatherSkill().
         */
        if (!gameObject->IsInSkillupList(
                gatherer->GetGUID()))
        {
            DebugLog(
                "Gathering skill update did not match pending GameObject.",
                gatherer,
                gameObject);

            return true;
        }

        DistributeGatherLoot(
            gatherer,
            gameObject,
            skillId);

        return true;
    }

    /*
     * mod-auto-gather Mining/Herbalism.
     */
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

        /*
         * This is intentionally performed only when no normal
         * GO_ACTIVATED pending operation exists.
         */
        GameObject* gameObject =
            FindAutoGatherNode(
                gatherer,
                skillId);

        if (!gameObject)
            return;

        DebugLog(
            "Detected mod-auto-gather operation.",
            gatherer,
            gameObject);

        /*
         * Do NOT change the GameObject state here.
         *
         * mod-auto-gather owns the GameObject lifecycle.
         */
        DistributeGatherLoot(
            gatherer,
            gameObject,
            skillId);
    }

    /*
     * Skinning support.
     *
     * Normal AzerothCore:
     *
     *   EffectSkinning()
     *       -> LOOT_SKINNING
     *       -> UpdateGatherSkill(SKILL_SKINNING)
     *
     * mod-auto-gather:
     *
     *   AutoSkinCreature()
     *       -> loot_type = LOOT_SKINNING
     *       -> clears normal corpse loot
     *       -> UpdateGatherSkill(SKILL_SKINNING)
     *
     * Therefore the same PlayerScript hook supports both.
     */
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
        {
            DebugLog(
                "No matching skinned corpse found.",
                skinner);

            return false;
        }

        /*
         * Prevent duplicate processing of the same corpse for
         * the same skinner.
         */
        if (WasSkinningCreatureRecentlyProcessed(
                skinner,
                creature))
        {
            DebugLog(
                "Skinning corpse was already processed.",
                skinner,
                nullptr,
                creature);

            return true;
        }

        /*
         * Mark before distributing.
         */
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
        RemoveRecentSkinningInternal(
            playerGuid);
    }

    /*
     * PlayerScript
     */
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
         * Skinning is deliberately handled separately.
         *
         * Do not add SKILL_SKINNING to IsGatheringSkill(),
         * because Mining/Herbalism AutoGather detection is
         * GameObject-based.
         */
        if (IsSkinningSkill(skillId))
        {
            ProcessSkinning(player);
            return;
        }

        if (!IsGatheringSkill(skillId))
            return;

        /*
         * First handle normal AzerothCore Mining/Herbalism.
         */
        if (ProcessPendingGather(
                player,
                skillId))
        {
            return;
        }

        /*
         * No normal pending operation means this may be
         * mod-auto-gather.
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
    }

    /*
     * GameObjectScript
     *
     * Normal AzerothCore Mining/Herbalism reaches GO_ACTIVATED here.
     *
     * mod-auto-gather does not use GO_ACTIVATED, which is why the
     * PlayerScript fallback exists above.
     *
     * Skinning does not use this callback.
     */
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
         * Do not determine Mining vs Herbalism here.
         *
         * AzerothCore determines the actual skill and sends it
         * through OnPlayerUpdateGatheringSkill().
         */
        AddPendingGather(
            gatherer,
            gameObject);
    }

    /*
     * Configuration
     */
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

        MiningMultiplier =
            std::min(
                sConfigMgr->GetOption<uint32>(
                    "ProfessionLootParty.MiningMultiplier",
                    1),
                MAX_PROFESSION_MULTIPLIER);

        HerbalismMultiplier =
            std::min(
                sConfigMgr->GetOption<uint32>(
                    "ProfessionLootParty.HerbalismMultiplier",
                    1),
                MAX_PROFESSION_MULTIPLIER);

        SkinningMultiplier =
            std::min(
                sConfigMgr->GetOption<uint32>(
                    "ProfessionLootParty.SkinningMultiplier",
                    1),
                MAX_PROFESSION_MULTIPLIER);

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
            "   Mining: {} ({}x)",
            MiningEnabled,
            MiningMultiplier);

        LOG_INFO(
            "server.loading",
            "   Herbalism: {} ({}x)",
            HerbalismEnabled,
            HerbalismMultiplier);

        LOG_INFO(
            "server.loading",
            "   Skinning: {} ({}x)",
            SkinningEnabled,
            SkinningMultiplier);

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
 *
 * IMPORTANT:
 *
 * The exact function name must match the symbol generated/referenced
 * by the module loader.
 */
void Addmod_profession_loot_partyScripts()
{
    new ProfessionLootParty::ConfigScript();
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}