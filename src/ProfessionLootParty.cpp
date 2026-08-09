/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Group profession loot with independent loot rolls.
 *
 * Example:
 *
 *   Alice - Mining
 *   Bob   - Mining
 *   Carol - Herbalism
 *
 * Alice mines a node:
 *
 *   Alice -> normal AzerothCore loot
 *   Bob   -> independent Mining loot roll
 *   Carol -> nothing
 *
 * Each additional player gets a NEW roll against the same
 * gameobject_loot_template.
 */

#include "ProfessionLootParty.h"

#include "Config.h"
#include "GameObject.h"
#include "Group.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Unit.h"

#include <chrono>
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
         * EffectOpenLock() calls SetLootState() before it calls
         * UpdateGatherSkill().
         *
         * We temporarily remember the GameObject here, then consume
         * the record from OnPlayerUpdateGatheringSkill().
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

            return uint32(now - pending.createdAt) > PENDING_TIMEOUT_MS;
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
             * The gatherer already received the normal AzerothCore
             * gathering loot. Never give that player a second roll.
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

            /*
             * RequireSkill means the recipient must actually possess
             * the profession.
             *
             * We intentionally do not duplicate CanOpenLock() here.
             * The exact gathering skill has already been determined by
             * the core for the original gatherer.
             */
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
             * IMPORTANT:
             *
             * AutoStoreLoot() creates/processes a fresh loot result
             * from LootTemplates_Gameobject.
             *
             * Therefore this is an independent roll, NOT a copy of
             * the gatherer's LootItem list.
             *
             * Example:
             *
             *   Node table:
             *       Ore: 1-3
             *       Rare item: 10%
             *
             *   Player A:
             *       2 Ore
             *
             *   Player B:
             *       3 Ore + rare item
             *
             *   Player C:
             *       1 Ore
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

        pending.gatherer = gatherer->GetGUID();
        pending.gameObject = gameObject->GetGUID();
        pending.createdAt = GetMSTime();

        PendingGathers[
            gatherer->GetGUID().GetRawValue()
        ] = pending;

        DebugLog(
            "Gathering operation queued.",
            gatherer,
            gameObject);
    }

    void RemovePendingGather(ObjectGuid playerGuid)
    {
        PendingGathers.erase(
            playerGuid.GetRawValue());
    }

    void ProcessPendingGather(
        Player* gatherer,
        uint32 skillId)
    {
        if (!gatherer)
            return;

        if (!IsGatheringSkill(skillId))
            return;

        auto itr =
            PendingGathers.find(
                gatherer->GetGUID().GetRawValue());

        if (itr == PendingGathers.end())
            return;

        PendingGather pending = itr->second;

        /*
         * Consume the pending operation immediately.
         *
         * This prevents a second gathering-skill hook from processing
         * the same node twice.
         */
        PendingGathers.erase(itr);

        if (IsPendingExpired(pending))
        {
            DebugLog(
                "Pending gathering operation expired.",
                gatherer);

            return;
        }

        if (!IsProfessionEnabled(skillId))
            return;

        /*
         * The GameObject must still exist.
         */
        GameObject* gameObject =
            ObjectAccessor::GetGameObject(
                *gatherer,
                pending.gameObject);

        if (!gameObject)
        {
            DebugLog(
                "Pending gathering GameObject no longer exists.",
                gatherer);

            return;
        }

        /*
         * This is the critical verification.
         *
         * EffectOpenLock() calls:
         *
         *   SendLoot()
         *   AddToSkillupList()
         *   UpdateGatherSkill()
         *
         * Therefore, when OnPlayerUpdateGatheringSkill() executes,
         * a genuine successful gathering operation has added this
         * GameObject to the gatherer's skill-up list.
         *
         * A chest activation, door, button, etc. won't pass this
         * check merely because the player knows Mining/Herbalism.
         */
        if (!gameObject->IsInSkillupList(
                gatherer->GetGUID()))
        {
            DebugLog(
                "Gathering skill update did not match the pending GameObject.",
                gatherer,
                gameObject);

            return;
        }

        Group* group = gatherer->GetGroup();

        if (!group)
            return;

        DebugLog(
            "Confirmed successful profession gathering.",
            gatherer,
            gameObject);

        /*
         * Iterate over every member of the same group/raid.
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

            if (!IsEligibleMember(
                    gatherer,
                    member,
                    gameObject,
                    skillId))
            {
                continue;
            }

            /*
             * Each member gets a completely independent roll.
             */
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

        ProcessPendingGather(
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
     */

    GameObjectScript::GameObjectScript()
        : ::GameObjectScript(
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
         * No group means there is nobody else to reward.
         */
        if (!gatherer->GetGroup())
            return;

        /*
         * Do NOT determine Mining/Herbalism here.
         *
         * EffectOpenLock() determines the actual SkillType in:
         *
         *   CanOpenLock(..., skillId, ...)
         *
         * The skill hook below receives that exact skillId.
         *
         * We therefore only remember the GameObject here.
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
            "   Distance: {}",
            MaxDistance);
    }
}

/*
 * AzerothCore module loader.
 *
 * This is the entry point used by the module CMake integration.
 */
void AddProfessionLootPartyScripts()
{
    new ProfessionLootParty::ConfigScript();
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}