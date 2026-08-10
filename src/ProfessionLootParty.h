/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Gives eligible group/raid members an independent profession-loot
 * roll when another member successfully gathers a resource.
 *
 * Supports:
 *   - Normal AzerothCore Mining/Herbalism gathering
 *   - mod-auto-gather Mining/Herbalism gathering
 *   - mod-auto-gather Skinning
 *
 * Important:
 *
 * Skinning support is specifically designed around the behavior of
 * mod-auto-gather's AutoSkinCreature():
 *
 *   1. SkinLootId is generated/stored.
 *   2. UNIT_FLAG_SKINNABLE is removed.
 *   3. Creature loot is cleared.
 *   4. loot_type is set to LOOT_SKINNING.
 *   5. UNIT_DYNFLAG_LOOTABLE is removed.
 *   6. UpdateGatherSkill(SKILL_SKINNING) is called.
 *
 * The original gatherer/skinner keeps their normal loot.
 * Other eligible group members receive an independent roll.
 */

#ifndef PROFESSION_LOOT_PARTY_H
#define PROFESSION_LOOT_PARTY_H

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
    struct PendingGather
    {
        ObjectGuid gatherer;
        ObjectGuid gameObject;
        uint32 createdAt = 0;
    };

    struct RecentSkinning
    {
        ObjectGuid creature;
        uint32 createdAt = 0;
    };

    bool IsEnabled();

    bool IsProfessionEnabled(uint32 skillId);

    /*
     * Normal AzerothCore GameObject gathering.
     */
    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject);

    void RemovePendingGather(
        ObjectGuid playerGuid);

    /*
     * Returns true when a normal gathering operation was found.
     *
     * This prevents the mod-auto-gather fallback from processing
     * the same normal gathering operation a second time.
     */
    bool ProcessPendingGather(
        Player* gatherer,
        uint32 skillId);

    /*
     * Existing mod-auto-gather Mining/Herbalism compatibility.
     */
    void ProcessAutoGather(
        Player* gatherer,
        uint32 skillId);

    /*
     * mod-auto-gather Skinning compatibility.
     *
     * This does NOT hook the normal Skinning spell.
     *
     * It identifies the creature selected by the player after
     * mod-auto-gather's AutoSkinCreature() has completed the
     * Skinning operation and before the skill-update hook returns.
     */
    void ProcessAutoSkinning(
        Player* gatherer);

    /*
     * Removes the short-lived Skinning duplicate-protection record.
     */
    void RemoveRecentSkinning(
        ObjectGuid playerGuid);

    class PlayerScript final : public ::PlayerScript
    {
    public:
        PlayerScript();

        void OnPlayerUpdateGatheringSkill(
            Player* player,
            uint32 skillId,
            uint32 currentLevel,
            uint32 gray,
            uint32 green,
            uint32 yellow,
            uint32& gain) override;

        void OnPlayerLogout(
            Player* player) override;
    };

    class GameObjectScript final : public ::AllGameObjectScript
    {
    public:
        GameObjectScript();

        void OnGameObjectLootStateChanged(
            GameObject* gameObject,
            uint32 state,
            Unit* unit) override;
    };

    class ConfigScript final : public ::WorldScript
    {
    public:
        ConfigScript();

        void OnBeforeConfigLoad(
            bool reload) override;
    };
}

void Addmod_profession_loot_partyScripts();

#endif