#include "GameRecord.h"

#include "Bases.h"
#include "Logger.h"
#include "OpponentModel.h"
#include "The.h"

using namespace UAlbertaBot;

BWAPI::Race GameRecord::charRace(char ch)
{
    if (ch == 'Z')
    {
        return BWAPI::Races::Zerg;
    }
    if (ch == 'P')
    {
        return BWAPI::Races::Protoss;
    }
    if (ch == 'T')
    {
        return BWAPI::Races::Terran;
    }
    return BWAPI::Races::Unknown;
}

// Read a number that is on a line by itself.
// The number must be an integer >= 0.
int GameRecord::readNumber(std::istream & input)
{
    std::string line;
    int n = -1;

    if (std::getline(input, line))
    {
        n = readNumber(line);
    }

    if (n >= 0)
    {
        return n;
    }

    throw game_record_read_error();
}

// Read a number from a string.
int GameRecord::readNumber(std::string & s)
{
    std::istringstream lineStream(s);
    int n;
    if (lineStream >> n)
    {
        return n;
    }

    // BWAPI::Broodwar->printf("read bad number");
    throw game_record_read_error();
}

void GameRecord::parseMatchup(const std::string & s)
{
    if (s.length() == 3)        // "ZvT"
    {
        if (s[1] != 'v')
        {
            throw game_record_read_error();
        }
        ourRace = charRace(s[0]);
        enemyRace = charRace(s[2]);
        enemyIsRandom = false;
    }
    else if (s.length() == 4)   // "ZvRT"
    {
        if (s[1] != 'v' || s[2] != 'R')
        {
            throw game_record_read_error();
        }
        ourRace = charRace(s[0]);
        enemyRace = charRace(s[3]);
        enemyIsRandom = true;
    }
    else
    {
        throw game_record_read_error();
    }

    // Validity check. We should know our own race.
    if (ourRace == BWAPI::Races::Unknown)
    {
        throw game_record_read_error();
    }
}

OpeningPlan GameRecord::readOpeningPlan(std::istream & input)
{
    std::string line;

    if (std::getline(input, line))
    {
        return OpeningPlanFromString(line);
    }

    // BWAPI::Broodwar->printf("read bad opening plan");
    throw game_record_read_error();
}

// Return true if the snapshot is valid and we should continue reading, otherwise false or throw.
bool GameRecord::readPlayerSnapshot(std::istream & input, PlayerSnapshot & snap)
{
    std::string line;

    if (std::getline(input, line))
    {
        if (line == gameEndMark)
        {
            return false;
        }

        std::istringstream lineStream(line);
        int bases, id, n;
        
        if (lineStream >> bases)
        {
            snap.numBases = bases;
        }
        else
        {
            throw game_record_read_error();
        }
        while (lineStream >> id >> n)
        {
            snap.unitCounts[BWAPI::UnitType(id)] = n;
        }
        return true;
    }
    throw game_record_read_error();
}

// Allocate and return the next snapshot, or null if none.
GameSnapshot * GameRecord::readGameSnapshot(std::istream & input)
{
    int t;
    PlayerSnapshot me;
    PlayerSnapshot you;

    std::string line;

    if (std::getline(input, line))
    {
        if (line == gameEndMark)
        {
            return nullptr;
        }
        t = readNumber(line);
    }

    if (valid && readPlayerSnapshot(input, me) && valid && readPlayerSnapshot(input, you) && valid)
    {
        return new GameSnapshot(t, me, you);
    }

    return nullptr;
}

// Reading a game record, we hit an error before the end of the record.
// Mark it invalid and skip to the end of game mark so we don't break the rest of the records.
void GameRecord::skipToEnd(std::istream & input)
{
    std::string line;
    while (std::getline(input, line))
    {
        if (line == gameEndMark)
        {
            break;
        }
    }
}

// Read a 3.0 game record.

// 3.0
// matchup (e.g. ZvP, ZvRP)
// map
// our starting position (base ID number >= 1)
// enemy starting position (base ID or 0 if unknown)
// opening
// expected enemy opening plan
// actual enemy opening plan
// result (1 or 0)
// frame our first combat unit was complete
// frame we first gathered gas
// frame enemy first scouts our base
// frame enemy first gets combat units
// frame enemy was first observed to have used gas (not mined it)
// frame enemy first gets air units
// frame enemy first gets static anti-air
// frame enemy first gets mobile anti-air
// frame enemy first gets cloaked units
// frame enemy first gets static detection
// frame enemy first gets mobile detection
// game duration in frames, 0 if the game is not over yet
// + skill kit records
// END GAME
void GameRecord::read_v3_0(std::istream & input)
{
    std::string matchupStr;
    if (std::getline(input, matchupStr))
    {
        parseMatchup(matchupStr);
    }
    else
    {
        throw game_record_read_error();
    }
    if (!std::getline(input, mapName))     { throw game_record_read_error(); }
    myStartingBaseID = readNumber(input);
    enemyStartingBaseID = readNumber(input);
    if (!std::getline(input, openingName)) { throw game_record_read_error(); }
    expectedEnemyPlan = readOpeningPlan(input);
    enemyPlan = readOpeningPlan(input);
    win = readNumber(input) != 0;

    frameWeMadeFirstCombatUnit = readNumber(input);
    frameWeGatheredGas = readNumber(input);

    frameEnemyScoutsOurBase = readNumber(input);
    frameEnemyGetsCombatUnits = readNumber(input);
    frameEnemyUsesGas = readNumber(input);
    frameEnemyGetsAirUnits = readNumber(input);
    frameEnemyGetsStaticAntiAir = readNumber(input);
    frameEnemyGetsMobileAntiAir = readNumber(input);
    frameEnemyGetsCloakedUnits = readNumber(input);
    frameEnemyGetsStaticDetection = readNumber(input);
    frameEnemyGetsMobileDetection = readNumber(input);
    frameGameEnds = readNumber(input);

    std::string skillLine;
    while (getline(input, skillLine))
    {
        if (skillLine == gameEndMark)
        {
            break;
        }
        skillKitText.push_back(skillLine);
        the.skillkit.read(*this, skillLine);
    }
}

// Read a 1.4 game record (the origial format).

// 1.4
// matchup (e.g. ZvP, ZvRP)
// map
// opening
// expected enemy opening plan
// actual enemy opening plan
// result (1 or 0)
// frame we dispatched a scout to steal gas (0 if no attempt)
// gas steal happened (1 or 0)
// frame enemy first scouts our base
// frame enemy first gets combat units
// frame enemy first gets air units
// frame enemy first gets static anti-air
// frame enemy first gets mobile anti-air
// frame enemy first gets cloaked units
// frame enemy first gets static detection
// frame enemy first gets mobile detection
// game duration in frames (0 if the game is not over yet)
// snapshots
// END GAME
void GameRecord::read_v1_4(std::istream & input)
{
    std::string matchupStr;
    if (std::getline(input, matchupStr))
    {
        parseMatchup(matchupStr);
    }
    else
    {
        throw game_record_read_error();
    }

    if (!std::getline(input, mapName))     { throw game_record_read_error(); }
    if (!std::getline(input, openingName)) { throw game_record_read_error(); }
    expectedEnemyPlan = readOpeningPlan(input);
    enemyPlan = readOpeningPlan(input);
    win = readNumber(input) != 0;
    frameScoutSentForGasSteal = readNumber(input);
    gasStealHappened = readNumber(input) != 0;
    frameEnemyScoutsOurBase = readNumber(input);
    frameEnemyGetsCombatUnits = readNumber(input);
    frameEnemyGetsAirUnits = readNumber(input);
    frameEnemyGetsStaticAntiAir = readNumber(input);
    frameEnemyGetsMobileAntiAir = readNumber(input);
    frameEnemyGetsCloakedUnits = readNumber(input);
    frameEnemyGetsStaticDetection = readNumber(input);
    frameEnemyGetsMobileDetection = readNumber(input);
    frameGameEnds = readNumber(input);

    GameSnapshot * snap;
    while (snap = readGameSnapshot(input))
    {
        snapshots.push_back(snap);
    }
}

// Read the game record from the given stream.
// NOTE Reading is line-oriented. We read each line with getline() before parsing it.
// In case of error, we try to read ahead to the end-of-game mark so that the next record
// will be read correctly. But there is not much error checking.
void GameRecord::read(std::istream & input)
{
    try
    {
        if (!std::getline(input, recordFormat))
        {
            throw game_record_read_error();
        }

        if (recordFormat == "3.0")
        {
            read_v3_0(input);
        }
        else if (recordFormat == "1.4")
        {
            read_v1_4(input);
        }
        else
        {
            throw game_record_read_error();
        }
    }
    catch (const game_record_read_error &)
    {
        skipToEnd(input);      // end of the game record
        valid = false;
    }
}

void GameRecord::writePlayerSnapshot(std::ostream & output, const PlayerSnapshot & snap)
{
    output << snap.numBases;
    for (auto unitCount : snap.unitCounts)
    {
        output << ' ' << unitCount.first.getID() << ' ' << unitCount.second;
    }
    output << '\n';
}

void GameRecord::writeGameSnapshot(std::ostream & output, const GameSnapshot * snap)
{
    output << snap->frame << '\n';
    writePlayerSnapshot(output, snap->us);
    writePlayerSnapshot(output, snap->them);
}

// Write the recorded skill kit data for a past game.
// GameRecordNow overrides this to write data for the current game.
void GameRecord::writeSkills(std::ostream & output) const
{
    for (const std::string & line : skillKitText)
    {
        output << line << '\n';
    }
}

// Calculate a similarity distance between 2 snapshots.
// This version is a simple first try. Some unit types should matter more than others.
// 12 vs. 10 zerglings should count less than 2 vs. 0 lurkers.
// Buildings and mobile units are hard to compare. Probably should weight by cost in some way.
// Part of distance().
int GameRecord::snapDistance(const PlayerSnapshot & a, const PlayerSnapshot & b) const
{
    int distance = 0;

    // From a to b, count all differences.
    for (std::pair<BWAPI::UnitType, int> unitCountA : a.unitCounts)
    {
        auto unitCountBIt = b.unitCounts.find(unitCountA.first);
        if (unitCountBIt == b.unitCounts.end())
        {
            distance += unitCountA.second;
        }
        else
        {
            distance += abs(unitCountA.second - (*unitCountBIt).second);
        }
    }

    // From b to a, count differences where a is missing the type.
    for (std::pair<BWAPI::UnitType, int> unitCountB : b.unitCounts)
    {
        if (a.unitCounts.find(unitCountB.first) == a.unitCounts.end())
        {
            distance += unitCountB.second;
        }
    }

    return distance;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Constructor for the record of the current game.
// When this object is initialized, the opening and some other items are not yet known.
GameRecord::GameRecord()
    : valid(true)                  // never invalid, since it is recorded live
    , savedRecord(false)
    , ourRace(BWAPI::Broodwar->self()->getRace())
    , enemyRace(BWAPI::Broodwar->enemy()->getRace())
    , enemyIsRandom(BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Unknown)
    , mapName(BWAPI::Broodwar->mapFileName())
    , myStartingBaseID(the.bases.myStart()->getID())
    , enemyStartingBaseID(the.bases.enemyStart() ? the.bases.enemyStart()->getID() : 0)
    , expectedEnemyPlan(OpeningPlan::Unknown)
    , enemyPlan(OpeningPlan::Unknown)
    , win(false)                   // until proven otherwise
    , frameScoutSentForGasSteal(0)
    , gasStealHappened(false)
    , frameWeMadeFirstCombatUnit(0)
    , frameWeGatheredGas(0)
    , frameEnemyScoutsOurBase(0)
    , frameEnemyGetsCombatUnits(0)
    , frameEnemyUsesGas(0)
    , frameEnemyGetsAirUnits(0)
    , frameEnemyGetsStaticAntiAir(0)
    , frameEnemyGetsMobileAntiAir(0)
    , frameEnemyGetsCloakedUnits(0)
    , frameEnemyGetsStaticDetection(0)
    , frameEnemyGetsMobileDetection(0)
    , frameGameEnds(0)
{
}

// Constructor for the record of a past game.
GameRecord::GameRecord(std::istream & input)
    : valid(true)                  // until proven otherwise
    , savedRecord(true)
    , ourRace(BWAPI::Races::Unknown)
    , enemyRace(BWAPI::Races::Unknown)
    , enemyIsRandom(false)
    , myStartingBaseID(0)
    , enemyStartingBaseID(0)
    , expectedEnemyPlan(OpeningPlan::Unknown)
    , enemyPlan(OpeningPlan::Unknown)
    , win(false)                   // until proven otherwise
    , frameScoutSentForGasSteal(0)
    , gasStealHappened(false)
    , frameWeMadeFirstCombatUnit(0)
    , frameWeGatheredGas(0)
    , frameEnemyScoutsOurBase(0)
    , frameEnemyGetsCombatUnits(0)
    , frameEnemyUsesGas(0)
    , frameEnemyGetsAirUnits(0)
    , frameEnemyGetsStaticAntiAir(0)
    , frameEnemyGetsMobileAntiAir(0)
    , frameEnemyGetsCloakedUnits(0)
    , frameEnemyGetsStaticDetection(0)
    , frameEnemyGetsMobileDetection(0)
    , frameGameEnds(0)
{
    read(input);
}

// Write the game record to the given stream. File format:
void GameRecord::write(std::ostream & output)
{
    // We only now notice that there was an expected enemy opening plan.
    // Can't initialize this right off, and there is no point in tracking it during the game.
    if (!savedRecord)
    {
        expectedEnemyPlan = OpponentModel::Instance().getInitialExpectedEnemyPlan();
    }

    output << "Our Race : " << RaceChar(ourRace) << '\n';
    output << "Enemy Race : " << (enemyIsRandom ? "Random " : "") << RaceChar(enemyRace) << '\n';
    output << "Map Name : " << mapName << '\n';
    output << "Opening : " << openingName << '\n';
    if (win)
    {
        output << "Result : Win" << '\n';
    }
    else
    {
        output << "Result : Lose" << '\n';
    }

    // TODO skip the snapshots for v3.0
    // for (const auto & snap : snapshots)
    // {
    // 	writeGameSnapshot(output, snap);
    // }

    writeSkills(output);

    output << gameEndMark << '\n';
}

// Calculate a similarity distance between two game records; -1 if they cannot be compared.
// The more similar they are, the less the distance.
int GameRecord::distance(const GameRecord & record) const
{
    // Return -1 if the records are for different matchups.
    if (ourRace != record.ourRace || enemyRace != record.enemyRace)
    {
        return -1;
    }

    // Also return -1 for any record which has no snapshots. It conveys no info.
    if (record.snapshots.size() == 0)
    {
        return -1;
    }

    int distance = 0;

    if (mapName != record.mapName)
    {
        distance += 20;
    }

    if (openingName != record.openingName)
    {
        distance += 200;
    }

    // Differences in enemy play count 5 times more than differences in our play.
    auto here = snapshots.begin();
    auto there = record.snapshots.begin();
    int latest = 0;
    while (here != snapshots.end() && there != record.snapshots.end())     // until one record runs out
    {
        distance +=     snapDistance((*here)->us,   (*there)->us);
        distance += 5 * snapDistance((*here)->them, (*there)->them);
        latest = (*there)->frame;

        ++here;
        ++there;
    }

    // If the 'there' record ends too early, the comparison is no good after all.
    // The game we're trying to compare to ended before this game and has no information for us.
    if (BWAPI::Broodwar->getFrameCount() - latest > snapshotInterval)
    {
        return -1;
    }

    return distance;
}

// Find the enemy snapshot closest in time to time t.
// The caller promises that there is one, but we check anyway.
bool GameRecord::findClosestSnapshot(int t, PlayerSnapshot & snap) const
{
    for (const auto & ourSnap : snapshots)
    {
        if (abs(ourSnap->frame - t) < snapshotInterval)
        {
            snap = ourSnap->them;
            return true;
        }
    }
    UAB_ASSERT(false, "opponent model - no snapshot @ t");
    return false;
}

// The game records have the same matchup, as best we can tell so far.
// For checks at the start of the game, when the enemy's race may be unknown, allow
// a special case for random enemies.
bool GameRecord::sameMatchup(const GameRecord & record) const
{
    return ourRace == record.ourRace &&
        (enemyRace == record.enemyRace ||
        enemyIsRandom && record.enemyIsRandom &&
        (enemyRace == BWAPI::Races::Unknown || record.enemyRace == BWAPI::Races::Unknown)
        );
}

const std::vector<int> * GameRecord::getSkillInfo(Skill * skill, int i) const
{
    auto it1 = skillData.find(skill);
    if (it1 != skillData.end())
    {
        auto it2 = (*it1).second.find(i);
        if (it2 != (*it1).second.end())
        {
            return &(*it2).second;
        }
    }

    return nullptr;
}

void GameRecord::setSkillInfo(Skill * skill, int i, const std::vector<int> & info)
{
    skillData[skill][i] = info;
}

void GameRecord::debugLog()
{
    BWAPI::Broodwar->printf("best %s %s", mapName, openingName);

    std::stringstream msg;

    msg << "best match, t = " << BWAPI::Broodwar->getFrameCount() << '\n'
        << mapName << ' ' << openingName << ' ' << (win ? "win" : "loss") << '\n'
        << "scout " << frameEnemyScoutsOurBase << '\n'
        << "combat " << frameEnemyGetsCombatUnits << '\n'
        << "air " << frameEnemyGetsAirUnits << '\n'
        << "turrets " << frameEnemyGetsStaticAntiAir << '\n'
        << "marines " << frameEnemyGetsMobileAntiAir << '\n'
        << "wraiths " << frameEnemyGetsCloakedUnits << '\n'
        << "turrets " << frameEnemyGetsStaticDetection << '\n'
        << "vessels " << frameEnemyGetsMobileDetection << '\n'
        << "end of game " << frameGameEnds << '\n';

    for (auto snap : snapshots)
    {
        msg << snap->frame << '\n'
            << snap->us.debugString()
            << snap->them.debugString();
    }
    msg  << '\n';

    Logger::LogAppendToFile(Config::IO::ErrorLogFilename, msg.str());
}
