#pragma once

#include <BWAPI.h>
#include "BuildingData.h"

namespace UAlbertaBot
{
class Base;

class BuildingManager
{
    std::vector<Building> _buildings;

    BuildingManager();

    int  _reservedMinerals;				    // minerals reserved for planned buildings
    int  _reservedGas;					    // gas reserved for planned buildings
    bool _stalledForLackOfSpace;			// no valid location to place a protoss building

    bool isBuildingPositionExplored(const Building & b) const;
    void undoBuildings(const std::vector< std::reference_wrapper<Building> > &toRemove);
    void removeBuildings(const std::vector< std::reference_wrapper<Building> > & toRemove);

    void validateBuildings();					// STEP 1
    void assignWorkersToUnassignedBuildings();	// STEP 2
    void constructAssignedBuildings();			// STEP 3
    void checkForStartedConstruction();			// STEP 4
    void checkForDeadTerranBuilders();			// STEP 5
    void checkForCompletedBuildings();			// STEP 6
    void checkReservedResources();				// error check
    void clearAbandonedTerranBuildings();       // error workaround

    bool buildingTimedOut(const Building & b) const;

    bool validBuilder(BWAPI::Unit worker) const;
    void setBuilderUnit(Building & b);
    void releaseBuilderUnit(Building & b);
    
public:
    
    static BuildingManager &	Instance();

    void                update();
    Building &		    addTrackedBuildingTask(const MacroAct & act, BWAPI::TilePosition desiredLocation, BWAPI::Unit builder, bool isGasSteal);
    void                addBuildingTask(const MacroAct & act, BWAPI::TilePosition desiredLocation, BWAPI::Unit builder, bool isGasSteal);
    void                drawBuildingInformation(int x,int y);
    BWAPI::TilePosition getBuildingLocation(const Building & b);

    int                 getReservedMinerals() const;
    int                 getReservedGas() const;

    bool				getStalledForLackOfSpace() const { return _stalledForLackOfSpace; };
    void				unstall() { _stalledForLackOfSpace = false; };

    bool				anythingBeingBuilt() const { return !_buildings.empty(); };
    bool                isBeingBuilt(BWAPI::UnitType type) const;
    size_t              getNumUnstarted() const;
    size_t              getNumUnstarted(BWAPI::UnitType type) const;
    bool				isGasStealInQueue() const;
    bool                isBasePlanned(const Base * base) const;

    std::vector<BWAPI::UnitType> buildingsQueued();

    void                cancelBuilding(Building & b);
    void				cancelQueuedBuildings();
    void				cancelBuildingType(BWAPI::UnitType t);

    bool				typeIsStalled(BWAPI::UnitType type) const;
};

}