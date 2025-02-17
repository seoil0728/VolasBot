/* 
 *----------------------------------------------------------------------
 * Steamhammer entry point.
 *----------------------------------------------------------------------
 */

#include "UAlbertaBotModule.h"

#include "../../BOSS/source/BOSS.h"
#include "GameCommander.h"
#include "OpeningTiming.h"
#include "ParseUtils.h"

using namespace UAlbertaBot;

UAlbertaBotModule::UAlbertaBotModule()
{
}

// BWAPI calls this when the bot starts.
void UAlbertaBotModule::onStart()
{
    // Initialize BOSS, the Build Order Search System
    BOSS::init();

    the.initialize();

    // Set our BWAPI options according to the configuration. 
    BWAPI::Broodwar->setLocalSpeed(Config::BWAPIOptions::SetLocalSpeed);
    BWAPI::Broodwar->setFrameSkip(Config::BWAPIOptions::SetFrameSkip);
    
    if (Config::BWAPIOptions::EnableCompleteMapInformation)
    {
        BWAPI::Broodwar->enableFlag(BWAPI::Flag::CompleteMapInformation);
    }

    if (Config::BWAPIOptions::EnableUserInput)
    {
        BWAPI::Broodwar->enableFlag(BWAPI::Flag::UserInput);
    }

    StrategyManager::Instance().setOpeningGroup();    // may depend on config and/or opponent model
    
    if (Config::BotInfo::PrintInfoOnStart)
    {
        BWAPI::Broodwar->printf("%s by %s, based on Steamhammer.", Config::BotInfo::BotName.c_str(), Config::BotInfo::Authors.c_str());
        if (Config::Skills::HumanOpponent)
        {
            GameMessage("gl hf");
        }
    }
    BWAPI::Broodwar->sendText("VolasBot is based on Steamhammer.");
    BWAPI::Broodwar->sendText("special thanks to Jay Scott and Dave Churchill.");
    // Turn off latency compensation, which is on by default.
    // BWAPI::Broodwar->setLatCom(false);

    // Turn on latency compensation, in case somebody sets it off by default.
    BWAPI::Broodwar->setLatCom(true);
}

void UAlbertaBotModule::onEnd(bool isWinner)
{
    GameCommander::Instance().onEnd(isWinner);
}

void UAlbertaBotModule::onFrame()
{
    if (!Config::ConfigFile::ConfigFileFound)
    {
        BWAPI::Broodwar->drawBoxScreen(0,0,450,100, BWAPI::Colors::Black, true);
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Huge);
        BWAPI::Broodwar->drawTextScreen(10, 5, "%c%s Config File Not Found", red, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Default);
        BWAPI::Broodwar->drawTextScreen(10, 30, "%c%s will not run without its configuration file", white, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->drawTextScreen(10, 45, "%cCheck that the file below exists. Incomplete paths are relative to Starcraft directory", white);
        BWAPI::Broodwar->drawTextScreen(10, 60, "%cYou can change this file location in Config::ConfigFile::ConfigFileLocation", white);
        BWAPI::Broodwar->drawTextScreen(10, 75, "%cFile Not Found (or is empty): %c %s", white, green, Config::ConfigFile::ConfigFileLocation.c_str());
        return;
    }

    if (!Config::ConfigFile::ConfigFileParsed)
    {
        BWAPI::Broodwar->drawBoxScreen(0,0,450,100, BWAPI::Colors::Black, true);
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Huge);
        BWAPI::Broodwar->drawTextScreen(10, 5, "%c%s Config File Parse Error", red, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Default);
        BWAPI::Broodwar->drawTextScreen(10, 30, "%c%s will not run without a properly formatted configuration file", white, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->drawTextScreen(10, 45, "%cThe configuration file was found, but could not be parsed. Check that it is valid JSON", white);
        BWAPI::Broodwar->drawTextScreen(10, 60, "%cFile Not Parsed: %c %s", white, green, Config::ConfigFile::ConfigFileLocation.c_str());
        return;
    }

    GameCommander::Instance().update();
}

void UAlbertaBotModule::onUnitDestroy(BWAPI::Unit unit)
{
    GameCommander::Instance().onUnitDestroy(unit);
}

void UAlbertaBotModule::onUnitMorph(BWAPI::Unit unit)
{
    GameCommander::Instance().onUnitMorph(unit);
}

void UAlbertaBotModule::onSendText(std::string text) 
{ 
    ParseUtils::ParseTextCommand(text);
}

void UAlbertaBotModule::onUnitCreate(BWAPI::Unit unit)
{ 
    GameCommander::Instance().onUnitCreate(unit);
}

void UAlbertaBotModule::onUnitComplete(BWAPI::Unit unit)
{
    GameCommander::Instance().onUnitComplete(unit);
}

void UAlbertaBotModule::onUnitShow(BWAPI::Unit unit)
{ 
    GameCommander::Instance().onUnitShow(unit);
}

void UAlbertaBotModule::onUnitHide(BWAPI::Unit unit)
{ 
    GameCommander::Instance().onUnitHide(unit);
}

void UAlbertaBotModule::onUnitRenegade(BWAPI::Unit unit)
{ 
    GameCommander::Instance().onUnitRenegade(unit);
}