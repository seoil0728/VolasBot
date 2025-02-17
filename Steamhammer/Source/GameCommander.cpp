#include "GameCommander.h"

#include "Bases.h"
#include "BuildingPlacer.h"
#include "MapTools.h"
#include "OpponentModel.h"
#include "UnitUtil.h"

#include "BuildingManager.h"
#include "InformationManager.h"
#include "MapGrid.h"
#include "OpeningTiming.h"
#include "OpponentModel.h"
#include "ProductionManager.h"
#include "ScoutManager.h"
#include "StrategyManager.h"
#include "WorkerManager.h"

using namespace UAlbertaBot;

GameCommander::GameCommander() 
    : _combatCommander(CombatCommander::Instance())
    , _initialScoutTime(0)
    , _surrenderTime(0)
    , _myHighWaterSupply(0)
{
}

void GameCommander::update()
{
    _timerManager.startTimer(TimerManager::Total);

    // populate the unit vectors we will pass into various managers
    handleUnitAssignments();

    // Decide whether to give up early. Implements config option SurrenderWhenHopeIsLost.
    if (!_surrenderTime && surrenderMonkey())
    {
        _surrenderTime = the.now();
        GameMessage("gg");
    }
    if (_surrenderTime)
    {
        if (the.now() - _surrenderTime >= 36)  // 36 frames = 1.5 game seconds
        {
            BWAPI::Broodwar->leaveGame();
        }
        _timerManager.stopTimer(TimerManager::Total);
        return;
    }

    // -- Managers that gather inforation. --

    _timerManager.startTimer(TimerManager::InformationManager);
    Bases::Instance().update();
    InformationManager::Instance().update();
    _timerManager.stopTimer(TimerManager::InformationManager);

    _timerManager.startTimer(TimerManager::MapGrid);
    the.update();
    MapGrid::Instance().update();
    _timerManager.stopTimer(TimerManager::MapGrid);

    _timerManager.startTimer(TimerManager::OpponentModel);
    OpponentModel::Instance().update();
    the.skillkit.update();
    _timerManager.stopTimer(TimerManager::OpponentModel);

    // -- Managers that act on information. --

    _timerManager.startTimer(TimerManager::Search);
    BOSSManager::Instance().update(35 - _timerManager.getMilliseconds());
    _timerManager.stopTimer(TimerManager::Search);

    // May steal workers from WorkerManager, so run it before WorkerManager.
    _timerManager.startTimer(TimerManager::Production);
    ProductionManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Production);

    // May steal workers from WorkerManager, so run it before WorkerManager.
    _timerManager.startTimer(TimerManager::Building);
    BuildingManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Building);

    _timerManager.startTimer(TimerManager::Worker);
    WorkerManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Worker);

    _timerManager.startTimer(TimerManager::Combat);
    _combatCommander.update(_combatUnits);
    _timerManager.stopTimer(TimerManager::Combat);

    _timerManager.startTimer(TimerManager::Scout);
    ScoutManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Scout);

    // Execute micro commands gathered above. Do this at the end of the frame.
    _timerManager.startTimer(TimerManager::Micro);
    the.micro.update();
    _timerManager.stopTimer(TimerManager::Micro);

    _timerManager.stopTimer(TimerManager::Total);

    drawDebugInterface();
}

void GameCommander::onEnd(bool isWinner)
{
    OpponentModel::Instance().setWin(isWinner);
    OpponentModel::Instance().write();

    // Clean up any data structures that may otherwise not be unwound in the correct order.
    // This fixes an end-of-game bug diagnosed by Bruce Nielsen.
    _combatCommander.onEnd();
}

void GameCommander::drawDebugInterface()
{
    InformationManager::Instance().drawExtendedInterface();
    InformationManager::Instance().drawUnitInformation(425,30);
    drawUnitCounts(345, 30);
    Bases::Instance().drawBaseInfo();
    Bases::Instance().drawBaseOwnership(575, 30);
    the.map.drawExpoScores();
    InformationManager::Instance().drawResourceAmounts();
    BuildingManager::Instance().drawBuildingInformation(200, 50);
    the.placer.drawReservedTiles();
    ProductionManager::Instance().drawProductionInformation(30, 60);
    BOSSManager::Instance().drawSearchInformation(490, 100);
    the.map.drawHomeDistances();
    drawTerrainHeights();
    drawDefenseClusters();
    
    _combatCommander.drawSquadInformation(170, 70);
    _timerManager.drawModuleTimers(490, 215);
    drawGameInformation(4, 1);

    drawUnitOrders();
    the.skillkit.draw();
}

void GameCommander::drawGameInformation(int x, int y)
{
    if (!Config::Debug::DrawGameInfo)
    {
        return;
    }

    const OpponentModel::OpponentSummary & summary = OpponentModel::Instance().getSummary();
    BWAPI::Broodwar->drawTextScreen(x, y, "%c%s %c%d-%d %c%s",
        BWAPI::Broodwar->self()->getTextColor(), BWAPI::Broodwar->self()->getName().c_str(),
        white, summary.totalWins, summary.totalGames - summary.totalWins,
        BWAPI::Broodwar->enemy()->getTextColor(), BWAPI::Broodwar->enemy()->getName().c_str());
    y += 12;
    
    const std::string & openingGroup = StrategyManager::Instance().getOpeningGroup();
    const auto openingInfoIt = summary.openingInfo.find(Config::Strategy::StrategyName);
    const int wins = openingInfoIt == summary.openingInfo.end() ? 0 : openingInfoIt->second.sameWins + openingInfoIt->second.otherWins;
    const int games = openingInfoIt == summary.openingInfo.end() ? 0 : openingInfoIt->second.sameGames + openingInfoIt->second.otherGames;
    bool gasSteal = ScoutManager::Instance().wantGasSteal();
    BWAPI::Broodwar->drawTextScreen(x, y, "\x03%s%s%s%s %c%d-%d",
        Config::Strategy::StrategyName.c_str(),
        openingGroup != "" ? (" (" + openingGroup + ")").c_str() : "",
        gasSteal ? " + steal gas" : "",
        Config::Strategy::FoundEnemySpecificStrategy ? " - enemy specific" : "",
        white, wins, games - wins);
    BWAPI::Broodwar->setTextSize();
    y += 12;

    std::string expect;
    std::string enemyPlanString;
    if (OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Unknown &&
        OpponentModel::Instance().getExpectedEnemyPlan() != OpeningPlan::Unknown)
    {
        if (OpponentModel::Instance().isEnemySingleStrategy())
        {
            expect = "surely ";
        }
        else
        {
            expect = "expect ";
        }
        enemyPlanString = OpponentModel::Instance().getExpectedEnemyPlanString();
    }
    else
    {
        enemyPlanString = OpponentModel::Instance().getEnemyPlanString();
    }
    BWAPI::Broodwar->drawTextScreen(x, y, "%cOpp Plan %c%s%c%s", white, orange, expect.c_str(), yellow, enemyPlanString.c_str());
    y += 12;

    std::string island = "";
    if (Bases::Instance().isIslandStart())
    {
        island = " (island)";
    }
    BWAPI::Broodwar->drawTextScreen(x, y, "%c%s%c%s", yellow, BWAPI::Broodwar->mapFileName().c_str(), orange, island.c_str());
    BWAPI::Broodwar->setTextSize();
    y += 12;

    int frame = BWAPI::Broodwar->getFrameCount();
    BWAPI::Broodwar->drawTextScreen(x, y, "\x04%d %2u:%02u mean %.1fms max %.1fms",
        frame,
        int(frame / (23.8 * 60)),
        int(frame / 23.8) % 60,
        _timerManager.getMeanMilliseconds(),
        _timerManager.getMaxMilliseconds());

    /*
    // latency display
    y += 12;
    BWAPI::Broodwar->drawTextScreen(x + 50, y, "\x04%d max %d",
        BWAPI::Broodwar->getRemainingLatencyFrames(),
        BWAPI::Broodwar->getLatencyFrames());
    */
}

void GameCommander::drawUnitOrders()
{
    if (!Config::Debug::DrawUnitOrders)
    {
        return;
    }

    for (BWAPI::Unit unit : BWAPI::Broodwar->getAllUnits())
    {
        if (!unit->getPosition().isValid())
        {
            continue;
        }

        std::string extra = "";
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Egg ||
            unit->getType() == BWAPI::UnitTypes::Zerg_Cocoon ||
            unit->getType().isBuilding() && !unit->isCompleted())
        {
            extra = UnitTypeName(unit->getBuildType());
        }
        else if (unit->isTraining() && !unit->getTrainingQueue().empty())
        {
            extra = UnitTypeName(unit->getTrainingQueue()[0]);
        }
        else if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            extra = UnitTypeName(unit);
        }
        else if (unit->isResearching())
        {
            extra = unit->getTech().getName();
        }
        else if (unit->isUpgrading())
        {
            extra = unit->getUpgrade().getName();
        }

        int x = unit->getPosition().x - 8;
        int y = unit->getPosition().y - 2;
        if (extra != "")
        {
            BWAPI::Broodwar->drawTextMap(x, y, "%c%s", yellow, extra.c_str());
        }
        if (unit->getOrder() != BWAPI::Orders::Nothing)
        {
            BWAPI::Broodwar->drawTextMap(x, y + 10, "%c%d %c%s", white, unit->getID(), cyan, unit->getOrder().getName().c_str());
        }
    }
}

void GameCommander::drawUnitCounts(int x, int y) const
{
    if (!Config::Debug::DrawUnitCounts)
    {
        return;
    }

    const int c1 = 17;
    const int c2 = 38;
    const int e0 = 160;
    int dy = 0;
    for (BWAPI::UnitType t : BWAPI::UnitTypes::allUnitTypes())
    {
        if (the.my.all.count(t) > 0)
        {
            BWAPI::Broodwar->drawTextScreen    (x     , y + dy, "%c%3d" , white , the.my.completed.count(t));
            if (the.my.all.count(t) - the.my.completed.count(t) > 0)
            {
                BWAPI::Broodwar->drawTextScreen(x + c1, y + dy, "%c%+2d", yellow, the.my.all.count(t) - the.my.completed.count(t));
            }
            BWAPI::Broodwar->drawTextScreen    (x + c2, y + dy, "%c%s" , green , UnitTypeName(t).c_str());
            dy += 12;
        }
    }

    dy = 0;
    for (BWAPI::UnitType t : BWAPI::UnitTypes::allUnitTypes())
    {
        if (the.your.seen.count(t) + the.your.inferred.count(t) > 0)
        {
            char color = red;
            int n = the.your.inferred.count(t);
            if (the.your.seen.count(t))
            {
                color = white;
                n = the.your.seen.count(t);
            }

            BWAPI::Broodwar->drawTextScreen    (x + e0          , y + dy, "%c%3d", color , n);
            BWAPI::Broodwar->drawTextScreen    (x + e0 + c2 - 13, y + dy, "%c%s" , orange, UnitTypeName(t).c_str());
            dy += 12;
        }
    }
}

void GameCommander::drawTerrainHeights() const
{
    if (!Config::Debug::DrawTerrainHeights)
    {
        return;
    }

    for (int x = 0; x < BWAPI::Broodwar->mapWidth(); ++x)
    {
        for (int y = 0; y < BWAPI::Broodwar->mapHeight(); ++y)
        {
            int h = BWAPI::Broodwar->getGroundHeight(x, y);
            char color = h % 2 ? purple : gray;

            BWAPI::Position pos(BWAPI::TilePosition(x, y));
            BWAPI::Broodwar->drawTextMap(pos + BWAPI::Position(12, 12), "%c%d", color, h);
        }
    }
}

void GameCommander::drawDefenseClusters()
{
    if (!Config::Debug::DrawDefenseClusters)
    {
        return;
    }

    const std::vector<UnitCluster> & groundClusters = the.ops.getGroundDefenseClusters();

    for (const UnitCluster & cluster : groundClusters)
    {
        cluster.draw(BWAPI::Colors::Brown, "vs ground");
    }

    const std::vector<UnitCluster> & airClusters = the.ops.getAirDefenseClusters();

    for (const UnitCluster & cluster : airClusters)
    {
        cluster.draw(BWAPI::Colors::Grey, "vs air");
    }
}

// assigns units to various managers
void GameCommander::handleUnitAssignments()
{
    _validUnits.clear();
    _combatUnits.clear();
    // Don't clear the scout units.

    // Only keep units which are completed and usable.
    setValidUnits();

    // set each type of unit
    setScoutUnits();
    setCombatUnits();
}

bool GameCommander::isAssigned(BWAPI::Unit unit) const
{
    return _combatUnits.contains(unit) || _scoutUnits.contains(unit);
}

// validates units as usable for distribution to various managers
void GameCommander::setValidUnits()
{
    /*
    // TODO testing
    std::string timingFile = Config::IO::WriteDir + "timing.csv";
    static bool speed = false;
    if (!speed && BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Metabolic_Boost) > 0)
    {
        BWAPI::Broodwar->printf("zergling speed %d", BWAPI::Broodwar->getFrameCount());
        Logger::LogAppendToFile(timingFile, "%d,%s\n", BWAPI::Broodwar->getFrameCount(), "speed");
        speed = true;
    }
    static BWAPI::Unitset lings;
    size_t nLings = lings.size();
    */

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (UnitUtil::IsValidUnit(unit))
        {	
            _validUnits.insert(unit);

            /*
            // TODO testing
            static bool firstTime1 = false;
            if (unit->getType() == BWAPI::UnitTypes::Zerg_Lair && !firstTime1)
            {
                BWAPI::Broodwar->printf("lair timing %d", BWAPI::Broodwar->getFrameCount());
                firstTime1 = true;
            }
            // TODO testing
            //if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
            //{
            //    lings.insert(unit);
            //}
            */

            /*
            // TODO testing
            static bool firstTime1 = false;
            static BWAPI::Unitset reported;
            if (unit->getType() == BWAPI::UnitTypes::Protoss_Gateway && !reported.contains(unit))
            {
                BWAPI::Broodwar->printf("unit timing %d", BWAPI::Broodwar->getFrameCount());
                firstTime1 = true;
                reported.insert(unit);
            }
            */
        }
        else
        {
            /*
            static bool firstTime2 = false;
            if (!firstTime2 && unit->getType() == BWAPI::UnitTypes::Zerg_Hatchery && !unit->isCompleted())
            {
                BWAPI::Broodwar->printf("hatchery timing %d", BWAPI::Broodwar->getFrameCount());
                firstTime2 = true;
            }
            */
        }
    }

    /*
    // TODO testing
    if (lings.size() > nLings)
    {
        BWAPI::Broodwar->printf("%d lings @ %d", lings.size(), BWAPI::Broodwar->getFrameCount());
        Logger::LogAppendToFile(timingFile, "%d,%d,%d\n", BWAPI::Broodwar->getFrameCount(), lings.size(), BWAPI::Broodwar->self()->minerals());
    }
    */
}

void GameCommander::setScoutUnits()
{
    // If we're zerg, assign the first overlord to scout.
    if (BWAPI::Broodwar->getFrameCount() == 0 &&
        BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Zerg)
    {
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit->getType() == BWAPI::UnitTypes::Zerg_Overlord)
            {
                ScoutManager::Instance().setOverlordScout(unit);
                assignUnit(unit, _scoutUnits);
                break;
            }
        }
    }

    // Send a scout worker if we haven't yet and should.
    if (!_initialScoutTime)
    {
        if (ScoutManager::Instance().shouldScout() && !Bases::Instance().isIslandStart())
        {
            BWAPI::Unit workerScout = getAnyFreeWorker();

            // If we find a worker, make it the scout unit.
            if (workerScout)
            {
                ScoutManager::Instance().setWorkerScout(workerScout);
                assignUnit(workerScout, _scoutUnits);
                _initialScoutTime = the.now();
            }
        }
    }
}

// Set combat units to be passed to CombatCommander.
void GameCommander::setCombatUnits()
{
    for (BWAPI::Unit unit : _validUnits)
    {
        if (!isAssigned(unit) && (UnitUtil::IsCombatUnit(unit) || unit->getType().isWorker()))		
        {	
            assignUnit(unit, _combatUnits);
        }
    }
}

// Release the zerg scouting overlord for other duty.
void GameCommander::releaseOverlord(BWAPI::Unit overlord)
{
    _scoutUnits.erase(overlord);
}

void GameCommander::onUnitShow(BWAPI::Unit unit)			
{ 
    InformationManager::Instance().onUnitShow(unit); 
    WorkerManager::Instance().onUnitShow(unit);
}

void GameCommander::onUnitHide(BWAPI::Unit unit)			
{ 
    InformationManager::Instance().onUnitHide(unit); 
}

void GameCommander::onUnitCreate(BWAPI::Unit unit)		
{ 
    InformationManager::Instance().onUnitCreate(unit); 
}

void GameCommander::onUnitComplete(BWAPI::Unit unit)
{
    InformationManager::Instance().onUnitComplete(unit);
}

void GameCommander::onUnitRenegade(BWAPI::Unit unit)		
{ 
    InformationManager::Instance().onUnitRenegade(unit); 
}

void GameCommander::onUnitDestroy(BWAPI::Unit unit)		
{ 	
    ProductionManager::Instance().onUnitDestroy(unit);
    WorkerManager::Instance().onUnitDestroy(unit);
    InformationManager::Instance().onUnitDestroy(unit); 
}

void GameCommander::onUnitMorph(BWAPI::Unit unit)		
{ 
    InformationManager::Instance().onUnitMorph(unit);
    WorkerManager::Instance().onUnitMorph(unit);
}

// Used only to choose a worker to scout.
BWAPI::Unit GameCommander::getAnyFreeWorker()
{
    for (BWAPI::Unit unit : _validUnits)
    {
        if (unit->getType().isWorker() &&
            !isAssigned(unit) &&
            WorkerManager::Instance().isFree(unit) &&
            !unit->isCarryingMinerals() &&
            !unit->isCarryingGas() &&
            unit->getOrder() != BWAPI::Orders::MiningMinerals)
        {
            return unit;
        }
    }

    return nullptr;
}

void GameCommander::assignUnit(BWAPI::Unit unit, BWAPI::Unitset & set)
{
    if (_scoutUnits.contains(unit)) { _scoutUnits.erase(unit); }
    else if (_combatUnits.contains(unit)) { _combatUnits.erase(unit); }

    set.insert(unit);
}

// Decide whether to give up early. See config option SurrenderWhenHopeIsLost.
// This depends on _validUnits, so call it after handleUnitAssignments().
// Giving up early is important in testing, to save time. In a serious tournament,
// it's a question of taste.
bool GameCommander::surrenderMonkey()
{
    if (!Config::Skills::SurrenderWhenHopeIsLost)
    {
        return false;
    }

    // Only check once every five seconds. No hurry to give up.
    if (BWAPI::Broodwar->getFrameCount() % (5 * 24) != 0)
    {
        return false;
    }

    // Don't surrender right at the start.
    if (BWAPI::Broodwar->getFrameCount() < 24 * 60)
    {
        return false;
    }

    // We are playing against a human. Surrender earlier to reduce frustration.
    if (Config::Skills::HumanOpponent)
    {
        int mySupply = BWAPI::Broodwar->self()->supplyUsed();
        PlayerSnapshot enemySnap(BWAPI::Broodwar->enemy());
        int knownEnemySupply = enemySnap.getSupply();

        // BWAPI::Broodwar->printf("supply %d < %d vs enemy %d", mySupply, _myHighWaterSupply, knownEnemySupply);

        if (mySupply > _myHighWaterSupply)
        {
            _myHighWaterSupply = mySupply;
            return false;
        }

        // Surrender if the enemy is way ahead AND we have been hurt.
        // We don't check that we were RECENTLY hurt.
        return mySupply < knownEnemySupply / 2 && mySupply < _myHighWaterSupply / 2;
    }

    // We assume we are playing against another bot.
    // The only reason to gg is to save time, so be conservative.
    // Surrender if all conditions are met:
    // 1. We don't have the cash to make a worker.
    // 2. We have no completed unit that can attack. (We may have incomplete units.)
    // 3. The enemy has at least one visible unit that can destroy buildings.
    // Terran does not float buildings, so we check whether the enemy can attack ground.

    // 1. Our cash.
    if (BWAPI::Broodwar->self()->minerals() >= 50)
    {
        return false;
    }

    // 2. Our units.
    for (BWAPI::Unit unit : _validUnits)
    {
        if (unit->canAttack())
        {
            return false;
        }
    }

    // 3. Enemy units.
    bool safe = true;
    for (BWAPI::Unit unit : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (unit->isVisible() && UnitUtil::CanAttackGround(unit))
        {
            safe = false;
            break;
        }
    }
    if (safe)
    {
        return false;
    }

    // Surrender monkey says surrender!
    return true;
}

GameCommander & GameCommander::Instance()
{
    static GameCommander instance;
    return instance;
}
