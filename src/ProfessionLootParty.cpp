/*
 * mod-profession-loot-party
 *
 * Independent profession gathering loot for group/raid members.
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Mining / Herbalism:
 *
 *   Player A gathers node
 *        |
 *        +-- A gets normal AzerothCore loot
 *        +-- B gets independent loot roll
 *        +-- C gets independent loot roll
 *
 * Only group members with the appropriate profession are eligible.
 */

#include "ProfessionLootParty.h"

#include "Config.h"
#include "GameObject.h"
#include "Group.h"
#include "LootMgr.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "World.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace ProfessionLootParty
{
    namespace
    {
        bool Enabled = true;
        bool MiningEnabled = true;
        bool HerbalismEnabled = true;
        bool SkinningEnabled = false;
        bool IncludeRaid = true;
        bool RequireSkill = true;
        bool Debug = false;

        float MaxDistance = 100.0f;

        constexpr uint32 PENDING_TIMEOUT_MS = 5000;

        std::unordered_map<uint64, PendingGather> PendingGathers;

        uint32 GetMSTime()
        {
            using namespace std::chrono;

            return static_cast<uint32>(
                duration_cast<milliseconds>(
                    steady_clock::now().time_since_epoch()).count());
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

        bool IsPendingExpired(PendingGather const& pending)
        {
            uint32 now = GetMSTime();

            return uint32(now - pending.createdAt) > PENDING_TIMEOUT_MS;
        }

        bool HasSkillForProfession(
            Player* player,
            Profession profession)
        {
            if (!player)
                return false;

            switch (profession)
            {
                case Profession::Mining:
                    return player->HasSkill(SKILL_MINING);

                case Profession::Herbalism:
                    return player->HasSkill(SKILL_HERBALISM);

                default:
                    return false;
            }
        }

        bool HasRequiredSkillLevel(
            Player* player,
            GameObject* gameObject,
            Profession profession)
        {
            if (!RequireSkill)
                return true;

            if (!player || !gameObject)
                return false;

            uint32 skill = 0;

            switch (profession)
            {
                case Profession::Mining:
                    skill = SKILL_MINING;
                    break;

                case Profession::Herbalism:
                    skill = SKILL_HERBALISM;
                    break;

                default:
                    return false;
            }

            /*
             * The exact required skill for a node is evaluated by
             * Spell::CanOpenLock() in the normal gathering path.
             *
             * We deliberately do not try to duplicate the lock table
             * calculation here. The player must at least possess the
             * gathering skill.
             *
             * The original gatherer has already passed the normal
             * CanOpenLock() check before this module is reached.
             */
            return player->GetSkillValue(skill) > 0;
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
            Player* gatherer,
            Player* member,
            GameObject* gameObject)
        {
            if (!gatherer || !member || !gameObject)
                return false;

            if (gatherer->GetMapId() != member->GetMapId())
                return false;

            if (gameObject->GetMapId() != member->GetMapId())
                return false;

            return member->GetDistance(gameObject) <= MaxDistance;
        }

        /*
         * Do not award another roll to the gatherer.
         *
         * AzerothCore already created the gatherer's normal loot through
         * Player::SendLoot() -> Loot::FillLoot().
         */
        bool IsGatherer(
            Player* gatherer,
            Player* member)
        {
            return gatherer->GetGUID() == member->GetGUID();
        }

        /*
         * AutoStoreLoot() uses the actual LootStore and therefore invokes
         * the normal loot-template processing path. This is intentional:
         * every recipient receives an independent roll.
         */
        void GiveIndependentRoll(
            Player* player,
            GameObject* gameObject)
        {
            if (!player || !gameObject)
                return;

            uint32 lootId = gameObject->GetGOInfo()->GetLootId();

            if (!lootId)
            {
                DebugLog(
                    "GameObject has no loot id; skipping independent roll.",
                    player,
                    gameObject);
                return;
            }

            /*
             * Use the exact same gameobject loot table that AzerothCore
             * uses for the node.
             *
             * This generates a NEW Loot object and therefore new chance
             * and count rolls for this player.
             */
            player->AutoStoreLoot(
                lootId,
                LootTemplates_Gameobject);

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

    bool IsProfessionEnabled(Profession profession)
    {
        switch (profession)
        {
            case Profession::Mining:
                return Enabled && MiningEnabled;

            case Profession::Herbalism:
                return Enabled && HerbalismEnabled;

            case Profession::None:
            default:
                return false;
        }
    }

    Profession GetGatheringProfession(
        Player* player,
        GameObject* gameObject)
    {
        if (!player || !gameObject)
            return Profession::None;

        /*
         * Gathering nodes are opened through SPELL_EFFECT_OPEN_LOCK.
         * At the point where GameObjectLootStateChanged fires, the
         * successful gathering operation has been performed.
         *
         * We determine the profession from the player's known gathering
         * skills. The pending operation is only created after the
         * successful GameObject activation.
         *
         * Mining takes precedence only when the player has Mining.
         * Herbalism is used otherwise.
         */
        if (MiningEnabled && player->HasSkill(SKILL_MINING))
            return Profession::Mining;

        if (HerbalismEnabled && player->HasSkill(SKILL_HERBALISM))
            return Profession::Herbalism;

        return Profession::None;
    }

    bool IsEligibleMember(
        Player* gatherer,
        Player* member,
        GameObject* gameObject,
        Profession profession)
    {
        if (!gatherer || !member || !gameObject)
            return false;

        if (IsGatherer(gatherer, member))
            return false;

        if (!member->IsInWorld() || !member->IsAlive())
            return false;

        if (!IsSameGroup(gatherer, member))
            return false;

        if (!IsCloseEnough(gatherer, member, gameObject))
            return false;

        if (!IsProfessionEnabled(profession))
            return false;

        if (!HasSkillForProfession(member, profession))
            return false;

        if (!HasRequiredSkillLevel(member, gameObject, profession))
            return false;

        return true;
    }

    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject,
        Profession profession)
    {
        if (!gatherer || !gameObject || profession == Profession::None)
            return;

        PendingGather pending;
        pending.gatherer = gatherer->GetGUID();
        pending.gameObject = gameObject->GetGUID();
        pending.profession = profession;
        pending.createdAt = GetMSTime();

        PendingGathers[gatherer->GetGUID().GetRawValue()] = pending;

        DebugLog(
            "Gathering operation queued.",
            gatherer,
            gameObject);
    }

    bool HasPendingGather(ObjectGuid playerGuid)
    {
        return PendingGathers.find(playerGuid.GetRawValue()) != PendingGathers.end();
    }

    void RemovePendingGather(ObjectGuid playerGuid)
    {
        PendingGathers.erase(playerGuid.GetRawValue());
    }

    void ProcessPendingGather(Player* gatherer)
    {
        if (!gatherer)
            return;

        auto itr = PendingGathers.find(gatherer->GetGUID().GetRawValue());

        if (itr == PendingGathers.end())
            return;

        PendingGather pending = itr->second;

        PendingGathers.erase(itr);

        if (IsPendingExpired(pending))
        {
            DebugLog(
                "Pending gathering operation expired.",
                gatherer);
            return;
        }

        GameObject* gameObject =
            ObjectAccessor::GetGameObject(*gatherer, pending.gameObject);

        if (!gameObject)
        {
            DebugLog(
                "Pending gathering GameObject no longer exists.",
                gatherer);
            return;
        }

        /*
         * The core adds the successful gatherer to the GameObject's
         * skill-up list after SendLoot(). Waiting for the next player
         * update guarantees that a failed lock attempt does not trigger
         * this module.
         */
        if (!gameObject->IsInSkillupList(gatherer->GetGUID()))
        {
            /*
             * The state change may have occurred for a normal chest or
             * another non-gathering GameObject. Do not distribute loot.
             */
            DebugLog(
                "GameObject activation was not confirmed as a successful gathering operation.",
                gatherer,
                gameObject);
            return;
        }

        if (!gatherer->GetGroup())
        {
            DebugLog(
                "Gatherer has no group; nothing to distribute.",
                gatherer,
                gameObject);
            return;
        }

        Group* group = gatherer->GetGroup();

        for (GroupReference* itrGroup = group->GetFirstMember();
             itrGroup != nullptr;
             itrGroup = itrGroup->next())
        {
            Player* member = itrGroup->GetSource();

            if (!member)
                continue;

            if (!IsEligibleMember(
                    gatherer,
                    member,
                    gameObject,
                    pending.profession))
                continue;

            GiveIndependentRoll(member, gameObject);
        }

        DebugLog(
            "Pending gathering operation processed.",
            gatherer,
            gameObject);
    }

    PlayerScript::PlayerScript()
        : ::PlayerScript("ProfessionLootParty_PlayerScript")
    {
    }

    void PlayerScript::OnPlayerUpdate(
        Player* player,
        uint32 /*diff*/)
    {
        if (!player || !IsEnabled())
            return;

        if (!HasPendingGather(player->GetGUID()))
            return;

        ProcessPendingGather(player);
    }

    void PlayerScript::OnPlayerLogout(Player* player)
    {
        if (!player)
            return;

        RemovePendingGather(player->GetGUID());
    }

    GameObjectScript::GameObjectScript()
        : ::GameObjectScript("ProfessionLootParty_GameObjectScript")
    {
    }

    void GameObjectScript::OnGameObjectLootStateChanged(
        GameObject* gameObject,
        uint32 state,
        Unit* unit)
    {
        if (!IsEnabled())
            return;

        if (!gameObject || !unit || !unit->IsPlayer())
            return;

        /*
         * EffectOpenLock() ultimately calls SetLootState(GO_ACTIVATED)
         * from the GameObject loot path.
         */
        if (state != GO_ACTIVATED)
            return;

        Player* gatherer = unit->ToPlayer();

        if (!gatherer)
            return;

        if (!gatherer->GetGroup())
            return;

        Profession profession =
            GetGatheringProfession(gatherer, gameObject);

        if (profession == Profession::None)
            return;

        if (!IsProfessionEnabled(profession))
            return;

        AddPendingGather(
            gatherer,
            gameObject,
            profession);
    }

    /*
     * Configuration.
     */
    class ConfigScript final : public WorldScript
    {
    public:
        ConfigScript()
            : WorldScript("ProfessionLootParty_ConfigScript")
        {
        }

        void OnBeforeConfigLoad(bool reload) override
        {
            if (reload)
                return;

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
                "   Mining: {}, Herbalism: {}, Raid: {}, Distance: {}",
                MiningEnabled,
                HerbalismEnabled,
                IncludeRaid,
                MaxDistance);
        }
    };

    class ProfessionLootPartyModule
    {
    public:
        ProfessionLootPartyModule()
        {
            new ConfigScript();
            new PlayerScript();
            new GameObjectScript();
        }
    };
}

/*
 * AzerothCore module loader.
 */
void AddProfessionLootPartyScripts()
{
    new ProfessionLootParty::PlayerScript();
    new ProfessionLootParty::GameObjectScript();
}