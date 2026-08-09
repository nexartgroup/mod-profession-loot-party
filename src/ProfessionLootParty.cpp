/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Group profession loot with independent loot rolls.
 *
 * Supports:
 *   - Normal AzerothCore Mining/Herbalism gathering
 *   - mod-auto-gather Mining/Herbalism gathering
 *
 * The original gatherer keeps their normal loot.
 * Other eligible group members receive an independent roll.
 */

#include "ProfessionLootParty.h"

#include "CellImpl.h"
#include "Config.h"
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

        std::unordered_map<uint64, PendingGather> PendingGathers;

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

        bool IsGatheringSkill(uint32 skillId)
        {
            return skillId == SKILL_MINING ||
                   skillId == SKILL_HERBALISM;
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
         * Distribute one successful gathering operation.
         *
         * This is shared by:
         *   - normal AzerothCore gathering
         *   - mod-auto-gather gathering
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
         * Resolve the gathering profession represented by a GameObject.
         *
         * This intentionally uses the lock data, exactly like the
         * auto-gather module does, rather than guessing from the
         * GameObject entry.
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

            uint32 lockId = goInfo->GetLockId();

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

                /*
                 * This is the same skill-level requirement used by
                 * mod-auto-gather when selecting its nodes.
                 */
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
         * Search for the exact GameObject that mod-auto-gather has
         * just processed.
         *
         * The important signal is:
         *
         *   GameObject == GO_READY
         *   +
         *   player is in GameObject's skill-up list
         *
         * mod-auto-gather does this immediately before calling
         * Player::UpdateGatherSkill().
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

                /*
                 * AutoGather calls UpdateGatherSkill() before
                 * SetLootState(GO_JUST_DEACTIVATED), so the node is
                 * still GO_READY at this exact hook.
                 */
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

                /*
                 * This is the critical compatibility check.
                 *
                 * AutoGather adds the player to this list immediately
                 * before UpdateGatherSkill().
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

            /*
             * The node must be near the gatherer. We use the module's
             * configured recipient distance as an upper bound.
             *
             * This also supports servers where AutoGather.LootRange
             * has been increased above its default.
             */
            float searchRange = MaxDistance;

            /*
             * Even when the party distance is configured to zero,
             * AutoGather still needs a useful search radius to find
             * the node. One yard is sufficient to avoid an invalid
             * zero-radius search while still ensuring no recipient
             * can actually qualify later.
             */
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
             * Prefer the closest matching node. This protects against
             * an older skill-up-list entry being present elsewhere in
             * the same grid.
             */
            auto itr = std::min_element(
                nodes.begin(),
                nodes.end(),
                [player](GameObject* left,
                         GameObject* right)
                {
                    return player->GetDistance(left) <
                           player->GetDistance(right);
                });

            return itr != nodes.end() ? *itr : nullptr;
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

            default:
                return false;
        }
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