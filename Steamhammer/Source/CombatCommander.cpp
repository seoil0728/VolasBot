#include <queue>

#include "CombatCommander.h"

#include "Bases.h"
#include "BuildingManager.h"
#include "BuildingPlacer.h"
#include "MapGrid.h"
#include "Micro.h"
#include "Random.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// Squad priorities: Which can steal units from others.
// Anyone can steal from the Idle squad.
const size_t IdlePriority = 0;
const size_t OverlordPriority = 1;
const size_t AttackPriority = 2;
const size_t ReconPriority = 3;
const size_t BaseDefensePriority = 4;
const size_t ScoutDefensePriority = 5;
const size_t WatchPriority = 6;
const size_t DropPriority = 7;			// don't steal from Drop squad for anything else
const size_t ScourgePriority = 8;		// all scourge and only scourge
const size_t IrradiatedPriority = 9;    // irradiated organic units

// The attack squads.
const int DefendFrontRadius = 400;
const int AttackRadius = 800;

// Reconnaissance squad.
const int ReconTargetTimeout = 40 * 24;
const int ReconRadius = 400;

// If there are this many carriers or more, carriers are in the flying squad
// and act independently.
const int CarrierIndependenceCount = 4;

CombatCommander::CombatCommander() 
    : _initialized(false)
    , _goAggressive(true)
    , _isWatching(false)
    , _reconSquadAlive(false)
    , _reconTarget(nullptr)   // it will be set when the squad is filled
    , _lastReconTargetChange(0)
    , _carrierCount(0)
{
}

// Called once at the start of the game.
// You can also create new squads at other times.
void CombatCommander::initializeSquads()
{
    // The idle squad includes workers at work (not idle at all) and unassigned units.
    _squadData.createSquad("Idle", IdlePriority).getOrder().setStatus("Work");

    // These squads don't care what order they are given.
    // They analyze the situation for themselves.
    if (the.self()->getRace() == BWAPI::Races::Zerg)
    {
        // The overlord squad has only overlords, but not all overlords.
        // They may be assigned elsewhere too.
        _squadData.createSquad("Overlord", OverlordPriority).getOrder().setStatus("Look");

        // The scourge squad has all the scourge.
        if (the.selfRace() == BWAPI::Races::Zerg)
        {
            _squadData.createSquad("Scourge", ScourgePriority).getOrder().setStatus("Wait");
        }
    }
    
    // The ground squad will pressure an enemy base.
    _squadData.createSquad("Ground", AttackPriority);

    // The flying squad separates air units so they can act independently.
    _squadData.createSquad("Flying", AttackPriority);

    // The recon squad carries out reconnaissance in force to deny enemy bases.
    // It is filled in when enough units are available.
    Squad & reconSquad = _squadData.createSquad("Recon", ReconPriority);
    reconSquad.setOrder(SquadOrder("Recon"));
    reconSquad.setCombatSimRadius(200);  // combat sim includes units in a smaller radius than for a combat squad
    // reconSquad.setFightVisible(true);    // combat sim sees only visible enemy units (not all known enemies)

    BWAPI::Position ourBasePosition = BWAPI::Position(the.self()->getStartLocation());

    // The scout defense squad chases the enemy worker scout.
    if (Config::Micro::ScoutDefenseRadius > 0)
    {
        _squadData.createSquad("ScoutDefense", ScoutDefensePriority).
            setOrder(SquadOrder(SquadOrderTypes::Defend, ourBasePosition, Config::Micro::ScoutDefenseRadius, false, "Stop that scout"));
    }

    // If we're expecting to drop, create a drop squad.
    // It is initially ordered to hold ground until it can load up and go.
    if (StrategyManager::Instance().dropIsPlanned())
    {
        _squadData.createSquad("Drop", DropPriority).
            setOrder(SquadOrder(SquadOrderTypes::Hold, ourBasePosition, AttackRadius, false, "Wait for transport"));
    }
}

void CombatCommander::update(const BWAPI::Unitset & combatUnits)
{
    if (!_initialized)
    {
        initializeSquads();
        _initialized = true;
    }

    _combatUnits = combatUnits;

    int frame8 = the.now() % 8;

    if (frame8 == 1)
    {
        updateIdleSquad();
        updateIrradiatedSquad();
        updateOverlordSquad();
        updateScourgeSquad();
        updateDropSquads();
        updateScoutDefenseSquad();
        updateBaseDefenseSquads();
        updateWatchSquads();
        updateReconSquad();
        updateAttackSquads();
    }
    else if (frame8 % 4 == 2)
    {
        doComsatScan();
    }

    if (the.now() % 20 == 1)
    {
        doLarvaTrick();
    }

    loadOrUnloadBunkers();

    //the.ops.update();

    _squadData.update();          // update() all the squads

    cancelDyingItems();
}

// Clean up any data structures that may otherwise not be unwound in the correct order.
// This fixes an end-of-game bug diagnosed by Bruce Nielsen.
// This onEnd() doesn't care who won.
void CombatCommander::onEnd()
{
    _squadData.clearSquadData();
}

// Called by LurkerSkill.
void CombatCommander::setGeneralLurkerTactic(LurkerTactic tactic)
{
    _lurkerOrders.generalTactic = tactic;
}

// Called by LurkerSkill.
void CombatCommander::addLurkerOrder(LurkerOrder & order)
{
    _lurkerOrders.orders[order.tactic] = order;
}

// Called by LurkerSkill.
void CombatCommander::clearLurkerOrder(LurkerTactic tactic)
{
    _lurkerOrders.orders.erase(tactic);
}

void CombatCommander::updateIdleSquad()
{
    Squad & idleSquad = _squadData.getSquad("Idle");
    for (BWAPI::Unit unit : _combatUnits)
    {
        // if it hasn't been assigned to a squad yet, put it in the low priority idle squad
        if (_squadData.canAssignUnitToSquad(unit, idleSquad))
        {
            _squadData.assignUnitToSquad(unit, idleSquad);
        }
    }
}

// Put irradiated organic units into the Irradiated squad.
// Exceptions: Queens and defilers.
void CombatCommander::updateIrradiatedSquad()
{
    Squad * radSquad = nullptr;
    if (_squadData.squadExists("Irradiated"))
    {
        radSquad = & _squadData.getSquad("Irradiated");
    }

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (unit->isIrradiated() && unit->getType().isOrganic() && (!radSquad || !radSquad->containsUnit(unit)))
        {
            if (unit->getType() == BWAPI::UnitTypes::Zerg_Queen && unit->getEnergy() > 65 ||
                unit->getType() == BWAPI::UnitTypes::Zerg_Defiler && the.self()->hasResearched(BWAPI::TechTypes::Consume))
            {
                // Ignore these spellcasters. They may want to cast while irradiated.
            }
            else
            {
                if (!radSquad)
                {
                    _squadData.createSquad("Irradiated", IrradiatedPriority).getOrder().setStatus("Ouch!");
                    radSquad = & _squadData.getSquad("Irradiated");
                }

                _squadData.assignUnitToSquad(unit, *radSquad);
            }
        }
        else if (!unit->isIrradiated() && radSquad && radSquad->containsUnit(unit))
        {
            // Irradiation wore off. Reassign the unit to the Idle squad.
            // It will be reassigned onward the same frame, since this is called first.
            _squadData.assignUnitToSquad(unit, _squadData.getSquad("Idle"));
        }
    }

    if (radSquad && radSquad->isEmpty())
    {
        _squadData.removeSquad("Irradiated");
    }
}

// Put all overlords which are not otherwise assigned into the Overlord squad.
void CombatCommander::updateOverlordSquad()
{
    // If we don't have an overlord squad, then do nothing.
    // It is created in initializeSquads().
    if (!_squadData.squadExists("Overlord"))
    {
        return;
    }

    Squad & ovieSquad = _squadData.getSquad("Overlord");
    for (BWAPI::Unit unit : _combatUnits)
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Overlord && _squadData.canAssignUnitToSquad(unit, ovieSquad))
        {
            _squadData.assignUnitToSquad(unit, ovieSquad);
        }
    }
}

void CombatCommander::chooseScourgeTarget(const Squad & sourgeSquad)
{
    BWAPI::Position center = sourgeSquad.calcCenter();

    BWAPI::Position bestTarget = the.bases.myMain()->getPosition();
    int bestScore = INT_MIN;

    for (const auto & kv : the.info.getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        // Skip inappropriate units and units known to have moved away some time ago.
        // Also stay out of range of enemy static air defense.
        if (!ui.type.isFlyer() ||           // excludes all buildings, including lifted buildings
            ui.type.isSpell() ||
            ui.type == BWAPI::UnitTypes::Protoss_Interceptor ||
            ui.type == BWAPI::UnitTypes::Zerg_Overlord ||
            ui.goneFromLastPosition && the.now() - ui.updateFrame > 5 * 24 ||
            the.airAttacks.inRange(BWAPI::TilePosition(ui.lastPosition)))
        {
            continue;
        }

        int score = MicroScourge::getAttackPriority(ui.type);

        if (ui.unit && ui.unit->isVisible())
        {
            score += 2;
        }

        // Each score increment is worth 2 tiles of distance.
        const int distance = center.getApproxDistance(ui.lastPosition);
        score -= distance / 16;
        if (score > bestScore)
        {
            bestTarget = ui.lastPosition;
            bestScore = score;
        }
    }

    _scourgeTarget = bestTarget;
}

// Put all scourge into the Scourge squad.
void CombatCommander::updateScourgeSquad()
{
    // If we don't have a scourge squad, then do nothing.
    // It is created in initializeSquads() for zerg only.
    if (!_squadData.squadExists("Scourge"))
    {
        return;
    }

    Squad & scourgeSquad = _squadData.getSquad("Scourge");

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Scourge && _squadData.canAssignUnitToSquad(unit, scourgeSquad))
        {
            _squadData.assignUnitToSquad(unit, scourgeSquad);
        }
    }

    // We want an overlord to come along if the enemy has arbiters or cloaked wraiths,
    // but only if we have overlord speed.
    bool wantDetector =
        the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Pneumatized_Carapace) > 0 &&
        the.info.enemyHasAirCloakTech();
    maybeAssignDetector(scourgeSquad, wantDetector);

    // Issue the order.
    chooseScourgeTarget(scourgeSquad);
    scourgeSquad.setOrder(SquadOrder(SquadOrderTypes::OmniAttack, _scourgeTarget, 300, false, _scourgeTarget == the.bases.myMain()->getPosition() ? "Stand by" : "Chase"));
}

// Comparison function for a sort done below.
bool compareFirst(const std::pair<int, Base *> & left, const std::pair<int, Base *> & right)
{
    return left.first < right.first;
}

// Update the watch squads, which set a sentry in each free base to see enemy expansions
// and possibly stop them. It also clears spider mines as a side effect.
// One squad per base being watched: A free base may get a watch squad of 1 zergling.
// For now, only zerg keeps watch squads, and only when units are available.
void CombatCommander::updateWatchSquads()
{
    // Only if we're zerg. Not implemented for other races.
    if (the.selfRace() != BWAPI::Races::Zerg)
    {
        return;
    }

    // We choose bases to watch relative to the enemy start, so we must know it first.
    if (!the.bases.enemyStart())
    {
        return;
    }

    // What to assign to Watch squads.
    const bool hasBurrow = the.self()->hasResearched(BWAPI::TechTypes::Burrowing);
    const int nLings = the.my.completed.count(BWAPI::UnitTypes::Zerg_Zergling);
    const int groundStrength =
        nLings +
        the.my.completed.count(BWAPI::UnitTypes::Zerg_Hydralisk) +
        2 * the.my.completed.count(BWAPI::UnitTypes::Zerg_Lurker) +
        3 * the.my.completed.count(BWAPI::UnitTypes::Zerg_Ultralisk);
    const int perWatcher = (hasBurrow && the.enemyRace() != BWAPI::Races::Zerg) ? 9 : 12;
    if (nLings == 0 || the.bases.freeLandBaseCount() == 0)
    {
        // We have either nothing to watch with, or nothing to watch over.
        _isWatching = false;
    }

    // When _isWatching is set, we ensure at least one watching zergling (if any exist).
    // Otherwise we might disband the most important squad intermittently, losing its value.
    int nWatchers = std::min(nLings, Clip(groundStrength / perWatcher, _isWatching ? 1 : 0, hasBurrow ? 4 : 2));

    // Sort free bases by nearness to enemy main, which must be known (we check above).
    // Distance scores for good bases, score -1 for others.
    // NOTE If the enemy main is destroyed, it becomes the top priority for watching.
    std::vector<std::pair<int, Base *>> baseScores;
    for (Base * base : the.bases.getAll())
    {
        if (nWatchers > 0 &&
            base->getOwner() == BWAPI::Broodwar->neutral() &&
            the.bases.connectedToStart(base->getTilePosition()) &&
            !base->isReserved() &&
            !the.placer.isReserved(base->getTilePosition()) &&
            !BuildingManager::Instance().isBasePlanned(base) &&
            (the.enemyRace() == BWAPI::Races::Terran || the.groundAttacks.at(base->getCenterTile()) == 0))
        {
            baseScores.push_back(std::pair<int, Base *>(base->getTileDistance(the.bases.enemyStart()->getTilePosition()), base));
        }
        else
        {
            baseScores.push_back(std::pair<int, Base *>(-1, base));
        }
    }
    std::stable_sort(baseScores.begin(), baseScores.end(), compareFirst);
    
    for (std::pair<int, Base *> baseScore : baseScores)
    {
        Base * base = baseScore.second;
        int score = baseScore.first;

        std::stringstream squadName;
        BWAPI::TilePosition tile(base->getTilePosition() + BWAPI::TilePosition(2, 1));
        squadName << "Watch " << tile.x << "," << tile.y;

        if (score < 0 && !_squadData.squadExists(squadName.str()))
        {
            continue;
        }

        // We need the squad object. Create it if it's not already there.
        if (!_squadData.squadExists(squadName.str()))
        {
            _squadData.createSquad(squadName.str(), WatchPriority).
                setOrder(SquadOrder(SquadOrderTypes::Watch, base, 0, true, "Watch"));
        }
        Squad & watchSquad = _squadData.getSquad(squadName.str());
        watchSquad.setCombatSimRadius(128);     // small radius
        watchSquad.setFightVisible(true);       // combat sime sees only visible enemy units (not all known enemies)

        // Add or remove the squad's watcher unit, or sentry.
        bool hasWatcher = watchSquad.containsUnitType(BWAPI::UnitTypes::Zerg_Zergling);
        if (hasWatcher)
        {
            if (score < 0 || nWatchers <= 0)
            {
                // Has watcher and should not.
                for (BWAPI::Unit unit : watchSquad.getUnits())
                {
                    if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
                    {
                        watchSquad.removeUnit(unit);
                        break;
                    }
                }
            }
            else
            {
                // Has watcher as it should. Count it.
                --nWatchers;
            }
        }
        else
        {
            if (score >= 0 && nWatchers > 0)
            {
                // Has no watcher and should have one.
                for (BWAPI::Unit unit : _combatUnits)
                {
                    if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling &&
                        _squadData.canAssignUnitToSquad(unit, watchSquad))
                    {
                        _squadData.assignUnitToSquad(unit, watchSquad);
                        --nWatchers;		// we used one up
                        // If we have burrow, we want to keep watching at least one neutral base. Flag it.
                        if (hasBurrow)
                        {
                            _isWatching = true;
                        }
                        break;
                    }
                }
            }
        }

        // Drop the squad if it is no longer needed. Don't clutter the squad display.
        if (watchSquad.isEmpty())
        {
            _squadData.removeSquad(squadName.str());
        }
    }
}

// Update the small recon squad which tries to find and deny enemy bases.
// Units available to the recon squad each have a "weight".
// Weights sum to no more than MaxWeight, set below.
void CombatCommander::updateReconSquad()
{
    // The Recon squad is not needed very early in the game.
    // If rushing, we want to dedicate all units to the rush.
    if (the.now() < 6 * 24 * 60)
    {
        return;
    }

    const int MaxWeight = 12;
    Squad & reconSquad = _squadData.getSquad("Recon");

    chooseReconTarget(reconSquad);

    // If nowhere needs seeing, disband the squad. We're done for now.
    // It can happen that the Watch squad sees all bases, meeting this condition.
    if (!_reconTarget)
    {
        reconSquad.clear();
        _reconSquadAlive = false;
        return;
    }

    // Issue the order.
    reconSquad.setOrder(SquadOrder(SquadOrderTypes::Attack, _reconTarget, ReconRadius, true, "Reconnaissance in force"));

    // Special case: If we started on an island, then the recon squad consists
    // entirely of one overlord--if we're lucky enough to be zerg.
    if (the.bases.isIslandStart())
    {
        if (reconSquad.getUnits().size() == 0)
        {
            for (BWAPI::Unit unit : _combatUnits)
            {
                if (unit->getType().isDetector() && _squadData.canAssignUnitToSquad(unit, reconSquad))
                {
                    _squadData.assignUnitToSquad(unit, reconSquad);
                    break;
                }
            }
        }
        _reconSquadAlive = !reconSquad.isEmpty();
        return;
    }

    // What is already in the squad?
    int squadWeight = 0;
    int nMarines = 0;
    int nMedics = 0;
    for (BWAPI::Unit unit : reconSquad.getUnits())
    {
        squadWeight += weighReconUnit(unit);
        if (unit->getType() == BWAPI::UnitTypes::Terran_Marine)
        {
            ++nMarines;
        }
        else if (unit->getType() == BWAPI::UnitTypes::Terran_Medic)
        {
            ++nMedics;
        }
    }

    // If everything except the detector is gone, let the detector go too.
    // It can't carry out reconnaissance in force.
    if (squadWeight == 0 && !reconSquad.isEmpty())
    {
        reconSquad.clear();
    }

    // What is available to put into the squad?
    int availableWeight = 0;
    for (BWAPI::Unit unit : _combatUnits)
    {
        availableWeight += weighReconUnit(unit);
    }

    // The allowed weight of the recon squad. It should steal few units.
    int weightLimit = availableWeight >= 24
        ? 2 + (availableWeight - 24) / 6
        : 0;
    if (weightLimit > MaxWeight)
    {
        weightLimit = MaxWeight;
    }

    // If the recon squad weighs more than it should, clear it and continue.
    // Also if all marines are gone, but medics remain.
    // Units will be added back in if they should be.
    if (squadWeight > weightLimit ||
        nMarines == 0 && nMedics > 0)
    {
        reconSquad.clear();
        squadWeight = 0;
        nMarines = nMedics = 0;
    }

    bool hasDetector = reconSquad.hasDetector();
    bool wantDetector = wantSquadDetectors();

    if (hasDetector && !wantDetector)
    {
        for (BWAPI::Unit unit : reconSquad.getUnits())
        {
            if (unit->getType().isDetector())
            {
                reconSquad.removeUnit(unit);
                break;
            }
        }
        hasDetector = false;
    }

    // Add units up to the weight limit.
    // In this loop, add no medics, and few enough marines to allow for 2 medics.
    for (BWAPI::Unit unit : _combatUnits)
    {
        if (squadWeight >= weightLimit)
        {
            break;
        }
        BWAPI::UnitType type = unit->getType();
        int weight = weighReconUnit(type);
        if (weight > 0 && squadWeight + weight <= weightLimit && _squadData.canAssignUnitToSquad(unit, reconSquad))
        {
            if (type == BWAPI::UnitTypes::Terran_Marine)
            {
                if (nMarines * weight < MaxWeight - 2 * weighReconUnit(BWAPI::UnitTypes::Terran_Medic))
                {
                    _squadData.assignUnitToSquad(unit, reconSquad);
                    squadWeight += weight;
                    nMarines += 1;
                }
            }
            else if (type != BWAPI::UnitTypes::Terran_Medic)
            {
                _squadData.assignUnitToSquad(unit, reconSquad);
                squadWeight += weight;
            }
        }
        else if (type.isDetector() && wantDetector && !hasDetector && _squadData.canAssignUnitToSquad(unit, reconSquad))
        {
            _squadData.assignUnitToSquad(unit, reconSquad);
            hasDetector = true;
        }
    }

    // Finally, fill in any needed medics.
    if (nMarines > 0 && nMedics < 2)
    {
        for (BWAPI::Unit unit : _combatUnits)
        {
            if (squadWeight >= weightLimit || nMedics >= 2)
            {
                break;
            }
            if (unit->getType() == BWAPI::UnitTypes::Terran_Medic &&
                _squadData.canAssignUnitToSquad(unit, reconSquad))
            {
                _squadData.assignUnitToSquad(unit, reconSquad);
                squadWeight += weighReconUnit(BWAPI::UnitTypes::Terran_Medic);
                nMedics += 1;
            }
        }
    }

    _reconSquadAlive = !reconSquad.isEmpty();
}

// The recon squad is allowed up to a certain "weight" of units.
int CombatCommander::weighReconUnit(const BWAPI::Unit unit) const
{
    return weighReconUnit(unit->getType());
}

// The recon squad is allowed up to a certain "weight" of units.
int CombatCommander::weighReconUnit(const BWAPI::UnitType type) const
{
    if (type == BWAPI::UnitTypes::Zerg_Zergling) return 2;
    if (type == BWAPI::UnitTypes::Zerg_Hydralisk) return 3;
    if (type == BWAPI::UnitTypes::Terran_Marine) return 2;
    if (type == BWAPI::UnitTypes::Terran_Medic) return 2;
    if (type == BWAPI::UnitTypes::Terran_Vulture) return 4;
    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) return 6;
    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) return 6;
    if (type == BWAPI::UnitTypes::Protoss_Zealot) return 4;
    if (type == BWAPI::UnitTypes::Protoss_Dragoon) return 4;
    if (type == BWAPI::UnitTypes::Protoss_Dark_Templar) return 4;

    return 0;
}

// Keep the same reconnaissance target or switch to a new one, depending.
void CombatCommander::chooseReconTarget(const Squad & reconSquad)
{
    bool change = false;       // switch targets?

    Base * nextTarget = getReconLocation();

    // There is nowhere that we need to see. Change to the invalid target.
    if (!nextTarget)
    {
        change = true;
    }

    // If the current target is invalid, find one.
    else if (!_reconTarget)
    {
        change = true;
    }

    // If we have spent too long on one target, then probably we haven't been able to reach it.
    else if (the.now() - _lastReconTargetChange >= ReconTargetTimeout)
    {
        change = true;
    }

    // The recon squad just lost all its members, a sign that it was going somewhere too dangerous.
    // Though it could be that they were all assigned back to the Ground squad.
    else if (_reconSquadAlive && reconSquad.isEmpty())
    {
        change = true;
    }

    // If the target is in sight (of any unit, not only the recon squad) and empty of enemies, we're done.
    else if (BWAPI::Broodwar->isVisible(_reconTarget->getCenterTile().x, _reconTarget->getCenterTile().y))
    {
        BWAPI::Unitset enemies;
        MapGrid::Instance().getUnits(enemies, _reconTarget->getCenter(), ReconRadius, false, true);
        // We don't particularly care about air units, even when we could engage them.
        for (auto it = enemies.begin(); it != enemies.end(); )
        {
            if ((*it)->isFlying())
            {
                it = enemies.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (enemies.empty())
        {
            change = true;
        }
    }

    if (change)
    {
        _reconTarget = nextTarget;
        _lastReconTargetChange = the.now();
    }
}

// Choose an empty base location for the recon squad to check out, or null if none.
// Called only by setReconTarget().
Base * CombatCommander::getReconLocation() const
{
    std::vector<Base *> choices;

    BWAPI::Position startPosition = the.bases.myStart()->getPosition();

    // The choices are neutral bases reachable by ground and not currently in view.
    // Or, if we started on an island, the choices are neutral bases anywhere.
    for (Base * base : the.bases.getAll())
    {
        if (base->owner == BWAPI::Broodwar->neutral() &&
            !base->isVisible() &&
            (the.bases.isIslandStart() || the.bases.connectedToStart(base->getTilePosition())))
        {
            choices.push_back(base);
        }
    }

    if (choices.empty())
    {
        return nullptr;
    }

    // Choose randomly.
    // We may choose the same target we already have. That's OK; if there's another choice,
    // we'll probably switch to it soon.
    Base * base = choices.at(Random::Instance().index(choices.size()));
    return base;
}

// Form the ground squad and the flying squad, the main attack squads.
// NOTE Arbiters and guardians go into the ground squad.
//      Devourers are flying squad if it exists, otherwise ground.
//		Carriers are flying squad if it already exists or if the carrier count is high enough.
//      Other air units always go into the flying squad (except scourge, they are in their own squad).
void CombatCommander::updateAttackSquads()
{
    Squad & groundSquad = _squadData.getSquad("Ground");
    Squad & flyingSquad = _squadData.getSquad("Flying");

    // _carrierCount is used only here and in dependencies.
    _carrierCount = the.my.completed.count(BWAPI::UnitTypes::Protoss_Carrier);

    bool groundSquadExists = groundSquad.hasCombatUnits();

    bool flyingSquadExists = false;
    for (BWAPI::Unit unit : flyingSquad.getUnits())
    {
        if (isFlyingSquadUnit(unit->getType()))
        {
            flyingSquadExists = true;
            break;
        }
    }

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (isFlyingSquadUnit(unit->getType()))
        {
            if (_squadData.canAssignUnitToSquad(unit, flyingSquad))
            {
                _squadData.assignUnitToSquad(unit, flyingSquad);
            }
        }

        // Certain flyers go into the flying squad only if it already exists.
        // Otherwise they go into the ground squad.
        else if (isOptionalFlyingSquadUnit(unit->getType()))
        {
            if (flyingSquadExists)
            {
                if (groundSquad.containsUnit(unit))
                {
                    groundSquad.removeUnit(unit);
                }
                if (_squadData.canAssignUnitToSquad(unit, flyingSquad))
                {
                    _squadData.assignUnitToSquad(unit, flyingSquad);
                }
            }
            else
            {
                if (flyingSquad.containsUnit(unit))
                {
                    flyingSquad.removeUnit(unit);
                }
                if (_squadData.canAssignUnitToSquad(unit, groundSquad))
                {
                    _squadData.assignUnitToSquad(unit, groundSquad);
                }
            }
        }

        // isGroundSquadUnit() is defined as a catchall, so it has to go last.
        else if (isGroundSquadUnit(unit->getType()))
        {
            if (_squadData.canAssignUnitToSquad(unit, groundSquad))
            {
                _squadData.assignUnitToSquad(unit, groundSquad);
            }
        }
    }

    // Add or remove detectors.
    bool wantDetector = wantSquadDetectors();
    maybeAssignDetector(groundSquad, wantDetector);
    maybeAssignDetector(flyingSquad, wantDetector);

    groundSquad.setOrder(getAttackOrder(&groundSquad));
    groundSquad.setLurkerTactic(_lurkerOrders.generalTactic);

    flyingSquad.setOrder(getAttackOrder(&flyingSquad));
}

// Unit definitely belongs in the Flying squad.
bool CombatCommander::isFlyingSquadUnit(const BWAPI::UnitType type) const
{
    return
        type == BWAPI::UnitTypes::Zerg_Mutalisk ||
        type == BWAPI::UnitTypes::Terran_Wraith ||
        type == BWAPI::UnitTypes::Terran_Valkyrie ||
        type == BWAPI::UnitTypes::Terran_Battlecruiser ||
        type == BWAPI::UnitTypes::Protoss_Corsair ||
        type == BWAPI::UnitTypes::Protoss_Scout ||
        _carrierCount >= CarrierIndependenceCount && type == BWAPI::UnitTypes::Protoss_Carrier;
}

// Unit belongs in the Flying squad if the Flying squad exists, otherwise the Ground squad.
// If carriers are independent, then the flying squad exists.
bool CombatCommander::isOptionalFlyingSquadUnit(const BWAPI::UnitType type) const
{
    return
        type == BWAPI::UnitTypes::Zerg_Devourer ||
        type == BWAPI::UnitTypes::Protoss_Carrier;
}

// Unit belongs in the ground squad.
// With the current definition, it includes everything except workers, so it captures
// everything that is not already taken. It must be the last condition checked.
// NOTE This includes queens.
bool CombatCommander::isGroundSquadUnit(const BWAPI::UnitType type) const
{
    return
        !type.isDetector() &&
        !type.isWorker();
}

// Despite the name, this supports only 1 drop squad which has 1 transport.
// Furthermore, it can only drop once and doesn't know how to reset itself to try again.
// Still, it's a start and it can be effective.
void CombatCommander::updateDropSquads()
{
    // If we don't have a drop squad, then we don't want to drop.
    // It is created in initializeSquads().
    if (!_squadData.squadExists("Drop"))
    {
        return;
    }

    Squad & dropSquad = _squadData.getSquad("Drop");

    // The squad is initialized with a Hold order.
    // There are 3 phases, and in each phase the squad is given a different order:
    // Collect units (Hold); load the transport (Load); go drop (Drop).

    if (dropSquad.getOrder().getType() == SquadOrderTypes::Drop)
    {
        // If it has already been told to Drop, we issue a new drop order in case the
        // target has changed.
        /* TODO not yet supported by the drop code
        SquadOrder dropOrder = SquadOrder(SquadOrderTypes::Drop, getDropLocation(dropSquad), 300, false, "Go drop!");
        dropSquad.setOrder(dropOrder);
        */

        return;
    }

    // If we get here, we haven't been ordered to Drop yet.

    // What units do we have, what units do we need?
    BWAPI::Unit transportUnit = nullptr;
    int transportSpotsRemaining = 8;      // all transports are the same size
    bool anyUnloadedUnits = false;
    const auto & dropUnits = dropSquad.getUnits();

    for (BWAPI::Unit unit : dropUnits)
    {
        if (unit->exists())
        {
            if (unit->isFlying() && unit->getType().spaceProvided() > 0)
            {
                transportUnit = unit;
            }
            else
            {
                transportSpotsRemaining -= unit->getType().spaceRequired();
                if (!unit->isLoaded())
                {
                    anyUnloadedUnits = true;
                }
            }
        }
    }

    if (transportUnit && transportSpotsRemaining == 0)
    {
        if (anyUnloadedUnits)
        {
            // The drop squad is complete. Load up.
            // See Squad::loadTransport().
            dropSquad.setOrder(SquadOrder(SquadOrderTypes::Load, transportUnit->getPosition(), AttackRadius, false, "Load up"));
        }
        else
        {
            // We're full. Change the order to Drop.
            dropSquad.setOrder(SquadOrder(SquadOrderTypes::Drop, getDropLocation(dropSquad), 300, false, "Go drop!"));
        }
    }
    else
    {
        // The drop squad is not complete. Look for more units.
        for (BWAPI::Unit unit : _combatUnits)
        {
            // If the squad doesn't have a transport, try to add one.
            if (!transportUnit &&
                unit->getType().spaceProvided() > 0 && unit->isFlying() &&
                _squadData.canAssignUnitToSquad(unit, dropSquad))
            {
                _squadData.assignUnitToSquad(unit, dropSquad);
                transportUnit = unit;
            }

            // If the unit fits and is good to drop, add it to the squad.
            // Rewrite unitIsGoodToDrop() to select the units of your choice to drop.
            // Simplest to stick to units that occupy the same space in a transport, to avoid difficulties
            // like "add zealot, add dragoon, can't add another dragoon--but transport is not full, can't go".
            else if (unit->getType().spaceRequired() <= transportSpotsRemaining &&
                unitIsGoodToDrop(unit) &&
                _squadData.canAssignUnitToSquad(unit, dropSquad))
            {
                _squadData.assignUnitToSquad(unit, dropSquad);
                transportSpotsRemaining -= unit->getType().spaceRequired();
            }
        }
    }
}

void CombatCommander::updateScoutDefenseSquad() 
{
    if (Config::Micro::ScoutDefenseRadius == 0 || _combatUnits.empty())
    { 
        return; 
    }

    // Get the region of our starting base.
    const Zone * myZone = the.zone.ptr(the.bases.myStart()->getTilePosition());
    if (myZone)
    {
        return;
    }

    // Get all of the visible enemy units in this region.
    BWAPI::Unitset enemyUnitsInRegion;
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->isInvincible() || unit->getType().isSpell())
        {
            continue;
        }
        if (myZone == the.zone.ptr(unit->getTilePosition()))
        {
            enemyUnitsInRegion.insert(unit);
        }
    }

    Squad & scoutDefenseSquad = _squadData.getSquad("ScoutDefense");

    // Is exactly one enemy worker here?
    bool assignScoutDefender = enemyUnitsInRegion.size() == 1 && (*enemyUnitsInRegion.begin())->getType().isWorker();

    if (assignScoutDefender)
    {
        if (scoutDefenseSquad.isEmpty())
        {
            // The enemy worker to catch.
            BWAPI::Unit enemyWorker = *enemyUnitsInRegion.begin();

            BWAPI::Unit workerDefender = findClosestWorkerToTarget(_combatUnits, enemyWorker);

            if (workerDefender)
            {
                // grab it from the worker manager and put it in the squad
                if (_squadData.canAssignUnitToSquad(workerDefender, scoutDefenseSquad))
                {
                    _squadData.assignUnitToSquad(workerDefender, scoutDefenseSquad);
                }
            }
        }
    }
    // Otherwise the squad should be empty. If not, make it so.
    else if (!scoutDefenseSquad.isEmpty())
    {
        scoutDefenseSquad.clear();
    }
}

void CombatCommander::updateBaseDefenseSquads()
{
    const int baseDefenseRadius = 19 * 32;
    const int baseDefenseHysteresis = 10 * 32;
    const int pullWorkerDistance = 8 * 32;
    const int pullWorkerVsBuildingDistance = baseDefenseRadius;
    const int pullWorkerHysteresis = 4 * 32;

    // NOTE This feature ameliorates some problems but causes others.
    //      That's why the duration is only 1 second.
    const int extraFrames = 1 * 24;     // stay this long after the last enemy is gone

    if (_combatUnits.empty()) 
    { 
        return; 
    }

    for (Base * base : the.bases.getAll())
    {
        std::stringstream squadName;
        squadName << "Base " << base->getTilePosition().x << "," << base->getTilePosition().y;

        // Don't defend inside the enemy region.
        // It will end badly when we are stealing gas or otherwise proxying.
        if (base->getOwner() != the.self())
        {
            // Clear any defense squad.
            if (_squadData.squadExists(squadName.str()))
            {
                _squadData.removeSquad(squadName.str());
            }
            continue;
        }

        // Start to defend when enemy comes within baseDefenseRadius.
        // Stop defending when enemy leaves baseDefenseRadius + baseDefenseHysteresis.
        const int defenseRadius = _squadData.squadExists(squadName.str())
            ? baseDefenseRadius + baseDefenseHysteresis
            : baseDefenseRadius;

        const Zone * zone = the.zone.ptr(base->getTilePosition());
        UAB_ASSERT(zone, "bad base location");

        // Assume for now that the base is not in danger.
        // We may prove otherwise below.
        base->setOverlordDanger(false);
        base->setWorkerDanger(false);
        base->setDoomed(false);

        // Find any enemy units that are bothering us.
        // Also note how far away the closest one is.
        bool firstWorkerSkipped = false;
        int closestEnemyDistance = MAX_DISTANCE;
        BWAPI::Unit closestEnemy = nullptr;
        int nEnemySupply = 0;
        int nEnemyWorkers = 0;
        int nEnemyGround = 0;
        int nEnemyAir = 0;
        bool enemyHitsGround = false;
        bool enemyHitsAir = false;
        bool enemyHasCloak = false;

        for (BWAPI::Unit unit : the.enemy()->getUnits())
        {
            if (unit->isInvincible() || unit->getType().isSpell())
            {
                continue;
            }
            const int dist = unit->getDistance(base->getCenter());
            if (dist < defenseRadius ||
                dist < defenseRadius + 384 && zone == the.zone.ptr(unit->getTilePosition()))
            {
                if (unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon &&
                    the.selfRace() == BWAPI::Races::Zerg)
                {
                    // Do not assign defense against cannons. Let the defensive sunken hold them.
                    // It's better to send the units to the enemy main.
                    continue;
                }
                if (dist < closestEnemyDistance)
                {
                    closestEnemyDistance = dist;
                    closestEnemy = unit;
                }

                // Count some non-attack buildings (not overlords) for later. We'll treat them specially,
                // to leave more units free to attack the enemy main.
                if (unit->getType() == BWAPI::UnitTypes::Terran_Supply_Depot ||
                    unit->getType() == BWAPI::UnitTypes::Terran_Engineering_Bay ||
                    unit->getType() == BWAPI::UnitTypes::Protoss_Pylon)
                {
                    ++nEnemySupply;
                }
                else if (unit->getType().isWorker())
                {
                    ++nEnemyWorkers;
                }
                else if (unit->isFlying())
                {
                    if (unit->getType() == BWAPI::UnitTypes::Terran_Battlecruiser ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Arbiter)
                    {
                        // NOTE Carriers don't need extra, they show interceptors.
                        nEnemyAir += 4;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Protoss_Scout)
                    {
                        nEnemyAir += 3;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Zerg_Guardian ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Devourer)
                    {
                        nEnemyAir += 2;
                    }
                    else
                    {
                        ++nEnemyAir;
                    }
                }
                else
                {
                    // Workers don't count as ground units here.
                    if (unit->getType() == BWAPI::UnitTypes::Terran_Goliath ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Zealot ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Dragoon ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Lurker ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Creep_Colony)
                    {
                        nEnemyGround += 2;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                        unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Archon ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Reaver ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Ultralisk)
                    {
                        nEnemyGround += 4;
                    }
                    else
                    {
                        ++nEnemyGround;
                    }
                }
                if (UnitUtil::CanAttackGround(unit))
                {
                    enemyHitsGround = true;
                }
                if (UnitUtil::CanAttackAir(unit))
                {
                    enemyHitsAir = true;
                }
                if (unit->isBurrowed() ||
                    unit->isCloaked() ||
                    unit->getType().hasPermanentCloak() ||
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
                    unit->getType() == BWAPI::UnitTypes::Protoss_Arbiter ||
                    unit->getType() == BWAPI::UnitTypes::Zerg_Lurker ||
                    unit->getType() == BWAPI::UnitTypes::Zerg_Lurker_Egg)
                {
                    enemyHasCloak = true;
                }
            }
        }

        // If the enemy has no dangerouus ground units, assign defenders to clear enemy "supply" units instead.
        // See above for what that includes.
        if (nEnemyGround == 0)
        {
            nEnemyGround = nEnemySupply;
        }

        if (!closestEnemy &&
            _squadData.squadExists(squadName.str()) &&
            the.now() > _squadData.getSquad(squadName.str()).getTimeMark() + extraFrames)
        {
            // No enemies and the extra time margin has passed. Drop the defense squad.
            _squadData.removeSquad(squadName.str());
            continue;
        }

        // We need a defense squad. If there isn't one, create it.
        // Its goal is not the base location itself, but the enemy closest to it, to ensure
        // that defenders will get close enough to the enemy to act.
        if (closestEnemy && !_squadData.squadExists(squadName.str()))
        {
            _squadData.createSquad(squadName.str(), BaseDefensePriority);
        }
        if (!_squadData.squadExists(squadName.str()))
        {
            continue;
        }
        if (!closestEnemy)
        {
            // Retain the existing members of the squad for extraFrames in case the enemy returns.
            continue;
        }
        Squad & defenseSquad = _squadData.getSquad(squadName.str());
        BWAPI::Position targetPosition = closestEnemy ? closestEnemy->getPosition() : base->getPosition();
        defenseSquad.setOrder(SquadOrder(SquadOrderTypes::Defend, targetPosition, defenseRadius, false, "Defend base"));
        defenseSquad.setLurkerTactic(LurkerTactic::Aggressive);     // ignore the generalTactic
        // There is somebody to fight, so remember that the squad is active as of this frame.
        defenseSquad.setTimeMark(the.now());

        // Next, figure out how many units we need to assign.

        // A simpleminded way of figuring out how much defense we need.
        const int numDefendersPerEnemyUnit = 2;

        int flyingDefendersNeeded = numDefendersPerEnemyUnit * nEnemyAir;
        int groundDefendersNeeded = nEnemyWorkers + numDefendersPerEnemyUnit * nEnemyGround;

        // Count static defense as defenders.
        // Ignore bunkers; they're more complicated.
        // Cannons are double-counted as air and ground, which can be a mistake.
        bool sunkenDefender = false;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if ((unit->getType() == BWAPI::UnitTypes::Terran_Missile_Turret ||
                unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                unit->getType() == BWAPI::UnitTypes::Zerg_Spore_Colony) &&
                unit->isCompleted() && unit->isPowered() &&
                zone == the.zone.ptr(unit->getTilePosition()))
            {
                flyingDefendersNeeded -= 3;
            }
            if ((unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                unit->getType() == BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
                unit->isCompleted() && unit->isPowered() &&
                zone == the.zone.ptr(unit->getTilePosition()))
            {
                sunkenDefender = true;
                groundDefendersNeeded -= 4;
            }
        }

        // Don't let the number of defenders go negative.
        flyingDefendersNeeded = nEnemyAir ? std::max(flyingDefendersNeeded, 2) : 0;
        if (nEnemyGround > 0)
        {
            groundDefendersNeeded = std::max(groundDefendersNeeded, 2 + nEnemyWorkers / 2);
        }
        else if (nEnemyWorkers > 0)
        {
            // Workers only, no other attackers.
            groundDefendersNeeded = std::max(groundDefendersNeeded, 1 + nEnemyWorkers / 2);
        }
        else
        {
            groundDefendersNeeded = 0;
        }

        // Drop unneeded defenders.
        if (groundDefendersNeeded <= 0 && flyingDefendersNeeded <= 0)
        {
            defenseSquad.clear();
            continue;
        }
        if (groundDefendersNeeded <= 0)
        {
            // Drop any defenders which can't shoot air.
            BWAPI::Unitset drop;
            for (BWAPI::Unit unit : defenseSquad.getUnits())
            {
                if (!unit->getType().isDetector() && !UnitUtil::CanAttackAir(unit))
                {
                    drop.insert(unit);
                }
            }
            for (BWAPI::Unit unit : drop)
            {
                defenseSquad.removeUnit(unit);
            }
            // And carry on. We may still want to add air defenders.
        }
        if (flyingDefendersNeeded <= 0)
        {
            // Drop any defenders which can't shoot ground.
            BWAPI::Unitset drop;
            for (BWAPI::Unit unit : defenseSquad.getUnits())
            {
                if (!unit->getType().isDetector() && !UnitUtil::CanAttackGround(unit))
                {
                    drop.insert(unit);
                }
            }
            for (BWAPI::Unit unit : drop)
            {
                defenseSquad.removeUnit(unit);
            }
            // And carry on. We may still want to add ground defenders.
        }

        const bool wePulledWorkers =
            std::any_of(defenseSquad.getUnits().begin(), defenseSquad.getUnits().end(), BWAPI::Filter::IsWorker);

        // Pull workers only in narrow conditions.
        // Versus units, we pull only a short distance to reduce mining losses.
        // Versus proxy buildings, we may need to pull a longer distance.
        const bool enemyProxy = buildingRush();
        const int workerDist = enemyProxy ? pullWorkerVsBuildingDistance : pullWorkerDistance;
        const bool pullWorkers =
            Config::Micro::WorkersDefendRush &&
            closestEnemyDistance <= (wePulledWorkers ? workerDist + pullWorkerHysteresis : workerDist) &&
            (enemyProxy || !sunkenDefender && numZerglingsInOurBase() > 2);

        if (wePulledWorkers && !pullWorkers)
        {
            defenseSquad.releaseWorkers();
        }

        // Now find the actual units to assign.
        updateDefenseSquadUnits(defenseSquad, flyingDefendersNeeded, groundDefendersNeeded, pullWorkers, enemyHitsAir);

        // Assign a detector if appropriate.
        const bool wantDetector =
            !enemyHitsAir ||
            enemyHasCloak && int(defenseSquad.getUnits().size()) >= flyingDefendersNeeded + groundDefendersNeeded;
        maybeAssignDetector(defenseSquad, wantDetector);

        // Estimate roughly whether overlords spawned here may be in danger.
        if (the.airAttacks.inRange(base->getDepot())
                ||
            enemyHitsAir &&
            groundDefendersNeeded + flyingDefendersNeeded > 1 &&
            closestEnemyDistance <= 7 * 32 &&
            int(defenseSquad.getUnits().size()) / 2 < groundDefendersNeeded + flyingDefendersNeeded)
        {
            base->setOverlordDanger(true);
        }

        // Estimate roughly whether the workers may be in danger.
        // If they are not at immediate risk, they should keep mining and we should even be willing to transfer in more.
        if (the.groundAttacks.inRange(base->getDepot())
                ||
            enemyHitsGround &&
            groundDefendersNeeded > 1 &&
            closestEnemyDistance <= (the.info.enemyHasSiegeMode() ? 10 * 32 : 6 * 32) &&
            int(defenseSquad.getUnits().size()) / 2 < groundDefendersNeeded + flyingDefendersNeeded)
        {
            base->setWorkerDanger(true);
        }

        // Decide whether the base cannot be saved and shold be abandoned.
        // NOTE Doomed does not imply worker danger.
        // NOTE This does not disband the defense squad. It is meant to reduce wasted spending on the base.
        //      A doomed base may be saved after all, especially if the enemy messes up.
        if (enemyHitsGround &&
            groundDefendersNeeded + flyingDefendersNeeded >= 8 &&
            int(defenseSquad.getUnits().size()) * 6 < groundDefendersNeeded + flyingDefendersNeeded &&
            (closestEnemyDistance <= 6 * 32 || the.groundAttacks.inRange(base->getDepot())) &&
            base->getNumUnits(UnitUtil::GetGroundStaticDefenseType(the.selfRace())) == 0)
        {
            base->setDoomed(true);
        }

        // Final check: If the squad is empty, clear it after all.
        // No advantage in having it stick around for extraFrames time.
        if (defenseSquad.getUnits().empty())
        {
            _squadData.removeSquad(squadName.str());
        }
    }
}

void CombatCommander::updateDefenseSquadUnits(Squad & defenseSquad, const size_t & flyingDefendersNeeded, const size_t & groundDefendersNeeded, bool pullWorkers, bool enemyHasAntiAir)
{
    const BWAPI::Unitset & squadUnits = defenseSquad.getUnits();

    // Count what is already in the squad, being careful not to double-count a unit as air and ground defender.
    size_t flyingDefendersInSquad = 0;
    size_t groundDefendersInSquad = 0;
    size_t versusBoth = 0;
    for (BWAPI::Unit defender : squadUnits)
    {
        bool versusAir = UnitUtil::CanAttackAir(defender);
        bool versusGround = UnitUtil::CanAttackGround(defender);
        if (versusGround && versusAir)
        {
            ++versusBoth;
        }
        else if (versusGround)
        {
            ++groundDefendersInSquad;
        }
        else if (versusAir)
        {
            ++flyingDefendersInSquad;
        }
    }
    // Assign dual-purpose units to whatever side needs them, priority to ground.
    if (groundDefendersNeeded > groundDefendersInSquad)
    {
        size_t add = std::min(versusBoth, groundDefendersNeeded - groundDefendersInSquad);
        groundDefendersInSquad += add;
        versusBoth -= add;
    }
    if (flyingDefendersNeeded > flyingDefendersInSquad)
    {
        size_t add = std::min(versusBoth, flyingDefendersNeeded - flyingDefendersInSquad);
        flyingDefendersInSquad += add;
    }

    //BWAPI::Broodwar->printf("defenders %d/%d %d/%d",
    //	groundDefendersInSquad, groundDefendersNeeded, flyingDefendersInSquad, flyingDefendersNeeded);

    // Add flying defenders.
    size_t flyingDefendersAdded = 0;
    BWAPI::Unit defenderToAdd;
    while (flyingDefendersNeeded > flyingDefendersInSquad + flyingDefendersAdded &&
        (defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getOrder().getPosition(), true, false, enemyHasAntiAir)))
    {
        _squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
        ++flyingDefendersAdded;
    }

    // Add ground defenders.
    size_t groundDefendersAdded = 0;
    while (groundDefendersNeeded > groundDefendersInSquad + groundDefendersAdded &&
        (defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getOrder().getPosition(), false, pullWorkers, enemyHasAntiAir)))
    {
        if (defenderToAdd->getType().isWorker())
        {
            UAB_ASSERT(pullWorkers, "pulled worker defender mistakenly");
            WorkerManager::Instance().setCombatWorker(defenderToAdd);
        }
        _squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
        ++groundDefendersAdded;
    }
}

// Choose a defender to join the base defense squad.
BWAPI::Unit CombatCommander::findClosestDefender(const Squad & defenseSquad, BWAPI::Position pos, bool flyingDefender, bool pullWorkers, bool enemyHasAntiAir)
{
    BWAPI::Unit closestDefender = nullptr;
    int minDistance = MAX_DISTANCE;

    for (BWAPI::Unit unit : _combatUnits) 
    {
        if (flyingDefender && !UnitUtil::CanAttackAir(unit) ||
            !flyingDefender && !UnitUtil::CanAttackGround(unit))
        {
            continue;
        }

        if (!_squadData.canAssignUnitToSquad(unit, defenseSquad))
        {
            continue;
        }

        int dist = unit->getDistance(pos);

        // Pull workers only if requested, and not from distant bases.
        if (unit->getType().isWorker())
        {
            if (!pullWorkers || dist > 18 * 32)
            {
                continue;
            }
            // Pull workers only if other units are considerably farther away.
            dist += 12 * 32;
        }

        // If the enemy can't shoot up, prefer air units as defenders.
        if (!enemyHasAntiAir && unit->isFlying())
        {
            dist -= 12 * 32;     // may become negative - that's OK
        }

        if (dist < minDistance)
        {
            closestDefender = unit;
            minDistance = dist;
        }
    }

    return closestDefender;
}

// NOTE This implementation is kind of cheesy. Orders ought to be delegated to a squad.
//      It can cause double-commanding, which is bad.
void CombatCommander::loadOrUnloadBunkers()
{
    if (the.self()->getRace() != BWAPI::Races::Terran)
    {
        return;
    }

    for (const auto bunker : the.self()->getUnits())
    {
        if (bunker->getType() == BWAPI::UnitTypes::Terran_Bunker)
        {
            // BWAPI::Broodwar->drawCircleMap(bunker->getPosition(), 12 * 32, BWAPI::Colors::Cyan);
            // BWAPI::Broodwar->drawCircleMap(bunker->getPosition(), 18 * 32, BWAPI::Colors::Orange);
            
            // Are there enemies close to the bunker?
            bool enemyIsNear = false;

            // 1. Is any enemy unit within a small radius?
            BWAPI::Unitset enemiesNear = BWAPI::Broodwar->getUnitsInRadius(bunker->getPosition(), 12 * 32,
                BWAPI::Filter::IsEnemy);
            if (enemiesNear.empty())
            {
                // 2. Is a fast enemy unit within a wider radius?
                enemiesNear = BWAPI::Broodwar->getUnitsInRadius(bunker->getPosition(), 18 * 32,
                    BWAPI::Filter::IsEnemy &&
                        (BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Vulture ||
                         BWAPI::Filter::GetType == BWAPI::UnitTypes::Zerg_Mutalisk)
                    );
                enemyIsNear = !enemiesNear.empty();
            }
            else
            {
                enemyIsNear = true;
            }

            if (enemyIsNear)
            {
                // Load one marine at a time if there is free space.
                if (bunker->getSpaceRemaining() > 0)
                {
                    BWAPI::Unit marine = BWAPI::Broodwar->getClosestUnit(
                        bunker->getPosition(),
                        BWAPI::Filter::IsOwned && BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Marine,
                        12 * 32);
                    if (marine)
                    {
                        the.micro.Load(bunker, marine);
                    }
                }
            }
            else
            {
                the.micro.UnloadAll(bunker);
            }
        }
    }
}

// Should squads have detectors assigned? Does not apply to all squads.
// Yes if the enemy has cloaked units. Also yes if the enemy is protoss and has observers
// and we have cloaked units--we want to shoot down those observers.
// Otherwise no if the detectors are in danger of dying.
bool CombatCommander::wantSquadDetectors() const
{
    if (the.enemy()->getRace() == BWAPI::Races::Protoss &&
        the.info.enemyHasMobileDetection() &&
        the.info.weHaveCloakTech())
    {
        return true;
    }

    return
        the.self()->getRace() == BWAPI::Races::Protoss ||      // observers should be safe-ish
        !the.info.enemyHasAntiAir() ||
        the.info.enemyCloakedUnitsSeen();
}

// Add or remove a given squad's detector, subject to availability.
// Because this checks the content of the squad, it should be called
// after any other units are added or removed.
void CombatCommander::maybeAssignDetector(Squad & squad, bool wantDetector)
{
    if (squad.hasDetector())
    {
        // If the detector is the only thing left in the squad, we don't want to keep it.
        if (!wantDetector || squad.getUnits().size() == 1)
        {
            for (BWAPI::Unit unit : squad.getUnits())
            {
                if (unit->getType().isDetector())
                {
                    squad.removeUnit(unit);
                    return;
                }
            }
        }
    }
    else
    {
        // Don't add a detector to an empty squad.
        if (wantDetector && !squad.getUnits().empty())
        {
            for (BWAPI::Unit unit : _combatUnits)
            {
                if (unit->getType().isDetector() && _squadData.canAssignUnitToSquad(unit, squad))
                {
                    _squadData.assignUnitToSquad(unit, squad);
                    return;
                }
            }
        }
    }
}

// Scan enemy cloaked units.
void CombatCommander::doComsatScan()
{
    if (the.selfRace() != BWAPI::Races::Terran)
    {
        return;
    }

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Comsat_Station) == 0)
    {
        return;
    }

    // Does the enemy have undetected cloaked units that we may be able to engage?
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->isVisible() &&
            (!unit->isDetected() || unit->getOrder() == BWAPI::Orders::Burrowing) &&
            !unit->isInvincible() &&
            !unit->getType().isSpell() &&
            unit->getPosition().isValid())
        {
            // At most one scan per call. We don't check whether it succeeds.
            (void) the.micro.Scan(unit->getPosition());
            // Also make sure the Info Manager knows that the enemy can burrow.
            the.info.enemySeenBurrowing();
            break;
        }
    }
}

// Do the larva trick, if conditions are favorable.
void CombatCommander::doLarvaTrick()
{
    if (the.now() < 5040 &&
        the.selfRace() == BWAPI::Races::Zerg &&
        the.my.completed.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0)
    {
        for (Base * base : the.bases.getAll())
        {
            if (base->getOwner() == the.self() &&
                base->getMineralOffset().x < 0)
            {
                the.micro.LarvaTrick(the.bases.myMain()->getDepot()->getLarva());
            }
        }
    }
}

// What units do you want to drop into the enemy base from a transport?
bool CombatCommander::unitIsGoodToDrop(const BWAPI::Unit unit) const
{
    return
        unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
        unit->getType() == BWAPI::UnitTypes::Terran_Vulture;
}

// Called once per frame from update(), above.
// Get our money back at the last second for stuff that is about to be destroyed.
// It is not ideal: A building which is destined to die only after it is completed
// will be completed and die.
// Special case for a zerg sunken colony while it is morphing: It will lose up to
// 100 hp when the morph finishes, so cancel if it would be weak when it finishes.
// NOTE See BuildingManager::cancelBuilding() for another way to cancel buildings.
void CombatCommander::cancelDyingItems()
{
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        BWAPI::UnitType type = unit->getType();
        if (type.isBuilding() && !unit->isCompleted() ||
            type == BWAPI::UnitTypes::Zerg_Egg ||
            type == BWAPI::UnitTypes::Zerg_Lurker_Egg ||
            type == BWAPI::UnitTypes::Zerg_Cocoon)
        {
            if (UnitUtil::ExpectedSurvivalTime(unit) <= 1 * 24 ||
                type == BWAPI::UnitTypes::Zerg_Sunken_Colony && unit->getHitPoints() < 130 && unit->getRemainingBuildTime() < 24 && unit->isUnderAttack())
            {
                //BWAPI::Broodwar->printf("canceling %s hp %d", UnitTypeName(unit).c_str(), unit->getHitPoints() + unit->getShields());
                (void) the.micro.Cancel(unit);
            }
        }
    }
}

// How good is it to pull this worker for combat?
int CombatCommander::workerPullScore(BWAPI::Unit worker)
{
    return
        (worker->getHitPoints() == worker->getType().maxHitPoints() ? 10 : 0) +
        (worker->getShields() == worker->getType().maxShields() ? 4 : 0) +
        (worker->isCarryingGas() ? -3 : 0) +
        (worker->isCarryingMinerals() ? -2 : 0);
}

// Pull workers off of mining and into the attack squad.
// The argument n can be zero or negative or huge. Nothing awful will happen.
// Tries to pull the "best" workers for combat, as decided by workerPullScore() above.
void CombatCommander::pullWorkers(int n)
{
    struct Compare
    {
        auto operator()(BWAPI::Unit left, BWAPI::Unit right) const -> bool
        {
            return workerPullScore(left) < workerPullScore(right);
        }
    };

    std::priority_queue<BWAPI::Unit, std::vector<BWAPI::Unit>, Compare> workers;

    Squad & groundSquad = _squadData.getSquad("Ground");

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (unit->getType().isWorker() &&
            WorkerManager::Instance().isFree(unit) &&
            _squadData.canAssignUnitToSquad(unit, groundSquad))
        {
            workers.push(unit);
        }
    }

    int nLeft = n;

    while (nLeft > 0 && !workers.empty())
    {
        BWAPI::Unit worker = workers.top();
        workers.pop();
        _squadData.assignUnitToSquad(worker, groundSquad);
        --nLeft;
    }
}

// Release workers from the attack squad.
void CombatCommander::releaseWorkers()
{
    Squad & groundSquad = _squadData.getSquad("Ground");
    groundSquad.releaseWorkers();
}

void CombatCommander::drawSquadInformation(int x, int y)
{
    _squadData.drawSquadInformation(x, y);
}

// Create an attack order for the given squad (which may be null).
// For a squad with ground units, ignore targets which are not accessible by ground.
SquadOrder CombatCommander::getAttackOrder(Squad * squad)
{
    // 1. Clear any destructible obstacles around our bases.
    // Most maps don't have any such thing, but see e.g. Arkanoid and Sparkle.
    // Only ground squads are sent to clear obstacles.
    // TODO There is a bug affecting the map Pathfinder.
    if (false && squad && squad->getUnits().size() > 0 && squad->hasGround() && squad->canAttackGround())
    {
        // We check our current bases (formerly plus 2 bases we may want to take next).
        /*
        Base * baseToClear1 = nullptr;
        Base * baseToClear2 = nullptr;
        BWAPI::TilePosition nextBasePos = the.map.getNextExpansion(false, true, false);
        if (nextBasePos.isValid())
        {
            // The next mineral-optional expansion.
            baseToClear1 = the.bases.getBaseAtTilePosition(nextBasePos);
        }
        nextBasePos = the.map.getNextExpansion(false, true, true);
        if (nextBasePos.isValid() && nextBasePos != baseToClear1)
        {
            // The next gas expansion.
            baseToClear2 = the.bases.getBaseAtTilePosition(nextBasePos);
        }
        */

        // Then pick any base with blockers and clear it.
        // The blockers are neutral buildings, and we can get their initial positions.
        for (Base * base : the.bases.getAll())
        {
            if (base->getBlockers().size() > 0 &&
                (base->getOwner() == the.self() /* || base == baseToClear1 || base == baseToClear2 */) &&
                squad->mapPartition() == the.partitions.id(base->getPosition()))
            {
                BWAPI::Unit target = *(base->getBlockers().begin());
                return SquadOrder(SquadOrderTypes::DestroyNeutral, target->getInitialPosition(), 64, false, "Destroy neutrals");
            }
        }
    }

    // A "ground squad" is a squad with any ground unit, so that it wants to use a ground distance map if possible.
    const bool isGroundSquad = squad->hasGround();

    // 2. If we're defensive, look for a front line to hold. No attacks.
    if (!_goAggressive)
    {
        return SquadOrder(SquadOrderTypes::Attack, getDefensiveBase(), DefendFrontRadius, isGroundSquad, "Defend front");
    }

    // 3. Otherwise we are aggressive. Look for a spot to attack.
    Base * base = nullptr;
    BWAPI::Position pos = BWAPI::Positions::Invalid;
    std::string key;
    getAttackLocation(squad, base, pos,  key);

    SquadOrder order;
    if (base)
    {
        order = SquadOrder(SquadOrderTypes::Attack, base, AttackRadius, isGroundSquad, "Attack base");
    }
    else
    {
        UAB_ASSERT(pos.isValid(), "bad attack location");
        order = SquadOrder(SquadOrderTypes::Attack, pos, AttackRadius, isGroundSquad, "Attack " + key);
    }
    order.setKey(key);
    return order;
}

// Choose a point of attack for the given squad (which may be null--no squad at all).
// For a squad with ground units, ignore targets which are not accessible by ground.
// Return either a base or a position, not both.
void CombatCommander::getAttackLocation(Squad * squad, Base * & returnBase, BWAPI::Position & returnPos, std::string & returnKey)
{
    // If the squad is empty, minimize effort.
    if (squad && squad->getUnits().empty())
    {
        returnBase = the.bases.myMain();
        returnKey = "nothing";
        return;
    }

    // Ground and air considerations.
    // NOTE The squad doesn't recalculate these values until it updates, which hasn't happened yet.
    //      So any changes we just made to the squad's units won't be reflected here yet. The slight
    //      delay doesn't cause any known problem (and changes that matter don't happen often).
    bool hasGround = true;
    bool hasAir = false;
    bool canAttackGround = true;
    bool canAttackAir = false;
    if (squad)
    {
        hasGround = squad->hasGround();
        hasAir = squad->hasAir();
        canAttackGround = squad->canAttackGround();
        canAttackAir = squad->canAttackAir();
    }

    // If the squad has no combat units, or is unable to attack, return home.
    if (!hasGround && !hasAir || !canAttackGround && !canAttackAir)
    {
        returnBase = the.bases.myMain();
        returnKey = "nothing";
        return;
    }

    // Know where the squad is. If it's empty or unknown, assume it is at our start position.
    // NOTE In principle, different members of the squad may be in different map partitions,
    // unable to reach each others' positions by ground. We ignore that complication.
    // NOTE Since we aren't doing islands, all squads are reachable from the start position,
    //      except in the case of accidentally pushing units through a barrier.
    const int squadPartition = the.partitions.id(the.bases.myStart()->getTilePosition());

    // 1. If we haven't been able to attack, look for an undefended target on the ground, either
    // a building or a ground unit that can't shoot up.
    // Only if the squad is all air. This is mainly for mutalisks.
    if (squad &&
        !hasGround &&
        canAttackGround &&
        squad->getVanguard() &&
        (squad->getOrder().getKey() == "undefended"
          ? the.now() - squad->getOrderFrame() < 12 * 24
          : the.now() - squad->getLastAttack() > 2 * 24 && the.now() - squad->getLastRetreat() <= 8 &&
            the.now() - squad->getOrderFrame() > 2 * 24))
    {
        int bestScore = -MAX_DISTANCE;      // higher is better, usually negative
        BWAPI::Position target = BWAPI::Positions::None;

        for (const auto & kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo & ui = kv.second;

            if (!UnitUtil::TypeCanAttackAir(ui.type) &&
                ui.lastPosition.isValid() &&
                !ui.goneFromLastPosition &&
                !ui.lifted &&
                !defendedTarget(ui.lastPosition, false, true))
            {
                int score = -squad->getVanguard()->getDistance(ui.lastPosition);
                if (ui.type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                {
                    score -= 10 * 32;
                }
                else if (ui.type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                         ui.type == BWAPI::UnitTypes::Protoss_High_Templar ||
                         ui.type == BWAPI::UnitTypes::Protoss_Reaver ||
                         ui.type == BWAPI::UnitTypes::Zerg_Lurker ||
                         ui.type == BWAPI::UnitTypes::Zerg_Sunken_Colony)
                {
                    score -= 6 * 32;
                }

                if (score > bestScore)
                {
                    bestScore = score;
                    target = ui.lastPosition;
                }
            }
        }

        if (target.isValid())
        {
            //BWAPI::Broodwar->printf("  undefended target @ %d,%d", target.x, target.y);
            returnPos = target;
            returnKey = "undefended";
            return;
        }
    }

    // 2. Attack the enemy base with the weakest defense.
    // Only if the squad can attack ground. Lift the command center and it is no longer counted as a base.
    if (canAttackGround)
    {
        Base * targetBase = nullptr;
        int bestScore = -MAX_DISTANCE;
        for (Base * base : the.bases.getAll())
        {
            if (base->getOwner() == the.enemy())
            {
                // Ground squads ignore enemy bases which they cannot reach.
                if (hasGround && squadPartition != the.partitions.id(base->getTilePosition()))
                {
                    continue;
                }

                int score = 0;     // the final score will be 0 or negative

                // Ground squads mildly prefer to attack a base other than the enemy main. It's a simple heuristic.
                // Air squads prefer to go for the main first.
                if (base == the.bases.enemyStart())
                {
                    score += hasGround ? -1 : 2;
                }

                // A base with low remaining minerals is lower priority.
                if (base->getLastKnownMinerals() < 300)
                {
                    score -= 2;
                }

                // The squad vanguard is the unit closest to the current attack location.
                // Prefer a new attack location which is close, preferably even closer.
                if (squad && squad->getVanguard())
                {
                    score -= squad->getVanguard()->getDistance(base->getCenter()) / (16 * 32);
                }

                std::vector<UnitInfo> enemies;
                int enemyDefenseRange = the.info.enemyHasSiegeMode() ? 12 * 32 : 8 * 32;
                the.info.getNearbyForce(enemies, base->getCenter(), the.enemy(), enemyDefenseRange);
                for (const UnitInfo & enemy : enemies)
                {
                    // Count enemies that are buildings or slow-moving units good for defense.
                    if (enemy.type.isBuilding() ||
                        enemy.type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                        enemy.type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                        enemy.type == BWAPI::UnitTypes::Protoss_Reaver ||
                        enemy.type == BWAPI::UnitTypes::Protoss_Arbiter ||
                        enemy.type == BWAPI::UnitTypes::Protoss_High_Templar ||
                        enemy.type == BWAPI::UnitTypes::Zerg_Lurker ||
                        enemy.type == BWAPI::UnitTypes::Zerg_Guardian)
                    {
                        // If the enemy unit could attack (some units of) the squad, count it.
                        if (hasGround && UnitUtil::TypeCanAttackGround(enemy.type) ||			// doesn't recognize casters
                            hasAir && UnitUtil::TypeCanAttackAir(enemy.type) ||					// doesn't recognize casters
                            enemy.type == BWAPI::UnitTypes::Protoss_High_Templar)				// spellcaster
                        {
                            --score;
                        }
                    }
                }
                if (score > bestScore)
                {
                    targetBase = base;
                    bestScore = score;
                }
            }
        }
        if (targetBase)
        {
            returnBase = targetBase;
            returnKey = "base";
            return;
        }
    }

    // 3. Attack known enemy buildings.
    // We assume that a terran can lift the buildings; otherwise, the squad must be able to attack ground.
    if (canAttackGround || the.enemyRace() == BWAPI::Races::Terran)
    {
        for (const auto & kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo & ui = kv.second;

            // Special case for refinery buildings because their ground reachability is tricky to check.
            // TODO This causes a bug where a ground squad may be ordered to attack an enemy refinery
            //      that is on an island and out of reach.
            if (ui.type.isBuilding() &&
                !ui.type.isAddon() &&
                ui.lastPosition.isValid() &&
                !ui.goneFromLastPosition &&
                (ui.type.isRefinery() || squadPartition == the.partitions.id(ui.lastPosition)))
                // (!hasGround || (!ui.type.isRefinery() && squadPartition == the.partitions.id(ui.lastPosition))))
            {
                if (ui.lifted)
                {
                    // The building is lifted (or was when last seen). Only if the squad can hit it.
                    if (canAttackAir)
                    {
                        returnPos = ui.lastPosition;
                        return;
                    }
                }
                else
                {
                    // The building is not thought to be lifted.
                    returnPos = ui.lastPosition;
                    returnKey = "building";
                    return;
                }
            }
        }
    }

    // 4. Attack visible enemy units.
    const BWAPI::Position squadCenter = squad
        ? squad->calcCenter()
        : the.bases.myStart()->getPosition();
    BWAPI::Unit bestUnit = nullptr;
    int bestDistance = MAX_DISTANCE;
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Larva ||
            !unit->isDetected() ||
            unit->isInvincible() ||
            unit->getType().isSpell())
        {
            continue;
        }

        // Ground squads ignore enemy units which are not accessible by ground, except when nearby.
        // The "when nearby" exception allows a chance to attack enemies that are in range,
        // even if they are beyond a barrier. It's very rough.
        int distance = squad && squad->getVanguard() ? unit->getDistance(squad->getVanguard()) : unit->getDistance(squadCenter);
        if (hasGround &&
            squadPartition != the.partitions.id(unit->getPosition()) &&
            distance > 300)
        {
            continue;
        }

        if (unit->isFlying() && canAttackAir || !unit->isFlying() && canAttackGround)
        {
            if (distance < bestDistance)
            {
                bestUnit = unit;
                bestDistance = distance;
            }
        }
    }
    if (bestUnit)
    {
        returnPos = bestUnit->getPosition();
        returnKey = "unit";
        return;
    }

    // 5. Attack the remembered locations of unseen enemy units which might still be there.
    // Choose the one most recently seen.
    int lastSeenFrame = 0;
    BWAPI::Position lastSeenPos = BWAPI::Positions::None;
    BWAPI::UnitType lastSeenType = BWAPI::UnitTypes::None;      // for debugging only
    for (const auto & kv : the.info.getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.updateFrame < the.now() &&
            ui.updateFrame > lastSeenFrame &&
            !ui.goneFromLastPosition &&
            !ui.type.isSpell() &&
            (hasAir || the.partitions.id(ui.lastPosition) == squadPartition) &&
            ((ui.type.isFlyer() || ui.lifted) && canAttackAir || (!ui.type.isFlyer() || !ui.lifted) && canAttackGround))
        {
            lastSeenFrame = ui.updateFrame;
            lastSeenPos = ui.lastPosition;
            lastSeenType = ui.type;
        }
    }
    if (lastSeenPos.isValid())
    {
        returnPos = lastSeenPos;
        returnKey = "possible unit";
        return;
    }

    // 6. We can't see anything, so explore the map until we find something.
    returnPos = MapGrid::Instance().getLeastExplored(!hasAir, squadPartition);
    returnKey = "explore";
}

// Does the enemy have defenders (static or mobile) near the given position?
// NOTE For now, only vsAir==true is implemented.
bool CombatCommander::defendedTarget(const BWAPI::Position & pos, bool vsGround, bool vsAir) const
{
    if (vsAir)
    {
        if (the.airAttacks.at(pos) > 0)
        {
            return true;
        }

        const std::vector<UnitCluster> & antiairClusters = the.ops.getAirDefenseClusters();
        for (const UnitCluster & cluster : antiairClusters)
        {
            int dist = pos.getApproxDistance(cluster.center);

            // The enemy defenders are "too close" if they can get within 8 tiles in 8 seconds.
            if (dist <= 8 * 32 + 8 * 24 / cluster.speed)
            {
                return true;
            }
        }
    }

    return false;
}

// Choose a point of attack for the given drop squad.
BWAPI::Position CombatCommander::getDropLocation(const Squad & squad)
{
    // 0. If we're defensive, stay at the start location.
    /* unneeded
    if (!_goAggressive)
    {
        return BWAPI::Position(the.self()->getStartLocation());
    }
    */

    // Otherwise we are aggressive. Look for a spot to attack.

    // 1. The enemy main base, if known.
    Base * enemyMain = the.bases.enemyStart();
    // if (enemyMain && enemyMain->getOwner() == the.enemy())
    if (enemyMain)
    {
        return enemyMain->getPosition();
    }

    // 2. Any known enemy base.
    /* TODO not ready yet
    Base * targetBase = nullptr;
    int bestScore = INT_MIN;
    for (Base * base : the.bases.getBases())
    {
        if (base->getOwner() == the.enemy())
        {
            int score = 0;     // the final score will be 0 or negative
            std::vector<UnitInfo> enemies;
            the.info.getNearbyForce(enemies, base->getCenter(), the.enemy(), 600);
            for (const auto & enemy : enemies)
            {
                if (enemy.type.isBuilding() && (UnitUtil::TypeCanAttackGround(enemy.type) || enemy.type.isDetector()))
                {
                    --score;
                }
            }
            if (score > bestScore)
            {
                targetBase = base;
                bestScore = score;
            }
        }
        if (targetBase)
        {
            return targetBase->getCenter();
        }
    }
    */

    // 3. Any known enemy buildings.
    for (const auto & kv : the.info.getUnitInfo(the.enemy()))
    {
        const UnitInfo & ui = kv.second;

        if (ui.type.isBuilding() && ui.lastPosition.isValid() && !ui.goneFromLastPosition)
        {
            return ui.lastPosition;
        }
    }

    // 4. We can't see anything, so explore the map until we find something.
    return MapGrid::Instance().getLeastExplored();
}

// We're being defensive. Get the location to defend.
Base * CombatCommander::getDefensiveBase()
{
    // We are guaranteed to always have a main base location, even if it has been destroyed.
    Base * base = the.bases.myMain();

    // We may have taken our natural. If so, call that the front line.
    Base * natural = the.bases.myNatural();
    if (natural && the.self() == natural->getOwner())
    {
        base = natural;
    }

    return base;
}

// Choose one worker to pull for scout defense.
BWAPI::Unit CombatCommander::findClosestWorkerToTarget(BWAPI::Unitset & unitsToAssign, BWAPI::Unit target)
{
    UAB_ASSERT(target != nullptr, "target was null");

    if (!target)
    {
        return nullptr;
    }

    BWAPI::Unit closestMineralWorker = nullptr;
    int closestDist = Config::Micro::ScoutDefenseRadius + 128;    // more distant workers do not get pulled
    
    for (BWAPI::Unit unit : unitsToAssign)
    {
        if (unit->getType().isWorker() && WorkerManager::Instance().isFree(unit))
        {
            int dist = unit->getDistance(target);
            if (unit->isCarryingMinerals())
            {
                dist += 96;
            }

            if (dist < closestDist)
            {
                closestMineralWorker = unit;
                dist = closestDist;
            }
        }
    }

    return closestMineralWorker;
}

int CombatCommander::numZerglingsInOurBase() const
{
    const int concernRadius = 300;
    int zerglings = 0;
    
    BWAPI::Position myBasePosition(the.bases.myStart()->getPosition());

    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling &&
            unit->getDistance(myBasePosition) < concernRadius)
        {
            ++zerglings;
        }
    }

    return zerglings;
}

// Is an enemy building near our base? If so, we may pull workers.
// Don't pull against a completed bunker, cannon, or sunken; we'll only lose workers.
bool CombatCommander::buildingRush() const
{
    // If we have units, there will likely be no gain in pulling workers.
    if (the.info.weHaveCombatUnits())
    {
        return false;
    }

    BWAPI::Position myBasePosition(the.bases.myStart()->getPosition());

    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType().isBuilding() &&
            unit->getDistance(myBasePosition) < 600 &&
            !unit->isLifted() &&
            (!unit->isCompleted() || unit->getType().groundWeapon() == BWAPI::WeaponTypes::None))
        {
            return true;
        }
    }

    return false;
}

CombatCommander & CombatCommander::Instance()
{
    static CombatCommander instance;
    return instance;
}
