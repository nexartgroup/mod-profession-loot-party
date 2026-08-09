/*
 * mod-profession-loot-party
 *
 * Allows nearby group/raid members with the appropriate gathering
 * profession to receive their own independent gathering loot roll.
 *
 * Supported:
 *   - Mining
 *   - Herbalism
 *
 * The original gatherer continues to use AzerothCore's normal loot path.
 * Additional eligible group members receive an independent roll from
 * the same gameobject_loot_template.
 */

#ifndef PROFESSION_LOOT_PARTY_H
#define PROFESSION_LOOT_PARTY_H

#include "Define.h"
#include "ObjectGuid.h"
#include "PlayerScript.h"
#include "GameObjectScript.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class GameObject;
class Player;
class Spell;

namespace ProfessionLootParty
{
    enum class Profession : uint8
    {
        None      = 0,
        Mining    = 1,
        Herbalism = 2
    };

    struct PendingGather
    {
        ObjectGuid gatherer;
        ObjectGuid gameObject;
        Profession profession = Profession::None;
        uint32 createdAt = 0;
    };

    bool IsEnabled();
    bool IsProfessionEnabled(Profession profession);

    Profession GetGatheringProfession(Player* player, GameObject* gameObject);

    bool IsEligibleMember(
        Player* gatherer,
        Player* member,
        GameObject* gameObject,
        Profession profession);

    void ProcessPendingGather(Player* gatherer);

    void AddPendingGather(
        Player* gatherer,
        GameObject* gameObject,
        Profession profession);

    void RemovePendingGather(ObjectGuid playerGuid);

    bool HasPendingGather(ObjectGuid playerGuid);

    class PlayerScript final : public ::PlayerScript
    {
    public:
        PlayerScript();

        void OnPlayerUpdate(Player* player, uint32 diff) override;
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
}

#endif