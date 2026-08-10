/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Group profession loot with independent loot rolls.
 *
 * Supports:
 *
 *   - Normal AzerothCore Mining gathering
 *   - Normal AzerothCore Herbalism gathering
 *   - mod-auto-gather Mining
 *   - mod-auto-gather Herbalism
 *   - mod-auto-gather Skinning
 *
 * The original gatherer keeps their normal loot.
 * Other eligible group members receive an independent roll.
 *
 * Skinning support intentionally targets mod-auto-gather's
 * AutoSkinCreature() behavior and does not modify or hook the
 * normal AzerothCore Skinning spell path.
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

        /*
         * Skinning is disabled by default to preserve the current
         * module behavior/configuration.
         *
         * Enable with:
         *
         * ProfessionLootParty.Skinning = 1
         */
        bool SkinningEnabled = false;

        bool IncludeRaid = true;
        bool RequireSkill = true;
        bool Debug = false;

        float MaxDistance = 100.0f;

        /*
         * Normal AzerothCore gathering:
         *
         * EffectOpenLock() changes the GameObject loot state to
         * GO_ACTIVATED before UpdateGatherSkill() is called.
         *
         * We temporarily remember the GameObject and consume it
         * when the gathering skill update arrives.
         */
        constexpr uint32 PENDING_TIMEOUT_MS = 5000;

        /*
         * Prevent duplicate Skinning processing if the same skill
         * update is somehow delivered more than once.
         */
        constexpr uint32 SKINNING_DUPLICATE_TIMEOUT_MS = 5000;

        std::unordered_map<uint64, PendingGather> PendingGathers;

        std::unordered_map<uint64, RecentSkinning> RecentSkinnings;

        uint32 GetMSTime()
        {
            using namespace std::chrono;

            return static_cast<uint32>(
                duration_cast<milliseconds>(
                    steady_clock::now().time_since_epoch()).count());
        }

        bool IsPendingExpired(PendingGather const& pending)
        {
            uint32 now = GetMSTime();

            return uint32(now - pending.createdAt) >
                   PENDING_TIMEOUT_MS;
        }

        bool IsRecentSkinningExpired(RecentSkinning const& recent)
        {
            uint32 now = GetMSTime();

            return uint32(now - recent.createdAt) >
                   SKINNING_DUPLICATE_TIMEOUT_MS;
        }

        void DebugLog(
            char const* message,
            Player* player = nullptr,
            GameObject* gameObject = nullptr)
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
            }
            else if (player)
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {} Player={}",
                    message,
                    player->GetName());
            }
            else
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {}",
                    message);
            }
        }

        void DebugSkinningLog(
            char const* message,
            Player* player = nullptr,
            Creature* creature = nullptr)
        {
            if (!Debug)
                return;

            if (player && creature)
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {} Player={} Creature={} Entry={}",
                    message,
                    player->GetName(),
                    creature->GetGUID().ToString(),
                    creature->GetEntry());
            }
            else if (player)
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {} Player={}",
                    message,
                    player->GetName());
            }
            else
            {
                LOG_DEBUG(
                    "module",
                    "[ProfessionLootParty] {}",
                    message);
            }
        }

        /*
         * Gathering professions handled by this module.
         *
         * Skinning is handled only by ProcessAutoSkinning().
         */
        bool IsGatheringSkill(uint32 skillId)
        {
            return skillId == SKILL_MINING ||
                   skillId == SKILL_HERBALISM ||
                   skillId == SKILL_SKINNING;
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
            Player* gatherer,
            Player* member)
        {
            if (!gatherer || !member)
                return false;

            Group* group = gatherer->GetGroup();

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

        /*
         * Existing Mining/Herbalism eligibility.
         *
         * This is intentionally kept equivalent to the existing
         * GameObject implementation.
         */
        bool IsEligibleMember(
            Player* gatherer,
            Player* member,
            GameObject* gameObject,
            uint32 skillId)
        {
            if (!gatherer || !member || !gameObject)
                return false;

            /*
             * The original gatherer already received their normal
             * AzerothCore/AutoGather loot.
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

        /*
         * Skinning-specific eligibility.
         *
         * Skinning recipients receive loot only.
         * They do NOT receive a Skinning skill-up.
         */
        bool IsEligibleSkinner(
            Player* gatherer,
            Player* member,
            Creature* creature)
        {
            if (!gatherer || !member || !creature)
                return false;

            /*
             * The original skinner already received the normal
             * mod-auto-gather Skinning loot.
             */
            if (gatherer->GetGUID() == member->GetGUID())
                return false;

            if (!member->IsInWorld())
                return false;

            if (!member->IsAlive())
                return false;

            if (!IsSameGroup(gatherer, member))
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
         * Existing independent GameObject roll.
         *
         * DO NOT change this to LootTemplates_Skinning.
         *
         * Mining/Herbalism continue to use the GameObject loot table.
         */
        void GiveIndependentRoll(
            Player* player,
            GameObject* gameObject)
        {
            if (!player || !gameObject)
                return;

            GameObjectTemplate const* goInfo =
                gameObject->GetGOInfo();

            if (!goInfo)
                return;

            uint32 lootId = goInfo->GetLootId();

            if (!lootId)
            {
                DebugLog(
                    "GameObject has no loot ID; skipping independent roll.",
                    player,
                    gameObject);

                return;
            }

            /*
             * AutoStoreLoot() creates a completely new loot result
             * from the same GameObject loot template.
             *
             * This is deliberately NOT a copy of the gatherer's loot.
             */
            player->AutoStoreLoot(
                lootId,
                LootTemplates_Gameobject,
                true);

            DebugLog(
                "Independent profession loot roll awarded.",
                player,
                gameObject);
        }

        /*
         * NEW: independent Skinning roll.
         *
         * This uses the exact same SkinLootId and
         * LootTemplates_Skinning path used by mod-auto-gather.
         *
         * Every recipient gets a NEW roll.
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

            uint32 skinLootId =
                creatureInfo->SkinLootId;

            if (!skinLootId)
            {
                DebugSkinningLog(
                    "Creature has no SkinLootId; skipping independent Skinning roll.",
                    player,
                    creature);

                return;
            }

            /*
             * Generate a fresh Skinning loot result.
             *
             * This does NOT use the original skinner's loot.
             */
            player->AutoStoreLoot(
                skinLootId,
                LootTemplates_Skinning,
                true);

            DebugSkinningLog(
                "Independent Skinning loot roll awarded.",
                player,
                creature);
        }

        /*
         * Existing Mining/Herbalism distribution.
         *
         * Left separate from Skinning so the existing GameObject
         * gathering behavior remains untouched.
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

            Group* group = gatherer->GetGroup();

            if (!group)
                return;

            DebugLog(
                "Confirmed successful profession gathering.",
                gatherer,
                gameObject);

            for (GroupReference* groupRef =
                     group->GetFirstMember();
                 groupRef != nullptr;
                 groupRef = groupRef->next())
            {
                Player* member =
                    groupRef->GetSource();

                if (!member)
                    continue;

                if (!IsEligibleMember(
                        gatherer,
                        member,
                        gameObject,
                        skillId))
                {
                    continue;
                }

                GiveIndependentRoll(
                    member,
                    gameObject);
            }

            DebugLog(
                "Profession group loot processing completed.",
                gatherer,
                gameObject);
        }

        /*
         * NEW: Skinning distribution.
         */
        void DistributeSkinningLoot(
            Player* gatherer,
            Creature* creature)
        {
            if (!gatherer || !creature)
                return;

            if (!IsProfessionEnabled(SKILL_SKINNING))
                return;

            Group* group = gatherer->GetGroup();

            if (!group)
                return;

            DebugSkinningLog(
                "Confirmed successful mod-auto-gather Skinning operation.",
                gatherer,
                creature);

            for (GroupReference* groupRef =
                     group->GetFirstMember();
                 groupRef != nullptr;
                 groupRef = groupRef->next())
            {
                Player* member =
                    groupRef->GetSource();

                if (!member)
                    continue;

                if (!IsEligibleSkinner(
                        gatherer,
                        member,
                        creature))
                {
                    continue;
                }

                GiveIndependentSkinningRoll(
                    member,
                    creature);
            }

            DebugSkinningLog(
                "Skinning group loot processing completed.",
                gatherer,
                creature);
        }

        /*
         * Resolve the gathering profession represented by a GameObject.
         *
         * Existing Mining/Herbalism implementation.
         */
        bool IsGatherableNodeForSkill(
            GameObject* gameObject,
            Player* player,
            uint32 skillId)
        {
            if (!gameObject || !player)
                return false;

            if (gameObject->GetGoType() != GAMEOBJECT_TYPE_CHEST)
                return false;

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

            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            {
                if (lockEntry->Type[i] != LOCK_KEY_SKILL)
                    continue;

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

                if (player->GetSkillValue(resolvedSkill) <
                    lockEntry->Skill[i])
                {
                    continue;
                }

                return true;
            }

            return false;
        }

        /*
         * Existing mod-auto-gather GameObject detection.
         *
         * This is unchanged.
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

            bool operator()(GameObject* gameObject)
            {
                if (!gameObject)
                    return false;

                if (gameObject->getLootState() != GO_READY)
                    return false;

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

            auto itr = std::min_element(
                nodes.begin(),
                nodes.end(),
                [player](GameObject* left,
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
         * NEW:
         *
         * Validate that the selected Creature matches the state left
         * by mod-auto-gather's AutoSkinCreature().
         *
         * AutoSkinCreature() does:
         *
         *   RemoveUnitFlag(UNIT_FLAG_SKINNABLE)
         *   creature->loot.clear()
         *   creature->loot.loot_type = LOOT_SKINNING
         *   RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE)
         *   UpdateGatherSkill(...)
         *
         * Therefore all of these checks are intentionally required.
         *
         * Requiring an empty loot container is particularly important:
         * it prevents this module from treating a normal, still-open
         * Skinning loot operation as an AutoGather operation.
         */
        bool IsAutoGatherSkinnedCreature(
            Player* player,
            Creature* creature)
        {
            if (!player || !creature)
                return false;

            if (creature->IsAlive())
                return false;

            if (!creature->IsInMap(player))
                return false;

            if (!creature->InSamePhase(player))
                return false;

            if (creature->GetMapId() != player->GetMapId())
                return false;

            if (player->GetDistance(creature) >
                MaxDistance)
            {
                return false;
            }

            CreatureTemplate const* creatureInfo =
                creature->GetCreatureTemplate();

            if (!creatureInfo)
                return false;

            if (!creatureInfo->SkinLootId)
                return false;

            /*
             * We only handle actual Skinning here.
             *
             * Special creatures using SKILL_MINING or SKILL_HERBALISM
             * as their required loot skill remain outside this path.
             */
            if (creatureInfo->GetRequiredLootSkill() !=
                SKILL_SKINNING)
            {
                return false;
            }

            /*
             * AutoSkinCreature() removes the skinnable flag.
             */
            if (creature->HasUnitFlag(
                    UNIT_FLAG_SKINNABLE))
            {
                return false;
            }

            /*
             * AutoSkinCreature() removes the lootable dynamic flag.
             */
            if (creature->HasDynamicFlag(
                    UNIT_DYNFLAG_LOOTABLE))
            {
                return false;
            }

            /*
             * AutoSkinCreature() clears the creature's loot before
             * calling UpdateGatherSkill().
             */
            if (!creature->loot.empty())
            {
                return false;
            }

            /*
             * AutoSkinCreature() explicitly changes the loot type.
             */
            if (creature->loot.loot_type !=
                LOOT_SKINNING)
            {
                return false;
            }

            return true;
        }

        /*
         * Return the Creature currently selected by the player.
         *
         * mod-auto-gather's AutoSkinCreature() operates on a concrete
         * Creature target, so using the selected unit prevents us from
         * guessing among multiple nearby corpses.
         */
        Creature* GetSelectedSkinningCreature(
            Player* player)
        {
            if (!player)
                return nullptr;

            Unit* selected =
                player->GetSelectedUnit();

            if (!selected)
                return nullptr;

            Creature* creature =
                selected->ToCreature();

            if (!creature)
                return nullptr;

            if (!IsAutoGatherSkinnedCreature(
                    player,
                    creature))
            {
                return nullptr;
            }

            return creature;
        }

        /*
         * Prevent duplicate processing of the same Skinning operation.
         */
        bool WasRecentlyProcessed(
            Player* player,
            Creature* creature)
        {
            if (!player || !creature)
                return false;

            uint64 key =
                player->GetGUID().GetRawValue();

            auto itr =
                RecentSkinnings.find(key);

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

        void MarkSkinningProcessed(
            Player* player,
            Creature* creature)
        {
            if (!player || !creature)
                return;

            RecentSkinning recent;

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

    bool IsProfessionEnabled(uint32 skillId)
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

    /*
     * Normal AzerothCore gathering.
     *
     * UNCHANGED BEHAVIOR.
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

    void RemoveRecentSkinning(
        ObjectGuid playerGuid)
    {
        RecentSkinnings.erase(
            playerGuid.GetRawValue());
    }

    /*
     * Existing normal gathering processing.
     *
     * UNCHANGED BEHAVIOR.
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
         * This is important because ProcessAutoGather() must NOT
         * process the same normal gathering operation again.
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
         * EffectOpenLock() adds the GameObject to the skill-up list
         * before UpdateGatherSkill().
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
     * Existing mod-auto-gather Mining/Herbalism processing.
     *
     * UNCHANGED BEHAVIOR.
     */
    void ProcessAutoGather(
        Player* gatherer,
        uint32 skillId)
    {
        if (!gatherer)
            return;

        if (skillId != SKILL_MINING &&
            skillId != SKILL_HERBALISM)
        {
            return;
        }

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
         * Do NOT call SetLootState() here.
         *
         * mod-auto-gather owns the GameObject lifecycle and will
         * perform SetLootState(GO_JUST_DEACTIVATED) immediately after
         * UpdateGatherSkill() returns.
         */
        DistributeGatherLoot(
            gatherer,
            gameObject,
            skillId);
    }

    /*
     * NEW: mod-auto-gather Skinning.
     *
     * This function intentionally does not call the normal Skinning
     * spell path.
     *
     * It runs only after the gathering skill hook reports SKILL_SKINNING
     * and looks for the selected Creature in the exact post-AutoSkin
     * state.
     */
    void ProcessAutoSkinning(
        Player* gatherer)
    {
        if (!gatherer)
            return;

        if (!IsEnabled())
            return;

        if (!SkinningEnabled)
            return;

        if (!gatherer->GetGroup())
            return;

        if (!gatherer->HasSkill(SKILL_SKINNING))
            return;

        Creature* creature =
            GetSelectedSkinningCreature(
                gatherer);

        if (!creature)
        {
            DebugSkinningLog(
                "No matching mod-auto-gather Skinning creature found.",
                gatherer);

            return;
        }

        /*
         * Do not process the same creature twice.
         */
        if (WasRecentlyProcessed(
                gatherer,
                creature))
        {
            DebugSkinningLog(
                "Duplicate mod-auto-gather Skinning operation ignored.",
                gatherer,
                creature);

            return;
        }

        MarkSkinningProcessed(
            gatherer,
            creature);

        DebugSkinningLog(
            "Detected mod-auto-gather Skinning operation.",
            gatherer,
            creature);

        /*
         * The original gatherer has already received their normal
         * Skinning loot from mod-auto-gather.
         *
         * We only generate additional independent rolls here.
         */
        DistributeSkinningLoot(
            gatherer,
            creature);
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

        if (!IsGatheringSkill(skillId))
            return;

        /*
         * Skinning is intentionally handled separately.
         *
         * Normal Mining/Herbalism behavior below remains the
         * existing implementation.
         */
        if (skillId == SKILL_SKINNING)
        {
            ProcessAutoSkinning(
                player);

            return;
        }

        /*
         * First handle normal AzerothCore gathering.
         *
         * If this returns true, the operation came through
         * GO_ACTIVATED and has already been processed.
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
     * Normal AzerothCore gathering reaches GO_ACTIVATED here.
     *
     * mod-auto-gather does not use GO_ACTIVATED, which is why the
     * PlayerScript fallback exists above.
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

        /*
         * No need to track anything if the player is not grouped.
         */
        if (!gatherer->GetGroup())
            return;

        /*
         * Do not determine Mining vs Herbalism here.
         *
         * AzerothCore determines the actual skill and sends it through
         * OnPlayerUpdateGatheringSkill().
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
                false);

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
            "   Mining: {}",
            MiningEnabled);

        LOG_INFO(
            "server.loading",
            "   Herbalism: {}",
            HerbalismEnabled);

        LOG_INFO(
            "server.loading",
            "   Skinning: {}",
            SkinningEnabled);

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
            "   AutoGather compatibility: Mining/Herbalism/Skinning");
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