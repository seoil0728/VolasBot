#include "FAP.h"
#include "BWAPI.h"
#include "UnitUtil.h"

UAlbertaBot::FastAPproximation fap;

// This is N00byEdge's original version of FAP, adjusted to fit into its new environment.
// Newer versions exist.
// https://github.com/N00byEdge/Neohuman/blob/master/FAP.cpp

// This version is also updated to understand dark swarm and ensnare, in an approximate way.
// There are a few bug fixes and other improvements.

// NOTE FAP does not use UnitInfo.goneFromLastPosition. The flag is always set false
// on a UnitInfo value which is passed in (CombatSimulation makes sure of it).

namespace UAlbertaBot {

    FastAPproximation::FastAPproximation() {

    }

    void FastAPproximation::addUnitPlayer1(FAPUnit fu) {
        player1.push_back(fu);
    }

    void FastAPproximation::addIfCombatUnitPlayer1(FAPUnit fu) {
        if (fu.groundDamage || fu.airDamage || fu.unitType == BWAPI::UnitTypes::Terran_Medic) {
            addUnitPlayer1(fu);
        }
    }

    void FastAPproximation::addUnitPlayer2(FAPUnit fu) {
        player2.push_back(fu);
    }

    void FastAPproximation::addIfCombatUnitPlayer2(FAPUnit fu) {
        if (fu.groundDamage || fu.airDamage || fu.unitType == BWAPI::UnitTypes::Terran_Medic) {
            addUnitPlayer2(fu);
        }
    }

    void FastAPproximation::simulate(int nFrames) {
        while (nFrames--) {
            if (!player1.size() || !player2.size())
                break;

            didSomething = false;

            isimulate(false);

            if (!didSomething)
                break;
        }
    }

    void FastAPproximation::simulateRetreat(const BWAPI::Position & retreatTo, int nFrames) {
        if (!player2.size())
        {
            return;
        }
        targetPosition = retreatTo;

        while (nFrames--) {
            if (!player1.size())
                break;

            didSomething = false;

            isimulate(true);

            if (!didSomething)
                break;
        }
    }
    
    std::pair <int, int> FastAPproximation::playerScores() const {
        std::pair <int, int> res;

        for (auto & u : player1)
            if (u.health && u.maxHealth)
                res.first += (u.score * u.health) / (u.maxHealth * 2);

        for (auto & u : player2)
            if (u.health && u.maxHealth)
                res.second += (u.score * u.health) / (u.maxHealth * 2);

        return res;
    }

    std::pair <int, int> FastAPproximation::playerScoresUnits() const {
        std::pair <int, int> res;

        for (auto & u : player1)
            if (u.health && u.maxHealth && !u.unitType.isBuilding())
                res.first += (u.score * u.health) / (u.maxHealth * 2);

        for (auto & u : player2)
            if (u.health && u.maxHealth && !u.unitType.isBuilding())
                res.second += (u.score * u.health) / (u.maxHealth * 2);

        return res;
    }

    std::pair <int, int> FastAPproximation::playerScoresBuildings() const {
        std::pair <int, int> res;

        for (auto & u : player1)
            if (u.health && u.maxHealth && u.unitType.isBuilding())
                res.first += (u.score * u.health) / (u.maxHealth * 2);

        for (auto & u : player2)
            if (u.health && u.maxHealth && u.unitType.isBuilding())
                res.second += (u.score * u.health) / (u.maxHealth * 2);

        return res;
    }

    std::pair<std::vector<FastAPproximation::FAPUnit>*, std::vector<FastAPproximation::FAPUnit>*> FastAPproximation::getState() {
        return { &player1, &player2 };
    }

    void FastAPproximation::clearState() {
        player1.clear(), player2.clear();
    }

    void FastAPproximation::dealDamage(const FastAPproximation::FAPUnit &fu, int damage, BWAPI::DamageType damageType) const {
        if (fu.shields >= damage - fu.shieldArmor) {
            fu.shields -= damage - fu.shieldArmor;
            return;
        }
        else if(fu.shields) {
            damage -= (fu.shields + fu.shieldArmor);
            fu.shields = 0;
        }

        if (!damage)
            return;

        if (damageType == BWAPI::DamageTypes::Concussive) {
            if(fu.unitSize == BWAPI::UnitSizeTypes::Large)
                damage = damage / 4;
            else if(fu.unitSize == BWAPI::UnitSizeTypes::Medium)
                damage = damage / 2;
        }
        else if (damageType == BWAPI::DamageTypes::Explosive) {
            if (fu.unitSize == BWAPI::UnitSizeTypes::Small)
                damage = damage / 2;
            else if (fu.unitSize == BWAPI::UnitSizeTypes::Medium)
                damage = (damage * 3) / 4;
        }

        fu.health -= std::max(1, damage - fu.armor);
    }

    // The square of the Euclidean distance between the unit's position and the given position.
    // Skip the expensive step of computing the square root.
    int inline FastAPproximation::distSquared(const FastAPproximation::FAPUnit &u1, const BWAPI::Position & xy) const {
        return (u1.x - xy.x)*(u1.x - xy.x) + (u1.y - xy.y)*(u1.y - xy.y);
    }

    // The square of the Euclidean distance between the units' positions.
    // Skip the expensive step of computing the square root.
    int inline FastAPproximation::distSquared(const FastAPproximation::FAPUnit &u1, const FastAPproximation::FAPUnit &u2) const {
        return (u1.x - u2.x)*(u1.x - u2.x) + (u1.y - u2.y)*(u1.y - u2.y);
    }

    bool FastAPproximation::isSuicideUnit(BWAPI::UnitType ut) {
        return
            ut == BWAPI::UnitTypes::Zerg_Scourge ||
            ut == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
            ut == BWAPI::UnitTypes::Zerg_Infested_Terran ||
            ut == BWAPI::UnitTypes::Protoss_Scarab;
    }

    void FastAPproximation::unitsim(const FastAPproximation::FAPUnit &fu, std::vector <FastAPproximation::FAPUnit> &enemyUnits) {
        if (fu.attackCooldownRemaining) {
            didSomething = true;
            return;
        }

        auto closestEnemy = enemyUnits.end();
        int closestDist = InfiniteDistanceSquared;  // distance squared

        // NOTE This skips siege tanks, which do splash damage under swarm.
        const bool hitUnderSwarm =
            fu.groundDamage &&
            !fu.unitType.isWorker() &&
            (	fu.groundMaxRange <= 32 * 32 ||     // "range" is actually squared range
                isSuicideUnit(fu.unitType) ||
                fu.unitType == BWAPI::UnitTypes::Protoss_Archon ||
                fu.unitType == BWAPI::UnitTypes::Protoss_Reaver ||
                fu.unitType == BWAPI::UnitTypes::Zerg_Lurker
            );

        // Find the closest enemy unit which is not too close to hit with our weapon.
        // A sieged tank has a minimum range; all other weapons have min range 0 (so we only check ground weapons).
        for (auto enemyIt = enemyUnits.begin(); enemyIt != enemyUnits.end(); ++enemyIt) {
            if (enemyIt->flying) {
                if (fu.airDamage) {
                    int d = distSquared(fu, *enemyIt);
                    if (closestEnemy == enemyUnits.end() || d < closestDist) {
                        closestDist = d;
                        closestEnemy = enemyIt;
                    }
                }
            }
            else {
                if (fu.groundDamage && (!enemyIt->underSwarm || hitUnderSwarm)) {
                    int d = distSquared(fu, *enemyIt);
                    if ((closestEnemy == enemyUnits.end() || d < closestDist) && d >= fu.groundMinRange) {
                        closestDist = d;
                        closestEnemy = enemyIt;
                    }
                }
            }
        }

        // If we can reach the enemy this simulated frame, do it and continue.

        if (closestEnemy != enemyUnits.end() &&
            closestDist <= fu.speed * fu.speed &&
            !(fu.x == closestEnemy->x && fu.y == closestEnemy->y)) {
            fu.x = closestEnemy->x;
            fu.y = closestEnemy->y;
            closestDist = 0;

            didSomething = true;
        }

        // Shoot at the enemy if in range, otherwise move toward the enemy.
        if (closestEnemy != enemyUnits.end() &&
            closestDist <= (closestEnemy->flying ? fu.airMaxRange : fu.groundMaxRange)) {
            if (closestEnemy->flying) {
                dealDamage(*closestEnemy, fu.airDamage, fu.airDamageType);
                fu.attackCooldownRemaining = fu.airCooldown;
            }
            else {
                dealDamage(*closestEnemy, fu.groundDamage, fu.groundDamageType);
                fu.attackCooldownRemaining = fu.groundCooldown;
                if (fu.elevation != -1 && closestEnemy->elevation != -1)
                    if (closestEnemy->elevation > fu.elevation)
                        fu.attackCooldownRemaining += fu.groundCooldown;
            }

            if (closestEnemy->health < 1) {
                auto temp = *closestEnemy;
                *closestEnemy = enemyUnits.back();
                enemyUnits.pop_back();
                unitDeath(temp, enemyUnits);
            }

            didSomething = true;
        }
        else if (closestEnemy != enemyUnits.end() && closestDist > fu.speed * fu.speed) {
            int dx = closestEnemy->x - fu.x, dy = closestEnemy->y - fu.y;

            fu.x += (int)(dx*(fu.speed / sqrt(dx*dx + dy*dy)));
            fu.y += (int)(dy*(fu.speed / sqrt(dx*dx + dy*dy)));
                
            didSomething = true;
        }
    }

    // Simulate moving while under fire, trying to reach a retreat point `targetPosition`.
    void FastAPproximation::movesim(const FastAPproximation::FAPUnit &fu, std::vector <FastAPproximation::FAPUnit> &enemyUnits) {
        int targetDist = distSquared(fu, targetPosition);

        if (targetDist > fu.speed * fu.speed) {
            int dx = targetPosition.x - fu.x, dy = targetPosition.y - fu.y;

            fu.x += (int)(dx*(fu.speed / sqrt(dx*dx + dy*dy)));
            fu.y += (int)(dy*(fu.speed / sqrt(dx*dx + dy*dy)));

            didSomething = true;
        }
    }

    void FastAPproximation::medicsim(const FAPUnit & fu, std::vector<FAPUnit> &friendlyUnits) {
        auto closestHealable = friendlyUnits.end();
        int closestDist = MAX_DISTANCE;

        for (auto it = friendlyUnits.begin(); it != friendlyUnits.end(); ++it) {
            if (it->isOrganic && it->health < it->maxHealth && !it->didHealThisFrame) {
                int d = distSquared(fu, *it);
                if (closestHealable == friendlyUnits.end() || d < closestDist) {
                    closestHealable = it;
                    closestDist = d;
                }
            }
        }

        if (closestHealable != friendlyUnits.end()) {
            fu.x = closestHealable->x;
            fu.y = closestHealable->y;

            // According to N00byEdge, 400 (instead of 300) is correct, but in reality medics
            // are not used optimally, so the smaller value is more accurate in practice.
            closestHealable->health += (closestHealable->healTimer += 300) / 256;
            closestHealable->healTimer %= 256;

            if (closestHealable->health > closestHealable->maxHealth)
                closestHealable->health = closestHealable->maxHealth;

            closestHealable->didHealThisFrame = false;
        }
    }

    bool FastAPproximation::suicideSim(const FAPUnit & fu, std::vector<FAPUnit>& enemyUnits) {
        auto closestEnemy = enemyUnits.end();
        int closestDist = MAX_DISTANCE;

        for (auto enemyIt = enemyUnits.begin(); enemyIt != enemyUnits.end(); ++enemyIt) {
            if (enemyIt->flying) {
                if (fu.airDamage) {
                    int d = distSquared(fu, *enemyIt);
                    if (closestEnemy == enemyUnits.end() || d < closestDist) {
                        closestDist = d;
                        closestEnemy = enemyIt;
                    }
                }
            }
            else {
                if (fu.groundDamage) {
                    int d = distSquared(fu, *enemyIt);
                    if ((closestEnemy == enemyUnits.end() || d < closestDist) && d >= fu.groundMinRange) {
                        closestDist = d;
                        closestEnemy = enemyIt;
                    }
                }
            }
        }

        if (closestEnemy != enemyUnits.end() && closestDist <= fu.speed * fu.speed) {
            if(closestEnemy->flying)
                dealDamage(*closestEnemy, fu.airDamage, fu.airDamageType);
            else 
                dealDamage(*closestEnemy, fu.groundDamage, fu.groundDamageType);

            if (closestEnemy->health < 1) {
                auto temp = *closestEnemy;
                *closestEnemy = enemyUnits.back();
                enemyUnits.pop_back();
                unitDeath(temp, enemyUnits);
            }

            didSomething = true;
            return true;
        }
        else if (closestEnemy != enemyUnits.end() && closestDist > fu.speed * fu.speed) {
            int dx = closestEnemy->x - fu.x, dy = closestEnemy->y - fu.y;

            fu.x += (int)(dx*(fu.speed / sqrt(dx*dx + dy*dy)));
            fu.y += (int)(dy*(fu.speed / sqrt(dx*dx + dy*dy)));

            didSomething = true;
        }

        return false;
    }

    // If `retreat` then we simulate player1 retreating from combat with player2, who follows and keeps shooting.
    void FastAPproximation::isimulate(bool retreat) {
        for (auto fu = player1.begin(); fu != player1.end();) {
            if (isSuicideUnit(fu->unitType)) {
                bool result = suicideSim(*fu, player2);
                if (result)
                    fu = player1.erase(fu);
                else
                    ++fu;
            }
            else {
                if (fu->unitType == BWAPI::UnitTypes::Terran_Medic)
                    medicsim(*fu, player1);
                else
                    if (retreat) movesim(*fu, player2); else unitsim(*fu, player2);
                ++fu;
            }
        }

        for (auto fu = player2.begin(); fu != player2.end();) {
            if (isSuicideUnit(fu->unitType)) {
                bool result = suicideSim(*fu, player1);
                if (result)
                    fu = player2.erase(fu);
                else
                    ++fu;
            }
            else {
                if (fu->unitType == BWAPI::UnitTypes::Terran_Medic)
                    medicsim(*fu, player2);
                else
                    unitsim(*fu, player1);
                ++fu;
            }
        }

        for (auto &fu : player1) {
            if (fu.attackCooldownRemaining)
                --fu.attackCooldownRemaining;
            if (fu.didHealThisFrame)
                fu.didHealThisFrame = false;
        }

        for (auto &fu : player2) {
            if (fu.attackCooldownRemaining)
                --fu.attackCooldownRemaining;
            if (fu.didHealThisFrame)
                fu.didHealThisFrame = false;
        }
    }

    void FastAPproximation::unitDeath(const FAPUnit &fu, std::vector<FAPUnit> &itsFriendlies) {
        if (fu.unitType == BWAPI::UnitTypes::Terran_Bunker) {
            convertToUnitType(fu, BWAPI::UnitTypes::Terran_Marine);

            for(unsigned i = 0; i < 4; ++ i)
                itsFriendlies.push_back(fu);
        }
    }

    void FastAPproximation::convertToUnitType(const FAPUnit &fu, BWAPI::UnitType ut)
    {
        UAlbertaBot::UnitInfo ui;
        ui.lastPosition = BWAPI::Position(fu.x, fu.y);
        ui.player = fu.player;
        ui.type = ut;

        FAPUnit funew(ui);
        funew.attackCooldownRemaining = fu.attackCooldownRemaining;
        funew.elevation = fu.elevation;

        fu.operator=(funew);
    }

    FastAPproximation::FAPUnit::FAPUnit(BWAPI::Unit u): FAPUnit(UnitInfo(u)) {
    }

    FastAPproximation::FAPUnit::FAPUnit(const UnitInfo & ui) :
        x(ui.lastPosition.x),
        y(ui.lastPosition.y),

        speed(ui.player->topSpeed(ui.type)),

        health(ui.estimateHP()),
        maxHealth(ui.type.maxHitPoints()),
        shields(ui.estimateShields()),
        shieldArmor(ui.player->getUpgradeLevel(BWAPI::UpgradeTypes::Protoss_Plasma_Shields)),
        maxShields(ui.type.maxShields()),
        armor(ui.player->armor(ui.type)),
        flying(ui.type.isFlyer()),
        underSwarm(ui.unit && ui.unit->isVisible() && ui.unit->isUnderDarkSwarm()),  // not too accurate

        groundDamage(ui.player->damage(ui.type.groundWeapon())),
        groundCooldown(ui.type.groundWeapon().damageFactor() && ui.type.maxGroundHits() ? ui.player->weaponDamageCooldown(ui.type) / (ui.type.groundWeapon().damageFactor() * ui.type.maxGroundHits()) : 0),
        groundMaxRange(ui.player->weaponMaxRange(ui.type.groundWeapon())),
        groundMinRange(ui.type.groundWeapon().minRange()),
        groundDamageType(ui.type.groundWeapon().damageType()),

        airDamage(ui.player->damage(ui.type.airWeapon())),
        airCooldown(ui.type.airWeapon().damageFactor() && ui.type.maxAirHits() ? ui.type.airWeapon().damageCooldown() / (ui.type.airWeapon().damageFactor() * ui.type.maxAirHits()) : 0),
        airMaxRange(ui.player->weaponMaxRange(ui.type.airWeapon())),
        airDamageType(ui.type.airWeapon().damageType()),

        unitType(ui.type),
        isOrganic(ui.type.isOrganic()),
        score(unitScore(ui.type)),
        player(ui.player)
    {
        static int nextId = 0;
        id = nextId++;

        if (ui.type == BWAPI::UnitTypes::Protoss_Carrier) {
            groundDamage = ui.player->damage(BWAPI::UnitTypes::Protoss_Interceptor.groundWeapon());
            groundDamageType = BWAPI::UnitTypes::Protoss_Interceptor.groundWeapon().damageType();
            groundCooldown = 5;
            groundMaxRange = 32 * 8;

            airDamage = groundDamage;
            airDamageType = groundDamageType;
            airCooldown = groundCooldown;
            airMaxRange = groundMaxRange;
        }
        else if (ui.type == BWAPI::UnitTypes::Terran_Bunker) {
            groundDamage = ui.player->damage(BWAPI::WeaponTypes::Gauss_Rifle);
            groundCooldown = BWAPI::UnitTypes::Terran_Marine.groundWeapon().damageCooldown() / 4;
            groundMaxRange = ui.player->weaponMaxRange(BWAPI::UnitTypes::Terran_Marine.groundWeapon()) + 32;

            airDamage = groundDamage;
            airCooldown = groundCooldown;
            airMaxRange = groundMaxRange;
        }
        else if (ui.type == BWAPI::UnitTypes::Protoss_Reaver) {
            groundDamage = ui.player->damage(BWAPI::WeaponTypes::Scarab);
        }

        // Stimmed units shoot faster, unless they are also ensnared.
        if (ui.unit && ui.unit->isStimmed() && !ui.unit->isEnsnared()) {
            groundCooldown /= 2;
            airCooldown /= 2;
        }

        if (ui.unit && ui.unit->isEnsnared())
        {
            // An ensnared unit moves and shoots more slowly.

            // Half speed movement.
            // NOTE The result is incorrect for stimmed units and units with a speed upgrade.
            //      But it's close enough for now.
            speed /= 2.0;

            // Cooldown increased by 25%, with exceptions.
            if (ui.type == BWAPI::UnitTypes::Zerg_Zergling && groundCooldown < 8)
            {
                // Zergling with the adrenal glands upgrade returns to its base cooldown of 8.
                groundCooldown = 8;
            }
            else if (
                ui.type != BWAPI::UnitTypes::Terran_Goliath &&
                ui.type != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode &&
                ui.type != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
                ui.type != BWAPI::UnitTypes::Zerg_Ultralisk &&
                !ui.unit->isStimmed())      // handled by earlier stimmed unit adjustment
            {
                groundCooldown = 5 * groundCooldown / 4;
                airCooldown = 5 * airCooldown / 4;
            }
        }

        // Ground height for ground units.
        if (ui.unit && !ui.unit->isFlying()) {
            elevation = BWAPI::Broodwar->getGroundHeight(BWAPI::TilePosition(x,y));
        }

        // Convert ranges to squared ranges so they can be compared with squared distances.
        groundMaxRange *= groundMaxRange;
        groundMinRange *= groundMinRange;
        airMaxRange *= airMaxRange;

        groundDamage *= 2;
        airDamage *= 2;

        shieldArmor *= 2;
        armor *= 2;

        health *= 2;
        maxHealth *= 2;
        shields *= 2;
        maxShields *= 2;
    }

    // Copy a FAPUnit.
    const FastAPproximation::FAPUnit &FastAPproximation::FAPUnit::operator=(const FAPUnit & other) const {
        x = other.x, y = other.y;
        health = other.health, maxHealth = other.maxHealth;
        shields = other.shields, maxShields = other.maxShields;
        speed = other.speed, armor = other.armor, flying = other.flying, underSwarm = other.underSwarm, unitSize = other.unitSize;
        groundDamage = other.groundDamage, groundCooldown = other.groundCooldown, groundMaxRange = other.groundMaxRange, groundMinRange = other.groundMinRange, groundDamageType = other.groundDamageType;
        airDamage = other.airDamage, airCooldown = other.airCooldown, airMaxRange = other.airMaxRange, airDamageType = other.airDamageType;
        score = other.score;
        attackCooldownRemaining = other.attackCooldownRemaining;
        unitType = other.unitType; isOrganic = other.isOrganic;
        healTimer = other.healTimer; didHealThisFrame = other.didHealThisFrame;
        elevation = other.elevation;
        player = other.player;

        return *this;
    }

    bool FastAPproximation::FAPUnit::operator<(const FAPUnit & other) const {
        return id < other.id;
    }

    // Some types get special case scores.
    int FastAPproximation::FAPUnit::unitScore(BWAPI::UnitType type) const {
        if (type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
        {
            return 20;
        }
        if (type == BWAPI::UnitTypes::Protoss_Archon)
        {
            return 2 * (50 + 150);
        }
        if (type == BWAPI::UnitTypes::Protoss_Dark_Archon)
        {
            return 2 * (125 + 100);
        }
        if (type == BWAPI::UnitTypes::Protoss_Reaver)
        {
            return 200 + 100 + 5 * 15;		// account for scarabs
        }
        if (type == BWAPI::UnitTypes::Protoss_Carrier)
        {
            return 350 + 250 + 8 * 25;		// account for interceptors
        }
        if (type.getRace() == BWAPI::Races::Zerg && (type.isBuilding() || UnitUtil::IsMorphedUnitType(type)))
        {
            // Add up the total price of a morphed unit, e.g. hydra + lurker morph, muta + guardian morph.
            // The longest chain that goes into combat sim is drone + creep colony + sunken/spore.
            int cost = 0;
            for (BWAPI::UnitType t = type; t != BWAPI::UnitTypes::Zerg_Larva; t = t.whatBuilds().first)
            {
                cost += t.mineralPrice() + t.gasPrice();
            }
            return cost;
        }
        if (type == BWAPI::UnitTypes::Zerg_Broodling)
        {
            return 5;
        }

        return type.isTwoUnitsInOneEgg() ? (type.mineralPrice() + type.gasPrice()) / 2 : type.mineralPrice() + type.gasPrice();
    }
}
