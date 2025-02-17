#include "CombatSimulation.h"

#include "FAP.h"
#include "The.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// Analyze our units to decide what kinds of enemy units count as their enemies.
// A unit that we can't hit and can't hit us is not an enemy.
// For example, both zerglings and zealots have ZerglingEnemies.
CombatSimEnemies CombatSimulation::analyzeForEnemies(const BWAPI::Unitset & units) const
{
    UAB_ASSERT(!units.empty(), "no units");

    bool nonscourge = false;
    bool hasGround = false;
    bool hasAir = false;
    bool hitsGround = false;
    bool hitsAir = false;

    for (BWAPI::Unit unit : units)
    {
        if (unit->getType() != BWAPI::UnitTypes::Zerg_Scourge)
        {
            nonscourge = true;
        }
        if (unit->isFlying())
        {
            hasAir = true;
        }
        else
        {
            hasGround = true;
        }
        if (UnitUtil::TypeCanAttackGround(unit->getType()))
        {
            hitsGround = true;
        }
        if (UnitUtil::TypeCanAttackAir(unit->getType()))
        {
            hitsAir = true;
        }

        if (hasGround && hasAir || hitsGround && hitsAir)
        {
            return CombatSimEnemies::AllEnemies;
        }
    }

    if (!nonscourge)
    {
        return CombatSimEnemies::ScourgeEnemies;
    }

    // We checked above that we don't have both ground and air, and can't hit both ground and air.
    // Exactly one of hasGround/hasAir is true. At most one of hitsGround/hitsAir is true.
    // Medics do not hit either ground or air.
    UAB_ASSERT(hasGround && !hasAir || hasAir && !hasGround, "air/ground mistake");

    if (hasGround && !hitsAir)
    {
        return CombatSimEnemies::ZerglingEnemies;
    }
    if (hasAir && !hitsAir)
    {
        return CombatSimEnemies::GuardianEnemies;
    }
    if (hasAir && !hitsGround)
    {
        return CombatSimEnemies::DevourerEnemies;
    }
    return CombatSimEnemies::AllEnemies;
}

// Are all of our units flyers?
bool CombatSimulation::allFlying(const BWAPI::Unitset & units) const
{
    for (BWAPI::Unit unit : units)
    {
        if (!unit->isFlying())
        {
            return false;
        }
    }
    return true;
}

void CombatSimulation::drawWhichEnemies(const BWAPI::Position & center) const
{
    std::string whichEnemies = "All Enemies";
    if (_whichEnemies == CombatSimEnemies::ZerglingEnemies) {
        whichEnemies = "Zergling Enemies";
    }
    else if (_whichEnemies == CombatSimEnemies::GuardianEnemies)
    {
        whichEnemies = "Guardian Enemies";
    }
    else if (_whichEnemies == CombatSimEnemies::DevourerEnemies)
    {
        whichEnemies = "Devourer Enemies";
    }
    else if (_whichEnemies == CombatSimEnemies::ScourgeEnemies)
    {
        whichEnemies = "Scourge Enemies";
    }
    BWAPI::Broodwar->drawTextMap(center + BWAPI::Position(0, 8), "%c %s", white, whichEnemies.c_str());
}

// Include an enemy unit (given by type) if we can hit it, or it can hit us.
bool CombatSimulation::includeEnemy(CombatSimEnemies which, BWAPI::UnitType type) const
{
    if (type.isSpell())
    {
        return false;
    }

    if (which == CombatSimEnemies::ZerglingEnemies)
    {
        // Ground enemies plus air enemies that can shoot down.
        // For combat sim with zergling-alikes: Ground units that cannot shoot air.
        return
            !type.isFlyer() || UnitUtil::TypeCanAttackGround(type);
    }

    if (which == CombatSimEnemies::GuardianEnemies)
    {
        // Ground enemies plus air enemies that can shoot air.
        // For combat sim with guardians: Air units that can only shoot ground.
        return
            !type.isFlyer() || UnitUtil::TypeCanAttackAir(type);
    }

    if (which == CombatSimEnemies::DevourerEnemies)
    {
        // Air enemies plus ground enemies that can shoot air.
        // For combat sim with devourer-alikes: Air units that can only shoot air.
        return
            type.isFlyer() || UnitUtil::TypeCanAttackAir(type);
    }

    if (which == CombatSimEnemies::ScourgeEnemies)
    {
        // Only ground enemies that can shoot up.
        // For scourge only. The scourge will take on air enemies no matter the odds.
        return
            !type.isFlyer() && UnitUtil::TypeCanAttackAir(type);
    }

    // AllEnemies.
    return true;
}

// This variant of includeEnemy() is called only when the enemy unit is visible.
// Our air units ignore undetected dark templar, since neither can hit the other.
// Burrowed units are not visible, so there's no need to ignore them.
bool CombatSimulation::includeEnemy(CombatSimEnemies which, BWAPI::Unit enemy) const
{
    if (_allFriendliesFlying &&
        enemy->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar &&
        !enemy->isDetected())
    {
        return false;
    }

    return includeEnemy(which, enemy->getType());
}

bool CombatSimulation::undetectedEnemy(BWAPI::Unit enemy) const
{
    if (enemy->isVisible())
    {
        return !enemy->isDetected();
    }

    // The enemy is out of vision.
    // Consider it undetected if it is likely to be cloaked, or (as an exceptional case) if it is an arbiter.
    // NOTE This will often be wrong!
    return
        enemy->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
        enemy->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
        enemy->getType() == BWAPI::UnitTypes::Protoss_Arbiter ||
        enemy->getType() == BWAPI::UnitTypes::Zerg_Lurker;
}

bool CombatSimulation::undetectedEnemy(const UnitInfo & enemyUI) const
{
    if (enemyUI.unit->isVisible())
    {
        return !enemyUI.unit->isDetected();
    }

    // The enemy is out of vision.
    // Consider it undetected if it is likely to be cloaked, or (as an exceptional case) if it is an arbiter.
    // NOTE This will often be wrong!
    return
        enemyUI.type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
        enemyUI.type == BWAPI::UnitTypes::Protoss_Dark_Templar ||
        enemyUI.type == BWAPI::UnitTypes::Protoss_Arbiter ||
        enemyUI.type == BWAPI::UnitTypes::Zerg_Lurker;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

CombatSimulation::CombatSimulation()
{
}

// Return the position of the closest enemy combat unit.
BWAPI::Position CombatSimulation::getClosestEnemyCombatUnit(const BWAPI::Position & center, int radius) const
{
    // NOTE The numbers match with Squad::unitNearEnemy().
    int closestDistance = radius + (the.info.enemyHasSiegeMode() ? 15 * 32 : 11 * 32);		// nothing farther than this

    BWAPI::Position closestEnemyPosition = BWAPI::Positions::Invalid;
    for (const auto & kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        const int dist = center.getApproxDistance(ui.lastPosition);
        if (dist < closestDistance &&
            !ui.goneFromLastPosition &&
            ui.isCompleted() &&
            ui.powered &&
            UnitUtil::IsCombatSimUnit(ui) &&
            includeEnemy(_whichEnemies, ui.type))
        {
            closestEnemyPosition = ui.lastPosition;
            closestDistance = dist;
        }
    }

    return closestEnemyPosition;
}

// Set up the combat sim state based on the given friendly units and the enemy units within a given circle.
// The circle center is the enemy combat unit closest to ourCenter, and the radius is passed in.
void CombatSimulation::setCombatUnits
    ( const BWAPI::Unitset & myUnits
    , const BWAPI::Position & ourCenter
    , int radius
    , bool visibleOnly
    )
{
    fap.clearState();

    // Center the circle of interest on the nearest enemy unit, not on one of our own units.
    // That reduces indecision: Enemy actions, not our own, induce us to move.
    BWAPI::Position center = getClosestEnemyCombatUnit(ourCenter, radius);
    if (!center.isValid())
    {
        // Do no combat sim, leave the state empty. It's fairly common.
        // The score will be 0, which counts as a win.
        // BWAPI::Broodwar->printf("no enemy near");
        return;
    }

    PlayerSnapshot snap;
    std::map<BWAPI::UnitType, int> & enemyCounts = snap.unitCounts;

    _whichEnemies = analyzeForEnemies(myUnits);
    _allFriendliesFlying = allFlying(myUnits);

    // If all enemies are cloaked and undetected, and can hit us,
    // then we can run away without needing to do a sim.
    _allEnemiesUndetected = true;       // until proven false
    _allEnemiesHitGroundOnly = true;    // until proven false

    // Work around poor play in mutalisks versus static defense:
    // We compensate by dropping a given number of our mutalisks.
    // Compensation only applies when visibleOnly is false.
    int compensatoryMutalisks = 0;

    // Add enemy units.
    if (visibleOnly)
    {
        // Static defense that is out of sight.
        std::vector<UnitInfo> enemyStaticDefense;
        InformationManager::Instance().getNearbyForce(enemyStaticDefense, center, the.enemy(), radius);
        for (const UnitInfo & ui : enemyStaticDefense)
        {
            if (ui.type.isBuilding() && !ui.unit->isVisible() && includeEnemy(_whichEnemies, ui.type))
            {
                _allEnemiesUndetected = false;
                if (UnitUtil::TypeCanAttackAir(ui.type))
                {
                    _allEnemiesHitGroundOnly = false;
                }
                fap.addIfCombatUnitPlayer2(ui);
                enemyCounts[ui.type] += 1;
                if (Config::Debug::DrawCombatSimulationInfo)
                {
                    BWAPI::Broodwar->drawCircleMap(ui.lastPosition, 3, BWAPI::Colors::Orange, true);
                }
            }
        }

        // Only units that we can see right now.
        BWAPI::Unitset enemyCombatUnits;
        MapGrid::Instance().getUnits(enemyCombatUnits, center, radius, false, true);
        for (BWAPI::Unit unit : enemyCombatUnits)
        {
            if (UnitUtil::IsCombatSimUnit(unit) &&
                includeEnemy(_whichEnemies, unit))
            {
                if (_allEnemiesUndetected && !undetectedEnemy(unit))
                {
                    _allEnemiesUndetected = false;
                }
                if (UnitUtil::TypeCanAttackAir(unit->getType()))
                {
                    _allEnemiesHitGroundOnly = false;
                }
                fap.addIfCombatUnitPlayer2(unit);
                enemyCounts[unit->getType()] += 1;
                if (Config::Debug::DrawCombatSimulationInfo)
                {
                    BWAPI::Broodwar->drawCircleMap(unit->getPosition(), 3, BWAPI::Colors::Orange, true);
                }
            }
        }
    }
    else
    {
        // All known enemy units, according to their most recently seen position.
        std::vector<UnitInfo> enemyCombatUnits;
        InformationManager::Instance().getNearbyForce(enemyCombatUnits, center, the.enemy(), radius);
        for (const UnitInfo & ui : enemyCombatUnits)
        {
            if (ui.unit && ui.unit->isVisible() ? includeEnemy(_whichEnemies, ui.unit) : includeEnemy(_whichEnemies, ui.type))
            {
                if (_allEnemiesUndetected && !undetectedEnemy(ui))
                {
                    _allEnemiesUndetected = false;
                }
                if (UnitUtil::TypeCanAttackAir(ui.type))
                {
                    _allEnemiesHitGroundOnly = false;
                }
                fap.addIfCombatUnitPlayer2(ui);
                enemyCounts[ui.type] += 1;

                if (ui.type == BWAPI::UnitTypes::Terran_Missile_Turret)
                {
                    compensatoryMutalisks += 2;
                }
                else if (ui.type == BWAPI::UnitTypes::Protoss_Photon_Cannon)
                {
                    compensatoryMutalisks += 1;
                }
                else if (ui.type == BWAPI::UnitTypes::Zerg_Spore_Colony)
                {
                    compensatoryMutalisks += 3;
                }

                if (Config::Debug::DrawCombatSimulationInfo)
                {
                    BWAPI::Broodwar->drawCircleMap(ui.lastPosition, 3, BWAPI::Colors::Red, true);
                }
            }
        }
    }

    // Remember the biggest battle.
    if (snap.getSupply() > biggestBattleEnemies.getSupply())
    {
        biggestBattleFrame = the.now();
        biggestBattleCenter = ourCenter;
        biggestBattleEnemies = snap;
    }

    // Add our units.
    // Add them from the input set. Other units have been given other instructions
    // and may not cooperate in the fight, so skip them.
    // NOTE This includes our static defense only if the caller passed it in!
    for (BWAPI::Unit unit : myUnits)
    {
        if (UnitUtil::IsCombatSimUnit(unit))
        {
            if (compensatoryMutalisks > 0 && unit->getType() == BWAPI::UnitTypes::Zerg_Mutalisk)
            {
                --compensatoryMutalisks;
            }
            else
            {
                fap.addIfCombatUnitPlayer1(unit);
                if (Config::Debug::DrawCombatSimulationInfo)
                {
                    BWAPI::Broodwar->drawCircleMap(unit->getPosition(), 3, BWAPI::Colors::Green, true);
                }
            }
        }
    }

    if (Config::Debug::DrawCombatSimulationInfo)
    {
        BWAPI::Broodwar->drawCircleMap(center, 6, BWAPI::Colors::Red, true);
        BWAPI::Broodwar->drawCircleMap(center, radius, BWAPI::Colors::Red);

        drawWhichEnemies(ourCenter + BWAPI::Position(-20, 28));
        BWAPI::Broodwar->drawTextMap(ourCenter + BWAPI::Position(-20, 44), "%c %s v %s%s", yellow,
            (_allFriendliesFlying ? "flyers" : ""),
            (_allEnemiesUndetected ? "unseen" : ""),
            (_allEnemiesHitGroundOnly ? "antiground" : "whatever"));
    }
}

// Simulate combat and return the result as a score. Score >= 0 means we win.
double CombatSimulation::simulateCombat(bool meatgrinder)
{
    std::pair<int, int> startScores = fap.playerScores();
    if (startScores.second == 0)
    {
        // No enemies. We win.
        return 0.01;
    }

    if (_allFriendliesFlying && _allEnemiesHitGroundOnly)
    {
        // The enemy can't hit us. We win.
        // It's true even for corner cases like guardians vs guardians.
        // NOTE This includes all cases of air units versus cloaked or burrowed units except enemy wraiths
        //      or units cloaked by an arbiter. Wraith or arbiter can hit both air and ground.
        //      The next check relies on this.
        return 0.02;
    }

    // If all enemies are undetected, and can hit us, we should run away.
    // If the check above passes, then any enemy cloaked units means the enemy can hit us,
    // so that part's done.
    if (_allEnemiesUndetected)
    {
        return -0.03;
    }

    fap.simulate();
    std::pair<int, int> endScores = fap.playerScores();

    const int myLosses = startScores.first - endScores.first;
    const int yourLosses = startScores.second - endScores.second;

    //BWAPI::Broodwar->printf("  p1 %d - %d = %d, p2 %d - %d = %d  ==>  %d",
    //	startScores.first, endScores.first, myLosses,
    //	startScores.second, endScores.second, yourLosses,
    //	(myLosses == 0) ? yourLosses : endScores.first - endScores.second);

    // If we lost nothing despite sending units in, it's a win (a draw counts as a win).
    // This is the most cautious possible loss comparison.
    if (myLosses == 0 && startScores.first > 0)
    {
        return double(yourLosses);
    }

    // Be more aggressive if requested. The setting is on the squad.
    // NOTE This tested poorly. I recommend against using it as it stands. - Jay
    if (meatgrinder)
    {
        // We only need to do a limited amount of damage to "win".
        // BWAPI::Broodwar->printf("  meatgrinder result = ", 3 * yourLosses - myLosses);

        // Call it a victory if we took down at least this fraction of the enemy army.
        return double(3 * yourLosses - myLosses);
        // return double(2 * endScores.first - endScores.second);
    }

    // Winner is the side with smaller losses.
    // return double(yourLosses - myLosses);

    // Original scoring: Winner is whoever has more stuff left.
    // NOTE This tested best for Steamhammer.
    return double(endScores.first - endScores.second);
}

// Simulate running away and return the proportion of our simulated losses, 0..1.
double CombatSimulation::simulateRetreat(const BWAPI::Position & retreatPosition)
{
    std::pair<int, int> startScores = fap.playerScores();
    if (startScores.second == 0)
    {
        // No enemies. We win.
        return 0.001;
    }

    if (_allFriendliesFlying && _allEnemiesHitGroundOnly)
    {
        // The enemy can't hit us. We win.
        return 0.002;
    }

    fap.simulateRetreat(retreatPosition);
    std::pair<int, int> endScores = fap.playerScores();

    const int myLosses = startScores.first - endScores.first;
    const int yourLosses = startScores.second - endScores.second;

    /*
    BWAPI::Broodwar->printf("%d - %d = %d ==>  %g",
        startScores.first, endScores.first, myLosses,
        double(myLosses) / startScores.first);
    */

    return double(myLosses) / startScores.first;
}
