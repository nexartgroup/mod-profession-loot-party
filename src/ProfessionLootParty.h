/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Gives eligible group/raid members an independent profession-loot
 * roll when another member successfully gathers a Mining or Herbalism
 * GameObject.
 *
 * Compatible with both:
 *   - normal AzerothCore gathering
 *   - mod-auto-gather
 */

#ifndef PROFESSION_LOOT_PARTY_H
#define PROFESSION_LOOT_PARTY_H

#include "AllGameObjectScript.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "PlayerScript.h"
#include "WorldScript.h"

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

    bool IsEnabled();

    bool IsProfessionEnabled(uint32 skillId);

    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject);

    void RemovePendingGather(
        ObjectGuid playerGuid);

    /*
     * Returns true when a normal gathering operation was found in the
     * pending queue. This prevents the auto-gather fallback from
     * processing the same operation a second time.
     */
    bool ProcessPendingGather(
        Player* gatherer,
        uint32 skillId);

    /*
     * Detects mod-auto-gather operations.
     *
     * mod-auto-gather does not transition the GameObject through
     * GO_ACTIVATED. Instead it:
     *
     *   1. generates/stores the loot
     *   2. adds the player to the GameObject skill-up list
     *   3. calls UpdateGatherSkill()
     *   4. sets GO_JUST_DEACTIVATED
     *
     * Therefore this fallback searches nearby GO_READY resource
     * nodes which are present in the player's skill-up list.
     */
    void ProcessAutoGather(
        Player* gatherer,
        uint32 skillId);

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