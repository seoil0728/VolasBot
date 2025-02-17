#include "MicroManager.h"
#include "MicroMelee.h"

#include "Bases.h"
#include "InformationManager.h"
#include "The.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// NOTE Melee units are ground units only. Scourge is treated as a ranged unit.

MicroMelee::MicroMelee()
{ 
}

void MicroMelee::executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster)
{
    BWAPI::Unitset units = Intersection(getUnits(), cluster.units);
    if (units.empty())
    {
        return;
    }
    assignTargets(units, targets);
}

void MicroMelee::assignTargets(const BWAPI::Unitset & meleeUnits, const BWAPI::Unitset & targets)
{
    BWAPI::Unitset meleeUnitTargets;
    for (BWAPI::Unit target : targets) 
    {
        if (!target->isFlying() &&
            target->getType() != BWAPI::UnitTypes::Zerg_Larva &&
            target->getType() != BWAPI::UnitTypes::Zerg_Egg &&
            !infestable(target) &&
            !target->isUnderDisruptionWeb())             // melee unit can't attack under dweb
        {
            meleeUnitTargets.insert(target);
        }
    }

    // Are any enemies in range to shoot at the melee units?
    bool underThreat = false;
    if (order->isCombatOrder())
    {
        underThreat = anyUnderThreat(meleeUnits);
    }

    for (BWAPI::Unit meleeUnit : meleeUnits)
    {
        // Try to avoid being hit by an undetected enemy dark templar.
        if (the.micro.fleeDT(meleeUnit))
        {
            continue;
        }

        if (order->isCombatOrder())
        {
            if (meleeUnitShouldRetreat(meleeUnit, targets))
            {
                BWAPI::Unit shieldBattery = InformationManager::Instance().nearestShieldBattery(meleeUnit->getPosition());
                if (false &&
                    shieldBattery &&
                    meleeUnit->getDistance(shieldBattery) < 400 &&
                    shieldBattery->getEnergy() >= 10)
                {
                    useShieldBattery(meleeUnit, shieldBattery);	// TODO not working yet
                }
                else
                {
                    // Clustering overrides the retreat once the melee unit retreats far enough to be outside
                    // attack range. So it rarely goes far. The retreat location rarely matters much.
                    BWAPI::Position fleeTo(the.bases.myMain()->getPosition());
                    the.micro.Move(meleeUnit, fleeTo);
                }
            }
            else
            {
                BWAPI::Unit target = getTarget(meleeUnit, meleeUnitTargets, underThreat);
                if (target)
                {
                    the.micro.CatchAndAttackUnit(meleeUnit, target);
                }
                else if (meleeUnit->getDistance(order->getPosition()) > 96)
                {
                    // There are no targets. Move to the order position if not already close.
                    the.micro.Move(meleeUnit, order->getPosition());
                }
            }
        }

        if (Config::Debug::DrawUnitTargets)
        {
            BWAPI::Broodwar->drawLineMap(meleeUnit->getPosition(), meleeUnit->getTargetPosition(),
                Config::Debug::ColorLineTarget);
        }
    }
}

// Choose a target from the set.
// underThreat is true if any of the melee units is under immediate threat of attack.
BWAPI::Unit MicroMelee::getTarget(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets, bool underThreat)
{
    int bestScore = INT_MIN;
    BWAPI::Unit bestTarget = nullptr;

    for (const auto target : targets)
    {
        const int priority = getAttackPriority(meleeUnit, target);		// 0..12
        const int range = meleeUnit->getDistance(target);				// 0..map size in pixels
        const int closerToGoal =										// positive if target is closer than us to the goal
            meleeUnit->getDistance(order->getPosition()) - target->getDistance(order->getPosition());

        // Skip targets that are too far away to worry about.
        if (range >= 13 * 32)
        {
            continue;
        }

        // TODO disabled - seems to be wrong, skips targets it should not
        // Don't chase targets that we can't catch.
        //if (!CanCatchUnit(meleeUnit, target))
        //{
        //	continue;
        //}

        // Let's say that 1 priority step is worth 64 pixels (2 tiles).
        // We care about unit-target range and target-order position distance.
        int score = 2 * 32 * priority - range;

        // Adjust for special features.

        // Prefer targets under dark swarm, on the expectation that then we'll be under it too.
        // It doesn't matter whether the target is a building.
        if (target->isUnderDarkSwarm())
        {
            if (meleeUnit->getType().isWorker())
            {
                // Workers can't hit under dark swarm. Skip this target.
                continue;
            }
            score += 4 * 32;
        }

        if (target->isUnderStorm())
        {
            score -= 6 * 32;
        }

        if (!underThreat)
        {
            // We're not under threat. Prefer to attack stuff outside enemy static defense range.
            if (!the.groundAttacks.inRange(target))
            {
                score += 2 * 32;
            }
            // Also prefer to attack stuff that can't shoot back.
            if (!UnitUtil::CanAttackGround(target))
            {
                score += 2 * 32;
            }
        }

        // A bonus for attacking enemies that are "in front".
        // It helps reduce distractions from moving toward the goal, the order position.
        if (closerToGoal > 0)
        {
            score += 2 * 32;
        }

        // This could adjust for relative speed and direction, so that we don't chase what we can't catch.
        if (meleeUnit->isInWeaponRange(target))
        {
            if (meleeUnit->getType() == BWAPI::UnitTypes::Zerg_Ultralisk)
            {
                score += 12 * 32;   // because they're big and awkward
            }
            else
            {
                score += 4 * 32;
            }
        }
        else if (!target->isMoving())
        {
            if (target->isSieged() ||
                target->getOrder() == BWAPI::Orders::Sieging ||
                target->getOrder() == BWAPI::Orders::Unsieging)
            {
                score += 48;
            }
            else
            {
                score += 32;
            }
        }
        else if (target->isBraking())
        {
            score += 16;
        }
        else if (target->getPlayer()->topSpeed(target->getType()) >= meleeUnit->getPlayer()->topSpeed(meleeUnit->getType()))
        {
            score -= 2 * 32;
        }

        // Prefer targets that are already hurt.
        if (target->getType().getRace() == BWAPI::Races::Protoss && target->getShields() == 0)
        {
            score += 32;
        }
        else if (target->getHitPoints() < target->getType().maxHitPoints())
        {
            score += 24;
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    return bestTarget;
}

int MicroMelee::getAttackPriority(BWAPI::Unit attacker, BWAPI::Unit target) const
{
    BWAPI::UnitType targetType = target->getType();

    // A ghost which is nuking is the highest priority by a mile.
    if (targetType == BWAPI::UnitTypes::Terran_Ghost &&
        target->getOrder() == BWAPI::Orders::NukePaint ||
        target->getOrder() == BWAPI::Orders::NukeTrack)
    {
        return 15;
    }

    // Exceptions for dark templar.
    if (attacker->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar)
    {
        if (targetType == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
        {
            return 10;
        }
        if ((targetType == BWAPI::UnitTypes::Terran_Missile_Turret || targetType == BWAPI::UnitTypes::Terran_Comsat_Station) &&
            (BWAPI::Broodwar->self()->deadUnitCount(BWAPI::UnitTypes::Protoss_Dark_Templar) == 0))
        {
            return 9;
        }
        if (targetType == BWAPI::UnitTypes::Zerg_Spore_Colony)
        {
            return 8;
        }
        if (targetType.isWorker())
        {
            return 8;
        }
    }

    // Short circuit: Enemy unit which is far enough outside its range is lower priority than a worker.
    int enemyRange = UnitUtil::GetAttackRange(target, attacker);
    if (enemyRange &&
        !targetType.isWorker() &&
        attacker->getDistance(target) > 32 + enemyRange)
    {
        return 8;
    }
    // Short circuit: Units before bunkers!
    if (targetType == BWAPI::UnitTypes::Terran_Bunker)
    {
        return 10;
    }
    // Medics and ordinary combat units. Include workers that are doing stuff.
    if (targetType == BWAPI::UnitTypes::Terran_Medic ||
        targetType == BWAPI::UnitTypes::Protoss_High_Templar ||
        targetType == BWAPI::UnitTypes::Zerg_Defiler ||
        UnitUtil::CanAttackGround(target) && !targetType.isWorker())  // includes cannons and sunkens
    {
        return 12;
    }
    if (targetType.isWorker() && (target->isRepairing() || target->isConstructing() || unitNearChokepoint(target)))
    {
        return 12;
    }
    // next priority is bored workers and turrets
    if (targetType.isWorker() || targetType == BWAPI::UnitTypes::Terran_Missile_Turret)
    {
        return 9;
    }

    return getBackstopAttackPriority(target);
}

// Retreat hurt units to allow them to regenerate health (zerg) or shields (protoss).
bool MicroMelee::meleeUnitShouldRetreat(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets)
{
    // Terran don't regen so it doesn't make sense to retreat.
    // NOTE We might want to retreat a firebat if medics are available.
    if (meleeUnit->getType().getRace() == BWAPI::Races::Terran)
    {
        return false;
    }

    // we don't want to retreat the melee unit if its shields or hit points are above the threshold set in the config file
    // set those values to zero if you never want the unit to retreat from combat individually
    if (meleeUnit->getShields() > Config::Micro::RetreatMeleeUnitShields || meleeUnit->getHitPoints() > Config::Micro::RetreatMeleeUnitHP)
    {
        return false;
    }

    // if there is a ranged enemy unit within attack range of this melee unit then we shouldn't bother retreating since it could fire and kill it anyway
    for (BWAPI::Unit unit : targets)
    {
        int groundWeaponRange = UnitUtil::GetAttackRange(unit, meleeUnit);
        if (groundWeaponRange >= 64 && unit->getDistance(meleeUnit) < groundWeaponRange)
        {
            return false;
        }
    }

    // A broodling should not retreat since it is on a timer.
    if (meleeUnit->getType() == BWAPI::UnitTypes::Zerg_Broodling)
    {
        return false;
    }

    return true;
}
