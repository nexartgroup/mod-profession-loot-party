/*
 * mod-profession-loot-party
 *
 * AzerothCore WotLK 3.3.5a
 *
 * When a grouped player successfully gathers a Mining or Herbalism
 * GameObject, eligible group/raid members with the same profession
 * receive their own independent loot roll from the same
 * gameobject_loot_template.
 *
 * The original gatherer continues to receive normal AzerothCore loot.
 */

#ifndef PROFESSION_LOOT_PARTY_H
#define PROFESSION_LOOT_PARTY_H

#include "Define.h"
#include "GameObjectScript.h"
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

    bool IsEligibleMember(
        Player* gatherer,
        Player* member,
        GameObject* gameObject,
        uint32 skillId);

    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject);

    void RemovePendingGather(ObjectGuid playerGuid);

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

        void OnPlayerLogout(Player* player) override;
    };

    class GameObjectScript final : public ::GameObjectScript
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

        void OnBeforeConfigLoad(bool reload) override;
    };
}

#endif