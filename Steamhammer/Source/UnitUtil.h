#pragma once

#include <Common.h>
#include <BWAPI.h>

namespace UAlbertaBot
{
struct UnitInfo;

namespace UnitUtil
{
    bool IsTechBuildingType(BWAPI::UnitType type);
    bool IsProductionBuildingType(BWAPI::UnitType type);
    bool IsMorphedBuildingType(BWAPI::UnitType type);
    bool IsMorphedUnitType(BWAPI::UnitType type);
    bool NeedsWorkerBuildingType(BWAPI::UnitType type);
    bool BuildingIsMorphedFrom(BWAPI::UnitType t2, BWAPI::UnitType t1);
    bool IsCompletedResourceDepot(BWAPI::Unit unit);
    bool IsNearlyCompletedResourceDepot(BWAPI::Unit unit, int framesLeft);
    int ExtraBuildingLatency(BWAPI::Race race);

    bool IsStaticDefense(BWAPI::UnitType type);
    bool IsComingStaticDefense(BWAPI::UnitType type);
    bool IsGroundStaticDefense(BWAPI::UnitType type);
    BWAPI::UnitType GetGroundStaticDefenseType(BWAPI::Race race);

    bool IsCombatSimUnit(const UnitInfo & ui);
    bool IsCombatSimUnit(BWAPI::Unit unit);
    bool IsCombatSimUnit(BWAPI::UnitType type);
    bool IsCombatUnit(BWAPI::UnitType type);
    bool IsCombatUnit(BWAPI::Unit unit);
    bool IsSuicideUnit(BWAPI::UnitType type);
    bool IsSuicideUnit(BWAPI::Unit unit);

    bool IsValidUnit(BWAPI::Unit unit);

    // Damage per frame (formerly CalculateLDT()).
    double DPF(BWAPI::Unit attacker, BWAPI::Unit target);
    double GroundDPF(BWAPI::Player player, BWAPI::UnitType type);
    double AirDPF(BWAPI::Player player, BWAPI::UnitType type);

    bool CanAttack(BWAPI::Unit attacker, BWAPI::Unit target);
    bool CanAttack(BWAPI::UnitType attacker, BWAPI::Unit target);
    bool CanAttack(BWAPI::UnitType attacker, BWAPI::UnitType target);
    bool CanAttackAir(BWAPI::Unit attacker);
    bool TypeCanAttackAir(BWAPI::UnitType attacker);
    bool CanAttackGround(BWAPI::Unit attacker);
    bool TypeCanAttackGround(BWAPI::UnitType attacker);
    bool TypeCanAttack(BWAPI::UnitType type);
    BWAPI::WeaponType GetGroundWeapon(BWAPI::Unit attacker);
    BWAPI::WeaponType GetGroundWeapon(BWAPI::UnitType attacker);
    BWAPI::WeaponType GetAirWeapon(BWAPI::Unit attacker);
    BWAPI::WeaponType GetAirWeapon(BWAPI::UnitType attacker);
    BWAPI::WeaponType GetWeapon(BWAPI::Unit attacker, BWAPI::Unit target);
    BWAPI::WeaponType GetWeapon(BWAPI::UnitType attacker, BWAPI::Unit target);
    BWAPI::WeaponType GetWeapon(BWAPI::UnitType attacker, BWAPI::UnitType target);
    int GetAttackRange(BWAPI::Unit attacker, BWAPI::Unit target);
    int GetAttackRangeAssumingUpgrades(BWAPI::UnitType attacker, BWAPI::UnitType target);
    int GetMaxAttackRange(BWAPI::UnitType attacker);    // air or ground
    int GroundCooldownLeft(BWAPI::Unit attacker);
    int AirCooldownLeft(BWAPI::Unit attacker);
    int CooldownLeft(BWAPI::Unit attacker, BWAPI::Unit target);
    int FramesToReachAttackRange(BWAPI::Unit attacker, BWAPI::Unit target);
    int GetWeaponDamageToWorker(BWAPI::Unit attacker);
    int ExpectedSurvivalTime(const BWAPI::Unitset & attackers, BWAPI::Unit target);
    int ExpectedSurvivalTime(BWAPI::Unit friendlyTarget);

    bool HitsUnderSwarm(BWAPI::UnitType type);
    bool HitsUnderSwarm(BWAPI::Unit unit);

    bool AttackOrder(BWAPI::Unit unit);
    bool MineralOrder(BWAPI::Unit unit);
    bool GasOrder(BWAPI::Unit unit);

    int GetDetectionRange(BWAPI::UnitType type);
    bool EnemyDetectorInRange(BWAPI::Position pos);
    bool EnemyDetectorInRange(BWAPI::Unit unit);

    int GetUncompletedUnitCount(BWAPI::UnitType type);

    bool MobilizeUnit(BWAPI::Unit unit);      // unsiege or unburrow
    bool ImmobilizeUnit(BWAPI::Unit unit);    // siege or burrow
};
}
