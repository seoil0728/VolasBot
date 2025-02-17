#include "MicroScourge.h"

#include "InformationManager.h"
#include "The.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// -----------------------------------------------------------------------------------------

MicroScourge::MicroScourge()
{
}

void MicroScourge::executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster)
{
    BWAPI::Unitset units = Intersection(getUnits(), cluster.units);
    if (units.empty())
    {
        return;
    }

    assignTargets(units, targets);
}

void MicroScourge::assignTargets(const BWAPI::Unitset & scourge, const BWAPI::Unitset & targets)
{
    // The set of potential targets.
    BWAPI::Unitset scourgeTargets;
    for (BWAPI::Unit target : targets)
    {
        if (target->isFlying() &&
            target->getType() != BWAPI::UnitTypes::Protoss_Interceptor &&
            target->getType() != BWAPI::UnitTypes::Zerg_Overlord &&
            !target->getType().isBuilding() &&
            !the.airAttacks.inRange(target->getTilePosition()))     // skip defended targets
        {
            scourgeTargets.insert(target);
        }
    }

    for (BWAPI::Unit scourgeUnit : scourge)
    {
        BWAPI::Unit target = getTarget(scourgeUnit, scourgeTargets);
        if (target)
        {
            // A target was found. Attack it.
            if (Config::Debug::DrawUnitTargets)
            {
                BWAPI::Broodwar->drawLineMap(scourgeUnit->getPosition(), scourgeUnit->getTargetPosition(), BWAPI::Colors::Blue);
            }

            the.micro.CatchAndAttackUnit(scourgeUnit, target);
        }
        else
        {
            // No target found. If we're not near the order position, go there.
            // Use Move (not AttackMove) so that we don't attack overlords and such along the way.
            if (scourgeUnit->getDistance(order->getPosition()) > 3 * 32)
            {
                the.micro.MoveNear(scourgeUnit, order->getPosition());
                if (Config::Debug::DrawUnitTargets)
                {
                    BWAPI::Broodwar->drawLineMap(scourgeUnit->getPosition(), order->getPosition(), BWAPI::Colors::Orange);
                }
            }
        }
    }
}

BWAPI::Unit MicroScourge::getTarget(BWAPI::Unit scourge, const BWAPI::Unitset & targets)
{
    int bestScore = INT_MIN;
    BWAPI::Unit bestTarget = nullptr;

    for (BWAPI::Unit target : targets)
    {
        const int priority = getAttackPriority(target->getType());	// 0..12
        const int range = scourge->getDistance(target);				// 0..map diameter in pixels

        // Let's say that 1 priority step is worth 3 tiles.
        // We care about unit-target range and target-order position distance.
        int score = 3 * 32 * priority - range;

        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    return bestTarget;
}

int MicroScourge::getAttackPriority(BWAPI::UnitType targetType)
{
    if (targetType == BWAPI::UnitTypes::Zerg_Cocoon ||
        targetType == BWAPI::UnitTypes::Zerg_Guardian)
    {
        // Helpless and valuable.
        return 10;
    }
    if (targetType == BWAPI::UnitTypes::Terran_Science_Vessel ||
        targetType == BWAPI::UnitTypes::Terran_Valkyrie ||
        targetType == BWAPI::UnitTypes::Protoss_Carrier ||
        targetType == BWAPI::UnitTypes::Protoss_Arbiter ||
        targetType == BWAPI::UnitTypes::Zerg_Devourer)
    {
        // Capital ships that are mostly vulnerable.
        return 9;
    }
    if (targetType == BWAPI::UnitTypes::Terran_Dropship ||
        targetType == BWAPI::UnitTypes::Protoss_Shuttle ||
        targetType == BWAPI::UnitTypes::Zerg_Queen)
    {
        // Transports other than overlords, plus queens. They are important and defenseless.
        return 8;
    }
    if (targetType == BWAPI::UnitTypes::Terran_Battlecruiser ||
        targetType == BWAPI::UnitTypes::Protoss_Scout)
    {
        // Capital ships that can shoot back efficiently.
        return 7;
    }
    if (targetType == BWAPI::UnitTypes::Terran_Wraith ||
        targetType == BWAPI::UnitTypes::Protoss_Corsair ||
        targetType == BWAPI::UnitTypes::Zerg_Mutalisk)
    {
        // Lesser flyers that can shoot back.
        return 5;
    }
    if (targetType == BWAPI::UnitTypes::Protoss_Observer)
    {
        // Higher priority if we have burrow or lurker tech.
        return
            (BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Burrowing) ||
            BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Lurker_Aspect))
                ? 7
                : 5;
    }

    // Overlords, scourge, interceptors, floating buildings.
    return 0;
}
