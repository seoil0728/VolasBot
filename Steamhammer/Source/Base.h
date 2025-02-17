#pragma once

#include "Common.h"
#include "GridDistances.h"

namespace UAlbertaBot
{
class Base
{
private:

    // Resources within this ground distance (in tiles) are considered to belong to this base.
    static const int BaseResourceRange = 14;

    int					id;					// ID number for drawing base info

    BWAPI::TilePosition	tilePosition;		// upper left corner of the resource depot spot
    BWAPI::Unitset		minerals;			// the base's mineral patches (some may be mined out)
    BWAPI::Unitset		initialGeysers;		// initial units of the geysers (taking one changes the unit)
    BWAPI::Unitset		geysers;			// the base's current associated geysers
    BWAPI::Unitset		blockers;			// destructible neutral units that may be in the way
    GridDistances		distances;			// ground distances from tilePosition
    bool				startingBase;		// is this one of the map's starting bases?
    Base *              naturalBase;        // if a starting base, the base's natural if any; else null
    Base *              mainBase;           // if the natural of a starting base, the corresponding main; else null
    BWAPI::TilePosition front;              // the front line: place approach defenses near here
    BWAPI::Position     mineralOffset;      // mean offset of minerals from the center of the depot

    bool				reserved;			// if this is a planned expansion
    bool                overlordDanger;     // for our own bases only; false for others
    bool				workerDanger;		// for our own bases only; false for others
    bool                doomed;             // for our own bases only; false for others
    int					failedPlacements;	// count building placements that failed

    bool                findIsStartingBase() const;	// to initialize the startingBase flag
    BWAPI::TilePosition findFront() const;
    BWAPI::Position     findMineralOffset() const;

public:

    // The resourceDepot pointer is set for a base if the depot has been seen.
    // It is possible to infer a base location without seeing the depot.
    BWAPI::Unit		resourceDepot;			// hatchery, etc., or null if none
    BWAPI::Player	owner;					// self, enemy, neutral

    Base(BWAPI::TilePosition pos, const BWAPI::Unitset & availableResources);
    void setID(int baseID);                 // called exactly once at startup

    void initializeNatural(const std::vector<Base *> & bases);
    void initializeFront();

    int				getID()           const { return id; };
    BWAPI::Unit		getDepot()        const { return resourceDepot; };
    BWAPI::Player	getOwner()        const { return owner; };
    bool            isAStartingBase() const { return startingBase; };
    bool            isIsland()        const;
    Base *          getNatural()      const { return naturalBase; };
    Base *          getMain()         const { return mainBase; };
    bool            isCompleted()     const;
    bool            isMyCompletedBase() const;
    bool            isInnerBase()     const;

    BWAPI::Position getFront()         const { return TileCenter(front); };
    BWAPI::TilePosition getFrontTile() const { return front; };

    void updateGeysers();

    const BWAPI::TilePosition & getTilePosition() const { return tilePosition; };
    BWAPI::Position getPosition() const { return BWAPI::Position(tilePosition); };
    BWAPI::TilePosition getCenterTile() const;
    BWAPI::Position getCenter() const;
    BWAPI::TilePosition getMineralLineTile() const;

    // Ground distances.
    const GridDistances & getDistances() const { return distances; };
    int getTileDistance(const BWAPI::Position & pos) const { return distances.at(pos); };
    int getTileDistance(const BWAPI::TilePosition & pos) const { return distances.at(pos); };
    int getDistance(const BWAPI::TilePosition & pos) const { return 32 * getTileDistance(pos); };
    int getDistance(const BWAPI::Position & pos) const { return 32 * getTileDistance(pos); };

    void setOwner(BWAPI::Unit depot, BWAPI::Player player);
    void setInferredEnemyBase();
    void placementFailed() { ++failedPlacements; };
    int  getFailedPlacements() const { return failedPlacements; };

    // The mineral patch units and geyser units. Blockers prevent the base from being used efficiently.
    const BWAPI::Unitset & getMinerals() const { return minerals; };
    const BWAPI::Unitset & getInitialGeysers() const { return initialGeysers; };
    const BWAPI::Unitset & getGeysers() const { return geysers; };
    const BWAPI::Unitset & getBlockers() const { return blockers; }

    // The latest reported total of resources available.
    int getLastKnownMinerals() const;
    int getLastKnownGas() const;

    // The total initial resources available.
    int getInitialMinerals() const;
    int getInitialGas() const;

    // Workers assigned to mine minerals or gas.
    int getMaxWorkers() const;
    int getNumWorkers() const;

    int getNumUnits(BWAPI::UnitType type) const;

    const BWAPI::Position & getMineralOffset() const { return mineralOffset; };

    BWAPI::Unit getPylon() const;

    bool isExplored() const;
    bool isVisible() const;

    void reserve() { reserved = true; };
    void unreserve() { reserved = false; };
    bool isReserved() const { return reserved; };

    // Outside tactical analysis decides this. See CombatCommander::updateBaseDefenseSquads().
    void setOverlordDanger(bool attack) { overlordDanger = attack; };
    bool inOverlordDanger() const { return overlordDanger; };
    void setWorkerDanger(bool attack) { workerDanger = attack; };
    bool inWorkerDanger() const { return workerDanger; };
    void setDoomed(bool bad) { doomed = bad; };
    bool isDoomed() const { return doomed; };

    void clearBlocker(BWAPI::Unit blocker);

    void drawBaseInfo() const;
};

}