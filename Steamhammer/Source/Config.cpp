#include "Config.h"
#include "UABAssert.h"

// Most values here are default values that apply if the configuration entry
// is missing from the config file, or is invalid.

// The ConfigFile record tells where to find the config file, so it's different.

namespace Config
{
    namespace ConfigFile
    {
        bool ConfigFileFound                = false;
        bool ConfigFileParsed               = false;
        std::string ConfigFileLocation      = "bwapi-data/AI/VolasBot.json";
    }

    namespace IO
    {
        std::string ErrorLogFilename		= "VolasBot_ErrorLog.txt";
        bool LogAssertToErrorFile			= false;

        std::string StaticDir               = "bwapi-data/AI/";
        std::string PreparedDataDir         = "bwapi-data/AI/om/";
        std::string ReadDir                 = "bwapi-data/read/";
        std::string WriteDir				= "bwapi-data/write/";
        std::string OpeningTimingFile       = "timings.txt";
        int MaxGameRecords					= 0;
        bool ReadOpponentModel				= false;
        bool WriteOpponentModel				= false;
    }

    namespace Skills
    {
        bool UnderSCHNAIL                   = false;
        bool SCHNAILMeansHuman              = true;
        bool HumanOpponent                  = false;
        bool SurrenderWhenHopeIsLost        = true;

        bool ScoutHarassEnemy               = false;
        bool GasSteal                       = true;

        bool Burrow                         = true;
        bool UseSunkenRangeBug              = true;

        int MaxQueens                       = 0;
        int MaxInfestedTerrans              = 0;
        int MaxDefilers                     = 0;
    }

    namespace Strategy
    {
        bool Crazyhammer                    = false;
        std::string ProtossStrategyName     = "1ZealotCore";			// default
        std::string TerranStrategyName      = "11Rax";					// default
        std::string ZergStrategyName        = "9PoolSpeed";				// default
        std::string StrategyName            = "9PoolSpeed";
        bool UsePlanRecognizer				= true;
        bool UseEnemySpecificStrategy       = true;
        bool FoundEnemySpecificStrategy     = false;
    }

    namespace BotInfo
    {
        std::string BotName                 = "VolasBot";
        std::string Authors                 = "SeoilGood";
        bool PrintInfoOnStart               = true;
    }

    namespace BWAPIOptions
    {
        int SetLocalSpeed                   = 42;
        int SetFrameSkip                    = 0;
        bool EnableUserInput                = true;
        bool EnableCompleteMapInformation   = false;
    }

    namespace Tournament
    {
        int GameEndFrame                    = 86400;
    }

    namespace Debug
    {
        bool DrawGameInfo                   = true;
        bool DrawUnitHealthBars             = false;
        bool DrawProductionInfo             = true;
        bool DrawBuildOrderSearchInfo       = false;
        bool DrawQueueFixInfo				= false;
        bool DrawScoutInfo                  = false;
        bool DrawWorkerInfo                 = false;
        bool DrawModuleTimers               = false;
        bool DrawReservedBuildingTiles      = false;
        bool DrawCombatSimulationInfo       = false;
        bool DrawBuildingInfo               = false;
        bool DrawStaticDefensePlan          = false;
        bool DrawEnemyUnitInfo              = false;
        bool DrawUnitCounts                 = false;
        bool DrawHiddenEnemies				= false;
        bool DrawMapInfo					= false;
        bool DrawMapGrid                    = false;
        bool DrawMapDistances				= false;
        bool DrawTerrainHeights             = false;
        bool DrawBaseInfo					= false;
        bool DrawExpoScores					= false;
        bool DrawStrategyBossInfo			= false;
        bool DrawUnitTargets				= false;
        bool DrawUnitOrders					= false;
        bool DrawMicroState					= false;
        bool DrawLurkerTactics              = false;
        bool DrawSquadInfo                  = false;
        bool DrawClusters					= false;
        bool DrawDefenseClusters			= false;
        bool DrawResourceAmounts            = false;

        BWAPI::Color ColorLineTarget        = BWAPI::Colors::White;
        BWAPI::Color ColorLineMineral       = BWAPI::Colors::Cyan;
        BWAPI::Color ColorUnitNearEnemy     = BWAPI::Colors::Red;
        BWAPI::Color ColorUnitNotNearEnemy  = BWAPI::Colors::Green;
    }

    namespace Micro
    {
        bool KiteWithRangedUnits            = true;
        bool WorkersDefendRush              = false;
        int RetreatMeleeUnitShields         = 0;
        int RetreatMeleeUnitHP              = 0;
        int CombatSimRadius					= 300;      // radius of units around frontmost unit for combat sim
        int ScoutDefenseRadius				= 600;		// radius to chase enemy scout worker
    }

    namespace Macro
    {
        int BOSSFrameLimit                  = 160;
        int ProductionJamFrameLimit			= 360;
        int WorkersPerRefinery              = 3;
        double WorkersPerPatch              = 3.0;
        int AbsoluteMaxWorkers				= 75;
        int BuildingSpacing                 = 1;
        int PylonSpacing                    = 3;
        bool ExpandToIslands				= false;
    }

    namespace Tools
    {
        extern int MAP_GRID_SIZE            = 320;      // size of grid spacing in MapGrid
    }
}
