/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Group profession loot with independent loot rolls and
 * independent profession skill-up attempts.
 *
 * Supported professions:
 *   - Mining
 *   - Herbalism
 *   - Skinning
 *
 * Supported gathering implementations:
 *   - Normal AzerothCore gathering
 *   - mod-auto-gather Mining/Herbalism
 *   - mod-auto-gather Skinning
 *
 * Loot and skill multipliers are configured independently.
 *
 * Example:
 *
 *   MiningLootMultiplier  = 4
 *   MiningSkillMultiplier = 3
 *
 * Actual gatherer:
 *   - normal core loot
 *   - 3 additional loot rolls
 *   - normal core skill-up attempt
 *   - 2 additional skill-up attempts
 *
 * Other Mining group member:
 *   - 4 independent loot rolls
 *   - 3 independent skill-up attempts
 *
 * Group member without Mining:
 *   - nothing
 */

#ifndef MOD_PROFESSION_LOOT_PARTY_H
#define MOD_PROFESSION_LOOT_PARTY_H

#include "AllGameObjectScript.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "PlayerScript.h"
#include "WorldScript.h"

class Creature;
class GameObject;
class Player;
class Unit;

namespace ProfessionLootParty
{
    /*
     * Internal profession slots.
     *
     * Used to index the settings array instead of repeating a
     * switch over the DBC skill ids in every accessor.
     */
    enum ProfessionSlot : uint8
    {
        PROFESSION_MINING    = 0,
        PROFESSION_HERBALISM = 1,
        PROFESSION_SKINNING  = 2,
        PROFESSION_MAX
    };

    struct ProfessionSettings
    {
        bool   Enabled         = true;
        uint32 LootMultiplier  = 1;
        uint32 SkillMultiplier = 1;
    };

    struct ModuleSettings
    {
        bool  Enabled          = true;

        ProfessionSettings Professions[PROFESSION_MAX];

        /* Maximum distance between the resource and a receiving member. */
        float MaxDistance      = 100.0f;

        /* Radius used to locate the node/corpse the gatherer just used. */
        float SearchDistance   = 10.0f;

        bool  IncludeRaid      = true;
        bool  RequireSkill     = true;
        bool  RequireNodeSkill = true;
        bool  RequireGroup     = true;
        bool  ApplyToGatherer  = true;
        bool  AutoGatherCompat = true;
        bool  Announce         = false;
        bool  Debug            = false;

        /* 0 = unlimited. */
        uint32 MaxRecipients   = 0;
    };

    /* --------------------------------------------------------------
     * Public query API (stable, usable by other modules)
     * -------------------------------------------------------------- */

    bool IsEnabled();
    bool IsProfessionEnabled(uint32 skillId);
    uint32 GetProfessionLootMultiplier(uint32 skillId);
    uint32 GetProfessionSkillMultiplier(uint32 skillId);

    ModuleSettings const& GetSettings();

    /* --------------------------------------------------------------
     * Internal pipeline (kept public for backwards compatibility)
     * -------------------------------------------------------------- */

    void AddPendingGather(Player* gatherer, GameObject* gameObject);
    void RemovePendingGather(ObjectGuid playerGuid);

    bool ProcessPendingGather(Player* gatherer, uint32 skillId);
    void ProcessAutoGather(Player* gatherer, uint32 skillId);
    bool ProcessSkinning(Player* skinner);

    void RemoveRecentSkinning(ObjectGuid playerGuid);
    void ForgetPlayer(ObjectGuid playerGuid);

    /* --------------------------------------------------------------
     * Scripts
     * -------------------------------------------------------------- */

    class PlayerScript final : public ::PlayerScript
    {
    public:
        PlayerScript();

        void OnPlayerUpdateGatheringSkill(Player* player, uint32 skillId, uint32 currentLevel,
            uint32 gray, uint32 green, uint32 yellow, uint32& gain) override;

        void OnPlayerLogout(Player* player) override;
    };

    class GameObjectScript final : public ::AllGameObjectScript
    {
    public:
        GameObjectScript();

        void OnGameObjectLootStateChanged(GameObject* gameObject, uint32 state, Unit* unit) override;
    };

    class ConfigScript final : public ::WorldScript
    {
    public:
        ConfigScript();

        void OnBeforeConfigLoad(bool reload) override;
        void OnAfterConfigLoad(bool reload) override;
    };
}

/*
 * AzerothCore module loader.
 *
 * This spelling must match the generated ModulesLoader.cpp symbol.
 */
void Addmod_profession_loot_partyScripts();

#endif // MOD_PROFESSION_LOOT_PARTY_H
