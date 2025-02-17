#include "MicroQueens.h"

#include "Bases.h"
#include "The.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// The queen is probably about to die. It should cast immediately if it is ever going to.
bool MicroQueens::aboutToDie(const BWAPI::Unit queen) const
{
    return
        queen->getHitPoints() < 30 ||
        queen->isIrradiated() ||
        queen->isPlagued();
}

// This unit is nearby. How much do we want to parasite it?
// Scores >= 100 are worth parasiting now. Scores < 100 are worth it if the queen is about to die.
int MicroQueens::parasiteScore(BWAPI::Unit u) const
{
    if (u->getPlayer() == the.neutral())
    {
        if (u->isFlying())
        {
            // It's a flying critter--worth tagging.
            return 100;
        }
        return 1;
    }

    // It's an enemy unit.

    BWAPI::UnitType type = u->getType();

    if (type == BWAPI::UnitTypes::Protoss_Arbiter)
    {
        return 110;
    }

    if (type == BWAPI::UnitTypes::Terran_Dropship ||

        type == BWAPI::UnitTypes::Protoss_Shuttle)
    {
        return 105;
    }

    if (type == BWAPI::UnitTypes::Terran_Battlecruiser ||
        type == BWAPI::UnitTypes::Terran_Science_Vessel || 

        type == BWAPI::UnitTypes::Protoss_Carrier)
    {
        return 101;
    }

    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode || 
        type == BWAPI::UnitTypes::Terran_Valkyrie ||
        
        type == BWAPI::UnitTypes::Protoss_Corsair ||
        type == BWAPI::UnitTypes::Protoss_Archon ||
        type == BWAPI::UnitTypes::Protoss_Dark_Archon ||
        type == BWAPI::UnitTypes::Protoss_Reaver ||
        type == BWAPI::UnitTypes::Protoss_Scout)
    {
        return 70;
    }

    if (type.isWorker() ||
        type == BWAPI::UnitTypes::Terran_Ghost ||
        type == BWAPI::UnitTypes::Terran_Medic ||
        type == BWAPI::UnitTypes::Terran_Wraith ||
        type == BWAPI::UnitTypes::Protoss_Observer)
    {
        return 60;
    }

    // A random enemy is worth something to parasite--but not much.
    return 2;
}

// Score units, pick the one with the highest score and maybe parasite it.
// Act only if the score >= minScore.
bool MicroQueens::maybeParasite(BWAPI::Unit queen, int minScore)
{
    // Parasite has range 12. We look for targets within the limit range.
    const int limit = 12 + 2;

    BWAPI::Unitset targets = BWAPI::Broodwar->getUnitsInRadius(queen->getPosition(), limit * 32,
        !BWAPI::Filter::IsBuilding && (BWAPI::Filter::IsEnemy || BWAPI::Filter::IsCritter) &&
        !BWAPI::Filter::IsInvincible && !BWAPI::Filter::IsParasited);

    if (targets.empty())
    {
        return false;
    }

    // Look for the target with the best score.
    const bool dying = aboutToDie(queen);
    int bestScore = dying ? 0 : minScore - 1;
    BWAPI::Unit bestTarget = nullptr;
    for (BWAPI::Unit target : targets)
    {
        int score = parasiteScore(target);
        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    if (bestTarget)
    {
        // Parasite something important.
        // Or, if the queen is at full energy, parasite something reasonable.
        // Or, if the queen is about to die, parasite anything.
        if (bestScore >= 100 ||
            bestScore >= 50 && queen->getEnergy() >= 200 && !the.self()->hasResearched(BWAPI::TechTypes::Spawn_Broodlings) ||
            bestScore >= 50 && queen->getEnergy() >= 225 ||
            dying)
        {
            //BWAPI::Broodwar->printf("parasite score %d on %s @ %d,%d",
            //	bestScore, UnitTypeName(bestTarget->getType()).c_str(), bestTarget->getPosition().x, bestTarget->getPosition().y);
            setReadyToCast(queen, CasterSpell::Parasite);
            return spell(queen, BWAPI::TechTypes::Parasite, bestTarget);
        }
    }

    return false;
}

// How much do we want to ensnare this unit?
int MicroQueens::ensnareScore(BWAPI::Unit u) const
{
    const BWAPI::UnitType type = u->getType();

    // Stuff that can't be ensnared, and spider mines. A spider mine above the ground is
    // unlikely to be affected by ensnare; it will explode or burrow too soon.
    if (type.isBuilding() ||
        u->isEnsnared() ||
        u->isBurrowed() ||
        type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
        u->isInvincible())
    {
        return 0;
    }

    int score = 1;

    // If it's cloaked, give it a bonus--a bigger bonus if it is not detected.
    if (!u->isDetected())
    {
        score += 80;
    }
    else if (u->isCloaked())
    {
        score += 40;		// because ensnare will keep it detected
    }

    if (type.isWorker())
    {
        score += 5;
    }
    else if (type.whatBuilds().first == BWAPI::UnitTypes::Terran_Barracks)
    {
        score += 10;
    }
    else if (
        type == BWAPI::UnitTypes::Terran_Wraith ||
        type == BWAPI::UnitTypes::Terran_Valkyrie ||
        type == BWAPI::UnitTypes::Protoss_Corsair ||
        type == BWAPI::UnitTypes::Protoss_Scout ||
        type == BWAPI::UnitTypes::Zerg_Mutalisk)
    {
        score += 33;
    }
    else if (
        type == BWAPI::UnitTypes::Terran_Dropship ||
        type == BWAPI::UnitTypes::Protoss_Shuttle)
    {
        score += 33;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Scourge)
    {
        score += 15;
    }
    else if (
        type == BWAPI::UnitTypes::Terran_Vulture ||
        type == BWAPI::UnitTypes::Protoss_Zealot ||
        type == BWAPI::UnitTypes::Protoss_Dragoon ||
        type == BWAPI::UnitTypes::Protoss_Archon ||
        type == BWAPI::UnitTypes::Protoss_Dark_Archon ||
        type == BWAPI::UnitTypes::Zerg_Zergling ||
        type == BWAPI::UnitTypes::Zerg_Hydralisk ||
        type == BWAPI::UnitTypes::Zerg_Ultralisk)
    {
        score += 10;
    }
    else
    {
        score += int(type.topSpeed());      // value up to 6
    }

    return score;
}

// We can ensnare. Look around to see if we should, and if so, do it.
bool MicroQueens::maybeEnsnare(BWAPI::Unit queen)
{
    // Ensnare has range 9 and affects a 4x4 box. We look a little beyond that range for targets.
    const int limit = 9 + 3;

    const bool dying = aboutToDie(queen);

    // Don't bother to look for units to ensnare if no enemy is close enough.
    BWAPI::Unit closest = BWAPI::Broodwar->getClosestUnit(queen->getPosition(),
        BWAPI::Filter::IsEnemy && !BWAPI::Filter::IsBuilding,
        limit * 32);

    if (!dying && !closest)
    {
        return false;
    }

    // Look for the 4x4 box with the best effect.
    int bestScore = 0;
    BWAPI::Position bestPlace;
    for (int tileX = std::max(2, queen->getTilePosition().x - limit); tileX <= std::min(BWAPI::Broodwar->mapWidth() - 3, queen->getTilePosition().x + limit); ++tileX)
    {
        for (int tileY = std::max(2, queen->getTilePosition().y - limit); tileY <= std::min(BWAPI::Broodwar->mapHeight() - 3, queen->getTilePosition().y + limit); ++tileY)
        {
            int score = 0;
            BWAPI::Position place(BWAPI::TilePosition(tileX, tileY));
            const BWAPI::Position offset(2 * 32, 2 * 32);
            BWAPI::Unitset affected = BWAPI::Broodwar->getUnitsInRectangle(place - offset, place + offset);
            for (BWAPI::Unit u : affected)
            {
                if (u->getPlayer() == the.self())
                {
                    score -= ensnareScore(u);
                }
                else if (u->getPlayer() == the.enemy())
                {
                    score += ensnareScore(u);
                }
            }
            if (score > bestScore)
            {
                bestScore = score;
                bestPlace = place;
            }
        }
    }

    if (bestScore > 0)
    {
        // BWAPI::Broodwar->printf("ensnare score %d at %d,%d", bestScore, bestPlace.x, bestPlace.y);
    }

    if (bestScore > 100 || dying && bestScore > 0)
    {
        setReadyToCast(queen, CasterSpell::Ensnare);
        return spell(queen, BWAPI::TechTypes::Ensnare, bestPlace);
    }

    return false;
}

// This unit is nearby. How much do we want to broodling it?
// Scores >= 100 are worth killing now.
// Scores >= 50+x are worth it if the queen has 250-x energy.
// Scores > 0 are worth it if the queen is about to die.
int MicroQueens::broodlingScore(BWAPI::Unit queen, BWAPI::Unit u) const
{
    // Extra points if the unit is defense matrixed.
    // Fewer points if the unit is already damaged. Shield damage counts.
    // Fewer points if the unit is plagued.
    // More points if the unit is in range, so that the queen does not have to move.
    const int bonus =
        (u->isDefenseMatrixed() ? 45 : 0) +
        int(40.0 * double(u->getHitPoints() + u->getShields()) / (u->getType().maxHitPoints() + u->getType().maxShields())) - 40 +
        (u->isPlagued() ? -5 : 5) +
        (u->isUnderDarkSwarm() ? 5 : -5) +
        ((queen->getDistance(u) <= 288) ? 30 : 0);

    if (u->getType() == BWAPI::UnitTypes::Terran_Ghost &&
        (u->getOrder() == BWAPI::Orders::NukePaint || u->getOrder() == BWAPI::Orders::NukeTrack))
    {
        return 200 + bonus;
    }

    if (u->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
        u->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        u->getType() == BWAPI::UnitTypes::Protoss_High_Templar ||
        u->getType() == BWAPI::UnitTypes::Zerg_Defiler)
    {
        return 110 + bonus;
    }

    if (u->getType() == BWAPI::UnitTypes::Zerg_Ultralisk)
    {
        return 120 + bonus;
    }

    // If no high-value enemies exist, accept a lower-value enemy.
    if (0 ==
        the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) +
        the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
        the.your.seen.count(BWAPI::UnitTypes::Protoss_High_Templar) +
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Defiler) +
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Ultralisk))
    {
        if (u->getType() == BWAPI::UnitTypes::Terran_Goliath ||
            u->getType() == BWAPI::UnitTypes::Terran_Ghost ||
            u->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
            u->getType() == BWAPI::UnitTypes::Protoss_Dragoon ||
            u->getType() == BWAPI::UnitTypes::Zerg_Lurker ||
            u->getType() == BWAPI::UnitTypes::Zerg_Lurker_Egg ||
            u->getType() == BWAPI::UnitTypes::Zerg_Infested_Terran)
        {
            return 100 + bonus;
        }
    }

    if (u->getType().gasPrice() > 0)
    {
        return 50 + u->getType().gasPrice() / 10 + bonus;
    }

    return u->getType().mineralPrice() / 10 + bonus;
}

// Score units, pick the one with the highest score and maybe broodling it.
bool MicroQueens::maybeBroodling(BWAPI::Unit queen)
{
    // Spawn broodlings has range 9. We look for targets within the limit range.
    const int limit = 9 + 4;

    // Ignore the possibility that you may want to broodling a non-enemy unit.
    // E.g., a neutral critter, so the broodlings can scout or tear down an unattended building.
    // Or maybe your own larva, say for your defiler to consume.
    BWAPI::Unitset targets = BWAPI::Broodwar->getUnitsInRadius(queen->getPosition(), limit * 32,
        BWAPI::Filter::IsEnemy && !BWAPI::Filter::IsBuilding && !BWAPI::Filter::IsFlyer &&
        !BWAPI::Filter::IsRobotic &&            // not probe or reaver
        BWAPI::Filter::GetType != BWAPI::UnitTypes::Protoss_Archon &&
        BWAPI::Filter::GetType != BWAPI::UnitTypes::Protoss_Dark_Archon &&
        BWAPI::Filter::IsDetected &&
        !BWAPI::Filter::IsInvincible);

    if (targets.empty())
    {
        return false;
    }

    // Look for the target with the best score.
    int bestScore = 0;
    BWAPI::Unit bestTarget = nullptr;
    for (BWAPI::Unit target : targets)
    {
        int score = broodlingScore(queen, target);
        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    if (bestTarget)
    {
        // Broodling something important.
        // Or, if the queen is at full energy, broodling something reasonable.
        // Or, if the queen is about to die, broodling anything.
        if (bestScore >= 100 ||
            bestScore >= 50 && queen->getEnergy() == 250 ||
            aboutToDie(queen))
        {
            // BWAPI::Broodwar->printf("broodling score %d on %s @ %d,%d",
            //    bestScore, UnitTypeName(bestTarget->getType()).c_str(), bestTarget->getPosition().x, bestTarget->getPosition().y);
            setReadyToCast(queen, CasterSpell::Broodling);
            return spell(queen, BWAPI::TechTypes::Spawn_Broodlings, bestTarget);
        }
    }

    return false;
}

// Each queen tries to avoid danger and keep a distance from other queens, when possible,
// while moving to the general area of the target position.
BWAPI::Position MicroQueens::getQueenDestination(BWAPI::Unit queen, const BWAPI::Position & target) const
{
    if (!queen->isIrradiated())
    {
        BWAPI::Race enemyRace = the.enemyRace();
        const int terranRange = the.your.seen.count(BWAPI::UnitTypes::Terran_Goliath) > 0 ? 10 : 7;
        int dangerRadius = enemyRace == BWAPI::Races::Terran ? terranRange : (enemyRace == BWAPI::Races::Protoss ? 8 : 7);
        if (queen->getHitPoints() < 65)
        {
            if (queen->getHitPoints() < 40)
            {
                dangerRadius += 2;
            }
            else
            {
                dangerRadius += 1;
            }
        }

        BWAPI::Unit danger = BWAPI::Broodwar->getClosestUnit(queen->getPosition(),
            BWAPI::Filter::IsEnemy &&
            (BWAPI::Filter::AirWeapon != BWAPI::WeaponTypes::None ||
            BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Science_Vessel ||
            BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Dark_Archon),
            32 * dangerRadius);

        if (danger)
        {
            return DistanceAndDirection(queen->getPosition(), danger->getPosition(), -dangerRadius * 32);
        }
    }

    const int keepAwayRadius = 4;
    int closestDist = 32 * keepAwayRadius;
    BWAPI::Unit sister = nullptr;
    for (BWAPI::Unit q : getUnits())
    {
        if (q != queen && queen->getDistance(q) < closestDist)
        {
            closestDist = queen->getDistance(q);
            sister = q;
        }
    }

    if (sister)
    {
        return DistanceAndDirection(queen->getPosition(), sister->getPosition(), -keepAwayRadius * 32);
    }

    return target;
}

int MicroQueens::totalEnergy() const
{
    int total = 0;
    for (BWAPI::Unit queen : getUnits())
    {
        total += queen->getEnergy();
    }
    return total;
}

// Move the queen. This includes moving to infest a command center.
void MicroQueens::updateMovement(BWAPI::Unit vanguard)
{
    for (BWAPI::Unit queen : getUnits())
    {
        // If it's intending to cast, we don't want to interrupt by moving.
        if (isReadyToCast(queen))
        {
            continue;
        }

        // Default destination if all else fails: The main base.
        BWAPI::Position destination = the.bases.myMain()->getPosition();

        // If we can see an infestable command center, move toward it.
        int nearestRange = MAX_DISTANCE;
        BWAPI::Unit nearestCC = nullptr;
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (enemy->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
                enemy->getHitPoints() < 750 &&
                enemy->isCompleted() &&
                queen->getDistance(enemy) < nearestRange)
            {
                nearestRange = queen->getDistance(enemy);
                nearestCC = enemy;
            }
        }

        if (nearestCC)
        {
            destination = nearestCC->getPosition();
            if (nearestRange < 4 * 32)
            {
                // We're close to an infestable CC, just go there. Accept the risk.
                the.micro.Move(queen, destination);
                return;
            }
            // Otherwise continue, and avoid danger as usual.
        }
        // Broodling costs 150 energy. Ensnare and parasite each cost 75.
        else if (vanguard &&
            queen->getEnergy() >= (the.self()->hasResearched(BWAPI::TechTypes::Spawn_Broodlings) ? 135 : 65))
        {
            destination = vanguard->getPosition();
        }
        else if (queen->getEnergy() >= (the.self()->hasResearched(BWAPI::TechTypes::Spawn_Broodlings) ? 150 : 75))
        {
            // No vanguard, but we have energy. Move to the front defense line and try to be useful.
            // This can happen when all units are assigned to defense squads.
            destination = the.bases.front();
        }

        if (destination.isValid())
        {
            the.micro.MoveNear(queen, getQueenDestination(queen, destination));
        }
    }
}

// Cast a spell if possible and useful.
// Called every frame and controls queens which are ready to cast every frame, but other queens
// only when allQueens is true. Finding a target can take its time, casting must be responsive.
void MicroQueens::updateAction(bool allQueens)
{
    for (BWAPI::Unit queen : getUnits())
    {
        if (allQueens || isReadyToCast(queen))
        {
            bool dying = aboutToDie(queen);
            bool foundTarget = false;

            if (the.self()->hasResearched(BWAPI::TechTypes::Spawn_Broodlings))
            {
                // The top priority is to parasite a top-priority enemy, like a droship.
                if (queen->getEnergy() >= 75 && maybeParasite(queen, 105))
                {
                    foundTarget = true;
                }
                // If we have broodling, then broodling is high priority.
                else if (queen->getEnergy() >= 150 && maybeBroodling(queen))
                {
                    // Broodling is set to be cast.
                    foundTarget = true;
                }
                else if (queen->getEnergy() >= 225 ||               // enough that broodling can happen after another spell
                    dying && queen->getEnergy() >= 75 ||
                    getUnits().size() >= 4 && totalEnergy() >= 600)
                {
                    // We have energy for either ensnare or parasite.
                    if (the.self()->hasResearched(BWAPI::TechTypes::Ensnare) && maybeEnsnare(queen))
                    {
                        foundTarget = true;
                    }
                    else
                    {
                        // Ensnare is not researched, or was not cast on this attempt. Consider parasite.
                        if (queen->getEnergy() == 250 || dying)
                        {
                            foundTarget = maybeParasite(queen, 50);
                        }
                        else if (queen->getEnergy() > 150)
                        {
                            foundTarget = maybeParasite(queen, 100);
                        }
                    }
                }
            }
            else if (the.self()->hasResearched(BWAPI::TechTypes::Ensnare))
            {
                // Ensnare but not broodling is available. Ensnare is higher priority than parasite.
                if (queen->getEnergy() >= 75 && maybeEnsnare(queen))
                {
                    // Ensnare is set to be cast.
                    foundTarget = true;
                }
                else if (queen->getEnergy() >= 150 ||               // enough that ensnare can happen after parasite
                    dying && queen->getEnergy() >= 75 ||
                    getUnits().size() >= 4 && totalEnergy() >= 400)
                {
                    foundTarget = maybeParasite(queen, 50);
                }
                else if (queen->getEnergy() >= 100)
                {
                    foundTarget = maybeParasite(queen, 100);
                }
            }
            else if (queen->getEnergy() >= 75)
            {
                // Parasite is the only possibility.
                foundTarget = maybeParasite(queen, 50);
            }

            // We used to have a target in mind, but lost it.
            if (!foundTarget)
            {
                clearReadyToCast(queen);
            }
        }
    }
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

MicroQueens::MicroQueens()
{ 
}

// Unused but required.
void MicroQueens::executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster)
{
}

// Control the queens.
// Queens are not clustered.
void MicroQueens::update(BWAPI::Unit vanguard)
{
    if (getUnits().empty())
    {
        return;
    }

    updateCasters(getUnits());

    const int phase = the.now() % 7;

    if (phase == 0)
    {
        updateMovement(vanguard);
    }

    // Called every frame, controls queens ready to cast every frame.
    // Controls queens not ready to cast only when the flag is true.
    updateAction(phase == 2);
}
