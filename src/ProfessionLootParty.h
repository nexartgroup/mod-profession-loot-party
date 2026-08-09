/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * Gives eligible group/raid members an independent profession-loot
 * roll when another member successfully gathers a Mining or Herbalism
 * GameObject.
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

    void ProcessPendingGather(
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

    /*
     * IMPORTANT:
     *
     * OnGameObjectLootStateChanged() belongs to
     * AllGameObjectScript in this AzerothCore revision.
     */
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

void AddProfessionLootPartyScripts();

#endif