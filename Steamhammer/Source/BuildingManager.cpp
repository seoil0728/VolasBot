#include "BuildingManager.h"

#include "Bases.h"
#include "BuildingPlacer.h"
#include "ProductionManager.h"
#include "ScoutManager.h"
#include "StrategyBossZerg.h"
#include "The.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

BuildingManager::BuildingManager()
    : _reservedMinerals(0)
    , _reservedGas(0)
    , _stalledForLackOfSpace(false)
{
}

// Called every frame from GameCommander.
void BuildingManager::update()
{
    validateBuildings();					// check to see if assigned workers have died en route or while constructing
    assignWorkersToUnassignedBuildings();   // assign workers to the unassigned buildings and label them 'planned'    
    constructAssignedBuildings();           // for each planned building, if the worker isn't constructing, send the command    
    checkForStartedConstruction();          // check to see if any buildings have started construction and update data structures    
    checkForDeadTerranBuilders();           // if we are terran and a building is under construction without a worker, assign a new one    
    checkForCompletedBuildings();           // check to see if any buildings have completed and update data structures
    checkReservedResources();               // verify that reserved minerals and gas are counted correctly

    if (the.now() % 72 == 0 && the.selfRace() == BWAPI::Races::Terran)
    {
        clearAbandonedTerranBuildings();
    }
}

// The building took too long to start, or we lost too many workers trying to build it.
// If true, the building will be canceled.
bool BuildingManager::buildingTimedOut(const Building & b) const
{
    return
        the.now() - b.startFrame > 60 * 24 ||
        b.buildersSent > 2;
}

// STEP 1: DO BOOK KEEPING ON BUILDINGS WHICH MAY HAVE DIED OR TIMED OUT
void BuildingManager::validateBuildings()
{
    // Not in the opening.
    if (!ProductionManager::Instance().isOutOfBook())
    {
        return;
    }

    // The purpose of this vector is to avoid changing the list of buildings
    // while we are in the midst of iterating through it.
    std::vector< std::reference_wrapper<Building> > toRemove;
    
    // Find and remove any buildings which have failed construction and should be deleted.
    for (Building & b : _buildings)
    {
        if (buildingTimedOut(b) &&
            (!b.buildingUnit || b.type.getRace() == BWAPI::Races::Terran && !b.builderUnit))
        {
            toRemove.push_back(b);
        }
        else if (b.status == BuildingStatus::UnderConstruction)
        {
            if (!b.buildingUnit ||
                !b.buildingUnit->exists() ||
                b.buildingUnit->getHitPoints() <= 0 ||
                !b.buildingUnit->getType().isBuilding())
            {
                toRemove.push_back(b);
            }
        }
    }

    undoBuildings(toRemove);
}

// STEP 2: ASSIGN WORKERS TO BUILDINGS WITHOUT THEM
// Also places the building.
void BuildingManager::assignWorkersToUnassignedBuildings()
{
    // For each building that doesn't have a builder, assign one.
    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::Unassigned)
        {
            continue;
        }

        // Skip protoss buildings that need pylon power if there is no space for them.
        if (typeIsStalled(b.type))
        {
            continue;
        }

        if (b.buildersSent > 1 &&
            b.type == BWAPI::UnitTypes::Zerg_Hatchery &&
            b.macroLocation != MacroLocation::Main &&
            the.my.all.count(BWAPI::UnitTypes::Zerg_Larva) == 0)	// yes, we may need more production
        {
            // This is a zerg expansion which failed--we sent a builder and it never built.
            // The builder was most likely killed along the way.
            // Conclude that we need more production and change it to a macro hatchery.
            //BWAPI::Broodwar->printf("change to macro hatch %d,%d", b.desiredPosition.x, b.desiredPosition.y);
            b.macroLocation = MacroLocation::Main;
            b.desiredPosition = the.bases.myMain()->getTilePosition();  // all macro hatches in main TODO for now
        }

        // The builder may have already been decided before we got this far.
        // If we don't already have a valid builder, choose one.
        // NOTE There is a dependency loop here. To get the builder, we want to know the
        //      building's position. But to check its final position we need to know the
        //      builder unit, because other units block the building placement while the
        //      builder unit itself does not. To solve this, we get the builder first based
        //      on the desired rather than the final position.
        //      Better solutions are possible!
        if (!validBuilder(b.builderUnit))
        {
            releaseBuilderUnit(b);		           // does nothing if it's null
            setBuilderUnit(b);
            if (b.builderUnit)
            {
                //BWAPI::Broodwar->printf("(re)assign builder %d to %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());
                ++b.buildersSent;
            }
        }

        // No good, we can't get a worker. Give up on the building for this frame.
        if (!b.builderUnit)
        {
            continue;
        }

        // We want the worker before the location, so that we can avoid counting the worker
        // as an obstacle for the location it is already in--ProductionManager sends the worker early.
        // Otherwise we may have to re-place the building elsewhere, causing unnecessary movement.
        BWAPI::TilePosition testLocation = getBuildingLocation(b);
        if (!testLocation.isValid())
        {
            // The building could not be placed (or was placed incorrectly due to a bug, which should not happen).
            // Recognize the case where protoss building placement is stalled for lack of space.
            // In principle, terran or zerg could run out of space, but it's rare in practice.
            if (b.type.requiresPsi() && testLocation == BWAPI::TilePositions::None)
            {
                _stalledForLackOfSpace = true;
            }
            if (b.builderUnit && b.buildersSent > 0)
            {
                //BWAPI::Broodwar->printf("bad location, clear builder %d of %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());
                --b.buildersSent;
            }
            releaseBuilderUnit(b);
            continue;
        }
        b.finalPosition = testLocation;

        //BWAPI::Broodwar->printf("assign builder %d to %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());
        the.placer.reserveTiles(b.finalPosition, b.type.tileWidth(), b.type.tileHeight());

        b.status = BuildingStatus::Assigned;
    }
}

// STEP 3: ISSUE CONSTRUCTION ORDERS TO ASSIGNED BUILDINGS AS NEEDED
void BuildingManager::constructAssignedBuildings()
{
    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::Assigned)
        {
            continue;
        }

        if (!b.builderUnit ||
            b.builderUnit->getPlayer() != the.self() ||
            !b.builderUnit->exists() && b.type != BWAPI::UnitTypes::Zerg_Extractor ||
            b.builderUnit->isLockedDown() ||
            b.builderUnit->isStasised() ||
            b.builderUnit->isMaelstrommed() ||
            b.builderUnit->isBurrowed())
        {
            // NOTE A zerg drone builderUnit no longer exists() after starting an extractor.
            //      The geyser unit changes into the building, the drone vanishes.
            //      For other zerg buildings, the drone changes into the building.
            releaseBuilderUnit(b);

            //BWAPI::Broodwar->printf("b.builderUnit gone, b.type = %s", UnitTypeName(b.type).c_str());

            b.buildCommandGiven = false;
            b.status = BuildingStatus::Unassigned;
            the.placer.freeTiles(b);
        }
        else if (!b.builderUnit->isConstructing())
        {
            if (!isBuildingPositionExplored(b))
            {
                // We haven't explored the build position. Go there.
                // If we're going to start a new base, use the base's distance map.
                // NOTE getBaseAtTilePosition() returns null if there is no base at or very near the position.
                Base * base = b.type.isResourceDepot() ? the.bases.getBaseAtTilePosition(b.finalPosition) : nullptr;
                the.micro.MoveSafely(b.builderUnit, b.getCenter(), base ? &base->getDistances() : nullptr);
            }
            // if this is not the first time we've sent this guy to build this
            // it must be the case that something was in the way
            else if (b.buildCommandGiven)
            {
                // Give it a little time to happen.
                // This mainly prevents over-frequent retrying.
                if (the.now() > b.placeBuildingDeadline)
                {
                    //BWAPI::Broodwar->printf("deadline passed, clear builder %d of %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());

                    // tell worker manager the unit we had is not needed now, since we might not be able
                    // to get a valid location soon enough
                    releaseBuilderUnit(b);

                    b.buildCommandGiven = false;
                    b.status = BuildingStatus::Unassigned;

                    // This is a normal event that may happen repeatedly before the building starts.
                    // Don't count it as a failure.
                    --b.buildersSent;

                    // Unreserve the building location. The building will be re-placed.
                    the.placer.freeTiles(b);
                }
            }
            else
            {
                // Special case for sunkens and spores: Build the creep colony then morph it.
                BWAPI::UnitType t = b.type;
                if (t == BWAPI::UnitTypes::Zerg_Sunken_Colony || t == BWAPI::UnitTypes::Zerg_Spore_Colony)
                {
                    t = BWAPI::UnitTypes::Zerg_Creep_Colony;
                }
                
                // Issue the build order and record whether it succeeded.
                // If the builderUnit is zerg, it changes to !exists() when it builds.
                b.buildCommandGiven = the.micro.Build(b.builderUnit, t, b.finalPosition);

                if (b.buildCommandGiven)
                {
                    b.placeBuildingDeadline =
                        the.now() + 10 * BWAPI::Broodwar->getLatencyFrames();
                }
                else
                {
                    //BWAPI::Broodwar->printf("failed to start construction of %s, error %d", UnitTypeName(b.type).c_str(), BWAPI::Broodwar->getLastError());
                }
            }
        }
    }
}

// STEP 4: UPDATE DATA STRUCTURES FOR BUILDINGS STARTING CONSTRUCTION
void BuildingManager::checkForStartedConstruction()
{
    for (BWAPI::Unit buildingStarted : the.self()->getUnits())
    {
        // filter out units which aren't buildings under construction
        if (!buildingStarted->getType().isBuilding() || !buildingStarted->isBeingConstructed())
        {
            continue;
        }

        // check all our building status objects to see if we have a match and if we do, update it
        for (Building & b : _buildings)
        {
            if (b.status != BuildingStatus::Assigned)
            {
                continue;
            }
        
            // check if the positions match
            if (b.finalPosition == buildingStarted->getTilePosition())
            {
                // the resources should now be spent, so unreserve them
                _reservedMinerals -= buildingStarted->getType().mineralPrice();
                _reservedGas      -= buildingStarted->getType().gasPrice();

                // flag it as started and set the buildingUnit
                b.underConstruction = true;
                b.buildingUnit = buildingStarted;

                // The building is started; handle zerg and protoss builders.
                // Terran builders are dealt with after the building finishes.
                if (the.selfRace() == BWAPI::Races::Zerg)
                {
                    // If we are zerg, the builderUnit no longest exists as such.
                    // If the building later gets canceled, a new drone will "mysteriously" appear.
                    // There's no drone to release, but we still want to let the ScoutManager know
                    // that the gas steal is accomplished. If it's not a gas steal, this does nothing.
                    releaseBuilderUnit(b);
                }
                else if (the.selfRace() == BWAPI::Races::Protoss)
                {
                    releaseBuilderUnit(b);
                }

                b.status = BuildingStatus::UnderConstruction;

                the.placer.freeTiles(b);

                // Only one Building will match.
                // We keep running the outer loop to look for more buildings starting construction.
                break;
            }
        }
    }
}

// STEP 5: IF THE SCV DIED DURING CONSTRUCTION, ASSIGN A NEW ONE
void BuildingManager::checkForDeadTerranBuilders()
{
    if (the.selfRace() != BWAPI::Races::Terran)
    {
        return;
    }

    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            continue;
        }

        UAB_ASSERT(b.buildingUnit, "null buildingUnit");

        if (!validBuilder(b.builderUnit))
        {
            releaseBuilderUnit(b);
            setBuilderUnit(b);      // don't count it in buildersSent
            if (b.builderUnit)
            {
                b.builderUnit->rightClick(b.buildingUnit);
            }
        }
    }
}

// STEP 6: CHECK FOR COMPLETED BUILDINGS
// In case of terran gas steal, stop construction a little early,
// so we can cancel the refinery later and recover resources. 
// Zerg and protoss can't do that.
void BuildingManager::checkForCompletedBuildings()
{
    std::vector< std::reference_wrapper<Building> > toRemove;

    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            continue;       
        }

        UAB_ASSERT(b.buildingUnit, "null buildingUnit");

        if (b.buildingUnit->isCompleted())
        {
            // If we are terran, give the worker back to worker manager.
            // Zerg and protoss are handled when the building starts.
            if (the.selfRace() == BWAPI::Races::Terran)
            {
                releaseBuilderUnit(b);
            }

            if ((b.type == BWAPI::UnitTypes::Zerg_Sunken_Colony || b.type == BWAPI::UnitTypes::Zerg_Spore_Colony) &&
                b.buildingUnit->getType() == BWAPI::UnitTypes::Zerg_Creep_Colony)
            {
                if (b.buildingUnit->canMorph(b.type))
                {
                    b.buildingUnit->morph(b.type);
                }
            }
            else
            {
                // Done with the building record.
                toRemove.push_back(b);
            }
        }
        else
        {
            // The building, whatever it is, is not completed.
            // If it is a terran gas steal, stop construction early.
            if (b.isGasSteal &&
                the.selfRace() == BWAPI::Races::Terran &&
                b.builderUnit &&
                b.builderUnit->canHaltConstruction() &&
                b.buildingUnit->getRemainingBuildTime() < 24)
            {
                b.builderUnit->haltConstruction();
                releaseBuilderUnit(b);

                // Call the building done. It's as finished as we want it to be.
                toRemove.push_back(b);
            }
        }
    }

    removeBuildings(toRemove);
}

// Error check: Bugs could cause resources to be reserved and never released.
// We correct the values as a workaround.
// Currently there are no known bugs causing this.
void BuildingManager::checkReservedResources()
{
    // Check for errors.
    int minerals = 0;
    int gas = 0;

    for (Building & b : _buildings)
    {
        if (b.status == BuildingStatus::Assigned || b.status == BuildingStatus::Unassigned)
        {
            minerals += b.type.mineralPrice();
            gas += b.type.gasPrice();
        }
    }

    if (minerals != _reservedMinerals || gas != _reservedGas)
    {
        // This should ideally never happen. If it does, we correct the error and carry on.
        //BWAPI::Broodwar->printf("reserves wrong: %d %d should be %d %d", _reservedMinerals, _reservedGas, minerals, gas);
        _reservedMinerals = minerals;
        _reservedGas = gas;
    }
}

// It's possible for a terran building to be abandoned unfinished and never canceled,
// due to a bug somewhere. Work around it: Cancel the building anyway.
void BuildingManager::clearAbandonedTerranBuildings()
{
    for (BWAPI::Unit building : the.self()->getUnits())
    {
        if (building->getType().isBuilding() &&
            !building->isCompleted() &&
            !building->isBeingConstructed() &&
            building->canCancelConstruction())
        {
            // The building is abandoned if it is not in the building queue.
            for (const Building & b : _buildings)
            {
                if (b.buildingUnit == building)
                {
                    goto not_abandoned;
                }
            }
            // If we got here, it was abandoned.
            if (Config::Debug::DrawQueueFixInfo)
            {
                BWAPI::Broodwar->printf("queue: cancel abandoned %s", UnitTypeName(building).c_str());
            }
            building->cancelConstruction();
        }
not_abandoned:
        ;
    }
}

// Add a new building to be constructed and return it.
// The builder may be null. In that case, the building manager will assign a worker on its own later.
Building & BuildingManager::addTrackedBuildingTask(const MacroAct & act, BWAPI::TilePosition desiredLocation, BWAPI::Unit builder, bool isGasSteal)
{
    UAB_ASSERT(act.isBuilding(), "bad building");

    if (isGasSteal)
    {
        //BWAPI::Broodwar->printf("gas steal into building manager");
    }

    BWAPI::UnitType type = act.getUnitType();

    _reservedMinerals += type.mineralPrice();
    _reservedGas += type.gasPrice();

    Building b(type, desiredLocation);
    b.macroLocation = act.getMacroLocation();
    if (b.macroLocation == MacroLocation::Tile)
    {
        b.desiredPosition = b.finalPosition = act.getTileLocation();
    }
    b.isGasSteal = isGasSteal;
    b.status = BuildingStatus::Unassigned;

    // The builder, if provided, may have been killed, or be otherwise unavailable.
    if (validBuilder(builder))
    {
        //BWAPI::Broodwar->printf("build man receives worker %d for %s", builder->getID(), UnitTypeName(type).c_str());
        b.builderUnit = builder;
        b.buildersSent = 1;
        // Ensure that our builder is assigned to be a builder. The caller may not have done it.
        WorkerManager::Instance().setBuildWorker(builder);
    }

    _buildings.push_back(b);      // make a "permanent" copy of the Building object
    return _buildings.back();     // return a reference to the permanent copy
}

// Add a new building to be constructed.
void BuildingManager::addBuildingTask(const MacroAct & act, BWAPI::TilePosition desiredLocation, BWAPI::Unit builder, bool isGasSteal)
{
    (void) addTrackedBuildingTask(act, desiredLocation, builder, isGasSteal);
}

bool BuildingManager::isBuildingPositionExplored(const Building & b) const
{
    BWAPI::TilePosition tile = b.finalPosition;

    // For each tile where the building will be built, is the tile explored?
    for (int x=0; x<b.type.tileWidth(); ++x)
    {
        for (int y=0; y<b.type.tileHeight(); ++y)
        {
            if (!BWAPI::Broodwar->isExplored(tile.x + x,tile.y + y))
            {
                return false;
            }
        }
    }

    return true;
}

// Do we have a builder unit that can build?
bool BuildingManager::validBuilder(BWAPI::Unit worker) const
{
    return
        worker &&
        worker->exists() &&
        !worker->isLockedDown() &&
        !worker->isStasised() &&
        !worker->isMaelstrommed() &&
        !worker->isBurrowed() &&
        worker->getPlayer() == the.self() &&     // not mind controlled
        worker->getType().isWorker();            // not morphed into a building already
}

// Assign a worker to contruct the building.
void BuildingManager::setBuilderUnit(Building & b)
{
    UAB_ASSERT(!b.builderUnit, "incorrectly replacing builder");

    // Grab the closest worker from WorkerManager.
    // It knows to return the scout worker if this is a gas steal.
    b.builderUnit = WorkerManager::Instance().getBuilder(b);
    if (b.builderUnit)
    {
        WorkerManager::Instance().setBuildWorker(b.builderUnit);
        //BWAPI::Broodwar->printf("build man assigns worker %d for %s", b.builderUnit ? b.builderUnit->getID() : -1, UnitTypeName(b.type).c_str());
    }
    else
    {
        //BWAPI::Broodwar->printf("no builder available to assign");
    }
}

// Notify the worker manager that the worker is free again,
// but not if the scout manager owns the worker.
void BuildingManager::releaseBuilderUnit(Building & b)
{
    if (b.isGasSteal)
    {
        ScoutManager::Instance().setGasStealOver();
    }
    else
    {
        if (b.builderUnit)
        {
            //BWAPI::Broodwar->printf("build man releases worker for %s", UnitTypeName(b.type).c_str());
            
            WorkerManager::Instance().finishedWithWorker(b.builderUnit);
        }
    }
    b.builderUnit = nullptr;
}

int BuildingManager::getReservedMinerals() const
{
    return _reservedMinerals;
}

int BuildingManager::getReservedGas() const
{
    return _reservedGas;
}

// In the building queue with any status.
bool BuildingManager::isBeingBuilt(BWAPI::UnitType type) const
{
    for (const Building & b : _buildings)
    {
        if (b.type == type)
        {
            return true;
        }
    }

    return false;
}

// Number in the building queue with status other than "under constrution".
size_t BuildingManager::getNumUnstarted() const
{
    size_t count = 0;

    for (const Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            ++count;
        }
    }

    return count;
}

// Number of a given type in the building queue with status other than "under constrution".
size_t BuildingManager::getNumUnstarted(BWAPI::UnitType type) const
{
    size_t count = 0;

    for (const Building & b : _buildings)
    {
        if (b.type == type && b.status != BuildingStatus::UnderConstruction)
        {
            ++count;
        }
    }

    return count;
}

bool BuildingManager::isGasStealInQueue() const
{
    for (const Building & b : _buildings)
    {
        if (b.isGasSteal)
        {
            return true;
        }
    }

    return false;
}

// We have queued an expansion to the given base.
// (Or at least some building at the same location.)
bool BuildingManager::isBasePlanned(const Base * base) const
{
    for (const Building & b : _buildings)
    {
        if (b.finalPosition == base->getTilePosition())
        {
            return true;
        }
    }

    return false;
}

void BuildingManager::drawBuildingInformation(int x, int y)
{
    if (!Config::Debug::DrawBuildingInfo)
    {
        return;
    }

    /*
    // Enemy building completion times, as best we can tell.
    for (const auto & kv : the.info.getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.type.isBuilding())
        {
            BWAPI::Broodwar->drawTextMap(ui.lastPosition.x,ui.lastPosition.y+17,"\x07%d", ui.completeBy);
        }
    }
    */

    /*
    // Unit IDs for our own units.
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        BWAPI::Broodwar->drawTextMap(unit->getPosition().x,unit->getPosition().y+5,"\x07%d",unit->getID());
    }
    */

    BWAPI::Broodwar->drawTextScreen(x,y+20,"\x04 Building");
    BWAPI::Broodwar->drawTextScreen(x+150,y+20,"\x04 State");

    int yspace = 0;

    for (const Building & b : _buildings)
    {
        std::string steal = b.isGasSteal ? " (steal)" : "";
        if (b.status == BuildingStatus::Unassigned)
        {
            int x1 = b.desiredPosition.x * 32;
            int y1 = b.desiredPosition.y * 32;
            int x2 = (b.desiredPosition.x + b.type.tileWidth()) * 32;
            int y2 = (b.desiredPosition.y + b.type.tileHeight()) * 32;

            char color = yellow;
            if (typeIsStalled(b.type))
            {
                color = red;
            }

            BWAPI::Broodwar->drawTextScreen(x, y + 40 + ((yspace)* 10), "%c %s%s", color, NiceMacroActName(b.type.getName()).c_str(), steal.c_str());
            BWAPI::Broodwar->drawTextScreen(x + 150, y + 40 + ((yspace++) * 10), "%c Waiting", color);
            BWAPI::Broodwar->drawBoxMap(x1, y1, x2, y2, BWAPI::Colors::Green, false);
        }
        else if (b.status == BuildingStatus::Assigned)
        {
            BWAPI::Broodwar->drawTextScreen(x, y + 40 + ((yspace)* 10), "\x03 %s%s %d", NiceMacroActName(b.type.getName()).c_str(), steal.c_str(), b.builderUnit->getID());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 Assigned (%d,%d)",b.finalPosition.x,b.finalPosition.y);

            int x1 = b.finalPosition.x*32;
            int y1 = b.finalPosition.y*32;
            int x2 = (b.finalPosition.x + b.type.tileWidth())*32;
            int y2 = (b.finalPosition.y + b.type.tileHeight())*32;

            BWAPI::Broodwar->drawLineMap(b.builderUnit->getPosition().x,b.builderUnit->getPosition().y,(x1+x2)/2,(y1+y2)/2,BWAPI::Colors::Orange);
            BWAPI::Broodwar->drawBoxMap(x1,y1,x2,y2,BWAPI::Colors::Red,false);
        }
        else if (b.status == BuildingStatus::UnderConstruction)
        {
            BWAPI::Broodwar->drawTextScreen(x, y + 40 + ((yspace)* 10), "\x03 %s%s %d", NiceMacroActName(b.type.getName()).c_str(), steal.c_str(), b.buildingUnit->getID());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 Const");
        }
    }
}

BuildingManager & BuildingManager::Instance()
{
    static BuildingManager instance;
    return instance;
}

// The buildings queued and not yet started.
std::vector<BWAPI::UnitType> BuildingManager::buildingsQueued()
{
    std::vector<BWAPI::UnitType> buildingsQueued;

    for (const Building & b : _buildings)
    {
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            buildingsQueued.push_back(b.type);
        }
    }

    return buildingsQueued;
}

// Cancel a given building when possible.
// Used as part of the extractor trick or in an emergency.
// NOTE CombatCommander::cancelDyingItems() can also cancel buildings, including
//      morphing zerg structures which the BuildingManager does not handle.
void BuildingManager::cancelBuilding(Building & b)
{
    std::vector< std::reference_wrapper<Building> > toRemove;

    // No matter the status, we don't want to keep the worker.
    releaseBuilderUnit(b);

    if (b.status == BuildingStatus::Unassigned)
    {
        toRemove.push_back(b);
        undoBuildings(toRemove);
    }
    else if (b.status == BuildingStatus::Assigned)
    {
        toRemove.push_back(b);
        undoBuildings(toRemove);
    }
    else if (b.status == BuildingStatus::UnderConstruction)
    {
        if (b.buildingUnit && b.buildingUnit->exists() && !b.buildingUnit->isCompleted())
        {
            the.micro.Cancel(b.buildingUnit);
        }
        toRemove.push_back(b);
        undoBuildings(toRemove);
    }
    else
    {
        UAB_ASSERT(false, "unexpected building status");
    }
}

// It's an emergency. Cancel all buildings which are not yet started.
void BuildingManager::cancelQueuedBuildings()
{
    std::vector< std::reference_wrapper<Building> > toCancel;

    for (Building & b : _buildings)
    {
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            toCancel.push_back(b);
        }
    }

    for (Building & b : toCancel)
    {
        cancelBuilding(b);
    }
}

// It's an emergency. Cancel all buildings of a given type.
void BuildingManager::cancelBuildingType(BWAPI::UnitType t)
{
    std::vector< std::reference_wrapper<Building> > toCancel;

    for (Building & b : _buildings)
    {
        if (b.type == t)
        {
            toCancel.push_back(b);
        }
    }

    for (Building & b : toCancel)
    {
        cancelBuilding(b);
    }
}

BWAPI::TilePosition BuildingManager::getBuildingLocation(const Building & b)
{
    if (b.isGasSteal)
    {
        Base * enemyBase = the.bases.enemyStart();
        UAB_ASSERT(enemyBase, "Should find enemy base before gas steal");
        UAB_ASSERT(enemyBase->getGeysers().size() > 0, "Should have spotted an enemy geyser");

        for (BWAPI::Unit geyser : enemyBase->getGeysers())
        {
            return geyser->getTilePosition();
        }
    }

    int numPylons = the.my.completed.count(BWAPI::UnitTypes::Protoss_Pylon);
    if (b.type.requiresPsi() && numPylons == 0)
    {
        return BWAPI::TilePositions::None;
    }

    if (b.type.isRefinery())
    {
        return the.placer.getRefineryPosition();
    }

    MacroLocation loc = b.macroLocation;

    if (loc == MacroLocation::Tile && b.finalPosition.isValid())
    {
        return the.placer.getBuildLocationNear(b, 0);
    }

    // A resource depot going Anywhere is really going to an expansion.
    if (b.type.isResourceDepot() && loc == MacroLocation::Anywhere)
    {
        loc = MacroLocation::Expo;
    }

    // A resource depot goes at an expansion location, except for certain non-base hatchery locations.
    // Only zerg macro hatcheries and proxy hatcheries are placed away from expo locations.
    if (b.type.isResourceDepot() &&
        b.macroLocation != MacroLocation::Main &&
        b.macroLocation != MacroLocation::Macro &&
        b.macroLocation != MacroLocation::Proxy &&
        b.macroLocation != MacroLocation::Front &&
        b.macroLocation != MacroLocation::Center)
    {
        BWAPI::TilePosition pos = the.placer.getExpoLocationTile(b.macroLocation);
        if (the.placer.buildingOK(b, pos) && !the.groundAttacks.inRange(b.type, pos))
        {
            return pos;
        }
        return BWAPI::TilePositions::None;
    }

    int distance = Config::Macro::BuildingSpacing;
    if (b.type == BWAPI::UnitTypes::Terran_Bunker ||
        b.type == BWAPI::UnitTypes::Terran_Missile_Turret ||
        b.type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
        b.type == BWAPI::UnitTypes::Zerg_Sunken_Colony ||
        b.type == BWAPI::UnitTypes::Zerg_Spore_Colony ||
        b.type == BWAPI::UnitTypes::Zerg_Creep_Colony)
    {
        // Pack defenses tightly together.
        distance = 0;
    }
    else if (b.type == BWAPI::UnitTypes::Protoss_Pylon)
    {
        if (numPylons < 3)
        {
            // Early pylons may be spaced differently than other buildings.
            distance = Config::Macro::PylonSpacing;
        }
        else
        {
            // Building spacing == 1 is usual. Be more generous with pylons.
            distance = 2;
        }
    }

    // The building placer does the rest.
    return the.placer.getBuildLocationNear(b, distance);
}

// The buildings failed or were canceled.
// Undo any connections with other data structures, then delete.
void BuildingManager::undoBuildings(const std::vector< std::reference_wrapper<Building> > & toRemove)
{
    for (Building & b : toRemove)
    {
        // If the building was to establish a base, unreserve the base location.
        if (b.type.isResourceDepot() && b.macroLocation != MacroLocation::Main && b.finalPosition.isValid())
        {
            Base * base = the.bases.getBaseAtTilePosition(b.finalPosition);
            if (base)
            {
                base->unreserve();
            }
        }

        releaseBuilderUnit(b);

        // If the building is not yet under construction, release its resources.
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            _reservedMinerals -= b.type.mineralPrice();
            _reservedGas -= b.type.gasPrice();
        }

        // Free tiles that were reserved for the building.
        if (b.status == BuildingStatus::Assigned || b.status == BuildingStatus::UnderConstruction)
        {
            the.placer.freeTiles(b);
        }

        // Cancel a terran building under construction. Zerg and protoss finish on their own,
        // but a terran building will be abandoned unfinished unless canceled.
        if (b.buildingUnit &&
            b.buildingUnit->getType().getRace() == BWAPI::Races::Terran &&
            b.buildingUnit->exists() &&
            b.buildingUnit->canCancelConstruction())
        {
            the.micro.Cancel(b.buildingUnit);
        }
    }

    removeBuildings(toRemove);
}

// Remove buildings from the list of buildings--nothing more, nothing less.
void BuildingManager::removeBuildings(const std::vector< std::reference_wrapper<Building> > & toRemove)
{
    for (Building & b : toRemove)
    {
        auto it = std::find(_buildings.begin(), _buildings.end(), b);

        if (it != _buildings.end())
        {
            _buildings.erase(it);
        }
    }
}

// Buildings of this type are stalled and can't be built yet.
// They are protoss buildings that require pylon power, and can be built after
// a pylon finishes and provides powered space.
bool BuildingManager::typeIsStalled(BWAPI::UnitType type) const
{
    return _stalledForLackOfSpace && type.requiresPsi();
}