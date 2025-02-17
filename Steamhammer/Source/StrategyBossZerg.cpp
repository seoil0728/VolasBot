#include "StrategyBossZerg.h"

#include "Bases.h"
#include "InformationManager.h"
#include "MacroAct.h"
#include "OpponentModel.h"
#include "OpponentPlan.h"
#include "ProductionManager.h"
#include "Random.h"
#include "ScoutManager.h"
#include "The.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// Never have more than this many devourers.
const int maxDevourers = 9;

StrategyBossZerg::StrategyBossZerg()
    : _self(BWAPI::Broodwar->self())
    , _enemy(BWAPI::Broodwar->enemy())
    , _enemyRace(_enemy->getRace())
    , _nonadaptive(false)
    , _techTarget(TechUnit::None)
    , _extraDronesWanted(0)
    , _extraOpeningExtractors(0)
    , _extraOpeningHatcheries(0)
    , _latestBuildOrder(BWAPI::Races::Zerg)
    , _emergencyGroundDefense(false)
    , _emergencyStartFrame(-1)
    , _emergencyNow(false)
    , _wantAirArmor(false)
    , _droneToHide(nullptr)
    , _existingSupply(-1)
    , _pendingSupply(-1)
    , _lastUpdateFrame(-1)
    , _lastInfestedTerranOrderFrame(0)
    , myArmySize(0)					// game starts with no army
    , enemyGroundArmySize(0)		// game starts with no army
    , enemyAntigroundArmySize(0)	// game starts with no army
    , defilerScore(0)
    , _enemySeemsToBeDead(false)    // "I'm not dead yet!"
    , _recommendDefensiveSpire(false)
    , _recommendParasite(0)
    , _recommendEnsnare(0)
    , _recommendBroodling(0)
    , _recommendQueens(0)
{
    resetTechScores();
    setUnitMix(BWAPI::UnitTypes::Zerg_Drone, BWAPI::UnitTypes::None);
    chooseAuxUnit();                // it chooses None initially
    chooseEconomyRatio();
}

// -- -- -- -- -- -- -- -- -- -- --
// Private methods.

// Calculate supply existing, pending, and used.
// For pending supply, we need to know about overlords just hatching.
// For supply used, the BWAPI self->supplyUsed() can be slightly wrong,
// especially when a unit is just started or just died. 
void StrategyBossZerg::updateSupply()
{
    int existingSupply = 0;
    int pendingSupply = 0;
    int supplyUsed = 0;

    for (BWAPI::Unit unit : _self->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Overlord)
        {
            if (unit->getOrder() == BWAPI::Orders::ZergBirth)
            {
                // Overlord is just hatching and doesn't provide supply yet.
                pendingSupply += 16;
            }
            else
            {
                existingSupply += 16;
            }
        }
        else if (unit->getType() == BWAPI::UnitTypes::Zerg_Egg)
        {
            if (unit->getBuildType() == BWAPI::UnitTypes::Zerg_Overlord)
            {
                pendingSupply += 16;
            }
            else if (unit->getBuildType().isTwoUnitsInOneEgg())
            {
                supplyUsed += 2 * unit->getBuildType().supplyRequired();
            }
            else
            {
                supplyUsed += unit->getBuildType().supplyRequired();
            }
        }
        else if (unit->getType() == BWAPI::UnitTypes::Zerg_Hatchery && !unit->isCompleted())
        {
            // Count an unfinished hatchery as pending supply only if it is close to finishing.
            // NOTE Hatchery build time = 1800; overlord build time = 600.
            // Hatcheries take too long to build to be worth counting otherwise.
            if (unit->getRemainingBuildTime() < 300)
            {
                pendingSupply += 2;
            }
        }
        else if (unit->getType().isResourceDepot())
        {
            // Only counts complete hatcheries because incomplete hatcheries are checked above.
            // Also counts lairs and hives whether complete or not, of course.
            existingSupply += 2;
        }
        else
        {
            supplyUsed += unit->getType().supplyRequired();
        }
    }
    
    _existingSupply = std::min(existingSupply, absoluteMaxSupply);
    _pendingSupply = pendingSupply;
    _supplyUsed = supplyUsed;

    // Note: _existingSupply is less than _self->supplyTotal() when an overlord
    // has just died. In other words, it recognizes the lost overlord sooner,
    // which is better for planning.
}

// Called once per frame, possibly more.
// Includes screen drawing calls.
void StrategyBossZerg::updateGameState()
{
    if (_lastUpdateFrame == the.now())
    {
        // No need to update more than once per frame.
        return;
    }
    _lastUpdateFrame = the.now();

    if (_emergencyGroundDefense && _lastUpdateFrame >= _emergencyStartFrame + (15 * 24))
    {
        // Danger has been past for long enough. Declare the end of the emergency.
        _emergencyGroundDefense = false;
    }

    minerals = std::max(0, _self->minerals() - BuildingManager::Instance().getReservedMinerals());
    gas = std::max(0, _self->gas() - BuildingManager::Instance().getReservedGas());

    // Unit stuff, including uncompleted units.
    nLairs = the.my.all.count(BWAPI::UnitTypes::Zerg_Lair);
    nHives = the.my.all.count(BWAPI::UnitTypes::Zerg_Hive);
    nHatches = the.my.all.count(BWAPI::UnitTypes::Zerg_Hatchery)
        + nLairs + nHives;
    nCompletedHatches = the.my.completed.count(BWAPI::UnitTypes::Zerg_Hatchery)
        + nLairs + nHives;
    nSpores = the.my.all.count(BWAPI::UnitTypes::Zerg_Spore_Colony);

    // nGas = number of geysers ready to mine (extractor must be complete)
    // nFreeGas = number of geysers free to be taken (no extractor, even uncompleted)
    the.bases.gasCounts(nGas, nFreeGas);

    nDrones = the.my.all.count(BWAPI::UnitTypes::Zerg_Drone);
    nMineralDrones = WorkerManager::Instance().getNumMineralWorkers();
    nGasDrones = WorkerManager::Instance().getNumGasWorkers();
    nLarvas = the.my.all.count(BWAPI::UnitTypes::Zerg_Larva);

    nLings = the.my.all.count(BWAPI::UnitTypes::Zerg_Zergling);
    nHydras = the.my.all.count(BWAPI::UnitTypes::Zerg_Hydralisk);
    nLurkers = the.my.all.count(BWAPI::UnitTypes::Zerg_Lurker);
    nMutas = the.my.all.count(BWAPI::UnitTypes::Zerg_Mutalisk);
    nQueens = the.my.all.count(BWAPI::UnitTypes::Zerg_Queen);
    nGuardians = the.my.all.count(BWAPI::UnitTypes::Zerg_Guardian);
    nDevourers = the.my.all.count(BWAPI::UnitTypes::Zerg_Devourer);
    nDefilers = the.my.all.count(BWAPI::UnitTypes::Zerg_Defiler);

    nInfestedCC = the.my.completed.count(BWAPI::UnitTypes::Zerg_Infested_Command_Center);
    nInfestedTerrans = the.my.all.count(BWAPI::UnitTypes::Zerg_Infested_Terran);

    // Tech stuff. It has to be completed for the tech to be available.
    nEvo = the.my.completed.count(BWAPI::UnitTypes::Zerg_Evolution_Chamber);
    hasPool = the.my.completed.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) > 0;
    hasDen = the.my.completed.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den) > 0;
    hasSpire =
        the.my.completed.count(BWAPI::UnitTypes::Zerg_Spire) > 0 ||
        the.my.all.count(BWAPI::UnitTypes::Zerg_Greater_Spire) > 0;
    hasGreaterSpire = the.my.completed.count(BWAPI::UnitTypes::Zerg_Greater_Spire) > 0;
    // We have lurkers if we have lurker aspect and we have a den to make the hydras.
    hasLurkers = hasDen && _self->hasResearched(BWAPI::TechTypes::Lurker_Aspect);
    hasQueensNest = the.my.completed.count(BWAPI::UnitTypes::Zerg_Queens_Nest) > 0;
    hasUltra = the.my.completed.count(BWAPI::UnitTypes::Zerg_Ultralisk_Cavern) > 0;
    // Enough upgrades that it is worth making ultras: Speed done, armor underway.
    hasUltraUps =
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Anabolic_Synthesis) != 0 &&
        (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Chitinous_Plating) != 0 ||
        _self->isUpgrading(BWAPI::UpgradeTypes::Chitinous_Plating));
    hasDefilerUps =
        the.my.completed.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) > 0 &&
        (_self->hasResearched(BWAPI::TechTypes::Consume) || _self->isResearching(BWAPI::TechTypes::Consume));
    hasNydus = the.my.all.count(BWAPI::UnitTypes::Zerg_Nydus_Canal) > 0;

    // hasLair means "can research stuff in the lair", like overlord speed.
    // hasLairTech means "can do stuff that needs lair", like research lurker aspect.
    // hasLairOrHive means "can research stuff in the lair or hive"
    hasHiveTech = the.my.completed.count(BWAPI::UnitTypes::Zerg_Hive) > 0;
    hasLair = the.my.completed.count(BWAPI::UnitTypes::Zerg_Lair) > 0;
    hasLairTech = hasLair || nHives > 0;
    hasLairOrHive = hasLair || hasHiveTech;
    
    // If we start on an island, we go air until we have nydus canals.
    goingIslandAir = the.bases.isIslandStart() && !hasNydus;
    
    outOfBook = ProductionManager::Instance().isOutOfBook();
    nBases = the.bases.baseCount(_self);
    nFreeBases = the.bases.freeLandBaseCount();
    nMineralPatches = the.bases.mineralPatchCount();
    maxDrones = WorkerManager::Instance().getMaxWorkers();
    // We may have lost all bases but still have macro hatcheries that can mine.
    if (maxDrones < 3 && nBases == 0 && nHatches > 0)
    {
        maxDrones = 3;
    }

    // Exception: If we have lost all our hatcheries, allow up to 2 drones,
    // one to build and one to mine. (Also the creep may disappear and we'll lose larvas.)
    if (nHatches == 0)
    {
        maxDrones = 2;
    }

    updateSupply();

    // Army sizes don't need to be updated as often.
    // -1 means it has not yet been updated for the first time.
    if (the.now() % 12 == 9)
    {
        updateArmySizes();
    }

    drawStrategyBossInformation();
}

void StrategyBossZerg::updateArmySizes()
{
    // Count the supply of all my mobile units that canAttack(), with no further restriction.
    PlayerSnapshot me(_self);

    myArmySize = 0;
    for (const std::pair<BWAPI::UnitType, int> & unitCount : me.getCounts())
    {
        BWAPI::UnitType type = unitCount.first;
        int n = unitCount.second;

        if (!type.isBuilding() && !type.isWorker() && type.canAttack())
        {
            myArmySize += n * type.supplyRequired();
        }
        else if (type == BWAPI::UnitTypes::Zerg_Sunken_Colony)
        {
            myArmySize += n * 4;			// as much as this many zerglings
        }
    }

    // Count the supply of enemy units that can attack ground. We don't care about e.g. corsairs.
    PlayerSnapshot you(_enemy);

    enemyAntigroundArmySize = 0;
    enemyGroundArmySize = 0;
    for (const std::pair<BWAPI::UnitType, int> & unitCount : you.getCounts())
    {
        BWAPI::UnitType type = unitCount.first;
        int n = unitCount.second;

        if (!type.isBuilding() && !type.isWorker())
        {
            if (!type.isFlyer())
            {
                enemyGroundArmySize += n * type.supplyRequired();
            }
            if (UnitUtil::TypeCanAttackGround(type))
            {
                enemyAntigroundArmySize += n * type.supplyRequired();
            }
        }
    }
}

// Do we have "enough" army that we can safely focus on making drones?
bool StrategyBossZerg::enoughArmy() const
{
    if (enemyAntigroundArmySize == 0)
    {
        return true;
    }

    double ratio = double(myArmySize) / enemyAntigroundArmySize;

    if (_enemyRace == BWAPI::Races::Protoss)
    {
        return ratio >= 0.65;
    }

    // Other races.
    return ratio >= 0.75;
}

// Do we have "enough" ground army that we can skip making zerglings?
// This is used in deciding whether to make emergency zerglings.
bool StrategyBossZerg::enoughGroundArmy() const
{
    if (enemyGroundArmySize == 0)
    {
        return true;
    }

    double ratio = double(myArmySize) / enemyGroundArmySize;

    if (_enemyRace == BWAPI::Races::Terran)
    {
        return ratio >= 0.75;
    }

    if (_enemyRace == BWAPI::Races::Protoss)
    {
        return ratio >= 0.6;
    }

    // Zerg.
    return ratio >= 1.0;
}

// Enemy is terran, has lifted all known buildings, and has no known ground units.
// Result is cached in _enemyIsAllAir.
bool StrategyBossZerg::enemyIsAllAir() const
{
    if (the.bases.baseCount(_enemy) > 0)
    {
        // NOTE A floating command center does not establish a base.
        return false;
    }

    for (const auto & kv : the.info.getUnitData(_enemy).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.type.isBuilding())
        {
            if (!ui.lifted)
            {
                return false;
            }
        }
        else
        {
            if (!ui.type.isFlyer())
            {
                return false;
            }
        }
    }

    return true;
}

// We know of at least one enemy building, and all known enemy buildings are unreachable by
// ground (they're on islands or lifted).
// We'll likely need air units.
bool StrategyBossZerg::enemyIsUnreachable() const
{
    bool any = false;

    for (const auto & kv : the.info.getUnitData(_enemy).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.type.isBuilding())
        {
            any = true;
            // It may be goneFromLastPosition, but we treat that as still unreachable.
            // The building may have been destroyed, or lifted and floated elsewhere.
            if (!ui.lifted && the.bases.connectedToStart(BWAPI::TilePosition(ui.lastPosition)))
            {
                return false;
            }
        }
    }

    return any;
}

// The enemy is toast, and all we have left to do is to eradicate the surviving enemy buildings.
// There might be stray units or whole bases that we haven't seen, but if so we can change
// gears then.
// When this is true, we prefer to make mutas because they can find terran floating buildings
// and buildings on islands. Also they scout faster.
bool StrategyBossZerg::enemySeemsToBeDead() const
{
    if (!the.bases.enemyStart())
    {
        // The enemy starting position has not been found.
        return false;
    }

    if (the.bases.baseCount(_enemy) > 0)
    {
        // The enemy still owns a base.
        return false;
    }

    if (!enoughGroundArmy())
    {
        // The enemy may be able to defeat us on the ground.
        return false;
    }

    for (const std::pair<BWAPI::UnitType, int> & unitCount : the.your.seen.getCounts())
    {
        if (UnitUtil::TypeCanAttackAir(unitCount.first))
        {
            // The enemy still has an anti-air unit.
            return false;
        }
    }

    // Enemy has no known bases or anti-air units and appears to have no winning chances.
    return true;
}

// Is it economically safe to mine gas?
// We don't check whether our gas bases are under attack.
bool StrategyBossZerg::canSafelyMineGas() const
{
    return
        nGas > 0 && nDrones > 3 * nGas;
}

// The count of evolution chambers which are not busy upgrading.
int StrategyBossZerg::nFreeEvo() const
{
    int count = 0;

    for (BWAPI::Unit u : _self->getUnits())
    {
        if (u->getType() == BWAPI::UnitTypes::Zerg_Evolution_Chamber && !u->isUpgrading() && u->isCompleted())
        {
            ++count;
        }
    }

    return count;
}

// Do not make more than this many devourers.
int StrategyBossZerg::devourerLimit() const
{
    int enemyAir =
        the.your.seen.count(BWAPI::UnitTypes::Terran_Wraith) +
        the.your.seen.count(BWAPI::UnitTypes::Terran_Valkyrie) +
        2 * the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser) +
        the.your.seen.count(BWAPI::UnitTypes::Protoss_Corsair) +
        the.your.seen.count(BWAPI::UnitTypes::Protoss_Scout) +
        the.your.seen.count(BWAPI::UnitTypes::Protoss_Carrier) +
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Mutalisk) +
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Devourer);
    return
        std::min(maxDevourers, std::min(nMutas / 2, enemyAir / 2));
}

// When will our spire finish?
// We compare with enemy spire timing to decide whether we need spores.
int StrategyBossZerg::getOurSpireTiming() const
{
    if (hasSpire)
    {
        // The spire is complete, return an early time.
        return 1;
    }

    // Assume that there is at most one spire.
    for (BWAPI::Unit unit : _self->getUnits())
    {
        // If it is a greater spire, then we have spire tech and it's handled above.
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Spire)
        {
            return the.now() + unit->getRemainingBuildTime();
        }
    }

    // We haven't started one. Return a distant future time.
    return MAX_FRAME;
}

// How many of our eggs will hatch into the given unit type?
// This does not adjust for zerglings or scourge, which are 2 to an egg.
int StrategyBossZerg::numInEgg(BWAPI::UnitType type) const
{
    int count = 0;

    for (BWAPI::Unit unit : _self->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Egg && unit->getBuildType() == type)
        {
            ++count;
        }
    }

    return count;
}

// Return true if the building is in the building queue with any status.
bool StrategyBossZerg::isBeingBuilt(const BWAPI::UnitType unitType) const
{
    UAB_ASSERT(unitType.isBuilding(), "not a building");
    return BuildingManager::Instance().isBeingBuilt(unitType);
}

// When you cancel a building, you get back 75% of its mineral cost, rounded down.
int StrategyBossZerg::mineralsBackOnCancel(BWAPI::UnitType type) const
{
    return 3 * type.mineralPrice() / 4;     // C++ integer arithmetic does the right thing
}

// Extreme emergency: We are out of drones and/or hatcheries.
// Cancel items to release their resources.
void StrategyBossZerg::cancelStuff(int mineralsNeeded)
{
    // BWAPI::Broodwar->printf("cancel to get %d minerals", mineralsNeeded);

    int mineralsSoFar = _self->minerals();

    if (mineralsSoFar >= mineralsNeeded)
    {
        return;
    }

    // Upgrades, in order from least to most important (in a sort of average sense).
    // ZvZ is the key matchup--the one where we're most likely to be able to recover.
    static const std::vector<BWAPI::UpgradeType> upgrades =
        { BWAPI::UpgradeTypes::Pneumatized_Carapace
        , BWAPI::UpgradeTypes::Zerg_Missile_Attacks
        , BWAPI::UpgradeTypes::Zerg_Carapace
        , BWAPI::UpgradeTypes::Zerg_Melee_Attacks
        , BWAPI::UpgradeTypes::Grooved_Spines
        , BWAPI::UpgradeTypes::Muscular_Augments
        , BWAPI::UpgradeTypes::Metabolic_Boost
        };
    for (BWAPI::UpgradeType upgrade : upgrades)
    {
        if (_self->isUpgrading(upgrade))
        {
            mineralsSoFar += upgrade.mineralPrice();		// ignore possible higher-level upgrades
            cancelUpgrade(upgrade);
            if (mineralsSoFar >= mineralsNeeded)
            {
                return;
            }
        }
    }

    for (BWAPI::Unit u : _self->getUnits())
    {
        if (u->getType() == BWAPI::UnitTypes::Zerg_Egg && u->getBuildType() == BWAPI::UnitTypes::Zerg_Overlord)
        {
            if (_existingSupply - _supplyUsed >= 6)  // enough to add 3 drones
            {
                // BWAPI::Broodwar->printf("emergency cancel %s", UnitTypeName(u).c_str());
                mineralsSoFar += 100;
                u->cancelMorph();
            }
        }
        else if (u->getType() == BWAPI::UnitTypes::Zerg_Egg && u->getBuildType() != BWAPI::UnitTypes::Zerg_Drone)
        {
            // BWAPI::Broodwar->printf("emergency cancel %s", UnitTypeName(u).c_str());
            mineralsSoFar += u->getType().mineralPrice();
            u->cancelMorph();
        }
        else if (u->getType() == BWAPI::UnitTypes::Zerg_Hatchery && !u->isCompleted() && nHatches > 0 ||
            u->getType() != BWAPI::UnitTypes::Zerg_Hatchery && u->getType().isBuilding() && !u->isCompleted())
        {
            // Canceling a building (including a morphed building like a lair)
            // returns 3/4 of the original cost in both minerals and gas (but we only count minerals).
            // BWAPI::Broodwar->printf("emergency cancel %s", UnitTypeName(u).c_str());
            mineralsSoFar += mineralsBackOnCancel(u->getType());
            u->cancelMorph();
        }
        if (mineralsSoFar >= mineralsNeeded)
        {
            return;
        }
    }
}

// Emergency: We urgently need a spawning pool and don't have the cash.
// Cancel hatcheries, extractors, or evo chambers to get the minerals.
void StrategyBossZerg::cancelForSpawningPool()
{
    int mineralsNeeded = 200 - _self->minerals();

    if (mineralsNeeded <= 0)
    {
        // We have enough.
        return;
    }

    // BWAPI::Broodwar->printf("cancel for spawning pool, needing %d", mineralsNeeded);

    // Cancel buildings in the building manager's queue.
    // They may or may not have started yet. We don't find out if this recovers resources.
    if (nHatches > 0)
    {
        BuildingManager::Instance().cancelBuildingType(BWAPI::UnitTypes::Zerg_Hatchery);
    }
    BuildingManager::Instance().cancelBuildingType(BWAPI::UnitTypes::Zerg_Extractor);
    BuildingManager::Instance().cancelBuildingType(BWAPI::UnitTypes::Zerg_Evolution_Chamber);

    // We make 2 loops, the first to see what we need to cancel and the second to cancel it.

    // First loop: What do we need to cancel? Count the cancel-able buildings.
    int hatcheries = 0;
    int extractors = 0;
    int evos = 0;
    for (BWAPI::Unit u : _self->getUnits())
    {
        if (u->getType() == BWAPI::UnitTypes::Zerg_Hatchery && u->canCancelMorph())
        {
            ++hatcheries;
        }
        else if (u->getType() == BWAPI::UnitTypes::Zerg_Extractor && u->canCancelMorph())
        {
            ++extractors;
        }
        else if (u->getType() == BWAPI::UnitTypes::Zerg_Evolution_Chamber && u->canCancelMorph())
        {
            ++evos;
        }
    }

    // Second loop: Cancel what needs it.
    // When you cancel a building, you get back 75% of its mineral cost, rounded down.
    bool cancelHatchery =
        nHatches > 0 &&
        hatcheries > 0 &&
        extractors * mineralsBackOnCancel(BWAPI::UnitTypes::Zerg_Extractor) + evos * mineralsBackOnCancel(BWAPI::UnitTypes::Zerg_Evolution_Chamber) < mineralsNeeded;
    for (BWAPI::Unit u : _self->getUnits())
    {
        if (cancelHatchery)
        {
            if (u->getType() == BWAPI::UnitTypes::Zerg_Hatchery && u->canCancelMorph())
            {
                // BWAPI::Broodwar->printf("cancel hatchery for spawning pool");
                u->cancelMorph();
                break;     // we only need to cancel one
            }
        }
        else
        {
            // Cancel extractors and evo chambers.
            if ((u->getType() == BWAPI::UnitTypes::Zerg_Extractor || u->getType() == BWAPI::UnitTypes::Zerg_Evolution_Chamber) &&
                u->canCancelMorph())
            {
                // BWAPI::Broodwar->printf("cancel %s for spawning pool", UnitTypeName(u).c_str());
                u->cancelMorph();
                // Stop as soon as we have canceled enough buildings.
                mineralsNeeded -= mineralsBackOnCancel(u->getType());
                if (mineralsNeeded <= 0)
                {
                    break;
                }
            }
        }
    }
}

// Find what building is performing this upgrade and cancel it.
void StrategyBossZerg::cancelUpgrade(BWAPI::UpgradeType upgrade)
{
    for (BWAPI::Unit unit : _self->getUnits())
    {
        if (unit->getUpgrade() == upgrade)
        {
            // BWAPI::Broodwar->printf("cancel upgrade %s", upgrade.getName().c_str());
            unit->cancelUpgrade();
            return;
        }
    }
    UAB_ASSERT(false, "no upgrade");
}

// Return the maximum attack/armor upgrade level, depending on hatchery/lair/hive.
int StrategyBossZerg::maxUpgrade() const
{
    return hasHiveTech ? 3 : (hasLairTech ? 2 : 1);
}

// The next item in the queue is useless and can be dropped.
// Top goal: Do not freeze the production queue by asking the impossible.
// But also try to reduce wasted production.
// NOTE Useless stuff is not always removed before it is built.
//      The order of events is: this check -> queue filling -> production.
bool StrategyBossZerg::nextInQueueIsUseless(BuildOrderQueue & queue) const
{
    if (queue.isEmpty() || queue.getHighestPriorityItem().isGasSteal)
    {
        return false;
    }

    const MacroAct act = queue.getHighestPriorityItem().macroAct;

    // It costs gas that we don't have and won't get.
    if (nGas == 0 && act.gasPrice() > gas &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Extractor) == 0)
    {
        return true;
    }

    if (act.isUpgrade())
    {
        const BWAPI::UpgradeType upInQueue = act.getUpgradeType();

        // Already have it or already getting it (due to a race condition).
        if (_self->getUpgradeLevel(upInQueue) >= upInQueue.maxRepeats() || _self->isUpgrading(upInQueue))
        {
            return true;
        }

        // Lost the building for it in the meantime.
        if (upInQueue == BWAPI::UpgradeTypes::Anabolic_Synthesis)
        {
            return !hasUltra ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Chitinous_Plating);
        }
        if (upInQueue == BWAPI::UpgradeTypes::Chitinous_Plating)
        {
            return !hasUltra ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Anabolic_Synthesis);
        }

        if (upInQueue == BWAPI::UpgradeTypes::Pneumatized_Carapace ||
            upInQueue == BWAPI::UpgradeTypes::Ventral_Sacs ||
            upInQueue == BWAPI::UpgradeTypes::Antennae)
        {
            // Assume that there is at most one lair or hive.
            return !hasLairOrHive ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Pneumatized_Carapace) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Antennae) ||
                // If there are no plain hatcheries but only a lair or hive, it may be researching burrow.
                nCompletedHatches == 1 && _self->isResearching(BWAPI::TechTypes::Burrowing);
        }

        if (upInQueue == BWAPI::UpgradeTypes::Muscular_Augments ||
            upInQueue == BWAPI::UpgradeTypes::Grooved_Spines)
        {
            // Assume there is at most one hydra den.
            return
                !hasDen && the.my.all.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den) == 0 && !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hydralisk_Den) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Muscular_Augments) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Grooved_Spines) ||
                _self->isResearching(BWAPI::TechTypes::Lurker_Aspect);
        }

        if (upInQueue == BWAPI::UpgradeTypes::Metabolic_Boost)
        {
            return
                !hasPool && the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0 ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Adrenal_Glands);
        }

        if (upInQueue == BWAPI::UpgradeTypes::Adrenal_Glands)
        {
            return
                !hasPool || !hasHiveTech ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Metabolic_Boost);
        }

        // Allow ground upgrades in any order, as long as we have the prerequisites.
        if (upInQueue == BWAPI::UpgradeTypes::Zerg_Carapace ||
            upInQueue == BWAPI::UpgradeTypes::Zerg_Melee_Attacks ||
            upInQueue == BWAPI::UpgradeTypes::Zerg_Missile_Attacks)
        {
            const int upCount =
                _self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Carapace) +
                _self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Melee_Attacks) +
                _self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Missile_Attacks);

            return
                _self->getUpgradeLevel(upInQueue) >= maxUpgrade() ||
                upCount >= the.my.all.count(BWAPI::UnitTypes::Zerg_Evolution_Chamber);
        }

        // Allow air upgrades in any order, in one or two spires.
        if (upInQueue == BWAPI::UpgradeTypes::Zerg_Flyer_Carapace ||
            upInQueue == BWAPI::UpgradeTypes::Zerg_Flyer_Attacks)
        {
            const int upCount =
                _self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Carapace) +
                _self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks);

            return
                _self->getUpgradeLevel(upInQueue) >= maxUpgrade() ||
                upCount >= the.my.completed.count(BWAPI::UnitTypes::Zerg_Spire)
                         + the.my.completed.count(BWAPI::UnitTypes::Zerg_Greater_Spire);
        }

        if (upInQueue == BWAPI::UpgradeTypes::Gamete_Meiosis)
        {
            return
                !hasQueensNest ||
                _self->isResearching(BWAPI::TechTypes::Ensnare) ||
                _self->isResearching(BWAPI::TechTypes::Spawn_Broodlings);
        }

        if (upInQueue == BWAPI::UpgradeTypes::Metasynaptic_Node)
        {
            return
                the.my.completed.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) == 0 ||
                _self->isResearching(BWAPI::TechTypes::Consume) ||
                _self->isResearching(BWAPI::TechTypes::Plague);
        }

        return false;
    }

    if (act.isTech())
    {
        const BWAPI::TechType techInQueue = act.getTechType();

        if (_self->hasResearched(techInQueue) || _self->isResearching(techInQueue))
        {
            return true;
        }

        if (techInQueue == BWAPI::TechTypes::Burrowing)
        {
            // Burrowing research is useless if we have no hatchery at all, 
            // or if our only hatchery is a lair or hive and it is researching something else.
            return nCompletedHatches == 0 ||
                nCompletedHatches == 1 &&
                    (_self->isUpgrading(BWAPI::UpgradeTypes::Pneumatized_Carapace) ||
                     _self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) ||
                     _self->isUpgrading(BWAPI::UpgradeTypes::Antennae));
        }

        if (techInQueue == BWAPI::TechTypes::Lurker_Aspect)
        {
            return !hasLairTech && nLairs == 0 ||
                !hasDen && the.my.all.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den) == 0 && !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hydralisk_Den) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Muscular_Augments) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Grooved_Spines);
        }

        if (techInQueue == BWAPI::TechTypes::Ensnare ||
            techInQueue == BWAPI::TechTypes::Spawn_Broodlings)
        {
            return
                !hasQueensNest ||
                _self->isResearching(BWAPI::TechTypes::Ensnare) ||
                _self->isResearching(BWAPI::TechTypes::Spawn_Broodlings) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Gamete_Meiosis);
        }

        if (techInQueue == BWAPI::TechTypes::Consume ||
            techInQueue == BWAPI::TechTypes::Plague)
        {
            return
                the.my.completed.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) == 0 ||
                _self->isResearching(BWAPI::TechTypes::Consume) ||
                _self->isResearching(BWAPI::TechTypes::Plague) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Metasynaptic_Node);
        }

        return false;
    }
    
    // After that, we only care about units.
    if (!act.isUnit())
    {
        return false;
    }

    const BWAPI::UnitType nextInQueue = act.getUnitType();

    // Buildings.
    if (nextInQueue.isBuilding())
    {
        // If we are very short of drones, don't spend one on a building no matter what.
        // Also drop morphed buildings except for static defense, which we may urgently need.
        if (nDrones <= 3 &&
            nextInQueue != BWAPI::UnitTypes::Zerg_Sunken_Colony &&
            nextInQueue != BWAPI::UnitTypes::Zerg_Spore_Colony)
        {
            return true;
        }

        if (nextInQueue == BWAPI::UnitTypes::Zerg_Hatchery)
        {
            // We're planning a hatchery but do not have the drones to support it.
            // 3 drones/hatchery is the minimum: It can support ling or drone production.
            // Also, it may still be OK if we have lots of minerals to spend.

            // Exception: Trust the opening book.
            if (!outOfBook && _self->deadUnitCount(BWAPI::UnitTypes::Zerg_Drone) == 0)
            {
                return false;
            }

            // TODO A bug can affect building 2 hatcheries in a row:
            //      getNumUnstarted() can return a number 1 too high for a frame immediately after
            //      one hatchery is built, causing the next hatchery to be dropped incorrectly.
            //      The error is in BuildingManager.
            int hatchCount = nHatches + BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Zerg_Hatchery);
            if (nCompletedHatches + hatchCount == 0)
            {
                return false;									// don't cancel our only hatchery!
            }

            return nDrones < 3 * (1 + hatchCount) - 1 &&
                minerals <= 300 + 150 * nCompletedHatches;		// new hatchery plus minimum production from each existing
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Lair)
        {
            return !hasPool && the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0 ||
                the.my.all.count(BWAPI::UnitTypes::Zerg_Hatchery) == 0;
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Hive)
        {
            return nLairs == 0 ||
                the.my.all.count(BWAPI::UnitTypes::Zerg_Queens_Nest) == 0 ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Pneumatized_Carapace) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) ||
                _self->isUpgrading(BWAPI::UpgradeTypes::Antennae) ||
                nCompletedHatches == 1 && _self->isResearching(BWAPI::TechTypes::Burrowing);
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Sunken_Colony)
        {
            return !hasPool && the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0 && !isBeingBuilt(BWAPI::UnitTypes::Zerg_Spawning_Pool);
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Spore_Colony)
        {
            return nEvo == 0 && the.my.all.count(BWAPI::UnitTypes::Zerg_Evolution_Chamber) == 0 && !isBeingBuilt(BWAPI::UnitTypes::Zerg_Evolution_Chamber);
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Extractor)
        {
            return nFreeGas == 0 ||												// nowhere to make an extractor
                nDrones < 1 + Config::Macro::WorkersPerRefinery * (nGas + 1);	// not enough drones to mine it
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Spawning_Pool)
        {
            return
                the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) > 0 ||
                isBeingBuilt(BWAPI::UnitTypes::Zerg_Spawning_Pool) ||
                nHatches == 0;
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Hydralisk_Den)
        {
            return !hasPool && the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0;
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Spire)
        {
            return !hasLairTech && nLairs == 0;
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Greater_Spire)
        {
            return nHives == 0 ||
                the.my.completed.count(BWAPI::UnitTypes::Zerg_Spire) == 0;
        }
        if (nextInQueue == BWAPI::UnitTypes::Zerg_Defiler_Mound)
        {
            return nHives == 0 ||
                nGas == 0 && gas < 350 ||
                nDrones < 10 ||
                the.my.all.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) > 0 ||
                isBeingBuilt(BWAPI::UnitTypes::Zerg_Defiler_Mound);
        }

        return false;
    }

    // Mobile units.
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Overlord)
    {
        const int totalSupply = _existingSupply + _pendingSupply;
        const int supplyExcess = totalSupply - _supplyUsed;

        // Opening book sometimes deliberately includes up to 3 extra overlords.
        // However, excess emergency overlords are still possible.
        if (!outOfBook)
        {
            return supplyExcess > 3 * 16;
        }

        // We may have extra overlords scheduled if, for example, we just lost a lot of units.
        // This is coordinated with makeOverlords() but skips less important steps.
        return totalSupply > absoluteMaxSupply + 16 ||
            totalSupply > 32 && supplyExcess >= totalSupply / 8 + 16;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Drone)
    {
        // We are planning more than the maximum reasonable number of drones.
        // nDrones can go slightly over maxDrones when queue filling adds drones.
        // It can also go over when maxDrones decreases (bases lost, minerals mined out).
        return nDrones >= maxDrones;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Zergling)
    {
        // We lost the tech.
        return !hasPool &&
            the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0 &&
            !isBeingBuilt(BWAPI::UnitTypes::Zerg_Spawning_Pool);		// needed for 4 pool to work
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Hydralisk)
    {
        // We lost the tech.
        return !hasDen && the.my.all.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den) == 0 && !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hydralisk_Den);
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Lurker)
    {
        // No hydra to morph, or we expected to have the tech and don't.
        return nHydras == 0 ||
            !_self->hasResearched(BWAPI::TechTypes::Lurker_Aspect) && !_self->isResearching(BWAPI::TechTypes::Lurker_Aspect);
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Mutalisk || nextInQueue == BWAPI::UnitTypes::Zerg_Scourge)
    {
        // We lost the tech.
        return !hasSpire &&
            the.my.all.count(BWAPI::UnitTypes::Zerg_Spire) == 0 &&
            the.my.all.count(BWAPI::UnitTypes::Zerg_Greater_Spire) == 0;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Queen)
    {
        // We lost the tech. It's OK to go over the limit.
        return
            the.my.all.count(BWAPI::UnitTypes::Zerg_Queens_Nest) == 0;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Ultralisk)
    {
        // We lost the tech.
        return !hasUltra && the.my.all.count(BWAPI::UnitTypes::Zerg_Ultralisk_Cavern) == 0;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Guardian)
    {
        // We lost the tech, or we don't have a mutalisk to morph.
        return nMutas == 0 ||
            !hasGreaterSpire && the.my.all.count(BWAPI::UnitTypes::Zerg_Greater_Spire) == 0;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Devourer)
    {
        // We lost the tech, or we don't have a mutalisk to morph, or we're over the devourer limit.
        return nMutas == 0 ||
            !hasGreaterSpire && the.my.all.count(BWAPI::UnitTypes::Zerg_Greater_Spire) == 0 ||
            nDevourers >= devourerLimit();
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Defiler)
    {
        // We lost the tech.
        return the.my.all.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) == 0;
    }
    if (nextInQueue == BWAPI::UnitTypes::Zerg_Infested_Terran)
    {
        // We lost the tech, or we'll go over the limit.
        return
            nInfestedCC == 0 ||
            nInfestedTerrans >= Config::Skills::MaxInfestedTerrans;
    }

    return false;
}

void StrategyBossZerg::produce(const MacroAct & act)
{
    // To restrict economy bookkeeping to cases that use a larva, try
    //  && act.whatBuilds() == BWAPI::UnitTypes::Zerg_Larva
    if (act.isUnit())
    {
        if (act.getUnitType() == BWAPI::UnitTypes::Zerg_Drone)
        {
            // If we should not make any more drones, then drop this one. Produce nothing.
            if (nDrones >= maxDrones)
            {
                return;
            }
            ++_economyDrones;
        }
        ++_economyTotal;
    }

    _latestBuildOrder.add(act);
}

// Make a drone instead of a combat unit with this larva?
// Even in an emergency, continue making drones at a low rate.
bool StrategyBossZerg::needDroneNext() const
{
    return
        nDrones < maxDrones &&
        (enoughArmy() && !_emergencyGroundDefense || Random::Instance().flag(std::min(_economyRatio, 0.15))) &&
        double(_economyDrones) / double(1 + _economyTotal) < _economyRatio;
}

// We think we want the given unit type. What type do we really want?
// 1. If we need a drone next for the economy, return a drone instead.
// 2. If the type is a morphed type and we don't have the precursor,
//    return the precursor type instead.
// Otherwise return the requested type.
BWAPI::UnitType StrategyBossZerg::findUnitType(BWAPI::UnitType type) const
{
    if (needDroneNext())
    {
        // Don't let a drone substitute for a needed tech unit.
        if ((type != BWAPI::UnitTypes::Zerg_Mutalisk || nMutas >= 6 || _enemyRace == BWAPI::Races::Zerg && nMutas >= 3) &&
            (type != BWAPI::UnitTypes::Zerg_Lurker || nLurkers >= 4))
        {
            return BWAPI::UnitTypes::Zerg_Drone;
        }
    }

    // The base unit of a morphed unit.
    if (type == BWAPI::UnitTypes::Zerg_Lurker && nHydras == 0)
    {
        return BWAPI::UnitTypes::Zerg_Hydralisk;
    }
    if ((type == BWAPI::UnitTypes::Zerg_Guardian || type == BWAPI::UnitTypes::Zerg_Devourer) && nMutas == 0)
    {
        return BWAPI::UnitTypes::Zerg_Mutalisk;
    }

    return type;
}

// Simulate the supply ahead in the queue to see if we may need an overlord soon.
// Stop when:
// - we're at the supply limit of 200
// - or excess supply goes negative - we will need an overlord
// - or an overlord is next in the queue
// - or the queue ends
bool StrategyBossZerg::queueSupplyIsOK(BuildOrderQueue & queue)
{
    int totalSupply = _existingSupply + _pendingSupply;
    if (totalSupply >= absoluteMaxSupply)
    {
        return true;
    }

    // Allow for the drones that are to turn into buildings.
    // This is not strictly accurate: Sometimes buildings are severely delayed.
    int supplyExcess = totalSupply - _supplyUsed + 2 * BuildingManager::Instance().getNumUnstarted();

    for (int i = queue.size() - 1; i >= 0; --i)
    {
        MacroAct act = queue[i].macroAct;
        if (act.isUnit())          // skip commands, research, etc.
        {
            if (act.getUnitType() == BWAPI::UnitTypes::Zerg_Overlord)
            {
                return true;
            }
            if (act.getUnitType().isBuilding())
            {
                if (!UnitUtil::IsMorphedBuildingType(act.getUnitType()))
                {
                    supplyExcess += 2;   // for the drone that will be used
                }
                // Morphed buildings have no effect on supply.
            }
            else
            {
                // If making the unit leaves us negative, we will need an overlord.
                supplyExcess -= act.supplyRequired();
                if (supplyExcess < 0)
                {
                    return false;
                }
            }
        }
    }

    // Call it unresolved: An overlord might be useful.
    return false;
}

// We need overlords.
// Do this last so that nothing gets pushed in front of the overlords (buildings would be OK sometimes).
// NOTE: If you change this, coordinate the change with nextInQueueIsUseless(),
// which has a feature to recognize unneeded overlords (e.g. after big army losses).
void StrategyBossZerg::makeOverlords(BuildOrderQueue & queue)
{
    // If we have queued all the supply we are going to need, there's nothing to do here.
    // This prevents excess overlords and allows last-moment overlords in book lines.
    if (queueSupplyIsOK(queue))
    {
        return;
    }

    int totalSupply = _existingSupply + _pendingSupply;
    if (totalSupply < absoluteMaxSupply)
    {
        // Don't account for drones to be used in buildings in this rough calculation.
        int supplyExcess = totalSupply - _supplyUsed;
        BWAPI::UnitType nextInQueue = queue.getNextUnit();

        // Adjust the number to account for the next queue item only.
        if (nextInQueue != BWAPI::UnitTypes::None)
        {
            if (nextInQueue.isBuilding())
            {
                if (!UnitUtil::IsMorphedBuildingType(nextInQueue))
                {
                    supplyExcess += 2;   // for the drone that will be used
                }
                // Morphed buildings have no immediate effect on supply.
            }
            else
            {
                if (nextInQueue.isTwoUnitsInOneEgg())
                {
                    supplyExcess -= 2 * nextInQueue.supplyRequired();
                }
                else
                {
                    supplyExcess -= nextInQueue.supplyRequired();
                }
            }
        }

        // If we're behind, catch up.
        for (; supplyExcess < 0; supplyExcess += 16)
        {
            queue.queueAfterSpore(BWAPI::UnitTypes::Zerg_Overlord);
        }
        // If we're only a little ahead, stay ahead depending on the supply.
        // This is a crude calculation. It seems not too far off.
        if (totalSupply > 20 && supplyExcess <= 0)								// > overlord + 2 hatcheries
        {
            queue.queueAfterSpore(BWAPI::UnitTypes::Zerg_Overlord);
        }
        else if (totalSupply > 32 && supplyExcess <= totalSupply / 8 - 1)		// >= 2 overlords + 1 hatchery
        {
            queue.queueAfterSpore(BWAPI::UnitTypes::Zerg_Overlord);
        }
        else if (totalSupply > 120 && supplyExcess <= totalSupply / 8 + 8)		// well into the game
        {
            // This sometimes produces an overlord that is then dropped as "useless",
            // that is, excess. It seems OK overall, though.
            queue.queueAfterSpore(BWAPI::UnitTypes::Zerg_Overlord);
        }
    }
}

// If necessary, take an emergency action and return true.
// Otherwise return false.
bool StrategyBossZerg::takeUrgentAction(BuildOrderQueue & queue)
{
    // Count only the actual recognized enemy plan.
    if (OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Contain ||
        OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Proxy)
    {
        if (!_droneToHide && WorkerManager::Instance().getNumPostedWorkers() == 0)
        {
            // There's no sign that we tried to move a worker outside the containment. So try now.
            // We will look for a worker to post to a hidden base for a later expansion.
            // 1. If there is a scout worker, break off its mission. It may already be outside.
            if (ScoutManager::Instance().getWorkerScout())
            {
                _droneToHide = ScoutManager::Instance().getWorkerScout();
                ScoutManager::Instance().releaseWorkerScout();
                WorkerManager::Instance().postGivenWorker(_droneToHide, MacroLocation::Hidden);
                //BWAPI::Broodwar->printf("posting scout");
            }
            // 2. Otherwise, just pick a worker to post and hope that it makes it.
            else
            {
                _droneToHide = WorkerManager::Instance().postWorker(MacroLocation::Hidden);
                //BWAPI::Broodwar->printf("posting other worker");
            }
            // NOTE _droneToHide may still be null, though it's unlikely.
            // Do not break out of the opening, at least not yet. Let the book do its thing.
        }
    }
    // And continue. We may want to take further actions and/or break out of the book.

    // Find the next thing remaining in the queue, but only if it is a unit.
    BWAPI::UnitType nextInQueue = queue.getNextUnit();

    // If we're fairly sure the enemy is following a plan
    // that our opening does not answer, break out of the opening.
    OpeningPlan plan = OpponentModel::Instance().getDarnLikelyEnemyPlan();

    bool breakOut = false;  // break out of the opening?

    // NOTE This excludes OpeningPlan::Contain, which we might want to react to.
    if (plan == OpeningPlan::WorkerRush ||
        plan == OpeningPlan::Proxy ||
        plan == OpeningPlan::FastRush && nLings == 0)  // don't react to fast rush if we're doing it too
    {
        // Actions, not breakout tests.
        // Action: If we need money for a spawning pool, cancel any hatchery, extractor, or evo chamber.
        if (outOfBook &&
            !hasPool &&
            (minerals < 150 || minerals < 200 && nDrones <= 6) &&
            the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0 &&
            BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0)
        {
            cancelForSpawningPool();
        }
        // Action: Start a sunken as soon as possible.
        if (outOfBook &&
            (hasPool || the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) > 0) &&
            nDrones >= 4)
        {
            if (plan != OpeningPlan::Proxy &&		// sunken is usually an overreaction to a proxy
                nextInQueue != BWAPI::UnitTypes::Zerg_Sunken_Colony &&
                the.my.all.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) == 0 &&
                !isBeingBuilt(BWAPI::UnitTypes::Zerg_Sunken_Colony))
            {
                // BWAPI::Broodwar->printf("anti rush/proxy sunken");
                queue.queueAsHighestPriority(MacroAct(BWAPI::UnitTypes::Zerg_Sunken_Colony, MacroLocation::Front));
                return true;
            }
        }
        // Action: Make zerglings if needed.
        if (outOfBook &&
            hasPool &&
            nDrones >= 3 &&
            nLings < 6 &&
            nextInQueue != BWAPI::UnitTypes::Zerg_Zergling &&
            nextInQueue != BWAPI::UnitTypes::Zerg_Creep_Colony &&
            nextInQueue != BWAPI::UnitTypes::Zerg_Sunken_Colony &&
            nextInQueue != BWAPI::UnitTypes::Zerg_Overlord)
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Zergling);
            return true;
        }

        // The rest is breakout checks: Break out of the opening book?
        if (!outOfBook)
        {
            int nDronesEver = nDrones + _self->deadUnitCount(BWAPI::UnitTypes::Zerg_Drone);
            if (nDronesEver < 9 &&
                nextInQueue != BWAPI::UnitTypes::Zerg_Drone &&
                nextInQueue != BWAPI::UnitTypes::Zerg_Spawning_Pool &&
                the.my.all.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den) == 0)  // we may be hydra rushing, making us safe
            {
                breakOut = true;
            }
            else if (nDronesEver >= 9 &&
                !hasPool &&
                nextInQueue != BWAPI::UnitTypes::Zerg_Spawning_Pool &&
                !isBeingBuilt(BWAPI::UnitTypes::Zerg_Spawning_Pool) &&
                the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0)
            {
                breakOut = true;
            }
            else if (nDronesEver >= 9 &&
                plan != OpeningPlan::Proxy &&
                (hasPool || the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) > 0) &&
                nextInQueue != BWAPI::UnitTypes::Zerg_Creep_Colony &&
                nextInQueue != BWAPI::UnitTypes::Zerg_Sunken_Colony &&
                !isBeingBuilt(BWAPI::UnitTypes::Zerg_Creep_Colony) &&
                !isBeingBuilt(BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
                the.my.all.count(BWAPI::UnitTypes::Zerg_Creep_Colony) == 0 &&
                the.my.all.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) == 0)
            {
                breakOut = true;
            }
        }

        if (breakOut)
        {
            ProductionManager::Instance().goOutOfBookAndClearQueue();
            nextInQueue = BWAPI::UnitTypes::None;
            // And continue, in case another urgent action is needed.
        }
    }

    // There are no drones.
    // NOTE maxDrones is never zero (we save one just in case), so this is always bad.
    if (nDrones == 0)
    {
        WorkerManager::Instance().setCollectGas(false);
        BuildingManager::Instance().cancelQueuedBuildings();
        if (nHatches == 0)
        {
            // No hatcheries either. Queue drones for a hatchery and mining, and hope.
            // NOTE Can't cancel all queued buildings. One is the hatchery that we need.
            // BWAPI::Broodwar->printf("no drones or hatcheries!");
            ProductionManager::Instance().goOutOfBookAndClearQueue();
            queue.queueAsLowestPriority(BWAPI::UnitTypes::Zerg_Drone);
            queue.queueAsLowestPriority(BWAPI::UnitTypes::Zerg_Drone);
            queue.queueAsLowestPriority(BWAPI::UnitTypes::Zerg_Hatchery);
            cancelStuff(400);
        }
        else
        {
            if (nextInQueue != BWAPI::UnitTypes::Zerg_Drone && numInEgg(BWAPI::UnitTypes::Zerg_Drone) == 0)
            {
                // Queue one drone to mine minerals.
                // BWAPI::Broodwar->printf("no drones!");
                ProductionManager::Instance().goOutOfBookAndClearQueue();
                BuildingManager::Instance().cancelQueuedBuildings();
                queue.queueAsLowestPriority(BWAPI::UnitTypes::Zerg_Drone);
                cancelStuff(50);
            }
            BuildingManager::Instance().cancelBuildingType(BWAPI::UnitTypes::Zerg_Hatchery);
        }
        return true;
    }

    // There are no hatcheries.
    if (nHatches == 0 &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Hatchery &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hatchery))
    {
        // BWAPI::Broodwar->printf("no hatcheries!");
        ProductionManager::Instance().goOutOfBookAndClearQueue();
        queue.queueAsLowestPriority(MacroAct(BWAPI::UnitTypes::Zerg_Hatchery, hiddenBaseNext() ? MacroLocation::Hidden : MacroLocation::Main));
        if (nDrones == 1)
        {
            ScoutManager::Instance().releaseWorkerScout();
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Drone);
            cancelStuff(350);
        }
        else
        {
            cancelStuff(300);
        }
        return true;
    }

    // There are < 3 drones. Make up to 3.
    // Making more than 3 breaks 4 pool openings.
    if (nDrones < 3 &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Drone &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Overlord)
    {
        ScoutManager::Instance().releaseWorkerScout();
        queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Drone);
        if (nDrones < 2)
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Drone);
        }
        // Don't cancel other stuff. A drone should be mining, it's not that big an emergency.
        return true;
    }

    // There are no drones on minerals. Turn off gas collection.
    if (_lastUpdateFrame >= 24 &&           // give it time!
        WorkerManager::Instance().isCollectingGas() &&
        nMineralPatches > 0 &&
        nDrones <= 3 * nGas &&
        WorkerManager::Instance().getNumMineralWorkers() == 0 &&
        WorkerManager::Instance().getNumReturnCargoWorkers() == 0 &&
        WorkerManager::Instance().getNumCombatWorkers() == 0 &&
        WorkerManager::Instance().getNumIdleWorkers() == 0)
    {
        // Leave the queue in place.
        ScoutManager::Instance().releaseWorkerScout();
        WorkerManager::Instance().setCollectGas(false);
        if (nHatches >= 2)
        {
            // BWAPI::Broodwar->printf("no drones on minerals, cancel hatchery");
            BuildingManager::Instance().cancelBuildingType(BWAPI::UnitTypes::Zerg_Hatchery);
        }
        return true;
    }

    if (breakOut)
    {
        return true;
    }

    return false;
}

// React to lesser emergencies.
void StrategyBossZerg::makeUrgentReaction(BuildOrderQueue & queue)
{
    // Find the next thing remaining in the queue, but only if it is a unit.
    // We may get back BWAPI::UnitTypes::None.
    const BWAPI::UnitType nextInQueue = queue.getNextUnit();

    // Anti-bunker: If the enemy has bunkered one of our bases, maybe defend with a sunken.
    if (the.enemyRace() == BWAPI::Races::Terran &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Sunken_Colony &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) == the.my.completed.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
        (hasPool || the.info.remainingBuildTime(BWAPI::UnitTypes::Zerg_Spawning_Pool) <= 300) &&
        nDrones >= 5)
    {
        for (Base * base : the.bases.getAll())
        {
            if (base->isMyCompletedBase())
            {
                BWAPI::Unitset bunkers =
                    BWAPI::Broodwar->getUnitsInRadius(base->getCenter(), 7 * 32,
                    BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Bunker && BWAPI::Filter::IsEnemy);
                // If there is more than one bunker, it may be hopeless. It's definitely more complicated.
                if (bunkers.size() == 1)
                {
                    BWAPI::Unit bunker = *bunkers.begin();
                    // If we already made a sunken here, we're done.
                    if (nullptr == BWAPI::Broodwar->getClosestUnit(
                        bunker->getPosition(),
                        BWAPI::Filter::GetType == BWAPI::UnitTypes::Zerg_Sunken_Colony && BWAPI::Filter::IsOwned,
                        7 * 32 + 16))
                    {
                        BWAPI::TilePosition tile = the.placer.getAntiBunkerSunkenPosition(base, bunker);
                        if (tile.isValid())
                        {
                            queue.queueAsHighestPriority(MacroAct(BWAPI::UnitTypes::Zerg_Sunken_Colony, tile));
                        }
                    }
                }
            }
        }
    }

    // Anti-cannon: If the enemy has us cannoned, place one sunken to prevent a cannon push.
    // Or place a sunken in range of a pylon; the sunken range bug may cause it to attack the cannon.
    // This is usually not a big hurry, so we can wait for the spawning pool if necessary.
    if (the.enemyRace() == BWAPI::Races::Protoss &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Sunken_Colony &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) == the.my.completed.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
        (hasPool || the.info.remainingBuildTime(BWAPI::UnitTypes::Zerg_Spawning_Pool) <= 300) &&
        nDrones >= 6)
    {
        for (Base * base : the.bases.getAll())
        {
            if (base->isMyCompletedBase())
            {
                BWAPI::Unit cannon =
                    BWAPI::Broodwar->getClosestUnit(base->getCenter(),
                    BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Photon_Cannon && BWAPI::Filter::IsEnemy,
                    14 * 32);
                if (cannon)
                {
                    BWAPI::TilePosition tile = the.placer.getAntiCannonSunkenPosition(base, cannon);
                    if (tile.isValid())
                    {
                        queue.queueAsHighestPriority(MacroAct(BWAPI::UnitTypes::Zerg_Sunken_Colony, tile));
                    }
                }
            }
        }
    }

    // Enemy has air. Make scourge if possible. Keep the numbers limited, especially in ZvZ.
    const int totalScourge =
        the.my.all.count(BWAPI::UnitTypes::Zerg_Scourge) +
        2 * numInEgg(BWAPI::UnitTypes::Zerg_Scourge) +
        2 * queue.numInQueue(BWAPI::UnitTypes::Zerg_Scourge);
    if (hasSpire && nGas > 0 &&
        the.info.enemyHasAirTech() &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Scourge &&
        totalScourge < 8 &&
        (_enemyRace != BWAPI::Races::Zerg || totalScourge < nMutas) &&
        the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser) < 6 &&
        the.your.seen.count(BWAPI::UnitTypes::Protoss_Corsair) < 8 &&
        (!outOfBook || queue.size() < 5))		// don't loop beyond this
    {
        // Not too much, and not too much at once. They cost a lot of gas.
        int nScourgeNeeded = std::min(8, the.info.nScourgeNeeded());
        int nToMake = 0;
        if (nScourgeNeeded > totalScourge && nLarvas > 0)
        {
            int nPairs = std::min(1 + gas / 75, (nScourgeNeeded - totalScourge + 1) / 2);
            nToMake = std::min(nPairs, 3);
        }
        for (int i = 0; i < nToMake; ++i)
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Scourge);
        }
        // And keep going.
    }

    int queueMinerals, queueGas;
    queue.totalCosts(queueMinerals, queueGas);

    // We have too much gas. Turn off gas collection.
    // Opening book sometimes collects extra gas on purpose.
    // This ties in via ELSE with the next check!
    if (outOfBook &&
        WorkerManager::Instance().isCollectingGas() &&
        gas > 300 &&
        gas > 3 * _self->minerals() &&              // raw mineral count, not adjusted for building reserve
        gas >= queueGas) // &&
        // nDrones <= maxDrones - nGasDrones &&        // no drones will become idle TODO this apparently doesn't work right
        // WorkerManager::Instance().getNumIdleWorkers() == 0) // no drones are already idle (redundant double-check)
    {
        //BWAPI::Broodwar->printf("excess gas");
        WorkerManager::Instance().setCollectGas(false);
        // And keep going.
    }

    // We're in book and should have enough gas but it's off. Something went wrong.
    // Note ELSE!
    else if (!outOfBook && queue.getNextGasCost(1) > gas && !WorkerManager::Instance().isCollectingGas())
    {
        if (nGas == 0 || nDrones < 9 && _self->deadUnitCount(BWAPI::UnitTypes::Zerg_Drone) > 1)	// it's OK to lose the scout
        {
            // Emergency. We lost the extractor, or lost drones very early, or lost many drones.
            // Give up and clear the queue.
            // BWAPI::Broodwar->printf("need more gas - breaking out");
            ProductionManager::Instance().goOutOfBookAndClearQueue();
            return;
        }
        // Not such an emergency. Turn gas on and keep going.
        WorkerManager::Instance().setCollectGas(true);
    }

    // We're out of book and trying to collect gas, but have no gas drones,
    // due to losses and likely because we are under attack.
    // Note ELSE!
    else if (outOfBook && queue.getNextGasCost(1) > gas &&
        nGas > 0 && nGasDrones == 0 && nDrones < 3 * nGas &&
        WorkerManager::Instance().isCollectingGas())
    {
        // Deadlock. Can't get gas. Give up and clear the queue.
        // BWAPI::Broodwar->printf("gas deadlock, clear queue");
        ProductionManager::Instance().goOutOfBookAndClearQueue();
        return;
    }

    // Gas is turned off, and upcoming items cost more gas than we have. Get gas.
    // NOTE isCollectingGas() can return false when gas is in the process of being turned off,
    // and some will still be collected.
    // Note ELSE!
    else if (outOfBook && queue.getNextGasCost(4) > gas && !WorkerManager::Instance().isCollectingGas())
    {
        if (nGas > 0 && nDrones > 3 * nGas)
        {
            // Leave it to the regular queue refill to add more extractors.
            WorkerManager::Instance().setCollectGas(true);
        }
        else
        {
            // Well, we can't collect gas.
            // Make enough drones to get an extractor.
            ScoutManager::Instance().releaseWorkerScout();   // don't throw off the drone count
            if (nGas == 0 && nDrones >= 5 && nFreeGas > 0 &&
                nextInQueue != BWAPI::UnitTypes::Zerg_Extractor &&
                !isBeingBuilt(BWAPI::UnitTypes::Zerg_Extractor))
            {
                queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Extractor);
            }
            else if (nGas == 0 && nDrones >= 4 && isBeingBuilt(BWAPI::UnitTypes::Zerg_Extractor))
            {
                // We have an unfinished extractor. Wait for it to finish.
                // Need 4 drones so that 1 can keep mining minerals (or the rules will loop).
                WorkerManager::Instance().setCollectGas(true);
            }
            else if (nextInQueue != BWAPI::UnitTypes::Zerg_Drone && nFreeGas > 0)
            {
                queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Drone);
            }
        }
        // And keep going.
    }

    // We're in book and want to make zerglings next, but we also want extra drones.
    // Change the zerglings to a drone, since they have the same cost.
    // When we want extra drones, _economyDrones is decreased, so we recognize that by negative values.
    // Don't make all the extra drones in book, save a couple for later, because it could mess stuff up.
    if (!outOfBook && _economyDrones < -2 && nextInQueue == BWAPI::UnitTypes::Zerg_Zergling && nLings > 0)
    {
        queue.removeHighestPriorityItem();
        queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Drone);
        ++_economyDrones;
        // And keep going.
    }

    // We need a macro hatchery.
    // Division of labor: Macro hatcheries are here, expansions are regular production.
    // However, some macro hatcheries may be placed at new expansions (it helps assert map control).
    // Macro hatcheries are automatic only out of book. Book openings must take care of themselves.
    const int hatcheriesUnstarted = BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Zerg_Hatchery);
    const int hatcheriesUnderConstruction =
        hatcheriesUnstarted
        + the.my.all.count(BWAPI::UnitTypes::Zerg_Hatchery)
        - the.my.completed.count(BWAPI::UnitTypes::Zerg_Hatchery);
    const int newHatcheryLimit = nDrones > 30 ? 4 : (nDrones > 20 ? 3 : 2);
    const bool enoughLairTechUnits =
        (_gasUnit == BWAPI::UnitTypes::Zerg_Lurker || _gasUnit == BWAPI::UnitTypes::Zerg_Mutalisk)
            ? the.my.all.count(_gasUnit) >= 4
            : true;
    // A liberal ceiling on the hatchery count, assuming production of drones + zerglings only.
    // It mainly prevents 3 hatcheries + gas with 9 drones in ZvZ.
    const int maxSensibleHatcheries = nDrones / 3 - (WorkerManager::Instance().isCollectingGas() ? nGas : 0);

    if (outOfBook && nHatches < 18 && nDrones >= 9 && _supplyUsed <= 380 &&
        nLarvas < 1 + minerals / 600 &&                     // sometimes Steamhammer doesn't spend all larvas, though it could
        minerals >= 300 + 300 * hatcheriesUnstarted &&
        (minerals > 500 || enoughLairTechUnits) &&
        (!outOfBook || nDrones > 3 * (nHatches + hatcheriesUnderConstruction)) &&
        nHatches + hatcheriesUnderConstruction < maxSensibleHatcheries &&
        hatcheriesUnderConstruction <= newHatcheryLimit &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Hatchery &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Overlord &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Lair &&       // try not to delay critical tech
        nextInQueue != BWAPI::UnitTypes::Zerg_Spire &&
        !queue.anyInNextN(BWAPI::TechTypes::Lurker_Aspect, 3) &&
        (hasSpire || the.info.remainingBuildTime(BWAPI::UnitTypes::Zerg_Spire) >= 20 * 24))	// spire not about to finish
    {
        MacroLocation loc = MacroLocation::Main;
        if (nBases <= 1 && nHatches >= 2 && nFreeBases > 0 ||
            nBases <= 2 && nHatches >= 3 && nFreeBases > 0 ||
            nHatches % 2 != 0 && nFreeBases > 2 && Random::Instance().flag(0.5))
        {
            // Expand with some macro hatcheries.
            // If we're expanding and have only one gas, insist on a second gas base.
            loc = nGas >= 2 ? MacroLocation::MinOnly : MacroLocation::Expo;
        }
        queue.queueAsHighestPriority(MacroAct(BWAPI::UnitTypes::Zerg_Hatchery, hiddenBaseNext() ? MacroLocation::Hidden : loc));
        // And keep going.
    }
    // If we are in the opening and have made compensatory drones, minerals may skyrocket.
    // Don't let them build up, take advantage!
    // NOTE We seek _extraDrones-2 extra drones during the opening (saving 2 for later).
    // NOTE We don't know what extra drones and minerals we may have due to canceling unneeded static defense.
    //      That stuff was dropped from the queue, and may have been planned for early or late.
    else if (!outOfBook && (_extraDronesWanted >= 5 || minerals > 1000))
    {
        if (_extraOpeningExtractors == 0 && nFreeGas > 0 && nDrones > 9 &&
            !(gas >= 300 && minerals < 100) &&      // not in case of excess gas + mineral shortage
            !queue.anyInNextN(BWAPI::UnitTypes::Zerg_Extractor, 3))
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Extractor);
            ++_extraOpeningExtractors;
        }
        else if (minerals >= 550 &&
            (minerals > 900 || _extraDronesWanted - 2 >= 3 * _extraOpeningExtractors + 5 * _extraOpeningHatcheries) &&
            !queue.anyInNextN(BWAPI::UnitTypes::Zerg_Hatchery, 3))
        {
            MacroLocation loc = MacroLocation::Main;
            if (_extraOpeningHatcheries == 2 ||
                OpponentModel::Instance().getDarnLikelyEnemyPlan() == OpeningPlan::SafeExpand ||
                OpponentModel::Instance().getDarnLikelyEnemyPlan() == OpeningPlan::Turtle)
            {
                loc = MacroLocation::Expo;
            }
            queue.queueAsHighestPriority(MacroAct(BWAPI::UnitTypes::Zerg_Hatchery, hiddenBaseNext() ? MacroLocation::Hidden : loc));
            ++_extraOpeningHatcheries;
        }
        // And keep going.
    }

    // If the enemy has cloaked stuff, consider overlord speed.
    if (the.info.enemyHasCloakTech())
    {
        if (hasLairOrHive &&
            minerals >= 150 && gas >= 150 &&
            _self->getUpgradeLevel(BWAPI::UpgradeTypes::Pneumatized_Carapace) == 0 &&
            !_self->isUpgrading(BWAPI::UpgradeTypes::Pneumatized_Carapace) &&
            !queue.anyInQueue(BWAPI::UpgradeTypes::Pneumatized_Carapace) &&
            (outOfBook || !queue.anyInQueue(BWAPI::UnitTypes::Zerg_Hive)))
        {
            queue.queueAsHighestPriority(MacroAct(BWAPI::UpgradeTypes::Pneumatized_Carapace));
        }
        // And keep going.
    }

    // Looks like wraiths will be coming. Make an evo now, unless we already have a spire.
    // If wraiths are already out, then the enemyHasOverlordHunters rule fires.
    if (OpponentModel::Instance().getDarnLikelyEnemyPlan() == OpeningPlan::Wraith)
    {
        if (nEvo == 0 && nDrones >= 9 && hasPool && !hasSpire &&
            !queue.anyInQueue(BWAPI::UnitTypes::Zerg_Evolution_Chamber) &&
            the.my.all.count(BWAPI::UnitTypes::Zerg_Evolution_Chamber) == 0 &&
            !isBeingBuilt(BWAPI::UnitTypes::Zerg_Evolution_Chamber))
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Evolution_Chamber);
        }
    }

    // Minerals are building up as we wait for a gas unit. Insert a drone or a zergling.
    if (minerals >= 350 &&  nLarvas > 1 && outOfBook &&
        !queue.isEmpty() &&
        queue.getHighestPriorityItem().macroAct.gasPrice() > gas &&		// waiting for enough gas
        _existingSupply - _supplyUsed >= 4 &&							// not waiting for an overlord
        _existingSupply < absoluteMaxSupply - 6)						// eh, we're nearly maxed anyway
    {
        bool preferDrone =
            nDrones < maxDrones &&
            !_emergencyGroundDefense &&
            !_emergencyNow &&
            enoughGroundArmy();

        queue.queueAsHighestPriority(preferDrone || !hasPool ? BWAPI::UnitTypes::Zerg_Drone : BWAPI::UnitTypes::Zerg_Zergling);
    }

    // We are out of book and have no basic ground units. Make a unit!
    // The basic ground units are zerglings and hydralisks.
    // It's important to keep tabs on the enemy.
    if (outOfBook && nLings == 0 && nHydras == 0 && nDrones >= 9 &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Zergling &&
        nextInQueue != BWAPI::UnitTypes::Zerg_Hydralisk)
    {
        if (hasPool)
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Zergling);
        }
        else if (hasDen && nGas > 0)
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Zerg_Hydralisk);
        }
    }
}

// Make special reactions to specific opponent opening plans.
// Return whether any action is taken.
// This is part of freshProductionPlan().
bool StrategyBossZerg::adaptToEnemyOpeningPlan()
{
    OpeningPlan plan = OpponentModel::Instance().getEnemyPlan();

    if (plan == OpeningPlan::WorkerRush || plan == OpeningPlan::Proxy || plan == OpeningPlan::FastRush)
    {
        // We react with 9 pool, or pool next if we have >= 9 drones.
        // "Proxy" here means a proxy in our main base or natural in the opening.
        // Part of the reaction is handled here, and part in takeUrgentAction().

        const bool poolStarted =
            isBeingBuilt(BWAPI::UnitTypes::Zerg_Spawning_Pool) ||
            the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) > 0;

        if (!hasPool &&
            nDrones >= 5 &&
            nDrones + _self->deadUnitCount(BWAPI::UnitTypes::Zerg_Drone) >= 9 &&
            !poolStarted)
        {
            produce(BWAPI::UnitTypes::Zerg_Spawning_Pool);
            produce(BWAPI::UnitTypes::Zerg_Drone);
            return true;
        }

        // If the spawning pool is under construction, save larvas, except against worker rush.
        if (!hasPool && poolStarted && nDrones >= 3 && plan != OpeningPlan::WorkerRush && nLings <= 5)
        {
            return true;
        }

        if (nDrones < 9)
        {
            // Mix in zerglings if possible and necessary. Can happen if we lost drones.
            if (hasPool && nDrones >= 3 && !enoughGroundArmy())
            {
                produce(BWAPI::UnitTypes::Zerg_Zergling);
            }
            produce(BWAPI::UnitTypes::Zerg_Drone);
            return true;
        }

        // If none of the above rules fires, we fall through and let the regular
        // production plan take effect.
    }
    
    return false;
}

// We always want 9 drones and a spawning pool. Return whether any action was taken.
// This is part of freshProductionPlan().
bool StrategyBossZerg::rebuildCriticalLosses()
{
    // 1. Add up to 9 drones if we're below.
    if (nDrones < 9)
    {
        // If we're behind in army, mix in zerglings so we have a chance to live.
        if (hasPool && nDrones >= 3 && !enoughArmy())
        {
            produce(BWAPI::UnitTypes::Zerg_Zergling);
        }
        produce(BWAPI::UnitTypes::Zerg_Drone);
        return true;
    }

    // 2. If there is no spawning pool, we always need that.
    if (!hasPool &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Spawning_Pool) &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) == 0)
    {
        produce(BWAPI::UnitTypes::Zerg_Spawning_Pool);
        // If we're low on drones, replace the drone.
        if (nDrones <= 9 && nDrones <= maxDrones)
        {
            produce(BWAPI::UnitTypes::Zerg_Drone);
        }
        return true;
    }

    return false;
}

// Check for possible ground attacks that we are may have trouble handling.
// React when it seems necessary, with sunkens, zerglings, or by pulling drones.
// If the opening book seems unready for the situation, break out of book.
// If a deadly attack seems impending, declare an emergency so that the
// regular production plan will concentrate on combat units.
void StrategyBossZerg::checkGroundDefenses(BuildOrderQueue & queue)
{
    // 1. Figure out where our front defense line is.
    Base * frontBase = the.bases.myFront();
    if (!frontBase)
    {
        // We don't have a place to defend at. It's that bad.
        return;
    }
    BWAPI::Unit ourHatchery = frontBase->getDepot();

    // 2. Count enemy ground power.
    int enemyPower = 0;
    int enemyPowerNearby = 0;
    int enemyPowerInOurFace = 0;
    int enemyDragoons = 0;
    int enemyMarines = 0;
    int enemyAcademyUnits = 0;    // count firebats and medics
    int enemyVultures = 0;

    for (const auto & kv : the.info.getUnitData(_enemy).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (!ui.type.isBuilding() && !ui.type.isWorker() &&
            !ui.type.isFlyer() &&
            UnitUtil::TypeCanAttackGround(ui.type))
        {
            int power = ui.type.supplyRequired();
            // Some types count more.
            if (ui.type == BWAPI::UnitTypes::Protoss_Dark_Templar)
            {
                power += 5;
            }
            else if (ui.type == BWAPI::UnitTypes::Protoss_Dragoon)
            {
                power += 1;
                enemyDragoons += 1;
            }
            enemyPower += power;
            if (ui.updateFrame >= _lastUpdateFrame - 30 * 24 &&         // seen in the last 30 seconds
                ui.lastPosition.isValid())                              // don't check goneFromLastPosition
            {
                int dist = ourHatchery->getDistance(ui.lastPosition);
                if (dist < 1500)	                                    // not far from our front base
                {
                    enemyPowerNearby += power;
                    if (dist < 384)
                    {
                        enemyPowerInOurFace += power;
                    }
                }
            }
            if (ui.type == BWAPI::UnitTypes::Terran_Marine)
            {
                ++enemyMarines;
            }
            if (ui.type == BWAPI::UnitTypes::Terran_Firebat || ui.type == BWAPI::UnitTypes::Terran_Medic)
            {
                // Ghosts are academy units but don't matter for this.
                ++enemyAcademyUnits;
            }
            else if (ui.type == BWAPI::UnitTypes::Terran_Vulture)
            {
                ++enemyVultures;
            }
        }
    }
    if (enemyDragoons >= 4)
    {
        enemyPower += 2 * enemyDragoons;
    }

    // 3. Count our anti-ground power, including air units.
    int ourPower = 0;
    int ourSunkens = 0;
    for (BWAPI::Unit u : _self->getUnits())
    {
        if (!u->getType().isBuilding() && !u->getType().isWorker() &&
            UnitUtil::TypeCanAttackGround(u->getType()))
        {
            ourPower += u->getType().supplyRequired();
        }
        else if (u->getType() == BWAPI::UnitTypes::Zerg_Sunken_Colony)
        {
            if (ourHatchery->getDistance(u) < 10 * 32)
            {
                ++ourSunkens;
            }
        }
    }

    int queuedSunkens =			// without checking location
        queue.numInQueue(BWAPI::UnitTypes::Zerg_Sunken_Colony) +
        BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Zerg_Sunken_Colony);
    int totalSunkens = ourSunkens + queuedSunkens;
    ourPower += 5 * totalSunkens;

    // 5. Declare an emergency.
    // The nHatches term adjusts for what we may be able to build before the enemy arrives.
    if (enemyPowerNearby > ourPower + nHatches)
    {
        _emergencyGroundDefense = true;
        _emergencyStartFrame = _lastUpdateFrame;
    }

    // _emergencyNow is updated independently of _emergencyGroundDefense.
    _emergencyNow = enemyPowerInOurFace > ourPower;
}

// If the enemy expanded or made static defense, we can spawn extra drones.
// Also try to compensate if we made sunkens.
// Exception: Enemy static defense near our base is a proxy.
void StrategyBossZerg::analyzeExtraDrones()
{
    if (_nonadaptive)
    {
        _extraDronesWanted = 0;
        return;
    }

    // 50 + 1/8 overlord = 62.5 minerals per drone.
    // Let's be a little more conservative than that, since we may scout it late.
    const double droneCost = 75;

    double extraDrones = 0.0;

    // Enemy bases beyond the main.
    int nBases = 0;
    for (const Base * base : the.bases.getAll())
    {
        if (base->getOwner() == _enemy)
        {
            ++nBases;
        }
    }
    if (nBases > 1)
    {
        extraDrones += (nBases - 1) * 300.0 / droneCost;
    }

    // Enemy static defenses.
    // We don't care whether they are completed or not.
    for (const auto & kv : the.info.getUnitData(_enemy).getUnits())
    {
        const UnitInfo & ui(kv.second);

        // A proxy near our starting base is not static defense, it is offense.
        // We don't care whether the building is lifted.
        // The starting base is guaranteed non-null.
        if (ui.type.isBuilding() &&
            !ui.goneFromLastPosition &&		// terran building might float out of sight
            ui.lastPosition.isValid() &&
            the.bases.myStart()->getPosition().getApproxDistance(ui.lastPosition) > 800)
        {
            if (ui.type == BWAPI::UnitTypes::Zerg_Creep_Colony)
            {
                extraDrones += 1.0 + 75.0 / droneCost;
            }
            else if (ui.type == BWAPI::UnitTypes::Zerg_Sunken_Colony || ui.type == BWAPI::UnitTypes::Zerg_Spore_Colony)
            {
                extraDrones += 1.0 + 125.0 / droneCost;
            }
            else if (ui.type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                ui.type == BWAPI::UnitTypes::Protoss_Shield_Battery ||
                ui.type == BWAPI::UnitTypes::Terran_Missile_Turret ||
                ui.type == BWAPI::UnitTypes::Terran_Bunker)
            {
                extraDrones += ui.type.mineralPrice() / droneCost;
            }
        }
    }

    // Account for our own static defense.
    // It helps keep us safe, so we should be able to make more drones than otherwise.
    int nSunks = the.my.all.count(BWAPI::UnitTypes::Zerg_Sunken_Colony);
    extraDrones += 1.8 * nSunks;

    // In ZvZ, deliberately undercompensate, because making too many drones is death.
    if (_enemyRace == BWAPI::Races::Zerg)
    {
        extraDrones *= 0.5;
    }

    // Enemy bases/static defense may have been added or destroyed, or both.
    // We don't keep track of what is destroyed, and react only to what is added since last check.
    int nExtraDrones = int(trunc(extraDrones));
    if (nExtraDrones > _extraDronesWanted)
    {
        _economyDrones -= nExtraDrones - _extraDronesWanted;   // pretend we made fewer drones
    }
    _extraDronesWanted = nExtraDrones;
}

bool StrategyBossZerg::lairTechUnit(TechUnit techUnit) const
{
    return
        techUnit == TechUnit::Mutalisks ||
        techUnit == TechUnit::Lurkers;
}

bool StrategyBossZerg::airTechUnit(TechUnit techUnit) const
{
    return
        techUnit == TechUnit::Mutalisks ||
        techUnit == TechUnit::Guardians ||
        techUnit == TechUnit::Devourers;
}

bool StrategyBossZerg::hiveTechUnit(TechUnit techUnit) const
{
    return
        techUnit == TechUnit::Ultralisks ||
        techUnit == TechUnit::Guardians ||
        techUnit == TechUnit::Devourers;
}

int StrategyBossZerg::techTier(TechUnit techUnit) const
{
    if (techUnit == TechUnit::Zerglings || techUnit == TechUnit::Hydralisks)
    {
        return 1;
    }

    if (techUnit == TechUnit::Lurkers || techUnit == TechUnit::Mutalisks)
    {
        // Lair tech.
        return 2;
    }

    if (techUnit == TechUnit::Ultralisks || techUnit == TechUnit::Guardians || techUnit == TechUnit::Devourers)
    {
        // Hive tech.
        return 3;
    }

    return 0;
}

// How much time left in the lair's current upgrade?
// 0 if it's not upgrading, infinity if there is no lair.
// We assume we have only one lair.
// NOTE It's easy to upgrade this to "when does this building time become free?"
int StrategyBossZerg::lairUpgradeTime() const
{
    for (BWAPI::Unit unit : _self->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Lair)
        {
            return unit->getRemainingUpgradeTime();
        }
    }

    return MAX_FRAME;
}

// We want to build a hydra den for lurkers. Is it time yet?
// We want to time is so that when the den finishes, lurker aspect research can start right away.
bool StrategyBossZerg::lurkerDenTiming() const
{
    if (hasLairTech)
    {
        // Lair is already finished. Den can start any time.
        return true;
    }

    return
        the.info.remainingBuildTime(BWAPI::UnitTypes::Zerg_Lair) <= 100 + (BWAPI::UnitTypes::Zerg_Hydralisk_Den).buildTime();
}

void StrategyBossZerg::resetTechScores()
{
    for (int i = 0; i < int(TechUnit::Size); ++i)
    {
        techScores[i] = 0;
    }
}

// A tech unit is available for selection in the unit mix if we have the tech for it.
// That's what this routine figures out.
// It is available for selection as a tech target if we do NOT have the tech for it.
void StrategyBossZerg::setAvailableTechUnits(std::array<bool, int(TechUnit::Size)> & available)
{
    available[int(TechUnit::None)] = false;       // avoid doing nothing if at all possible

    // Tier 1.
    available[int(TechUnit::Zerglings)] = hasPool;
    available[int(TechUnit::Hydralisks)] = hasDen && nGas > 0;

    // Lair tech.
    available[int(TechUnit::Lurkers)] = hasLurkers && nGas > 0;
    available[int(TechUnit::Mutalisks)] = hasSpire && nGas > 0;

    // Hive tech.
    available[int(TechUnit::Ultralisks)] = hasUltra && hasUltraUps && nGas >= 2;
    available[int(TechUnit::Guardians)] = hasGreaterSpire && nGas >= 2;
    available[int(TechUnit::Devourers)] = hasGreaterSpire && nGas >= 2;
}

// Set flags or counts to recommend that certain things are worth having.
void StrategyBossZerg::recommendTech()
{
    if (_enemyRace == BWAPI::Races::Terran)
    {
        _recommendDefensiveSpire =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Dropship) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Terran_Science_Vessel) > 0;

        // Always make a queen if there is an enemy base, because we may be able to infest a command center.
        _recommendParasite = the.bases.baseCount(the.enemy()) > 0 ? 1 : 0;
        _recommendEnsnare =
            (the.your.seen.count(BWAPI::UnitTypes::Terran_Marine) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Medic) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Firebat) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Ghost) +
            6 * the.your.seen.count(BWAPI::UnitTypes::Terran_Wraith) +
            2 * the.your.seen.count(BWAPI::UnitTypes::Terran_Valkyrie))
                / 48;
        const int nTanks =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);
        const int enemyAirToAir =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Wraith) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Valkyrie);
        const int enemyAirToGround =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Wraith) +
            4 * the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser);
        _recommendBroodling =
            (nTanks < 4 || enemyAirToAir >= 8) ? 0 : nTanks / 2;
    }
    else if (_enemyRace == BWAPI::Races::Protoss)
    {
        _recommendDefensiveSpire =
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Shuttle) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Arbiter) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Carrier) > 0;

        const bool hasDarkArchon = the.your.seen.count(BWAPI::UnitTypes::Protoss_Dark_Archon);

        _recommendParasite =
            !hasDarkArchon &&
            (the.your.seen.count(BWAPI::UnitTypes::Protoss_Shuttle) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Arbiter) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Carrier) > 0)
                ? 1 : 0;

        _recommendEnsnare = hasDarkArchon
            ? 0
            : (the.your.seen.count(BWAPI::UnitTypes::Protoss_Corsair) >= 7 ? 1 : 0) +
              (the.your.seen.count(BWAPI::UnitTypes::Protoss_Scout) >= 6 ? 1 : 0) +
              (the.your.seen.count(BWAPI::UnitTypes::Protoss_Dark_Templar) >= 8 ? 1 : 0);

        const int nTemplar = the.your.seen.count(BWAPI::UnitTypes::Protoss_High_Templar);
        _recommendBroodling =
            (nTemplar < 6 || the.your.seen.count(BWAPI::UnitTypes::Protoss_Corsair) >= 7) ? 0 : nTemplar / 2;
    }
    else
    {
        // Zerg.
        _recommendDefensiveSpire = false;

        _recommendParasite = 0;
        _recommendEnsnare =
            (the.your.seen.count(BWAPI::UnitTypes::Zerg_Mutalisk) > 12 ||
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Hydralisk) > 16)
                ? 1 : 0;
        _recommendBroodling =
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Lurker) / 4 +
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Defiler) / 2 +
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Ultralisk);
    }

    _recommendQueens = std::min(
        Config::Skills::MaxQueens,
        std::max({ _recommendParasite, _recommendEnsnare, _recommendBroodling })
    );
}

// Decide what units counter the terran unit mix.
void StrategyBossZerg::vTerranTechScores(const PlayerSnapshot & snap)
{
    _wantAirArmor = snap.count(BWAPI::UnitTypes::Terran_Valkyrie) >= 3;

    // Bias.
    techScores[int(TechUnit::Mutalisks)]  =   1;   // default lair tech
    techScores[int(TechUnit::Ultralisks)] =  25;   // default hive tech
    techScores[int(TechUnit::Guardians)]  =   6;   // other hive tech
    techScores[int(TechUnit::Devourers)]  =   3;   // other hive tech

    // Hysteresis.
    if (_techTarget != TechUnit::None)
    {
        techScores[int(_techTarget)] += 13;
    }

    for (const std::pair<BWAPI::UnitType, int> & unitCount : snap.getCounts())
    {
        BWAPI::UnitType type = unitCount.first;
        int count = unitCount.second;

        if (type == BWAPI::UnitTypes::Terran_Marine ||
            type == BWAPI::UnitTypes::Terran_Medic ||
            type == BWAPI::UnitTypes::Terran_Ghost)
        {
            if (type == BWAPI::UnitTypes::Terran_Medic)
            {
                // Medics make other infantry much more effective vs ground, especially vs tier 1.
                techScores[int(TechUnit::Zerglings)] -= count;
                techScores[int(TechUnit::Hydralisks)] -= count * 2;
            }
            techScores[int(TechUnit::Lurkers)] += count * 2;
            techScores[int(TechUnit::Guardians)] += count;
            techScores[int(TechUnit::Ultralisks)] += count * 3;
            defilerScore += 1;
        }
        else if (type == BWAPI::UnitTypes::Terran_Firebat)
        {
            techScores[int(TechUnit::Zerglings)] -= count * 2;
            techScores[int(TechUnit::Mutalisks)] += count * 2;
            techScores[int(TechUnit::Lurkers)] += count * 2;
            techScores[int(TechUnit::Guardians)] += count * 2;
            techScores[int(TechUnit::Ultralisks)] += count * 4;
        }
        else if (type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
        {
            techScores[int(TechUnit::Zerglings)] -= count;
            techScores[int(TechUnit::Lurkers)] -= count;
            techScores[int(TechUnit::Mutalisks)] += count;
            techScores[int(TechUnit::Guardians)] += count;
            techScores[int(TechUnit::Ultralisks)] -= count;
            defilerScore -= 1;
        }
        else if (type == BWAPI::UnitTypes::Terran_Vulture)
        {
            techScores[int(TechUnit::Zerglings)] -= count * 3;
            techScores[int(TechUnit::Hydralisks)] += count * 2;
            techScores[int(TechUnit::Lurkers)] -= count * 2;
            techScores[int(TechUnit::Mutalisks)] += count * 3;
            techScores[int(TechUnit::Ultralisks)] += count;
        }
        else if (type == BWAPI::UnitTypes::Terran_Goliath)
        {
            techScores[int(TechUnit::Zerglings)] -= count * 3;
            techScores[int(TechUnit::Hydralisks)] += count * 3;
            techScores[int(TechUnit::Lurkers)] -= count * 2;
            techScores[int(TechUnit::Mutalisks)] -= count * 3;
            techScores[int(TechUnit::Guardians)] -= count * 2;
            techScores[int(TechUnit::Ultralisks)] += count * 5;
        }
        else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
        {
            techScores[int(TechUnit::Zerglings)] += count;
            techScores[int(TechUnit::Hydralisks)] -= count * 5;
            techScores[int(TechUnit::Mutalisks)] += count * 6;
            techScores[int(TechUnit::Guardians)] += count * 5;
            techScores[int(TechUnit::Lurkers)] -= count * (the.info.enemyHasMobileDetection() ? 7 : 4);
        }
        else if (type == BWAPI::UnitTypes::Terran_Wraith)
        {
            techScores[int(TechUnit::Hydralisks)] += count * 5;
            techScores[int(TechUnit::Lurkers)] -= count * 2;
            techScores[int(TechUnit::Guardians)] -= count * 4;
            techScores[int(TechUnit::Devourers)] += count * 4;
            defilerScore += 2;
        }
        else if (type == BWAPI::UnitTypes::Terran_Valkyrie)
        {
            techScores[int(TechUnit::Hydralisks)] += count * 3;
            // Large valkyrie counts mow down mutalisks in passing.
            techScores[int(TechUnit::Mutalisks)] -= count * count * 2;
            techScores[int(TechUnit::Guardians)] -= count * count * 4;
            techScores[int(TechUnit::Devourers)] += count * 7;
        }
        else if (type == BWAPI::UnitTypes::Terran_Battlecruiser)
        {
            techScores[int(TechUnit::Hydralisks)] += count * 8;
            techScores[int(TechUnit::Mutalisks)] -= count * 2;
            techScores[int(TechUnit::Guardians)] -= count * 7;
            techScores[int(TechUnit::Devourers)] += count * 6;
            techScores[int(TechUnit::Ultralisks)] -= count * 6;
            defilerScore += 8;
        }
        else if (type == BWAPI::UnitTypes::Terran_Missile_Turret)
        {
            techScores[int(TechUnit::Zerglings)] += count;
            techScores[int(TechUnit::Hydralisks)] += count;
            techScores[int(TechUnit::Lurkers)] -= count;
            techScores[int(TechUnit::Ultralisks)] += count * 2;
        }
        else if (type == BWAPI::UnitTypes::Terran_Bunker)
        {
            techScores[int(TechUnit::Ultralisks)] += count * 4;
            techScores[int(TechUnit::Guardians)] += count * 4;
            defilerScore += 4;
        }
        else if (type == BWAPI::UnitTypes::Terran_Science_Vessel)
        {
            techScores[int(TechUnit::Mutalisks)] -= count;
            techScores[int(TechUnit::Ultralisks)] += count;
            techScores[int(TechUnit::Guardians)] -= count;
        }
        else if (type == BWAPI::UnitTypes::Terran_Dropship)
        {
            techScores[int(TechUnit::Mutalisks)] += count * 8;
            techScores[int(TechUnit::Ultralisks)] += count;
        }
        else if (type == BWAPI::UnitTypes::Terran_Starport)
        {
            // Multiple starports and no known vessels or dropships: Wraiths seem likely.
            if (count >= 2 &&
                the.your.seen.count(BWAPI::UnitTypes::Terran_Science_Vessel) == 0 &&
                the.your.seen.count(BWAPI::UnitTypes::Terran_Dropship) == 0)
            {
                techScores[int(TechUnit::Hydralisks)] += count * 2;
                techScores[int(TechUnit::Lurkers)] -= count;
                techScores[int(TechUnit::Guardians)] -= count * 2;
                techScores[int(TechUnit::Devourers)] += count * 2;
                defilerScore += 1;
            }
        }
    }
}

void StrategyBossZerg::vProtossTechScores(const PlayerSnapshot & snap)
{
    _wantAirArmor = snap.count(BWAPI::UnitTypes::Protoss_Corsair) >= 4;

    // Bias.
    techScores[int(TechUnit::Hydralisks)] =  11;
    techScores[int(TechUnit::Ultralisks)] =  18;   // default hive tech
    techScores[int(TechUnit::Guardians)]  =   4;   // other hive tech
    techScores[int(TechUnit::Devourers)]  =   0;   // no devourers unless they have a job

    // Hysteresis.
    if (_techTarget != TechUnit::None)
    {
        techScores[int(_techTarget)] += 11;
    }
    
    // If hydra upgrades are done, favor lurkers more.
    int lurkerBonus = 0;
    if (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Grooved_Spines) > 0)
    {
        lurkerBonus += 1;
        if (hasLairTech)
        {
            lurkerBonus += 1;
        }
    }

    for (const std::pair<BWAPI::UnitType, int> & unitCount : snap.getCounts())
    {
        BWAPI::UnitType type = unitCount.first;
        int count = unitCount.second;

        if (!type.isWorker() && !type.isBuilding() && type != BWAPI::UnitTypes::Protoss_Interceptor)
        {
            // The base score: Enemy mobile combat units.
            techScores[int(TechUnit::Hydralisks)] += count * (type.supplyRequired() + 1);       // hydras vs. all
            if (type.isFlyer())
            {
                // Enemy air units.
                techScores[int(TechUnit::Hydralisks)] += count * (type.supplyRequired() + 1);   // more for hydras vs. air
                if (type == BWAPI::UnitTypes::Protoss_Corsair || type == BWAPI::UnitTypes::Protoss_Scout)
                {
                    techScores[int(TechUnit::Mutalisks)] -= count * (type.supplyRequired() + 2);
                    techScores[int(TechUnit::Guardians)] -= 8 * count * type.supplyRequired();
                    techScores[int(TechUnit::Devourers)] += 4 * count * (type.supplyRequired() + 3);
                }
                else if (type == BWAPI::UnitTypes::Protoss_Carrier)
                {
                    techScores[int(TechUnit::Guardians)] -= 3 * count * (type.supplyRequired() + 2);
                    techScores[int(TechUnit::Devourers)] += count * (type.supplyRequired() - 1);
                }
                if (type != BWAPI::UnitTypes::Protoss_Corsair && count >= 4)
                {
                    defilerScore += 4;
                }
            }
            else
            {
                // Enemy ground units.
                if (type == BWAPI::UnitTypes::Protoss_Dark_Templar)
                {
                    techScores[int(TechUnit::Zerglings)] -= count * type.supplyRequired();
                }
                techScores[int(TechUnit::Lurkers)] += count * (type.supplyRequired() + lurkerBonus);
                techScores[int(TechUnit::Ultralisks)] += count * (type.supplyRequired() + 1);
                if (type != BWAPI::UnitTypes::Protoss_Dragoon)
                {
                    techScores[int(TechUnit::Guardians)] += count * type.supplyRequired();
                }
                else if (count >= 6)
                {
                    // Many dragoons. Defilers are good.
                    defilerScore += 4;
                }
            }

            // Various adjustments to the score.
            if (!UnitUtil::TypeCanAttackAir(type) && type != BWAPI::UnitTypes::Protoss_Observer)
            {
                // Enemy units that cannot shoot up.

                techScores[int(TechUnit::Mutalisks)] += count * (type.supplyRequired() + 4);
                techScores[int(TechUnit::Guardians)] += count * type.supplyRequired();

                // Stuff that extra-favors spire.
                if (type == BWAPI::UnitTypes::Protoss_High_Templar ||
                    type == BWAPI::UnitTypes::Protoss_Shuttle ||
                    type == BWAPI::UnitTypes::Protoss_Reaver)
                {
                    techScores[int(TechUnit::Mutalisks)] += 2 * count * type.supplyRequired();

                    // And other adjustments for some of the units.
                    if (type == BWAPI::UnitTypes::Protoss_High_Templar)
                    {
                        // OK, not hydras so much versus high templar.
                        techScores[int(TechUnit::Hydralisks)] -= count * (type.supplyRequired() + 1);
                        techScores[int(TechUnit::Guardians)] -= count * 2;
                    }
                    else if (type == BWAPI::UnitTypes::Protoss_Reaver)
                    {
                        techScores[int(TechUnit::Hydralisks)] -= count * type.supplyRequired();
                        techScores[int(TechUnit::Mutalisks)] += 3 * count;
                        // Reavers eat lurkers, yum.
                        techScores[int(TechUnit::Lurkers)] -= count * type.supplyRequired();
                        techScores[int(TechUnit::Ultralisks)] -= count;
                    }
                }
            }

            if (type == BWAPI::UnitTypes::Protoss_Archon ||
                type == BWAPI::UnitTypes::Protoss_Dragoon ||
                type == BWAPI::UnitTypes::Protoss_Scout)
            {
                // Enemy units that counter air units but suffer against hydras.
                // Note that scouts are double-counted due to being checked above.
                techScores[int(TechUnit::Hydralisks)] += count * type.supplyRequired();
                techScores[int(TechUnit::Mutalisks)] -= count * type.supplyRequired();
                if (type == BWAPI::UnitTypes::Protoss_Dragoon)
                {
                    techScores[int(TechUnit::Zerglings)] += count * 4;  // lings are also OK vs goons
                }
                else if (type == BWAPI::UnitTypes::Protoss_Archon)
                {
                    techScores[int(TechUnit::Zerglings)] -= count * 4;  // but bad against archons
                }
            }
        }
        else if (type == BWAPI::UnitTypes::Protoss_Photon_Cannon)
        {
            // Hydralisks are efficient against cannons.
            OpeningPlan plan = OpponentModel::Instance().getDarnLikelyEnemyPlan();
            techScores[int(TechUnit::Hydralisks)] += count * 2;
            if (plan == OpeningPlan::Proxy || plan == OpeningPlan::Contain)
            {
                // Favor hydras much more. We need to break the cannons.
                techScores[int(TechUnit::Zerglings)] -= count * 3;
                techScores[int(TechUnit::Hydralisks)] += count * 3;
            }
            techScores[int(TechUnit::Lurkers)] -= count * 3;
            techScores[int(TechUnit::Ultralisks)] += count * 6;
            techScores[int(TechUnit::Guardians)] += count * 6;
        }
    }

    // Score tech buildings. The count only matters for production buildings.
    // NOTE Using `ever` means it could be an inferred tech building.

    if (int count = the.your.seen.count(BWAPI::UnitTypes::Protoss_Robotics_Facility))
    {
        // Observers are quick to get if they already have robo.
        techScores[int(TechUnit::Lurkers)] -= 5;
        // Spire is good against anything from the robo fac.
        techScores[int(TechUnit::Mutalisks)] += count * 6;
    }

    if (the.your.ever.count(BWAPI::UnitTypes::Protoss_Robotics_Support_Bay) > 0)
    {
        // Reavers eat lurkers.
        techScores[int(TechUnit::Lurkers)] -= 6;
        // Spire is especially good against reavers,
        // and may help against speed shuttles.
        techScores[int(TechUnit::Mutalisks)] += 10;
    }

    if (the.your.ever.count(BWAPI::UnitTypes::Protoss_Stargate) > 0)
    {
        // Merely having a stargate discourages guardians.
        techScores[int(TechUnit::Guardians)] -= 6;
    }
}

// Decide what units counter the zerg unit mix.
void StrategyBossZerg::vZergTechScores(const PlayerSnapshot & snap)
{
    // Bias.
    techScores[int(TechUnit::Zerglings)]  =   1;
    techScores[int(TechUnit::Mutalisks)]  =   3;   // default lair tech
    techScores[int(TechUnit::Ultralisks)] =  11;   // default hive tech

    // Hysteresis.
    if (_techTarget != TechUnit::None)
    {
        techScores[int(_techTarget)] += 4;
    }

    // NOTE Nothing decreases the zergling score or increases the hydra score.
    //      We never go hydra in ZvZ.
    //      But after getting hive we may go lurkers.
    for (const std::pair<BWAPI::UnitType, int> & unitCount : snap.getCounts())
    {
        BWAPI::UnitType type = unitCount.first;
        int count = unitCount.second;

        if (type == BWAPI::UnitTypes::Zerg_Sunken_Colony)
        {
            techScores[int(TechUnit::Zerglings)] -= count;
            techScores[int(TechUnit::Mutalisks)] += count * 2;
            techScores[int(TechUnit::Ultralisks)] += count * 2;
            techScores[int(TechUnit::Guardians)] += count;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Spore_Colony)
        {
            techScores[int(TechUnit::Zerglings)] += count;
            techScores[int(TechUnit::Ultralisks)] += count * 2;
            techScores[int(TechUnit::Guardians)] += count;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Zergling)
        {
            techScores[int(TechUnit::Mutalisks)] += count;
            if (hasHiveTech)
            {
                techScores[int(TechUnit::Lurkers)] += count;
            }
        }
        else if (type == BWAPI::UnitTypes::Zerg_Hydralisk)
        {
            techScores[int(TechUnit::Mutalisks)] += count * 2;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Lurker)
        {
            techScores[int(TechUnit::Mutalisks)] += count * 4;
            techScores[int(TechUnit::Guardians)] += count * 3;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Mutalisk)
        {
            techScores[int(TechUnit::Mutalisks)] += count;
            techScores[int(TechUnit::Lurkers)] -= count * 2;
            techScores[int(TechUnit::Guardians)] -= count * 3;
            techScores[int(TechUnit::Devourers)] += count * 2;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Scourge)
        {
            techScores[int(TechUnit::Zerglings)] += count;
            techScores[int(TechUnit::Ultralisks)] += count;
            techScores[int(TechUnit::Guardians)] -= count * 4;
            techScores[int(TechUnit::Devourers)] -= count * 4;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Guardian)
        {
            techScores[int(TechUnit::Lurkers)] -= count * 2;
            techScores[int(TechUnit::Mutalisks)] += count * 2;
            techScores[int(TechUnit::Devourers)] += count;
        }
        else if (type == BWAPI::UnitTypes::Zerg_Devourer)
        {
            techScores[int(TechUnit::Mutalisks)] -= count * 2;
            techScores[int(TechUnit::Ultralisks)] += count;
            techScores[int(TechUnit::Guardians)] -= count * 2;
            techScores[int(TechUnit::Devourers)] += count;
        }
    }
}

// Calculate scores used to decide on tech target and unit mix, based on what the opponent has.
void StrategyBossZerg::calculateTechScores(int lookaheadFrames)
{
    resetTechScores();		// reset all to 0
    defilerScore = 0;

    PlayerSnapshot snap(_enemy);

    recommendTech();

    _enemySeemsToBeDead = enemySeemsToBeDead();
    if (_enemySeemsToBeDead)
    {
        // Special case: We'll tech to and make mutalisks to finish off the enemy.
        return;
    }
    
    if (_enemyRace == BWAPI::Races::Protoss)
    {
        vProtossTechScores(snap);
    }
    else if (_enemyRace == BWAPI::Races::Terran)
    {
        vTerranTechScores(snap);
    }
    else if (_enemyRace == BWAPI::Races::Zerg)
    {
        vZergTechScores(snap);
    }

    // Otherwise enemy went random and we haven't seen any enemy unit yet.
    // Leave all the tech scores as 0 and go with the defaults.

    // Upgrades make units more valuable.
    if (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Metabolic_Boost) > 0)
    {
        techScores[int(TechUnit::Zerglings)] += 5;
    }
    if (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Adrenal_Glands) > 0)
    {
        techScores[int(TechUnit::Zerglings)] += 15;
    }
    if (hasUltraUps)
    {
        techScores[int(TechUnit::Ultralisks)] += 24;
    }

    int meleeUpScore =
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Melee_Attacks) +
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Carapace);
    techScores[int(TechUnit::Zerglings)] += 2 * meleeUpScore;
    techScores[int(TechUnit::Ultralisks)] += 4 * meleeUpScore;

    int missileUpScore =
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Missile_Attacks) +
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Carapace);
    techScores[int(TechUnit::Hydralisks)] += 2 * missileUpScore;
    techScores[int(TechUnit::Lurkers)] += 3 * missileUpScore;

    int airUpScore =
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks) +
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Flyer_Carapace);
    techScores[int(TechUnit::Mutalisks)] += airUpScore;
    techScores[int(TechUnit::Guardians)] += 2 * airUpScore;
    techScores[int(TechUnit::Devourers)] += 2 * airUpScore;

    // Undetected lurkers are more valuable.
    if (!the.info.enemyHasMobileDetection())
    {
        if (!the.info.enemyHasStaticDetection())
        {
            techScores[int(TechUnit::Lurkers)] += 5;
        }

        if (techScores[int(TechUnit::Lurkers)] == 0)
        {
            techScores[int(TechUnit::Lurkers)] = 3;
        }
        else
        {
            techScores[int(TechUnit::Lurkers)] = 3 * techScores[int(TechUnit::Lurkers)] / 2;
        }
    }

    // If all known enemy buildings are unreachable by ground, strongly prefer air.
    // The enemy may still have army and transport, so we don't want to rule out ground units altogether.
    if (enemyIsUnreachable())
    {
        techScores[int(TechUnit::Zerglings)] /= 2;
        techScores[int(TechUnit::Hydralisks)] /= 2;
        techScores[int(TechUnit::Lurkers)] /= 2;
        techScores[int(TechUnit::Ultralisks)] /= 2;
    }
}

// Choose the next tech to aim for, whether sooner or later.
// This tells freshProductionPlan() what to move toward, not when to take each step.
void StrategyBossZerg::chooseTechTarget()
{
    // Special case: Tech to mutas to finish off the enemy.
    if (_enemySeemsToBeDead)
    {
        _techTarget = TechUnit::None;
        if (!hasSpire)
        {
            _techTarget = TechUnit::Mutalisks;
        }
        return;
    }

    // Special case: If zerglings are bad and hydras are good, and it will take a long time
    // to get lair tech, then short-circuit the process and call for hydralisk tech.
    // This happens when terran goes vultures and we are still on zerglings.
    // It can also happen against protoss.
    if (techScores[int(TechUnit::Zerglings)] <= 0 &&
        techScores[int(TechUnit::Hydralisks)] > 0 &&
        !hasDen && !goingIslandAir &&
        nLairs + nHives == 0)                           // no lair (or hive) started yet
    {
        _techTarget = TechUnit::Hydralisks;
        return;
    }

    // Find our current tech tier.
    int theTier = 1;           // we can assume a spawning pool
    if (hasHiveTech)
    {
        theTier = 3;
    }
    else if (hasLairTech)
    {
        theTier = 2;
    }

    // Mark which tech units are available as tech targets.
    // First: If we already have it, it's not a target.
    std::array<bool, int(TechUnit::Size)> targetTaken;
    setAvailableTechUnits(targetTaken);

    // Interlude: Find the score of the best taken tech unit up to our current tier,
    // considering only positive scores. We never want to take a zero or negative score.
    // Do this before adding fictional taken techs.
    // Skip over the potential complication of a lost lair or hive: We may in fact have tech
    // that is beyond our current tech level because we have been set back.
    int maxTechScore = 0;
    for (int i = int(TechUnit::None); i < int(TechUnit::Size); ++i)
    {
        if (targetTaken[i] && techScores[i] > maxTechScore && techTier(TechUnit(i)) <= theTier)
        {
            maxTechScore = techScores[i];
        }
    }

    // Second: If we don't have either lair tech yet, and they're not both useless,
    // then don't jump ahead to hive tech. Fictionally call the hive tech units "taken".
    // A tech is useless if it's worth 0 or less, or if it's worth less than the best current tech.
    // (The best current tech might have negative value, though it's rare.)
    if (!hasSpire && !hasLurkers &&
        (techScores[int(TechUnit::Mutalisks)] > 0 || techScores[int(TechUnit::Lurkers)] > 0) &&
        (techScores[int(TechUnit::Mutalisks)] >= maxTechScore || techScores[int(TechUnit::Lurkers)] >= maxTechScore))
    {
        targetTaken[int(TechUnit::Ultralisks)] = true;
        targetTaken[int(TechUnit::Guardians)] = true;
        targetTaken[int(TechUnit::Devourers)] = true;
    }

    // Third: In ZvZ, don't make hydras ever, and make lurkers only after hive.
    // Call those tech units "taken".
    if (_enemyRace == BWAPI::Races::Zerg)
    {
        targetTaken[int(TechUnit::Hydralisks)] = true;
        if (!hasHiveTech)
        {
            targetTaken[int(TechUnit::Lurkers)] = true;
        }
    }

    // Fourth: On an island map, go all air until nydus canals are established.
    if (goingIslandAir)
    {
        targetTaken[int(TechUnit::Hydralisks)] = true;
        targetTaken[int(TechUnit::Lurkers)] = true;
        targetTaken[int(TechUnit::Ultralisks)] = true;
    }

    // Default. Value at the start of the game and after all tech is taken.
    _techTarget = TechUnit::None;

    // Choose the tech target, an untaken tech.
    // 1. If a tech at the current tier or below beats the best taken tech so far, take it.
    // That is, stay at the same tier or drop down if we can do better.
    // If we're already at hive tech, no need for this step. Keep going.
    if (theTier != 3)
    {
        int techScore = maxTechScore;    // accept only a tech which exceeds this value
        for (int i = int(TechUnit::None); i < int(TechUnit::Size); ++i)
        {
            if (!targetTaken[i] && techScores[i] > techScore && techTier(TechUnit(i)) <= theTier)
            {
                _techTarget = TechUnit(i);
                techScore = techScores[i];
            }
        }
        if (_techTarget != TechUnit::None)
        {
            return;
        }
    }

    // 2. Otherwise choose a target at any tier. Just pick the highest score.
    // If we should not skip from tier 1 to hive, that has already been coded into targetTaken[].
    int techScore = maxTechScore;    // accept only a tech which exceeds this value
    for (int i = int(TechUnit::None); i < int(TechUnit::Size); ++i)
    {
        if (!targetTaken[i] && techScores[i] > techScore)
        {
            _techTarget = TechUnit(i);
            techScore = techScores[i];
        }
    }
}

// Set _mineralUnit and _gasUnit depending on our tech and the game situation.
// This tells freshProductionPlan() what units to make.
void StrategyBossZerg::chooseUnitMix()
{
    // Special case: Make mutas to finish off the enemy.
    if (_enemySeemsToBeDead && hasSpire)
    {
        setUnitMix(BWAPI::UnitTypes::Zerg_Drone, BWAPI::UnitTypes::Zerg_Mutalisk);
        return;
    }

    // Mark which tech units are available for the unit mix.
    // If we have the tech for it, it can be in the unit mix.
    std::array<bool, int(TechUnit::Size)> available;
    setAvailableTechUnits(available);
    
    // Special case: If we're on an island map, go all air until nydus canals are established.
    // Ground units are "not available".
    if (goingIslandAir)
    {
        // Zerglings are OK only if we have all the drones we need.
        if (nDrones < maxDrones)
        {
            available[int(TechUnit::Zerglings)] = false;
        }
        available[int(TechUnit::Hydralisks)] = false;
        available[int(TechUnit::Lurkers)] = false;
        available[int(TechUnit::Ultralisks)] = false;
    }

    // Special case: The terran enemy has no ground units and has lifted all buildings.
    // It happens!
    // Require a unit mix that can shoot up. In the worst case, we may have to tech to hydras
    // before we can make any combat unit at all (that is, the mix may be Drone + None, and
    // if we're maxed on drones we'll produce nothing).
    if (enemyIsAllAir())
    {
        // BWAPI::Broodwar->printf("enemy is all air");
        available[int(TechUnit::Zerglings)] = false;
        available[int(TechUnit::Lurkers)] = false;
        available[int(TechUnit::Ultralisks)] = false;
        available[int(TechUnit::Guardians)] = false;
    }
    
    // Strictly limit the number of devourers we can make.
    if (nDevourers >= devourerLimit())
    {
        available[int(TechUnit::Devourers)] = false;
    }

    // Find the best available unit to be the main unit of the mix.
    // Special case: If we're going air, zerglings cannot be the best.
    // Other unit types are excluded above by calling them unavailable.
    TechUnit bestUnit = TechUnit::None;
    int techScore = INT_MIN;
    for (int i = int(TechUnit::None); i < int(TechUnit::Size); ++i)
    {
        if (available[i] && techScores[i] > techScore &&
            (!goingIslandAir || !hasSpire || TechUnit(i) != TechUnit::Zerglings))
        {
            bestUnit = TechUnit(i);
            techScore = techScores[i];
        }
    }

    // Defaults in case no unit type is available.
    BWAPI::UnitType minUnit = BWAPI::UnitTypes::Zerg_Drone;
    BWAPI::UnitType gasUnit = BWAPI::UnitTypes::None;

    // bestUnit is one unit of the mix. The other we fill in as reasonable.
    // If we're going air, the bestUnit cannot be zerglings. Those are only for defense.
    if (bestUnit == TechUnit::Zerglings)
    {
        if (hasPool)
        {
            minUnit = BWAPI::UnitTypes::Zerg_Zergling;
        }
    }
    else if (bestUnit == TechUnit::Hydralisks)
    {
        if (hasPool && available[int(TechUnit::Zerglings)])
        {
            minUnit = BWAPI::UnitTypes::Zerg_Zergling;
        }
        gasUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
    }
    else if (bestUnit == TechUnit::Lurkers)
    {
        if (!hasPool)
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (nGas >= 2 &&
            techScores[int(TechUnit::Hydralisks)] > 0 &&
            techScores[int(TechUnit::Hydralisks)] > 2 * (5 + techScores[int(TechUnit::Zerglings)]))
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else
        {
            minUnit = BWAPI::UnitTypes::Zerg_Zergling;
        }
        gasUnit = BWAPI::UnitTypes::Zerg_Lurker;
    }
    else if (bestUnit == TechUnit::Mutalisks)
    {
        if (!hasPool && hasDen)
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (hasDen && nGas >= 2 &&
            techScores[int(TechUnit::Hydralisks)] > 0 &&
            techScores[int(TechUnit::Hydralisks)] > 2 * (5 + techScores[int(TechUnit::Zerglings)]))
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (hasPool && available[int(TechUnit::Zerglings)])
        {
            minUnit = BWAPI::UnitTypes::Zerg_Zergling;
        }
        gasUnit = BWAPI::UnitTypes::Zerg_Mutalisk;
    }
    else if (bestUnit == TechUnit::Guardians)
    {
        if (!hasPool && hasDen)
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (hasDen && nGas >= 3 && techScores[int(TechUnit::Hydralisks)] > techScores[int(TechUnit::Zerglings)])
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (hasPool && available[int(TechUnit::Zerglings)])
        {
            minUnit = BWAPI::UnitTypes::Zerg_Zergling;
        }
        gasUnit = BWAPI::UnitTypes::Zerg_Guardian;
    }
    else if (bestUnit == TechUnit::Devourers)
    {
        // We want an anti-air unit in the mix to make use of the acid spores.
        if (hasDen && techScores[int(TechUnit::Hydralisks)] > techScores[int(TechUnit::Mutalisks)])
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else
        {
            minUnit = BWAPI::UnitTypes::Zerg_Mutalisk;
        }
        gasUnit = BWAPI::UnitTypes::Zerg_Devourer;
    }
    else if (bestUnit == TechUnit::Ultralisks)
    {
        if (!hasPool && hasDen)
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (hasDen && nGas >= 4 &&
            techScores[int(TechUnit::Hydralisks)] > 0 &&
            techScores[int(TechUnit::Hydralisks)] > 3 * (5 + techScores[int(TechUnit::Zerglings)]))
        {
            minUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        }
        else if (hasPool && available[int(TechUnit::Zerglings)])
        {
            minUnit = BWAPI::UnitTypes::Zerg_Zergling;
        }
        gasUnit = BWAPI::UnitTypes::Zerg_Ultralisk;
    }

    setUnitMix(minUnit, gasUnit);
}

// An auxiliary unit may be made in smaller numbers alongside the main unit mix.
// Case 1: We're preparing a morphed-unit tech and want some units to morph later.
// Case 2: We have a tech that can play a useful secondary role.
// NOTE This is a hack to tide the bot over until better production decisions can be made.
void StrategyBossZerg::chooseAuxUnit()
{
    const int maxAuxGuardians = 8;
    const int maxAuxDevourers = std::min(4, devourerLimit());

    // The default is no aux unit.
    _auxUnit = BWAPI::UnitTypes::None;
    _auxUnitCount = 0;

    // Case 1: Getting a morphed unit tech.
    if (_techTarget == TechUnit::Lurkers &&
        hasDen &&
        _mineralUnit != BWAPI::UnitTypes::Zerg_Hydralisk &&
        _gasUnit != BWAPI::UnitTypes::Zerg_Hydralisk)
    {
        _auxUnit = BWAPI::UnitTypes::Zerg_Hydralisk;
        _auxUnitCount = 4;
    }
    else if ((_techTarget == TechUnit::Guardians || _techTarget == TechUnit::Devourers) &&
        hasSpire &&
        hasHiveTech &&
        _gasUnit != BWAPI::UnitTypes::Zerg_Mutalisk)
    {
        _auxUnit = BWAPI::UnitTypes::Zerg_Mutalisk;
        _auxUnitCount = 6;
    }
    // Case 2: Secondary tech.
    else if (hasGreaterSpire &&
        _gasUnit != BWAPI::UnitTypes::Zerg_Guardian &&
        (_gasUnit != BWAPI::UnitTypes::Zerg_Devourer || nDevourers >= 3) &&
        techScores[int(TechUnit::Guardians)] >= 3 &&
        nGuardians < maxAuxGuardians)
    {
        _auxUnit = BWAPI::UnitTypes::Zerg_Guardian;
        _auxUnitCount = std::min(maxAuxGuardians, techScores[int(TechUnit::Guardians)] / 3);
    }
    else if (hasGreaterSpire &&
        (nHydras >= 8 || nMutas >= 6) &&
        _gasUnit != BWAPI::UnitTypes::Zerg_Devourer &&
        techScores[int(TechUnit::Devourers)] >= 3 &&
        nDevourers < maxAuxDevourers)
    {
        _auxUnit = BWAPI::UnitTypes::Zerg_Devourer;
        _auxUnitCount = std::min(maxAuxDevourers, techScores[int(TechUnit::Devourers)] / 3);
    }
    else if (hasLurkers &&
        _gasUnit != BWAPI::UnitTypes::Zerg_Lurker &&
        techScores[int(TechUnit::Lurkers)] > 0)
    {
        _auxUnit = BWAPI::UnitTypes::Zerg_Lurker;
        int nMineralUnits = the.my.completed.count(_mineralUnit);
        if (nMineralUnits >= 12)
        {
            _auxUnitCount = nMineralUnits / 12;
        }
        else if (nMineralPatches >= 8)
        {
            _auxUnitCount = 1;
        }
        // else default to 0
    }
}

// Set the economy ratio according to the enemy race.
// If the enemy went random, the enemy race may change!
// This resets the drone/economy counts, so don't call it too often
// or you will get nothing but drones.
void StrategyBossZerg::chooseEconomyRatio()
{
    if (_enemyRace == BWAPI::Races::Zerg)
    {
        setEconomyRatio(0.15);
    }
    else if (_enemyRace == BWAPI::Races::Terran)
    {
        setEconomyRatio(0.45);
    }
    else if (_enemyRace == BWAPI::Races::Protoss)
    {
        setEconomyRatio(0.35);
    }
    else
    {
        // Enemy went random, race is still unknown. Choose cautiously.
        // We should find the truth soon enough.
        setEconomyRatio(0.20);
    }
}

// Choose current unit mix and next tech target to aim for.
// Called when the queue is empty and no future production is planned yet.
void StrategyBossZerg::chooseStrategy()
{
    // Reset the economy ratio if the enemy's race has changed.
    // It can change from Unknown to another race if the enemy went random.
    // Do this first so that the calls below know the enemy's race!
    if (_enemyRace != _enemy->getRace())
    {
        _enemyRace = _enemy->getRace();
        chooseEconomyRatio();
    }

    calculateTechScores(0);
    chooseTechTarget();
    chooseUnitMix();
    chooseAuxUnit();        // must be after the unit mix is set
}

void StrategyBossZerg::produceUnits(int & mineralsLeft, int & gasLeft)
{
    const int numMineralUnits = the.my.all.count(_mineralUnit);
    const int numGasUnits = _gasUnit == BWAPI::UnitTypes::None ? 0 : the.my.all.count(_gasUnit);

    int larvasLeft = nLarvas;

    // First of all, if we have defilers, make sure they get some food.
    if (nDefilers > 0 &&
        nLings < 4 * nDefilers &&
        _mineralUnit != BWAPI::UnitTypes::Zerg_Zergling &&
        hasPool && nDrones >= 9)
    {
        for (int i = 0; i < 4 * nDefilers - nLings && larvasLeft > 0; i += 2)
        {
            produce(BWAPI::UnitTypes::Zerg_Zergling);
            --larvasLeft;
            mineralsLeft -= 50;
        }
    }

    // Before the main production, squeeze out one aux unit, if we want one. Only one per call.
    if (_auxUnit != BWAPI::UnitTypes::None &&
        the.my.all.count(_auxUnit) < _auxUnitCount &&
        larvasLeft > 0 &&
        numMineralUnits > 2 &&
        gasLeft >= _auxUnit.gasPrice())
    {
        BWAPI::UnitType auxType = findUnitType(_auxUnit);
        produce(auxType);
        if (!UnitUtil::IsMorphedUnitType(auxType))
        {
            --larvasLeft;
        }
        mineralsLeft -= auxType.mineralPrice();
        gasLeft -= auxType.gasPrice();
    }

    // If we have resources left, make units.
    // Substitute in drones according to _economyRatio (findUnitType() does this).
    if (_gasUnit == BWAPI::UnitTypes::None ||
        gas < _gasUnit.gasPrice() ||
        double(numMineralUnits) / double(numGasUnits) < 0.2 ||
        _gasUnit == BWAPI::UnitTypes::Zerg_Devourer && nDevourers >= devourerLimit())
    {
        // Only the mineral unit.
        while (larvasLeft >= 0 && mineralsLeft >= 0 && gasLeft >= 0)
        {
            BWAPI::UnitType type = findUnitType(_mineralUnit);
            produce(type);
            if (!UnitUtil::IsMorphedUnitType(type))
            {
                --larvasLeft;
            }
            mineralsLeft -= type.mineralPrice();
            gasLeft -= type.gasPrice();
        }
    }
    else
    {
        // Make both units. The mineral unit may also need gas.
        // Make as many gas units as gas allows, mixing in mineral units as possible.
        // NOTE nGasUnits can be wrong for morphed units like lurkers!
        int nGasUnits = 1 + gas / _gasUnit.gasPrice();    // number remaining to make
        bool gasUnitNext = true;
        while (larvasLeft >= 0 && mineralsLeft >= 0 && gasLeft >= 9)
        {
            BWAPI::UnitType type;
            if (nGasUnits > 0 && gasUnitNext)
            {
                type = findUnitType(_gasUnit);
                // If we expect to want mineral units, mix them in.
                if (nGasUnits < larvasLeft && nGasUnits * type.mineralPrice() < mineralsLeft)
                {
                    gasUnitNext = false;
                }
                if (type == _gasUnit)
                {
                    --nGasUnits;
                }
            }
            else
            {
                type = findUnitType(_mineralUnit);
                gasUnitNext = true;
            }
            produce(type);
            if (!UnitUtil::IsMorphedUnitType(type))
            {
                --larvasLeft;
            }
            mineralsLeft -= type.mineralPrice();
            gasLeft -= type.gasPrice();
        }
    }

    // Try for extra drones and/or zerglings from the dregs, especially if we are low on gas.
    if (_emergencyGroundDefense || gasLeft < 100 && mineralsLeft >= 100 || mineralsLeft > 300)
    {
        int dronesToAdd = 0;
        if (numMineralUnits + numGasUnits >= 36)
        {
            dronesToAdd = maxDrones - nDrones;       // may be negative; that is OK
        }
        if (hasPool)
        {
            while (larvasLeft > 0 && mineralsLeft >= 50)
            {
                if (dronesToAdd > 0 && !_emergencyGroundDefense && enoughArmy())
                {
                    produce(BWAPI::UnitTypes::Zerg_Drone);
                    --dronesToAdd;
                }
                else
                {
                    produce(BWAPI::UnitTypes::Zerg_Zergling);
                }
                --larvasLeft;
                mineralsLeft -= 50;
            }
        }
        else
        {
            // Can't make zerglings, so don't try.
            while (larvasLeft > 0 && mineralsLeft >= 50 && dronesToAdd > 0)
            {
                produce(BWAPI::UnitTypes::Zerg_Drone);
                --dronesToAdd;
                --larvasLeft;
                mineralsLeft -= 50;
            }
        }
    }
}

// Also makes queens and infested terrans--units used sparingly.
void StrategyBossZerg::produceOtherStuff(int & mineralsLeft, int & gasLeft, bool hasEnoughUnits)
{
    // Used in conditions for some rules below.
    const int armorUps = _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Carapace);

    // Get zergling speed if at all sensible.
    if (hasPool && nDrones >= 9 && (nGas > 0 || gas >= 100) &&
        (nLings >= 6 || _mineralUnit == BWAPI::UnitTypes::Zerg_Zergling) &&
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Metabolic_Boost) == 0 &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Metabolic_Boost) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Adrenal_Glands))
    {
        produce(BWAPI::UpgradeTypes::Metabolic_Boost);
        mineralsLeft -= 100;
        gasLeft -= 100;
    }

    // Ditto zergling attack rate.
    if (hasPool && hasHiveTech && nDrones >= 12 && (nGas > 0 || gas >= 200) &&
        (nLings >= 8 || _mineralUnit == BWAPI::UnitTypes::Zerg_Zergling) &&
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Adrenal_Glands) == 0 &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Adrenal_Glands) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Metabolic_Boost))
    {
        produce(BWAPI::UpgradeTypes::Adrenal_Glands);
        mineralsLeft -= 200;
        gasLeft -= 200;
    }

    // Get hydralisk den if it's next.
    if ((_techTarget == TechUnit::Hydralisks || _techTarget == TechUnit::Lurkers && lurkerDenTiming()) &&
        !hasDen && hasPool && nDrones >= 10 && nGas > 0 &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hydralisk_Den) &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den) == 0)
    {
        produce(BWAPI::UnitTypes::Zerg_Hydralisk_Den);
        mineralsLeft -= 100;
        gasLeft -= 50;
    }

    // Get hydra speed and range if they make sense.
    if (hasDen && nDrones >= 11 && nGas > 0 &&
        (_mineralUnit == BWAPI::UnitTypes::Zerg_Hydralisk || _gasUnit == BWAPI::UnitTypes::Zerg_Hydralisk) &&
        // Lurker aspect has priority, but we can get hydra upgrades until the lair starts.
        (_techTarget != TechUnit::Lurkers || nLairs + nHives == 0) &&
        !_self->isResearching(BWAPI::TechTypes::Lurker_Aspect))
    {
        if (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Muscular_Augments) == 0 &&
            !_self->isUpgrading(BWAPI::UpgradeTypes::Muscular_Augments))
        {
            produce(BWAPI::UpgradeTypes::Muscular_Augments);
            mineralsLeft -= 150;
            gasLeft -= 150;
        }
        else if (nHydras >= 3 &&
            _self->getUpgradeLevel(BWAPI::UpgradeTypes::Muscular_Augments) != 0 &&
            _self->getUpgradeLevel(BWAPI::UpgradeTypes::Grooved_Spines) == 0 &&
            !_self->isUpgrading(BWAPI::UpgradeTypes::Grooved_Spines))
        {
            produce(BWAPI::UpgradeTypes::Grooved_Spines);
            mineralsLeft -= 150;
            gasLeft -= 150;
        }
    }

    // Get lurker aspect if it's next.
    if (_techTarget == TechUnit::Lurkers &&
        hasDen && hasLairTech && nDrones >= 9 && nGas > 0 &&
        (!_emergencyGroundDefense || gasLeft >= 150) &&
        !_self->hasResearched(BWAPI::TechTypes::Lurker_Aspect) &&
        !_self->isResearching(BWAPI::TechTypes::Lurker_Aspect) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Muscular_Augments) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Grooved_Spines))
    {
        produce(BWAPI::TechTypes::Lurker_Aspect);
        mineralsLeft -= 200;
        gasLeft -= 200;
    }

    // Make a lair. Make it earlier in ZvZ.
    // Make it anyway if the enemy already has higher tech or we have excess cash.
    if ((lairTechUnit(_techTarget) || hiveTechUnit(_techTarget) ||
         armorUps > 0 ||
         the.info.enemyHasAirTech() ||
         the.info.enemyHasCloakTech() ||
         mineralsLeft > 400 + 100 * nLarvas && gasLeft > 300 + 75 * nLarvas
        ) &&
        hasPool && nLairs + nHives == 0 && nGas > 0 &&
        (!_emergencyGroundDefense || gasLeft >= 75) &&
        (nDrones >= 12 || _enemyRace == BWAPI::Races::Zerg && nDrones >= 9))
    {
        produce(BWAPI::UnitTypes::Zerg_Lair);
        mineralsLeft -= 150;
        gasLeft -= 100;
    }

    // Make a spire. Make it earlier in ZvZ.
    if (!hasSpire && hasLairTech && nGas > 0 &&
        (_recommendDefensiveSpire || airTechUnit(_techTarget)) &&
        (nDrones >= 13 || _enemyRace == BWAPI::Races::Zerg && nDrones >= 9) &&
        hasEnoughUnits &&
        (!_emergencyGroundDefense || gasLeft >= 75) &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Spire) &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Spire) == 0)
    {
        produce(BWAPI::UnitTypes::Zerg_Spire);
        mineralsLeft -= 200;
        gasLeft -= 150;
    }

    // Make a greater spire.
    if ((_techTarget == TechUnit::Guardians || _techTarget == TechUnit::Devourers) &&
        hasEnoughUnits &&
        hasHiveTech && hasSpire && !hasGreaterSpire && nGas >= 2 && nDrones >= 15 &&
        (!_emergencyGroundDefense || gasLeft >= 75) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Carapace) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks) &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Greater_Spire) == 0)
    {
        produce(BWAPI::UnitTypes::Zerg_Greater_Spire);
        mineralsLeft -= 100;
        gasLeft -= 150;
    }

    // Get overlord speed.
    // Skip it versus zerg, since it's rarely worth the cost.
    if (hasLairOrHive && nGas > 0 && nDrones >= 15 &&
        _enemyRace != BWAPI::Races::Zerg &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        _techTarget != TechUnit::Mutalisks && _techTarget != TechUnit::Lurkers &&    // get your lair tech FIRST
        (_gasUnit != BWAPI::UnitTypes::Zerg_Mutalisk || nMutas >= 6) &&
        (_gasUnit != BWAPI::UnitTypes::Zerg_Lurker || nLurkers >= 4) &&
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Pneumatized_Carapace) == 0 &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Pneumatized_Carapace) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Antennae))
    {
        produce(BWAPI::UpgradeTypes::Pneumatized_Carapace);
        mineralsLeft -= 150;
        gasLeft -= 150;
    }

    // Get overlord drop if we plan to drop.
    if (hasLairOrHive && nGas > 0 && nDrones >= 18 &&
        nBases >= 3 &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        StrategyManager::Instance().dropIsPlanned() &&
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Pneumatized_Carapace) == 1 &&
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Ventral_Sacs) == 0 &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Antennae))
    {
        produce(BWAPI::UpgradeTypes::Ventral_Sacs);
        mineralsLeft -= 200;
        gasLeft -= 200;
    }

    // Get overlord sight range if we're maxed and accumulating resources.
    // This will be rare.
    if (_supplyUsed > 400 - 12 && mineralsLeft > 500 && gasLeft > 500 &&
        hasLairOrHive && nGas >= 3 && nDrones >= 60 &&
        nBases >= 3 &&
        _self->getUpgradeLevel(BWAPI::UpgradeTypes::Pneumatized_Carapace) == 1 &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Antennae))
    {
        produce(BWAPI::UpgradeTypes::Antennae);
        mineralsLeft -= 150;
        gasLeft -= 150;
    }

    // Make a queen's nest.
    // Make it later versus zerg.
    // Wait until pneumatized carapace is done (except versus zerg, when we don't get that),
    // because the bot has often been getting a queen's nest too early.
    if (!hasQueensNest && hasLairTech && nGas >= 2 &&
        (nDrones >= 30 || (_recommendQueens || defilerScore > 4) && nDrones >= 22) &&
        (!hasHiveTech || _recommendQueens) &&
        !_emergencyGroundDefense && hasEnoughUnits &&

        (hiveTechUnit(_techTarget) ||
        defilerScore > 4 ||
        armorUps == 2 ||
        _enemyRace == BWAPI::Races::Zerg && nMutas >= 12) &&

        the.my.all.count(BWAPI::UnitTypes::Zerg_Queens_Nest) == 0 &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Queens_Nest))
    {
        produce(BWAPI::UnitTypes::Zerg_Queens_Nest);
        mineralsLeft -= 150;
        gasLeft -= 100;
    }

    // Possibly research queen spells.
    // We don't even think about it unless we already have a queen.
    if (nQueens > 0 && hasQueensNest && nGas >= 2 && nDrones >= 24 &&
        _recommendQueens > 0 &&
        !_self->isResearching(BWAPI::TechTypes::Ensnare) &&
        !_self->isResearching(BWAPI::TechTypes::Spawn_Broodlings) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Gamete_Meiosis))       // +50 energy
    {
        if (_recommendEnsnare && !_self->hasResearched(BWAPI::TechTypes::Ensnare))
        {
            produce(BWAPI::TechTypes::Ensnare);
            mineralsLeft -= 100;
            gasLeft -= 100;
        }

        if (_recommendBroodling && !_self->hasResearched(BWAPI::TechTypes::Spawn_Broodlings))
        {
            produce(BWAPI::TechTypes::Spawn_Broodlings);
            mineralsLeft -= 100;
            gasLeft -= 100;
        }

        // NOTE We only get this far if ensnare or broodling is already researched.
        if ((_recommendEnsnare || _recommendBroodling >= 2) &&
            _self->getUpgradeLevel(BWAPI::UpgradeTypes::Gamete_Meiosis) == 0 &&
            std::max(nQueens, _recommendQueens) >= 4 &&
            nGas >= 3 && (nDrones >= 60 || nDrones >= 30 && _recommendQueens >= 6) &&
            !_emergencyGroundDefense && !_emergencyNow && enoughArmy())
        {
            produce(BWAPI::UpgradeTypes::Gamete_Meiosis);
            mineralsLeft -= 150;
            gasLeft -= 150;
        }
    }

    // Make a hive.
    // Ongoing lair research will delay the hive.
    // In ZvZ, get hive only if plenty of mutas are already in the air. Otherwise hive can be too fast.
    if ((hiveTechUnit(_techTarget) || armorUps >= 2) &&
        nHives == 0 && hasLair && hasQueensNest && nDrones >= 16 && nGas >= 2 &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        (_enemyRace != BWAPI::Races::Zerg || nMutas >= 12) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Pneumatized_Carapace) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Ventral_Sacs) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Antennae))
    {
        produce(BWAPI::UnitTypes::Zerg_Hive);
        mineralsLeft -= 200;
        gasLeft -= 150;
    }

    // Move toward ultralisks.
    if (_techTarget == TechUnit::Ultralisks && !hasUltra && hasHiveTech && nDrones >= 24 && nGas >= 3 &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Ultralisk_Cavern) &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Ultralisk_Cavern) == 0)
    {
        produce(BWAPI::UnitTypes::Zerg_Ultralisk_Cavern);
        mineralsLeft -= 150;
        gasLeft -= 200;
    }
    else if (hasUltra && nDrones >= 24 && nGas >= 3)
    {
        if (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Anabolic_Synthesis) == 0 &&
            !_self->isUpgrading(BWAPI::UpgradeTypes::Anabolic_Synthesis))
        {
            produce(BWAPI::UpgradeTypes::Anabolic_Synthesis);
            mineralsLeft -= 200;
            gasLeft -= 200;
        }
        else if (_self->getUpgradeLevel(BWAPI::UpgradeTypes::Anabolic_Synthesis) != 0 &&
            _self->getUpgradeLevel(BWAPI::UpgradeTypes::Chitinous_Plating) == 0 &&
            !_self->isUpgrading(BWAPI::UpgradeTypes::Chitinous_Plating))
        {
            produce(BWAPI::UpgradeTypes::Chitinous_Plating);
            mineralsLeft -= 150;
            gasLeft -= 150;
        }
    }

    // We want to expand.
    // Division of labor: Expansions are here, macro hatcheries are "urgent production issues".
    // However, macro hatcheries may be placed at new expansions.
    // Don't expand if we're getting rushed and it's not safe.
    if (nDrones > nMineralPatches + 3 * nGas &&
        nFreeBases > 0 &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hatchery))
    {
        bool makeIt = true;
        MacroLocation loc = hiddenBaseNext() ? MacroLocation::Hidden : MacroLocation::Expo;
        if (OpponentModel::Instance().getDarnLikelyEnemyPlan() == OpeningPlan::FastRush &&
            nHatches <= 1 &&
            the.bases.baseCount(_enemy) <= 1)
        {
            // In case of fast rush, macro hatchery first, or skip it altogether.
            if (mineralsLeft >= 300 && nLarvas == 0)
            {
                loc = MacroLocation::Main;
            }
            else if (nLings < 6)
            {
                makeIt = false;
            }
            else if (nLings < 12 && !enoughArmy())
            {
                loc = MacroLocation::Main;
            }
            else if (Random::Instance().flag(0.20))
            {
                loc = MacroLocation::Hidden;
            }
        }
        else if (!hiddenBaseNext() && (_gasUnit == BWAPI::UnitTypes::None || nHatches % 2 == 0))
        {
            // Be a little generous with mineral-only expansions
            loc = MacroLocation::MinOnly;
        }
        if (makeIt)
        {
            produce(MacroAct(BWAPI::UnitTypes::Zerg_Hatchery, loc));
            mineralsLeft -= 300;
        }
    }

    // Get gas. If necessary, expand for it.
    // We make only one extractor at a time, because the building reservation system
    // does not work for geysers: If we made two at once, they would often be on the
    // same geyser.
    bool addExtractor = false;
    if (nFreeGas > 0 && hasPool && !isBeingBuilt(BWAPI::UnitTypes::Zerg_Extractor))
    {
        // A. Late in the game, we have many drones. Make extractors while possible.
        if (nDrones >= 50)
        {
            addExtractor = true;
        }
        // B. If we have a functioning economy, get our first gas.
        else if (nGas == 0 && nDrones >= 10)
        {
            addExtractor = true;
            if (!WorkerManager::Instance().isCollectingGas())
            {
                produce(MacroCommandType::StartGas);
            }
        }
        // C. Make more extractors if we have a low ratio of gas to minerals.
        else if ((_gasUnit != BWAPI::UnitTypes::None || _mineralUnit.gasPrice() > 0) &&
            nDrones > 3 * the.bases.baseCount(_self) + 3 * nGas + 4 &&
            (minerals + 50) / (gas + 50) >= 3)
        {
            addExtractor = true;
        }
        // D. At least make a second extractor if our gas unit is expensive in gas (most are).
        else if (nGas < 2 && nDrones >= 12 &&
            _gasUnit != BWAPI::UnitTypes::None && _gasUnit.gasPrice() >= 100)
        {
            addExtractor = true;
        }
        // E. If we have a big mineral excess and enough drones, get more extractors no matter what.
        else if (nDrones > 3 * the.bases.baseCount(_self) + 3 * nGas + 6 &&
            (minerals + 50) / (gas + 50) >= 6)
        {
            addExtractor = true;
        }
        // E. If we have lair tech, get at least 2 extractors.
        // If aiming for hive tech, get at least 3.
        // Doesn't break 1-base tech strategies, because then the geyser is not available.
        else if (hasLairTech && nGas < 2 && nDrones >= 12 ||
            hiveTechUnit(_techTarget) && nGas < 3 && nDrones >= 21)
        {
            addExtractor = true;
        }
        if (addExtractor)
        {
            produce(BWAPI::UnitTypes::Zerg_Extractor);
            mineralsLeft -= 50;
        }
    }
    // F. Or expand if we are out of free geysers.
    else if ((_mineralUnit.gasPrice() > 0 || _gasUnit != BWAPI::UnitTypes::None) &&
        nFreeGas == 0 && nFreeBases > 0 && hasPool &&
        nDrones > 3 * the.bases.baseCount(_self) + 3 * nGas + 5 &&
        (minerals + 100) / (gas + 100) >= 4 && minerals > 350 &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Hatchery))
    {
        // This asks for a gas base, but we didn't check whether any are available.
        // If none are left, we'll get a mineral only.
        // NOTE A hidden base also has gas.
        produce(MacroAct(BWAPI::UnitTypes::Zerg_Hatchery, hiddenBaseNext() ? MacroLocation::Hidden : MacroLocation::GasOnly));
        mineralsLeft -= 300;
    }

    // Prepare an evo chamber or two.
    // Terran doesn't want the first evo until after den or spire.
    // On islands, we go air so get only 1 evo and get it later.
    if (hasPool && nGas > 0 &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        nEvo == the.my.all.count(BWAPI::UnitTypes::Zerg_Evolution_Chamber) &&     // none under construction
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Evolution_Chamber))
    {
        if (nEvo == 0 && nDrones >= 18 && (_enemyRace != BWAPI::Races::Terran || hasDen || hasSpire || hasUltra) ||
            nEvo == 1 && nDrones >= 30 && nGas >= 2 && (hasDen || hasSpire || hasUltra) && nFreeEvo() == 0 ||
            nEvo == 0 && nDrones >= 30 && nGas > 0 && hasLairTech && the.bases.isIslandStart())
        {
            produce(BWAPI::UnitTypes::Zerg_Evolution_Chamber);
            mineralsLeft -= 75;
        }
    }

    // If we're in reasonable shape, get ground upgrades.
    // Ground upgrade priorities, in order.
    // NOTE Code below will skip over any invalid upgrades. But don't list the same upgrade twice.
    std::vector<BWAPI::UpgradeType> groundUpgrades;     // treat this as a fixed-size array[0..2]
    groundUpgrades.push_back(BWAPI::UpgradeTypes::Zerg_Carapace);       // 0
    groundUpgrades.push_back(BWAPI::UpgradeTypes::Zerg_Melee_Attacks);  // 1
    groundUpgrades.push_back(BWAPI::UpgradeTypes::None);                // 2
    if (_enemyRace == BWAPI::Races::Terran)
    {
        // ZvT, Steamhammer relies on hydras in specific situations. Only upgrade missile attacks then.
        if (_mineralUnit == BWAPI::UnitTypes::Zerg_Hydralisk ||
            _gasUnit == BWAPI::UnitTypes::Zerg_Hydralisk)
        {
            if (the.your.seen.count(BWAPI::UnitTypes::Terran_Goliath) >= 8 ||
                the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser) >= 2)
            {
                groundUpgrades.at(0) = BWAPI::UpgradeTypes::Zerg_Missile_Attacks;
                groundUpgrades.at(2) = BWAPI::UpgradeTypes::Zerg_Carapace;
            }
            else
            {
                groundUpgrades.at(2) = BWAPI::UpgradeTypes::Zerg_Missile_Attacks;
            }
        }
    }
    else if (_enemyRace == BWAPI::Races::Protoss)
    {
        // ZvP, Steamhammer often sticks with hydras most or all of the game.
        if (_mineralUnit == BWAPI::UnitTypes::Zerg_Hydralisk ||
            _gasUnit == BWAPI::UnitTypes::Zerg_Hydralisk ||
            _gasUnit == BWAPI::UnitTypes::Zerg_Lurker)
        {
            groundUpgrades.at(0) = BWAPI::UpgradeTypes::Zerg_Missile_Attacks;
            groundUpgrades.at(1) = BWAPI::UpgradeTypes::Zerg_Carapace;
            groundUpgrades.at(2) = BWAPI::UpgradeTypes::Zerg_Melee_Attacks;
        }
    }
    else // zerg
    {
        groundUpgrades.at(0) = BWAPI::UpgradeTypes::Zerg_Melee_Attacks;
        groundUpgrades.at(1) = BWAPI::UpgradeTypes::None;
        if (hasHiveTech)
        {
            groundUpgrades.at(1) = BWAPI::UpgradeTypes::Zerg_Carapace;
            if (_gasUnit == BWAPI::UnitTypes::Zerg_Lurker)
            {
                groundUpgrades.at(2) = BWAPI::UpgradeTypes::Zerg_Missile_Attacks;
            }
        }
    }

    if (nEvo > 0 && nDrones >= 12 && nGas > 0 &&
        hasPool &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        (!the.bases.isIslandStart() || nHives > 0) &&
        // Delay if we're going mutas and don't have many. They want the resources.
        !(_gasUnit == BWAPI::UnitTypes::Zerg_Mutalisk && nMutas < 6))
    {
        int nFreeEvos = nFreeEvo();

        for (int i = 0; nFreeEvos > 0 && i < 3; ++i)
        {
            BWAPI::UpgradeType up = groundUpgrades.at(i);
            int upLevel = _self->getUpgradeLevel(up);

            if (up.isValid() && !_self->isUpgrading(up) && upLevel < maxUpgrade())
            {
                // Tech first: Don't get upgrades > 1 unless we already have some tech.
                if (upLevel == 0 || hasDen || hasSpire || hasUltra)
                {
                    produce(up);
                    --nFreeEvos;
                    mineralsLeft -= 100;     // TODO too low in most cases
                    gasLeft -= 100;          // ditto
                }
            }
        }
    }

    // Possibly get burrow.
    if (Config::Skills::Burrow &&
        !_self->hasResearched(BWAPI::TechTypes::Burrowing) &&
        !_self->isResearching(BWAPI::TechTypes::Burrowing) &&
        nGas >= 2 &&
        (nBases >= 3 && nDrones >= 24 || nBases >= 1 && mineralsLeft > 350 + 75 * nLarvas && gasLeft > 300 + 50 * nLarvas) &&
        !_emergencyGroundDefense && hasEnoughUnits && enoughArmy())
    {
        // Burrow is researched in a hatchery, and we have a bunch.
        // No need to check whether one is free.
        produce(BWAPI::TechTypes::Burrowing);
        mineralsLeft -= 100;
        gasLeft -= 100;
    }

    // If we're going air and expect to keep needing air units, get air upgrades.
    if (hasSpire && nDrones >= 15 && nGas > 0 &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Greater_Spire) == 0 &&
        hasPool &&
        !_emergencyGroundDefense && hasEnoughUnits &&
        (_wantAirArmor ||
        nMutas + nGuardians + nDevourers >= 8 &&
        (_gasUnit != BWAPI::UnitTypes::None && _gasUnit.isFlyer() || goingIslandAir)) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Carapace) &&
        !_self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks))
    {
        const int airArmorUps = _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Flyer_Carapace);
        const int airAttackUps = _self->getUpgradeLevel(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks);

        if (airArmorUps < 2 && hasLairTech ||
            airArmorUps < 3 && hasHiveTech)
        {
            produce(BWAPI::UpgradeTypes::Zerg_Flyer_Carapace);
            mineralsLeft -= 150;     // TODO not correct for upgrades 2 or 3
            gasLeft -= 150;          // ditto
        }
        else if (airArmorUps == 3 && airAttackUps < 3 && hasGreaterSpire && hasHiveTech &&
                 nMutas + nGuardians >= 12 &&		// devourers don't count
                 nDrones >= 50 && nGas >= 3 &&
                 !_self->isUpgrading(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks))
        {
            produce(BWAPI::UpgradeTypes::Zerg_Flyer_Attacks);
            mineralsLeft -= 100;     // TODO not correct for upgrades 2 or 3
            gasLeft -= 100;          // ditto
        }
    }

    // Possibly produce queens, if we already have a queen's nest.
    if (Config::Skills::MaxQueens > 0 &&
        hasQueensNest && nQueens < _recommendQueens &&
        nDrones >= 20 + 4 * nQueens &&
        nGas >= 2 &&
        // Not if we're short of other units.
        !_emergencyGroundDefense && !_emergencyNow && enoughArmy() &&
        // Not if the enemy is apparently on their last legs.
        enemyGroundArmySize > 0 && enemyAntigroundArmySize > 0 && the.bases.baseCount(_enemy) >= 2)
    {
        int nToMake = _recommendQueens - nQueens;
        while (nToMake > 0)
        {
            produce(BWAPI::UnitTypes::Zerg_Queen);
            nToMake -= 1;
            mineralsLeft -= 100;
            gasLeft -= 100;
            nLarvas -= 1;
        }
    }

    // Possibly make an infested terran.
    // TODO when infesteds can be used more effectively, change 90 to a small number or remove that line
    if (nInfestedCC > 0 &&
        nInfestedTerrans < Config::Skills::MaxInfestedTerrans &&
        _lastInfestedTerranOrderFrame + 90 * 24 < the.now() &&
        nGas >= 2 &&
        mineralsLeft >= 100 && gasLeft >= 50 &&
        enemyGroundArmySize > 0 && enemyAntigroundArmySize > 0 && the.bases.baseCount(_enemy) > 0)
    {
        _lastInfestedTerranOrderFrame = the.now();
        produce(BWAPI::UnitTypes::Zerg_Infested_Terran);
        mineralsLeft -= 100;
        gasLeft -= 50;
    }

    // In the late game, add defilers.
    // Add them later if the defilerScore is negative--currently, only terran sets it other than 0.
    // Get defilers and consume even if we are in a state of emergency. We need defilers then!
    if (hasHiveTech &&
        Config::Skills::MaxDefilers > 0 &&
        ( defilerScore >= 8 && nDrones >= 18 && nGas >= 1 ||
          defilerScore >= 0 && nDrones >= 35 && nGas >= 2 ||
          hasUltraUps       && nDrones >= 60 && nGas >= 4) &&
        the.my.all.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) == 0 &&
        !isBeingBuilt(BWAPI::UnitTypes::Zerg_Defiler_Mound))
    {
        produce(BWAPI::UnitTypes::Zerg_Defiler_Mound);
        mineralsLeft -= 100;
        gasLeft -= 100;
    }

    // And, of course, defiler abilities.
    if (hasPool && nGas >= 1 &&
        the.my.completed.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) > 0)
    {
        if (!_self->hasResearched(BWAPI::TechTypes::Consume) &&
            !_self->isResearching(BWAPI::TechTypes::Consume))
        {
            produce(BWAPI::TechTypes::Consume);
            mineralsLeft -= 100;
            gasLeft -= 100;
        }
        else if (nDrones >= 40 && nGas >= 3 && !_emergencyNow)
        {
            if (_self->hasResearched(BWAPI::TechTypes::Consume) &&
                !_self->hasResearched(BWAPI::TechTypes::Plague) &&
                !_self->isResearching(BWAPI::TechTypes::Plague))
            {
                produce(BWAPI::TechTypes::Plague);
                mineralsLeft -= 200;
                gasLeft -= 200;
            }
            else if (nDrones >= 55 &&
                enoughArmy() && enoughGroundArmy() &&
                _self->hasResearched(BWAPI::TechTypes::Plague) &&
                _self->getUpgradeLevel(BWAPI::UpgradeTypes::Metasynaptic_Node) == 0 &&
                !_self->isUpgrading(BWAPI::UpgradeTypes::Metasynaptic_Node))
            {
                produce(BWAPI::UpgradeTypes::Metasynaptic_Node);
                mineralsLeft -= 150;
                gasLeft -= 150;
            }
        }
    }

    // Not too many defilers. Defiler micro is computationally expensive.
    // Special case: Treat defilers as if they were tech, not units.
    if (hasDefilerUps && nDefilers < Config::Skills::MaxDefilers && nGas >= 1)
    {
        int limit = std::min(Config::Skills::MaxDefilers, nGas);
        if (!_self->hasResearched(BWAPI::TechTypes::Consume) || _enemySeemsToBeDead)
        {
            limit = 1;
        }
        if (nDefilers < limit)
        {
            // One at a time.
            produce(BWAPI::UnitTypes::Zerg_Defiler);
            mineralsLeft -= 50;
            gasLeft -= 150;
            nLarvas -= 1;
        }
    }
}

std::string StrategyBossZerg::techTargetToString(TechUnit target)
{
    if (target == TechUnit::Zerglings) return "Lings";
    if (target == TechUnit::Hydralisks) return "Hydras";
    if (target == TechUnit::Lurkers) return "Lurkers";
    if (target == TechUnit::Mutalisks) return "Mutas";
    if (target == TechUnit::Ultralisks) return "Ultras";
    if (target == TechUnit::Guardians) return "Guardians";
    if (target == TechUnit::Devourers) return "Devourers";
    return "[none]";
}

// Draw various internal information bits, by default on the right side left of Bases.
void StrategyBossZerg::drawStrategyBossInformation()
{
    if (!Config::Debug::DrawStrategyBossInfo)
    {
        return;
    }

    const int x = 500;
    int y = 30;

    BWAPI::Broodwar->drawTextScreen(x, y, "%cStrat Boss", white);
    y += 13;
    BWAPI::Broodwar->drawTextScreen(x, y, "%cbases %c%d/%d", yellow, cyan, nBases, nBases+nFreeBases);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%cpatches %c%d", yellow, cyan, nMineralPatches);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%cgeysers %c%d+%d", yellow, cyan, nGas, nFreeGas);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%cdrones%c %d/%d", yellow, cyan, nDrones, maxDrones);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%c mins %c%d", yellow, cyan, nMineralDrones);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%c gas %c%d", yellow, cyan, nGasDrones);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%c react%c +%d", yellow, cyan, _extraDronesWanted);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%clarvas %c%d", yellow, cyan, nLarvas);
    y += 10;
    BWAPI::Broodwar->drawTextScreen(x, y, "%cbuild %c+%dg +%dh", yellow, cyan, _extraOpeningExtractors, _extraOpeningHatcheries);
    if (hiddenBaseNext())
    {
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y, "%chidden base next %c(%d)", green, cyan, _droneToHide->getID());
    }
    y += 13;
    if (outOfBook)
    {
        BWAPI::Broodwar->drawTextScreen(x, y, "%ceco %c%.2f %d/%d", yellow, cyan, _economyRatio, _economyDrones, 1 + _economyTotal);
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y, "%carmy %c%d %d %c%s", yellow, cyan, myArmySize, enemyAntigroundArmySize, enoughArmy() ? green : red, enoughArmy() ? "ok" : "bad");

        std::array<bool, int(TechUnit::Size)> available;
        setAvailableTechUnits(available);
        for (int i = 1 + int(TechUnit::None); i < int(TechUnit::Size); ++i)
        {
            y += 10;
            BWAPI::Broodwar->drawTextScreen(x, y, "%c%s%c%s %c%d",
                white, available[i] ? "* " : "",
                orange, techTargetToString(TechUnit(i)).c_str(),
                cyan, techScores[i]);
        }
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y, "%c%s", green, UnitTypeName(_mineralUnit).c_str());
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y, "%c%s", green, UnitTypeName(_gasUnit).c_str());
        if (_auxUnit != BWAPI::UnitTypes::None)
        {
            y += 10;
            BWAPI::Broodwar->drawTextScreen(x, y, "%c%d/%d%c %s", cyan, the.my.all.count(_auxUnit), _auxUnitCount, green, UnitTypeName(_auxUnit).c_str());
        }
        if (_recommendQueens || nQueens > 0)
        {
            y += 10;
            BWAPI::Broodwar->drawTextScreen(x, y, "%c%d/%d%c Queen%s (%d %d %d)",
                cyan, nQueens, _recommendQueens,
                green, _recommendQueens == 1 ? "" : "s",
                _recommendParasite, _recommendEnsnare, _recommendBroodling);
        }
        if (_techTarget != TechUnit::None)
        {
            y += 10;
            BWAPI::Broodwar->drawTextScreen(x, y, "%cplan %c%s", white, green,
                techTargetToString(_techTarget).c_str());
        }
    }
    else
    {
        BWAPI::Broodwar->drawTextScreen(x, y, "%c[book]", white);
    }
    if (_emergencyGroundDefense)
    {
        y += 13;
        BWAPI::Broodwar->drawTextScreen(x, y, "%cemergency", red);
    }
    if (_emergencyNow)
    {
        y += 13;
        BWAPI::Broodwar->drawTextScreen(x, y, "%cEMERGENCY NOW!", red);
    }
}

// -- -- -- -- -- -- -- -- -- -- --
// Public methods.

StrategyBossZerg & StrategyBossZerg::Instance()
{
    static StrategyBossZerg instance;
    return instance;
}

// Set the unit mix.
// The mineral unit can be set to Drone, but cannot be None.
// The mineral unit must be less gas-intensive than the gas unit.
// The idea is to make as many gas units as gas allows, and use any extra minerals
// on the mineral units (which may want gas too).
void StrategyBossZerg::setUnitMix(BWAPI::UnitType minUnit, BWAPI::UnitType gasUnit)
{
    UAB_ASSERT(minUnit.isValid(), "bad mineral unit");
    UAB_ASSERT(gasUnit.isValid() || gasUnit == BWAPI::UnitTypes::None, "bad gas unit");

    _mineralUnit = minUnit;
    _gasUnit = gasUnit;
}

void StrategyBossZerg::setEconomyRatio(double ratio)
{
    UAB_ASSERT(ratio >= 0.0 && ratio < 1.0, "bad economy ratio");
    _economyRatio = ratio;
    _economyDrones = 0;
    _economyTotal = 0;
}

// The next base we want to take is at MacroLocation::Hidden.
bool StrategyBossZerg::hiddenBaseNext() const
{
    // Not dead, not morphed, not mind controlled.
    return
        _droneToHide &&
        _droneToHide->exists() &&
        _droneToHide->getType() == BWAPI::UnitTypes::Zerg_Drone &&
        _droneToHide->getPlayer() == the.self();
}

// Solve urgent production issues. Called once per frame.
// If we're in trouble, clear the production queue and/or insert emergency actions.
// Or if we just need overlords, make them.
// This routine is allowed to take direct actions or cancel stuff to get or preserve resources.
void StrategyBossZerg::handleUrgentProductionIssues(BuildOrderQueue & queue)
{
    updateGameState();

    while (nextInQueueIsUseless(queue))
    {
        if (Config::Debug::DrawQueueFixInfo)
        {
            BWAPI::Broodwar->printf("queue: drop useless %s", queue.getHighestPriorityItem().macroAct.getName().c_str());
        }

        BWAPI::UnitType nextInQueue = BWAPI::UnitTypes::None;
        if (queue.getHighestPriorityItem().macroAct.isUnit())
        {
            nextInQueue = queue.getHighestPriorityItem().macroAct.getUnitType();
        }
        
        queue.removeHighestPriorityItem();

        if (nextInQueue == BWAPI::UnitTypes::Zerg_Hatchery)
        {
            // We only cancel a hatchery in case of dire emergency. Get the scout drone back home.
            ScoutManager::Instance().releaseWorkerScout();
            // Also cancel hatcheries already sent away for.
            // BWAPI::Broodwar->printf("cancel useless hatchery");
            BuildingManager::Instance().cancelBuildingType(BWAPI::UnitTypes::Zerg_Hatchery);
        }
        else if (nextInQueue == BWAPI::UnitTypes::Zerg_Lair ||
            nextInQueue == BWAPI::UnitTypes::Zerg_Spire ||
            nextInQueue == BWAPI::UnitTypes::Zerg_Hydralisk_Den)
        {
            // We can't make a key tech building. If it's the opening, better break out.
            ProductionManager::Instance().goOutOfBook();
            // If we were in the opening book, then the queue is now empty.
            // If we were not in the opening, nothing happened.
        }
    }

    // Check for the most urgent actions once per frame.
    if (takeUrgentAction(queue))
    {
        // These are serious emergencies, and it's no help to check further.
        makeOverlords(queue);
    }
    else
    {
        // Check for less urgent reactions less often.
        int frameOffset = the.now() % 32;
        if (frameOffset == 0 || frameOffset == 15)
        {
            makeUrgentReaction(queue);
            makeOverlords(queue);
        }
        else if (frameOffset == 16)
        {
            checkGroundDefenses(queue);
        }
        else if (frameOffset == 24)
        {
            analyzeExtraDrones();       // no need to make overlords
        }
        else if (frameOffset == 25)
        {
            if (queue.size() > 3 && _supplyUsed > 2 * 198)
            {
                // We have grown a long queue but we are maxed, or nearly so.
                // Clear it except for the top element so we can react to surprises
                // without having to slowly run through the whole queue first.
                // BWAPI::Broodwar->printf("trim queue");
                BuildOrderItem item = queue.getHighestPriorityItem();
                queue.clearAll();
                queue.queueAsHighestPriority(item.macroAct, item.isGasSteal);
            }
        }
    }
}

// Called when the queue is empty, which means that we are out of book.
// Fill up the production queue with new stuff.
BuildOrder & StrategyBossZerg::freshProductionPlan()
{
    _latestBuildOrder.clearAll();

    updateGameState();

    // If we have enough idle drones, might as well put them to work gathering gas.
    if (!WorkerManager::Instance().isCollectingGas() &&
        WorkerManager::Instance().getNumIdleWorkers() >= 3 * nGas)
    {
        produce(MacroCommandType::StartGas);
    }

    // Special adaptations to specific opponent plans.
    if (adaptToEnemyOpeningPlan())
    {
        return _latestBuildOrder;
    }

    // We always want at least 9 drones and a spawning pool.
    if (rebuildCriticalLosses())
    {
        return _latestBuildOrder;
    }

    // Set the tech target and unit mix.
    chooseStrategy();

    // If we're making gas units, short on gas, and not gathering gas, fix that first.
    // NOTE Does not check whether we have any extractors. Doesn't matter.
    if ((_gasUnit != BWAPI::UnitTypes::None && gas < _gasUnit.gasPrice() || gas < _mineralUnit.gasPrice()) &&
        !WorkerManager::Instance().isCollectingGas())
    {
        produce(MacroCommandType::StartGas);
    }

    // Decide whether we have "enough" units to be safe while we tech up or otherwise spend resources.
    // This helps the bot, e.g., not make a spire right off when it has just finished lurker research.
    // This ought to be based on the danger of the enemy's army, but for now we use an arbitrary low limit.
    const int numMineralUnits = the.my.all.count(_mineralUnit);
    const int numGasUnits = (_gasUnit == BWAPI::UnitTypes::None) ? 0 : the.my.all.count(_gasUnit);
    const bool hasEnoughUnits =
        numMineralUnits + 2 * numGasUnits >= 10 &&
        (!hasSpire || !the.info.enemyHasAirTech() ||
        nMutas >= 4 || the.my.all.count(BWAPI::UnitTypes::Zerg_Scourge) >= 2 ||
        numMineralUnits >= 6 && numGasUnits >= 6);
    
    int mineralsLeft = minerals;
    int gasLeft = gas;

    if (hasEnoughUnits)
    {
        produceOtherStuff(mineralsLeft, gasLeft, hasEnoughUnits);
        produceUnits(mineralsLeft, gasLeft);
    }
    else
    {
        produceUnits(mineralsLeft, gasLeft);
    }

    return _latestBuildOrder;
}
