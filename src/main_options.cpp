// SPDX-License-Identifier: GPL-2.0-or-later
// main_detail: the policy options from the settings and the data files, and the scenario (declared in main_internal.h, called from main()).
#include "main_internal.h"

namespace main_detail {

// The policy options the settings give directly, and the crafting foci and the avoid spells their files give.
void PolicyFromSettings(PolicyOptions& policyOptions, BotSettings& settings)
{
    policyOptions.TestSpell = settings.TestSpell;
    policyOptions.ClassId = settings.Class;
    policyOptions.ServiceEveryMs = std::uint64_t(settings.ServiceEverySec) * 1000;
    policyOptions.TrainEveryMs = std::uint64_t(settings.TrainEverySec) * 1000;
    policyOptions.TameDeadPetBackoffMs = std::uint64_t(settings.TameDeadPetBackoffSec) * 1000;
    policyOptions.Supplies = settings.Supplies;
    policyOptions.Repair = settings.Repair;
    policyOptions.VendorSellGear = settings.VendorSellGear;
    policyOptions.SpiritHealerAfterMs = settings.SpiritHealerAfterMs;
    policyOptions.PullHoldBudgetMs = std::uint64_t(settings.PullHoldBudgetSec) * 1000;
    policyOptions.SpiritHealerOnRepeat = settings.SpiritHealerOnRepeat;
    policyOptions.HealDownrank = settings.HealDownrank;
    policyOptions.IdleEnemyAttackersOnly = settings.IdleEnemyAttackersOnly;
    policyOptions.CasterMeleeWithoutSpell = settings.CasterMeleeWithoutSpell;
    policyOptions.EngageMaxLevelAbove = settings.EngageMaxLevelAbove;
    policyOptions.EngageEliteSolo = settings.EngageEliteSolo;
    policyOptions.GhostFallbackMs = std::uint64_t(settings.GhostFallbackSec) * 1000;
    policyOptions.GhostMaxMs = std::uint64_t(settings.GhostMaxSec) * 1000;
    policyOptions.DeathAreaCount = settings.DeathAreaCount;
    policyOptions.DeathAreaWindowMs = std::uint64_t(settings.DeathAreaWindowSec) * 1000;
    policyOptions.DeathAreaYards = settings.DeathAreaYards;
    policyOptions.DeathAreaAvoidMs = std::uint64_t(settings.DeathAreaAvoidSec) * 1000;
    policyOptions.ThreatHold = settings.ThreatHold;
    policyOptions.ThreatHoldPct = settings.ThreatHoldPct;
    policyOptions.ThreatReleasePct = settings.ThreatReleasePct;
    policyOptions.ThreatHoldMaxMs = std::uint64_t(settings.ThreatHoldMaxSec) * 1000;
    policyOptions.ThreatNearDeathPct = settings.ThreatNearDeathPct;
    policyOptions.ThreatDropAbilities = settings.ThreatDropAbilities;
    policyOptions.DungeonTeleport = settings.DungeonTeleport;
    policyOptions.LevelDungeons = settings.LevelDungeons && settings.Scenarios.empty();   // a scenario run drives its own parties
    policyOptions.LevelDungeonRuns = settings.LevelDungeonRuns;
    policyOptions.LevelDungeonEarlyLevels = settings.LevelDungeonEarlyLevels;
    policyOptions.LevelDungeonSides = settings.LevelDungeonSides;
    policyOptions.LevelDungeonFileDuration = settings.LevelDungeonFileDuration;
    policyOptions.LevelDungeonNeedRoles = settings.LevelDungeonNeedRoles;
    policyOptions.LevelDungeonLog = settings.LevelDungeonLog;
    policyOptions.LfgFill = settings.LfgFill;
    policyOptions.Duels = settings.Duels;
    policyOptions.Battlegrounds = settings.Battlegrounds;
    policyOptions.GuildJoin = settings.GuildJoin;
    policyOptions.Guilds = settings.Guilds;
    policyOptions.MailGifts = settings.MailGifts;
    policyOptions.BotMail = settings.BotMail;
    policyOptions.DungeonReviveOutOfCombat = settings.DungeonReviveOutOfCombat;
    policyOptions.DungeonPullLeashYards = settings.DungeonPullLeashYards;
    policyOptions.DungeonChaseLeashYards = settings.DungeonChaseLeashYards;
    policyOptions.DungeonLeaderRejoin = settings.DungeonLeaderRejoin;
    policyOptions.GhostTeleportPerSecond = settings.GhostTeleportPerSecond;
    policyOptions.GhostTeleportHold = settings.GhostTeleportHold;
    policyOptions.GhostTeleportHoldMaxMs = settings.GhostTeleportHoldMaxMs;
    policyOptions.GhostHoldAvoidEnemyMaxMs = settings.GhostHoldAvoidEnemyMaxMs;
    policyOptions.GhostHoldPartyFightMaxMs = settings.GhostHoldPartyFightMaxMs;
    policyOptions.DungeonRevivesPerMinute = settings.DungeonRevivesPerMinute;
    policyOptions.DeathLockoutCount = settings.DeathLockoutCount;
    policyOptions.DeathLockoutWindowMs = settings.DeathLockoutWindowMs;
    policyOptions.DeathLockoutHoldMs = settings.DeathLockoutHoldMs;
    policyOptions.CorpseRunTeleportMs = std::uint64_t(settings.CorpseRunTeleportSec) * 1000;
    policyOptions.AggressiveStart = settings.AggressiveStart;
    policyOptions.Crafting = settings.Crafting;
    policyOptions.GatherWalkGapMs = std::uint64_t(settings.GatherWalkGapSec) * 1000;
    policyOptions.MountMinLevel = settings.MountMinLevel;
    policyOptions.TacticFocus = settings.TacticFocus;
    policyOptions.TacticInterrupt = settings.TacticInterrupt;
    policyOptions.TacticHeal = settings.TacticHeal;
    policyOptions.TacticRest = settings.TacticRest;
    policyOptions.TacticRecover = settings.TacticRecover;
    policyOptions.TacticCc = settings.TacticCc;
    policyOptions.PartyPlanner.Enabled = settings.PartyPlanner;
    policyOptions.HealPlan.Enabled = settings.HealPlan;
    policyOptions.HealPlan.PreHealMs = settings.HealPlanPreHealMs;
    policyOptions.HealPlan.PreHealBelowPct = settings.HealPlanPreHealPct;
    policyOptions.PartyViewPlanner = settings.PartyViewPlanner;
    policyOptions.DungeonHealBelowPct = settings.DungeonHealBelowPct;
    policyOptions.DungeonTankHealBelowPct = settings.DungeonTankHealBelowPct;
    policyOptions.HealerDpsManaPct = settings.HealerDpsManaPct;
    policyOptions.HealerCombatManaPct = settings.HealerCombatManaPct;
    policyOptions.TankPickup = settings.TankPickup;
    policyOptions.TankLoosePickup = settings.TankLoosePickup;
    policyOptions.TankLooseCooldownMs = settings.TankLooseCooldownMs;
    policyOptions.TankLooseRangeYards = settings.TankLooseRangeYards;
    policyOptions.TankLooseChaseYards = settings.TankLooseChaseYards;
    policyOptions.PullTankFirst = settings.PullTankFirst;
    policyOptions.HealWaitTankHold = settings.HealWaitTankHold;
    policyOptions.HealSpellMix = settings.HealSpellMix;
    policyOptions.PullTankHoldMs = settings.PullTankHoldMs;
    policyOptions.PullMemberRangeYards = settings.PullMemberRangeYards;
    policyOptions.PullMinHpPct = settings.PullMinHpPct;
    policyOptions.PullReadyManaPct = settings.PullReadyManaPct;
    policyOptions.PullReadyMaxMs = settings.PullReadyMaxMs;
    policyOptions.DotPerTarget = settings.DotPerTarget;
    policyOptions.SoloPullManaPct = settings.SoloPullManaPct;
    policyOptions.KillerAvoidMs = settings.KillerAvoidMs;
    policyOptions.AoeCcProtect = settings.AoeCcProtect;
    policyOptions.PullBuffGate = settings.PullBuffGate;
    policyOptions.SoloPullHpPct = settings.SoloPullHpPct;
    policyOptions.PullMaxGroup = settings.PullMaxGroup;
    policyOptions.PullMaxLevelAbove = settings.PullMaxLevelAbove;
    policyOptions.ResurrectBackoff = settings.ResurrectBackoff;
    policyOptions.PullWaitMaxMs = settings.PullWaitMaxMs;
    policyOptions.WarderPartyHold = settings.WarderPartyHold;
    policyOptions.HealerLosMoveYards = settings.HealerLosMoveYards;
    policyOptions.Gathering = settings.Gathering;
    if (!settings.CraftingFociFile.empty())
    {
        std::ifstream fociFile(settings.CraftingFociFile);
        std::stringstream fociText;
        fociText << fociFile.rdbuf();
        std::vector<std::string> fociErrors;
        auto foci = std::make_shared<std::vector<crafting::FocusPoint>>(crafting::ParseFociTable(fociText.str(), fociErrors));
        for (std::string const& error : fociErrors)
            std::cerr << "forever-bots: CraftingFociFile: " << error << "\n";
        if (!fociFile || foci->empty())
            std::cerr << "forever-bots: CraftingFociFile: cannot read '" << settings.CraftingFociFile << "' (or it holds no object); bots do not walk to a recipe's anvil, forge or fire\n";
        else
            std::cerr << "forever-bots: crafting foci: " << foci->size() << " object(s) from " << settings.CraftingFociFile << "\n";
        policyOptions.Foci = std::move(foci);
    }
    // Professions (doc/professions.md): the options, and the recipe table of the craft plan.
    policyOptions.Professions.Enabled = settings.Professions;
    policyOptions.Professions.SkillCapPerLevel = settings.ProfessionSkillCapPerLevel;
    policyOptions.Professions.SkinAfterLoot = settings.ProfessionSkinAfterLoot;
    policyOptions.Professions.NodeWalk = settings.ProfessionNodeWalk;
    policyOptions.Professions.NodeMaxYards = float(settings.ProfessionNodeMaxYards);
    policyOptions.Professions.NodeGapMs = std::uint64_t(settings.ProfessionNodeGapSec) * 1000;
    policyOptions.Professions.NodeEmptyMs = std::uint64_t(settings.ProfessionNodeEmptyMin) * 60000;
    policyOptions.Professions.CraftPlan = settings.ProfessionCraftPlan;
    if (settings.Professions && settings.ProfessionCraftPlan && !settings.ProfessionRecipeFile.empty())
    {
        std::ifstream recipeFile(settings.ProfessionRecipeFile);
        std::stringstream recipeText;
        recipeText << recipeFile.rdbuf();
        std::vector<std::string> recipeErrors;
        auto recipes = std::make_shared<std::vector<professions::Recipe>>(professions::ParseRecipeTable(recipeText.str(), recipeErrors));
        for (std::string const& error : recipeErrors)
            std::cerr << "forever-bots: ProfessionRecipeFile: " << error << "\n";
        if (!recipeFile || recipes->empty())
            std::cerr << "forever-bots: ProfessionRecipeFile: cannot read '" << settings.ProfessionRecipeFile << "' (or it holds no recipe); no craft plan\n";
        else
        {
            std::cerr << "forever-bots: profession recipes: " << recipes->size() << " recipe(s) from " << settings.ProfessionRecipeFile << "\n";
            policyOptions.ProfessionRecipes = std::move(recipes);
        }
    }
    // The node table is read for GatherWalk, and for the Professions node walk (which does not switch GatherWalk on).
    if ((settings.GatherWalk || (settings.Professions && settings.ProfessionNodeWalk)) && !settings.GatherNodesFile.empty())
    {
        std::ifstream nodesFile(settings.GatherNodesFile);
        std::stringstream nodesText;
        nodesText << nodesFile.rdbuf();
        std::vector<std::string> nodeErrors;
        auto nodes = std::make_shared<std::vector<gather::NodePoint>>(gather::ParseNodeTable(nodesText.str(), nodeErrors));
        for (std::string const& error : nodeErrors)
            std::cerr << "forever-bots: GatherNodesFile: " << error << "\n";
        if (!nodesFile || nodes->empty())
            std::cerr << "forever-bots: GatherNodesFile: cannot read '" << settings.GatherNodesFile << "' (or it holds no node); gatherers do not walk to nodes\n";
        else
        {
            std::cerr << "forever-bots: gather nodes: " << nodes->size() << " node(s) from " << settings.GatherNodesFile << "\n";
            policyOptions.GatherWalk = settings.GatherWalk;
            policyOptions.GatherNodes = std::move(nodes);
        }
    }
    if (!settings.RaidRulesFile.empty())
    {
        std::ifstream rulesFile(settings.RaidRulesFile);
        std::stringstream rulesText;
        rulesText << rulesFile.rdbuf();
        std::vector<std::string> rulesErrors;
        auto rules = std::make_shared<raid_rules::Table>(raid_rules::Parse(rulesText.str(), rulesErrors));
        for (std::string const& error : rulesErrors)
            std::cerr << "forever-bots: RaidRulesFile: " << error << "\n";
        if (!rulesFile || rules->Empty())
            std::cerr << "forever-bots: RaidRulesFile: cannot read '" << settings.RaidRulesFile << "' (or it holds no row); no raid rules\n";
        else
        {
            std::cerr << "forever-bots: raid rules: " << rules->MapCount() << " map(s) from " << settings.RaidRulesFile << "\n";
            policyOptions.RaidRules = std::move(rules);
        }
    }
    policyOptions.AvoidGroundEffects = settings.AvoidGroundEffects;
    policyOptions.TelegraphEscape = settings.TelegraphEscape;
    policyOptions.TelegraphEscapeCasters = settings.TelegraphEscapeCasters;
    policyOptions.RaidSpread = settings.RaidSpread;
    policyOptions.RaidAvoidEnemy = settings.RaidAvoidEnemy;
    policyOptions.RaidPurge = settings.RaidPurge;
    policyOptions.RaidMindControl = settings.RaidMindControl;
    policyOptions.RaidAvoidEnemyMelee = settings.RaidAvoidEnemyMelee;
    policyOptions.RaidAvoidEnemyTank = settings.RaidAvoidEnemyTank;
    policyOptions.RaidFocusAvoidEnemy = settings.RaidFocusAvoidEnemy;
    policyOptions.PullOnePack = settings.PullOnePack;
    policyOptions.TankLooseSkipAvoidEnemy = settings.TankLooseSkipAvoidEnemy;
    policyOptions.FlexRoles = settings.FlexRoles;
    policyOptions.RegroupRallyMs = settings.RegroupRallyMs;
    policyOptions.FightHeightAvoidMs = settings.FightHeightAvoidMs;
    policyOptions.FightStallDiag = settings.FightStallDiag;
    policyOptions.WalkDecideEvery = settings.WalkDecideEvery;
    policyOptions.DecideScheduler.Enabled = settings.DecideScheduler;
    policyOptions.DecideScheduler.BudgetUs = settings.DecideBudgetUs;
    policyOptions.DecideScheduler.TierWalk = settings.DecideTierWalk;
    policyOptions.DecideScheduler.TierIdle = settings.DecideTierIdle;
    policyOptions.DecideScheduler.MaxSkipTicks = settings.DecideMaxSkipTicks;
    policyOptions.RaidBossPositions = settings.RaidBossPositions;
    policyOptions.RaidPullSingle = settings.RaidPullSingle;
    policyOptions.RaidGhostFightHoldMaxMs = settings.RaidGhostFightHoldMaxMs;
    if (settings.AvoidGroundEffects)
    {
        auto avoidTable = std::make_shared<ground_avoid::AvoidTable>();
        if (!settings.AvoidSpellsFile.empty())
        {
            std::ifstream avoidFile(settings.AvoidSpellsFile);
            std::stringstream avoidText;
            avoidText << avoidFile.rdbuf();
            std::vector<std::string> avoidErrors;
            *avoidTable = ground_avoid::ParseAvoidSpells(avoidText.str(), avoidErrors);
            for (std::string const& error : avoidErrors)
                std::cerr << "forever-bots: AvoidSpellsFile: " << error << "\n";
            if (!avoidFile || avoidTable->Size() == 0)
                std::cerr << "forever-bots: AvoidSpellsFile: cannot read '" << settings.AvoidSpellsFile << "' (or it holds no spell); only effects of hostile casters are avoided\n";
            else
                std::cerr << "forever-bots: avoid spells: " << avoidTable->Size() << " spell(s) from " << settings.AvoidSpellsFile << "\n";
        }
        policyOptions.AvoidSpells = std::move(avoidTable);
        std::cerr << "forever-bots: bots step out of harmful ground effects (the core needs Bot.Snapshot.GroundEffects = 1)\n";
    }
}

std::vector<dungeon_events::Event> LoadDungeons(PolicyOptions& policyOptions, BotSettings& settings, std::shared_ptr<DungeonReloadBox> const& dungeonBox)
{
    // Dungeon routes for "tank lead" (every dungeon, by the map of its entry) and the events table, shared by all dungeons. They come from the JSON files of
    // DungeonDir (yours) and DungeonDataDir (shipped), and from the old text files for maps without a JSON file (dungeon_files.h).
    DungeonSources dungeonSources;
    dungeonSources.File = settings.DungeonFile;
    dungeonSources.EventsFile = settings.DungeonEventsFile;
    dungeonSources.DataDir = settings.DungeonDataDir;
    dungeonSources.OverrideDir = settings.DungeonDir;
    {
        std::lock_guard<std::mutex> lock(dungeonBox->Lock);
        dungeonBox->Sources = dungeonSources;
    }
    std::vector<dungeon_events::Event> allEvents;
    if (!dungeonSources.File.empty() || !dungeonSources.DataDir.empty() || !dungeonSources.OverrideDir.empty() || !dungeonSources.EventsFile.empty())
    {
        std::vector<std::string> dungeonWarnings;
        DungeonSet const set = LoadDungeonSet(dungeonSources, dungeonWarnings);
        for (std::string const& warning : dungeonWarnings)
            std::cerr << "forever-bots: dungeons: " << warning << "\n";
        for (std::string const& line : set.Log)
            std::cerr << "forever-bots: " << line << "\n";
        allEvents = set.Events;
        policyOptions.DungeonsByMap = DungeonRoutes(set.Dungeons);
        std::cerr << "forever-bots: dungeon events: " << allEvents.size() << "\n";
        std::cerr << "forever-bots: tank lead knows " << policyOptions.DungeonsByMap.size() << " dungeon(s)\n";
    }
    return allEvents;
}

// The flight places, the parties, the hubs, and the quest, tame, rotation, spell cost and grind files.
void LoadPolicyTables(PolicyOptions& policyOptions, BotSettings& settings)
{
    policyOptions.Taxi = settings.Taxi;
    policyOptions.Fidget = settings.Fidget;
    policyOptions.ReadDelay = settings.ReadDelay;
    policyOptions.PassingBuff = settings.PassingBuff;
    policyOptions.TownAfk = settings.TownAfk;
    // The line pools of the advisor features (each feature adds its own below; a pool is read only when its feature is on).
    auto pools = std::make_shared<advisor::Pools>();
    policyOptions.LfgQueueFill = settings.LfgQueueFill;
    policyOptions.DeathPause = settings.DeathPause;
    policyOptions.TradeAds = settings.TradeAds;
    policyOptions.PartyMana = settings.PartyMana;
    policyOptions.WhisperMatrix = settings.WhisperMatrix;
    policyOptions.PathJitter = settings.PathJitter;
    policyOptions.Personality = settings.Personality;
    policyOptions.PathVariety = settings.PathVariety;
    policyOptions.GuildMilestone = settings.GuildMilestone;
    policyOptions.CityInspect = settings.CityInspect;
    policyOptions.TransportRide = settings.TransportRide;
    policyOptions.CityGreet = settings.CityGreet;
    policyOptions.QuestInvite = settings.QuestInvite;
    policyOptions.MeetGreet = settings.MeetGreet;
    policyOptions.GroupEtiquette = settings.GroupEtiquette;
    // ADVISOR-COPY
    if (settings.GroupEtiquette.Enabled)
        pools->GroupEtiquette = advisor::LoadLines(BesideTheModule(settings.GroupEtiquetteFile));
    // ADVISOR-POOLS
    if (settings.CityGreet.Enabled)
    {
        pools->CityGreetings = advisor::LoadLines(BesideTheModule(settings.CityGreetFile));
        pools->AuctionTalk = advisor::LoadLines(BesideTheModule(settings.AuctionTalkFile));
    }
    if (settings.MeetGreet.Enabled)
        pools->MeetGreetings = advisor::LoadLines(BesideTheModule(settings.MeetGreetFile));
    if (settings.GuildMilestone.Enabled)
        pools->GuildMilestones = advisor::LoadLines(BesideTheModule(settings.GuildMilestoneFile));
    if (settings.WhisperMatrix.Enabled)
        pools->WhisperReplies = advisor::LoadLines(BesideTheModule(settings.WhisperMatrixFile));
    if (settings.PartyMana.Enabled)
        pools->PartyMana = advisor::LoadLines(BesideTheModule(settings.PartyManaFile));
    if (settings.TradeAds.Enabled)
    {
        pools->TradeAds = advisor::LoadLines(BesideTheModule(settings.TradeAdsFile));
        std::cerr << "forever-bots: trade ads: " << pools->TradeAds.size() << " template(s) from " << settings.TradeAdsFile << "\n";
    }
    policyOptions.Pools = pools;
    // Bot guild names (GuildsNameFile, doc/bot-guilds.md): no file or no valid row keeps the "BotLvl A001" names.
    if (settings.Guilds.Enabled && !settings.Guilds.NameFile.empty())
    {
        std::string const path = BesideTheModule(settings.Guilds.NameFile);
        std::ifstream in(path);
        std::ostringstream text;
        if (in)
            text << in.rdbuf();
        std::vector<std::string> warnings;
        auto names = std::make_shared<std::vector<bot_guilds::NameEntry>>(bot_guilds::ParseNames(text.str(), warnings));
        for (std::string const& warning : warnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: guild names: " << names->size() << " from " << path << (in ? "" : " (not readable: the BotLvl names are used)") << "\n";
        policyOptions.GuildNames = names;
    }
    if (settings.Taxi.Enabled)
    {
        // The city named in the taxi journal lines comes from the capitals' trainer table (ErrandTrainerFile); without it every city is "unknown".
        std::ifstream in(settings.ErrandTrainerFile);
        if (in)
        {
            std::ostringstream text;
            text << in.rdbuf();
            std::vector<std::string> ignored;
            policyOptions.TaxiPlaces = std::make_shared<std::vector<taxi::Place>>(errand::CityPlaces(errand::ParseTrainerTable(text.str(), ignored)));
        }
    }
    policyOptions.Reputation = settings.Reputation;
    if (settings.Reputation.Enabled)
    {
        std::ifstream in(settings.ReputationGoalFile);
        std::ostringstream text;
        text << in.rdbuf();
        std::vector<std::string> goalWarnings;
        policyOptions.ReputationGoals = reputation::ParseGoals(text.str(), goalWarnings);
        for (std::string const& warning : goalWarnings)
            std::cerr << "forever-bots: " << settings.ReputationGoalFile << ": " << warning << "\n";
        std::cerr << "forever-bots: reputation goals: " << policyOptions.ReputationGoals.size() << " from " << settings.ReputationGoalFile
                  << (in ? "" : " (not readable)") << "; no reputation facts from the core yet, so no visits happen (doc/reputation.md)\n";
    }
    policyOptions.ReputationPlan = settings.ReputationPlan;
    if (settings.Reputation.Enabled && settings.ReputationPlan.Enabled)
    {
        auto slurp = [](std::string const& path, bool& readable) {
            std::ifstream in(path);
            readable = static_cast<bool>(in);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        };
        bool targetsOk = false, sourcesOk = false;
        std::vector<std::string> planWarnings;
        std::string const targetText = slurp(settings.ReputationTargetFile, targetsOk);
        std::string const sourceText = slurp(settings.ReputationSourceFile, sourcesOk);
        auto targets = reputation::ParseTargets(targetText, planWarnings);
        auto sources = reputation::ParseSources(sourceText, planWarnings);
        for (std::string const& warning : planWarnings)
            std::cerr << "forever-bots: reputation plan: " << warning << "\n";
        policyOptions.ReputationPlanData = std::make_shared<reputation::PlanData>(reputation::BuildPlanData(std::move(targets), std::move(sources)));
        std::cerr << "forever-bots: reputation plan: " << policyOptions.ReputationPlanData->Targets.size() << " targets, " << policyOptions.ReputationPlanData->Sources.size()
                  << " sources" << (targetsOk && sourcesOk ? "" : " (a file is not readable)") << "\n";
    }
    policyOptions.PartySize = settings.PartySize;
    policyOptions.PartyCount = settings.PartyCount;
    policyOptions.PartyPercent = settings.PartyPercent;
    policyOptions.RandomPct = settings.RandomPct;
    policyOptions.RandomSeed = settings.RandomSeed;
    for (auto const& [mapId, spots] : settings.Hubs)
        for (auto const& spot : spots)
            policyOptions.HubsByMap[mapId].push_back({ spot[0], spot[1], spot[2] });
    if (!settings.QuestFile.empty())
    {
        std::vector<std::string> questWarnings;
        auto quests = std::make_shared<QuestData>(LoadQuestData(settings.QuestFile, questWarnings));
        if (!settings.AttunementFile.empty())
        {
            QuestData const attunement = LoadQuestData(BesideTheModule(settings.AttunementFile), questWarnings);
            MergeQuestData(*quests, attunement);
            std::cerr << "forever-bots: attunement: " << attunement.Starts.size() << " start(s) from " << BesideTheModule(settings.AttunementFile) << "\n";
        }
        for (std::string const& warning : questWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: quests: " << quests->Starts.size() << " start(s) from " << settings.QuestFile << "\n";
        policyOptions.Quests = std::move(quests);
    }
    {
        // Faction and level 60 lists (side 1 Alliance, 2 Horde; band 1 = level 60).
        struct ListFile { std::string const* Path; std::size_t Side; std::size_t Band; };
        ListFile const lists[] = { { &settings.QuestFileAlliance, 1, 0 }, { &settings.QuestFileHorde, 2, 0 },
                                   { &settings.QuestFileAlliance60, 1, 1 }, { &settings.QuestFileHorde60, 2, 1 } };
        for (ListFile const& list : lists)
        {
            if (list.Path->empty())
                continue;
            std::vector<std::string> warnings;
            auto data = std::make_shared<QuestData>(LoadQuestData(*list.Path, warnings));
            for (std::string const& warning : warnings)
                std::cerr << "forever-bots: " << warning << "\n";
            std::cerr << "forever-bots: quests: " << data->Starts.size() << " start(s) from " << *list.Path << " (side " << list.Side << ", band " << list.Band << ")\n";
            policyOptions.QuestLists[list.Side][list.Band] = std::move(data);
        }
    }
    policyOptions.QuestTeleport = settings.QuestTeleport;
    policyOptions.SkyborneExitQuests = settings.SkyborneExitQuests;
    policyOptions.SkyborneExitMakeRoom = settings.SkyborneExitQuests && settings.SkyborneExitMakeRoom;
    policyOptions.OffMap.Enabled = settings.OffMapRecover;
    policyOptions.OffMap.OffForMs = std::uint64_t(settings.OffMapSeconds) * 1000;
    policyOptions.OffMap.CooldownMs = std::uint64_t(settings.OffMapCooldownSec) * 1000;
    policyOptions.OffMap.MarginYards = float(settings.OffMapMarginYards);
    policyOptions.LevelSafe.Walks = settings.LevelSafeWalks;
    policyOptions.LevelSafe.Graveyard = settings.LevelSafeGraveyard;
    policyOptions.LevelSafe.Escape = settings.LevelSafeEscape;
    policyOptions.LevelSafe.MarginLevels = settings.LevelSafeMarginLevels;
    policyOptions.LevelSafe.EscapeMarginLevels = settings.LevelSafeEscapeMarginLevels;
    policyOptions.LevelSafe.RadiusYards = float(settings.LevelSafeRadiusYards);
    policyOptions.LevelSafe.EscapeCooldownMs = std::uint64_t(settings.LevelSafeEscapeCooldownSec) * 1000;
    policyOptions.LevelSafe.LogThrottleMs = std::uint64_t(settings.LevelSafeLogThrottleSec) * 1000;
    policyOptions.DeathLoopBreaker = settings.DeathLoopBreaker;
    policyOptions.SafeRevive.Enabled = settings.SafeRevive;
    policyOptions.SafeRevive.LevelMargin = settings.SafeReviveLevelMargin;
    policyOptions.FightRisk.Enabled = settings.FightRiskRetreat;
    policyOptions.FightRisk.MarginPct = settings.FightRiskMarginPct;
    policyOptions.FightRisk.Teleport = settings.FightRiskTeleport;
    policyOptions.DangerMap.Enabled = settings.DangerMap;
    policyOptions.DangerMap.DeathsPerHour = settings.DangerMapDeathsPerHour;
    policyOptions.DangerMap.DecayMs = std::uint64_t(settings.DangerMapDecayMin) * 60000;
    policyOptions.BelowMap.Enabled = settings.BelowMapRecover;
    policyOptions.BelowMap.DropYards = float(settings.BelowMapDropYards);
    policyOptions.BelowMap.StormCount = settings.BelowMapStormCount;
    policyOptions.BelowMap.CooldownMs = std::uint64_t(settings.BelowMapCooldownSec) * 1000;
    policyOptions.BelowMap.BelowChecks = settings.BelowMapChecks;
    policyOptions.BelowMap.MinPlaces = settings.BelowMapMinPlaces;
    policyOptions.BelowMap.RequireStill = settings.BelowMapRequireStill;
    policyOptions.BelowMap.StormRiseYards = float(settings.BelowMapStormRiseYards);
    policyOptions.BelowMap.StormDistinct = settings.BelowMapStormDistinct;
    policyOptions.NoRouteBackoff.Enabled = settings.NoRouteBackoff;
    policyOptions.NoRouteBackoff.BaseMs = std::uint64_t(settings.NoRouteBackoffSec) * 1000;
    policyOptions.AuctionWalk3D = settings.Auction.Enabled && settings.Auction.Walk3D;
    policyOptions.DeathsTopEveryMs = std::uint64_t(settings.DeathsTopEveryMin) * 60000;
    policyOptions.QuestGroups = settings.QuestGroups;
    policyOptions.DynamicParties = settings.DynamicParties;
    if (!settings.TameBeastFile.empty())
    {
        std::vector<std::string> tameWarnings;
        auto beasts = std::make_shared<tame::TameBeasts>(tame::LoadTameBeasts(BesideTheModule(settings.TameBeastFile), tameWarnings));
        for (std::string const& warning : tameWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: tame beasts: " << beasts->ByEntry.size() << " from " << BesideTheModule(settings.TameBeastFile) << "\n";
        if (!beasts->ByEntry.empty())
            policyOptions.TameBeasts = std::move(beasts);
    }
    if (!settings.RotationFile.empty())
    {
        std::vector<std::string> rotationWarnings;
        auto rotations = std::make_shared<rotation::RotationData>(rotation::LoadRotationData(BesideTheModule(settings.RotationFile), rotationWarnings));
        for (std::string const& warning : rotationWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: rotations for " << rotations->ByClass.size() << " class(es) and " << rotations->BySpec.size() << " spec(s) from " << BesideTheModule(settings.RotationFile) << "\n";
        policyOptions.Rotations = std::move(rotations);
    }
    if (!settings.SpellCostFile.empty())
    {
        std::vector<std::string> costWarnings;
        auto costs = std::make_shared<spell_cost::SpellCostData>(spell_cost::LoadSpellCosts(settings.SpellCostFile, costWarnings));
        for (std::string const& warning : costWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: spell costs for " << costs->BySpell.size() << " spell(s) from " << settings.SpellCostFile << "\n";
        policyOptions.SpellCosts = std::move(costs);
    }
    if (!settings.GrindFile.empty())
    {
        std::vector<std::string> grindWarnings;
        auto grind = std::make_shared<GrindData>(LoadGrindData(settings.GrindFile, grindWarnings));
        for (std::string const& warning : grindWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: grind spots: " << grind->Spots.size() << " from " << settings.GrindFile << "\n";
        if (settings.PathVariety.Enabled)
        {
            std::vector<path_variety::ZoneSample> samples;
            if (!settings.PathZoneFile.empty())
                samples = path_variety::LoadZoneSamples(settings.PathZoneFile, grindWarnings);
            for (std::string const& warning : grindWarnings)
                std::cerr << "forever-bots: " << warning << "\n";
            policyOptions.PathZones = std::make_shared<path_variety::ZoneIndex>(path_variety::BuildZoneIndex(*grind, samples));
            std::cerr << "forever-bots: path variety on, " << samples.size() << " zone sample(s)\n";
        }
        policyOptions.Grind = std::move(grind);
    }
}

// The policy's lookups into the module's tables (race, side, class, spec, alts) and its event log.
void ConnectPolicyLookups(PolicyOptions& policyOptions, BotSettings& settings, pick::ClassTable& races, pick::ClassTable& classes, RunContext& run,
                          std::shared_ptr<AltRegistry> const& alts, std::shared_ptr<bot_trace::Tracer> const& trace)
{
    policyOptions.RaceOf = [&races](std::uint64_t botGuid) { return races.Get(botGuid); };
    policyOptions.SideOf = [&races](std::uint64_t botGuid) { return SideOfRace(races.Get(botGuid)); };
    policyOptions.PartySideOf = [&races](std::uint64_t botGuid) -> std::uint8_t
    {
        std::uint32_t const race = races.Get(botGuid);
        if (race == 95)
            return 1; // Skyborne (Alliance)
        if (race == 96)
            return 2; // Skyborne (Horde)
        return SideOfRace(race);
    };
    policyOptions.ClassOf = [&classes](std::uint64_t botGuid) { return classes.Get(botGuid); };
    policyOptions.SpecOf = [&run](std::uint64_t botGuid) { return run.Specs.Get(botGuid); };
    policyOptions.AutoGearing = settings.AutoGearing;
    policyOptions.TalentRespec = settings.TalentRespec;
    policyOptions.AcceptInvites = settings.ChatCommands;
    policyOptions.WhisperReplies = settings.ChatWhisperReplies;
    policyOptions.WhisperCooldownMs = settings.WhisperCooldownMs;
    policyOptions.WhisperMaxPerMin = settings.WhisperMaxPerMin;
    policyOptions.IsAlt = [alts](std::uint64_t guid) { return alts->Has(guid); };
    // The one event log of the policy: " arm=A|B" when ArmSplit is on (ab_split::Tag), and a traced bot's line also to its trace file (TraceBots).
    policyOptions.OnEvent = [trace](std::string const& line)
    {
        std::cerr << "forever-bots: event " << ab_split::Tag(line) << "\n";
        if (trace)
            trace->Event(line);
    };
    policyOptions.AutoGearEveryLevels = settings.AutoGearEveryLevels;
}

std::shared_ptr<PolicyOptions> ArmBOptions(PolicyOptions const& armA, BotSettings& settings, pick::ClassTable& races, pick::ClassTable& classes, RunContext& run,
                                           std::shared_ptr<AltRegistry> const& alts, std::shared_ptr<bot_trace::Tracer> const& trace)
{
    if (!settings.ArmSplit || !settings.ArmB)
        return nullptr;
    BotSettings& b = *settings.ArmB;
    std::string applied;
    for (std::string const& item : settings.ArmBApplied)
        applied += (applied.empty() ? "" : ", ") + item;
    std::cerr << "forever-bots: arm split on: "
              << (settings.ArmSplitMode == ab_split::SplitMode::Strata ? "strata, each (Skyborne|race) x class stratum alternates A/B at first sight"
                                                                       : "even guids are arm A, odd guids arm B")
              << "; arm B overrides: " << (applied.empty() ? std::string("none (an A=B run)") : applied)
              << "; the table lines up to 'arm B ready' are arm B's\n";
    auto out = std::make_shared<PolicyOptions>();
    PolicyFromSettings(*out, b);
    out->DungeonsByMap = armA.DungeonsByMap;   // the dungeon files are the process's (reloaded for both arms, BotPolicy::SetDungeons)
    LoadPolicyTables(*out, b);
    ConnectPolicyLookups(*out, b, races, classes, run, alts, trace);
    // The same file read twice is one table: arm B uses arm A's copy (the sim's RAM).
    if (b.QuestFile == settings.QuestFile && b.AttunementFile == settings.AttunementFile)
        out->Quests = armA.Quests;
    if (b.QuestFileAlliance == settings.QuestFileAlliance && b.QuestFileHorde == settings.QuestFileHorde && b.QuestFileAlliance60 == settings.QuestFileAlliance60 &&
        b.QuestFileHorde60 == settings.QuestFileHorde60)
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 2; ++j)
                out->QuestLists[i][j] = armA.QuestLists[i][j];
    if (b.GrindFile == settings.GrindFile)
    {
        out->Grind = armA.Grind;
        if (b.PathVariety.Enabled && settings.PathVariety.Enabled && b.PathZoneFile == settings.PathZoneFile)
            out->PathZones = armA.PathZones;
    }
    if (b.RotationFile == settings.RotationFile)
        out->Rotations = armA.Rotations;
    if (b.SpellCostFile == settings.SpellCostFile)
        out->SpellCosts = armA.SpellCosts;
    if (b.TameBeastFile == settings.TameBeastFile)
        out->TameBeasts = armA.TameBeasts;
    std::cerr << "forever-bots: arm B ready\n";
    return out;
}

std::shared_ptr<bot_trace::Tracer> MakeTracer(BotSettings const& settings)
{
    if (settings.TraceBots.empty())
        return nullptr;
    if (settings.TraceDir.empty())
    {
        std::cerr << "forever-bots: TraceBots is set but TraceDir is empty; no trace\n";
        return nullptr;
    }
    bot_trace::Options options;
    options.Bots = settings.TraceBots;
    options.Dir = settings.TraceDir;
    options.MaxBytesPerBot = std::uint64_t(settings.TraceMaxMB) * 1024 * 1024;
    auto tracer = std::make_shared<bot_trace::Tracer>(std::move(options));
    std::cerr << "forever-bots: tracing " << settings.TraceBots.size() << " bot(s) to " << settings.TraceDir << "/bot-<guid>.trace, at most " << settings.TraceMaxMB
              << " MB each\n";
    return tracer;
}

// A scenario: the policy options it overrides, and each dungeon's plan and route for the pump.
void SetUpScenario(PolicyOptions& policyOptions, PumpExtras& extras, BotSettings& settings, std::vector<dungeon_events::Event> const& allEvents)
{
    // The scenario takes over the party, the route and the dressing: no vendor walks, no ramp, one party of each roster. Every dungeon's roster has one size
    // (bot_settings.h), so the bots in guid order cut into parties of that size, the first dungeon's parties first.
    policyOptions.PartySize = std::uint32_t(settings.Scenario.Roster.size());
    policyOptions.PartyCount = ScenarioPartyCount(settings.Scenarios);
    policyOptions.PartyPercent = 100; // every bot of the roster is in its party
    policyOptions.DynamicParties.Enabled = false; // and stays in it: a scenario's parties (ZG, Onyxia, MC/BWL) are assigned groups
    policyOptions.RandomPct = 0;      // and a scenario is a test run: it is not varied
    policyOptions.Crafting = false;
    policyOptions.Professions.Enabled = false; // no profession asks or node walks in a dungeon test run
    policyOptions.ServiceEveryMs = 0;
    policyOptions.AutoGearing = false;
    // 40-man raid support (doc 108): a raid scenario's roster is one raid group of 8 subgroups, not a flat party (bot_policy.cpp's
    // party-formation already cuts one party of PartySize bots regardless of size; Raid only changes how that one party is
    // handled once formed). settings.Scenarios front is used the same way policyOptions.PartySize already is above, since a raid
    // runs on its own schedule (doc 108), never mixed into the dungeon list.
    policyOptions.Raid = settings.Scenarios.empty() ? false : settings.Scenarios.front().Raid;
    for (ScenarioSettings const& scenario : settings.Scenarios)
    {
        PolicyOptions::ScenarioRoute route;
        for (auto const& point : scenario.Route)
            route.Route.push_back({ point[0], point[1], point[2] });
        for (auto const& [point, entry] : scenario.BossAt)
            route.RouteBossAt[point] = entry;
        route.Events = dungeon_events::ForMap(allEvents, std::uint32_t(scenario.Entry[0]));
        route.RouteMap = static_cast<std::uint32_t>(scenario.Entry[0]);
        for (std::size_t i = 0; i < scenario.KillPriority.size(); ++i)
            route.KillPriority.emplace(scenario.KillPriority[i], std::uint32_t(i));
        // the raid rule table's adds of this map (the dungeon file's own ranks stay)
        if (policyOptions.RaidRules)
            raid_rules::MergeKillPriority(*policyOptions.RaidRules, route.RouteMap, route.KillPriority);
        // The dungeon's summons only where DungeonSummons lists this scenario (off by default: the route then looks as it did before).
        if (std::find(settings.DungeonSummons.begin(), settings.DungeonSummons.end(), scenario.Name) != settings.DungeonSummons.end())
            route.Summons = scenario.Summons;

        ScenarioPlan plan;
        plan.Name = scenario.Name;
        plan.RosterSize = std::uint32_t(scenario.Roster.size());
        plan.Raid = scenario.Raid;
        plan.EntryMap = static_cast<std::uint32_t>(scenario.Entry[0]);
        plan.Entry = { scenario.Entry[1], scenario.Entry[2], scenario.Entry[3] };
        for (auto const& point : scenario.Route)
            plan.Route.push_back({ point[0], point[1], point[2] });
        plan.DurationMs = std::uint64_t(scenario.DurationSec) * 1000;
        plan.Gear = scenario.Gear;
        plan.Supplies = scenario.Supplies;
        plan.ClassSupplies = scenario.ClassSupplies;
        plan.ClassQuests = scenario.ClassQuests;
        plan.AmmoCount = scenario.Ammo;
        plan.Bosses = scenario.Bosses;
        plan.ClassOf = policyOptions.ClassOf;
        plan.RaceOf = policyOptions.RaceOf;
        plan.TameEnabled = scenario.HasTameSpot;
        plan.TameMap = static_cast<std::uint32_t>(scenario.TameSpot[0]);
        plan.TameSpot = { scenario.TameSpot[1], scenario.TameSpot[2], scenario.TameSpot[3] };
        plan.WipeRecovery = scenario.WipeRecovery;
        plan.WipeRecoveryMs = std::uint64_t(scenario.WipeRecoverySec) * 1000;
        plan.WipeReadyPct = settings.WipeReadyPct;
        plan.WipeReadyMaxMs = settings.WipeReadyMaxMs;
        plan.WipeBackoffCount = settings.WipeBackoffCount;
        plan.WipeBackoffMs = settings.WipeBackoffMs;
        plan.RequireBosses = scenario.RequireBosses;
        plan.VerifyConfig = settings.ScenarioConfigGuard;
        plan.MinAlive = scenario.MinAlive;
        plan.LeaderDeadAbandonMs = std::uint64_t(scenario.LeaderDeadAbandonSec) * 1000;
        if (scenario.HasPortal)
        {
            route.HasPortal = true;
            route.PortalMap = static_cast<std::uint32_t>(scenario.Portal[0]);
            route.PortalPoint = { scenario.Portal[1], scenario.Portal[2], scenario.Portal[3] };
            route.PortalInsideMap = plan.EntryMap;
        }
        extras.Scenarios.push_back(PumpExtras::ScenarioDungeon{ std::move(plan), scenario.Parties, std::move(route) });
    }
    // The single-dungeon fields are the first dungeon's: with one dungeon they are the scenario's, with several each party's own comes from SetScenarioRouteFor.
    PolicyOptions::ScenarioRoute const& first = extras.Scenarios.front().Route;
    policyOptions.Route = first.Route;
    policyOptions.RouteBossAt = first.RouteBossAt;
    policyOptions.Events = first.Events;
    policyOptions.RouteMap = first.RouteMap;
    policyOptions.KillPriority = first.KillPriority;
    policyOptions.Summons = first.Summons;
    policyOptions.HasPortal = first.HasPortal;
    policyOptions.PortalMap = first.PortalMap;
    policyOptions.PortalPoint = first.PortalPoint;
    policyOptions.PortalInsideMap = first.PortalInsideMap;
    extras.ScenarioResultFile = settings.Scenario.ResultFile;
    extras.RunTelemetryDir = settings.RunTelemetryDir;
}

} // namespace main_detail
