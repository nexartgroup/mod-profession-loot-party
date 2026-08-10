/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Gives eligible group/raid members an independent profession-loot
 * roll when another member successfully gathers a resource.
 *
 * Supported professions:
 *
 *   - Mining
 *   - Herbalism
 *   - Skinning
 *
 * Supported gathering implementations:
 *
 *   - Normal AzerothCore gathering
 *   - mod-auto-gather Mining/Herbalism
 *   - mod-auto-gather Skinning
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

    /*
     * Used to prevent the same skinned corpse from being processed
     * more than once.
     *
     * This is needed because Skinning has no GameObject loot-state
     * callback equivalent to normal Mining/Herbalism gathering.
     */
    struct RecentSkinning
    {
        ObjectGuid gatherer;
        ObjectGuid creature;
        uint32 createdAt = 0;
    };

    bool IsEnabled();

    bool IsProfessionEnabled(uint32 skillId);

    /*
     * Normal Mining/Herbalism gathering.
     */
    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject);

    void RemovePendingGather(
        ObjectGuid playerGuid);

    bool ProcessPendingGather(
        Player* gatherer,
        uint32 skillId);

    /*
     * mod-auto-gather Mining/Herbalism compatibility.
     */
    void ProcessAutoGather(
        Player* gatherer,
        uint32 skillId);

    /*
     * Skinning.
     *
     * Handles both:
     *
     *   - normal AzerothCore Skinning
     *   - mod-auto-gather AutoSkinCreature()
     */
    bool ProcessSkinning(
        Player* skinner);

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