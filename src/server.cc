/*
 * Copyright (C) 2004,2005 Daniel Heck
 * Copyright (C) 2007,2008,2009 Ronald Lamprecht
 * 2026 LLM generated contribution - concept, review and revision by Ferdinand Strixner
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#include "server.hh"

#include "errors.hh"
#include "game.hh"
#include "actors.hh"
#include "client.hh"
#include "lua.hh"
#include "lev/Index.hh"
#include "lev/PersistentIndex.hh"
#include "lev/Proxy.hh"
#include "main.hh"
#include "nls.hh"
#include "options.hh"
#include "player.hh"
#include "player.hh"
#include "input.hh"
#include "multiplayer_rollback.hh"
#include "multiplayer_state.hh"
#include "StateManager.hh"
#include "world.hh"
#include "MusicManager.hh"

#include "enet/enet.h"

#ifdef WIN32
// SendMessage is a Syscall on Windows, so we simply undefine it to use this
// name for one of the methods
#undef SendMessage
#endif

#include <cctype>

using namespace std;

namespace enigma {
namespace server {

enum ServerState {
    sv_idle,
    sv_waiting_for_clients,
    sv_running,
    sv_paused,
    sv_teatime,
    sv_restart_level,
    sv_restart_game,
    sv_finishing,
    sv_finished,
};

class Server {
public:
private:
    static Server *instance;
};

void PrepareLevel();

/* -------------------- Global variables -------------------- */

double LastMenuTime;
int MenuCount;
lev::Proxy *LoadedProxy;
unsigned SublevelNumber;
std::string SublevelTitle;

bool NoCollisions = false;

bool AllowSingleOxyds;
bool AllowSuicide;
bool AllowTogglePlayer;
bool AutoRespawn;
bool CreatingPreview = false;  // read only for Lua
bool ConserveLevel;
bool IsDifficult;     // read only for Lua
bool IsLevelRestart;  // no Lua access
bool ProvideExtralifes;
bool InfiniteReincarnation;
bool SurviveFinish;
int AddSecondsToScore;

Value FollowAction;
bool FollowGrid;
int FollowMethod;
Value FollowThreshold;

double LevelTime;  // read only for Lua (> 1.10)
bool ShowMoves;
bool SingleComputerGame;     // no Lua access
bool TwoPlayerGame;          // no Lua access
GameType GameCompatibility;  // no Lua access
bool WorldSized;             // no Lua access
bool WorldInitialized;       // no Lua access
double Brittleness;
double Fragility;
double CrackSpreading;
ecl::V2 ConstantForce;
double BumperForce;
double ElectricForce;
double EnigmaCompatibility;  // no Lua access
double FlatForce;
double FrictionFactor;
int GlassesVisibility;  // no Lua access
int ExtralifeGlasses;
std::string FallenPuzzle;
double HoleForce;
lev::levelStatusType LevelStatus;  // no Lua access
double MagnetForce;
double MagnetRange;
int MaxOxydColor;
int32_t RandomState;  // no Lua access
double RubberViolationStrength;
double SlopeForce;
int SubSoil;
double SwampSinkSpeed;
double WaterSinkSpeed;
double WormholeForce;
double WormholeRange;

/* -------------------- Local variables -------------------- */

namespace {

ServerState state = sv_idle;
ServerState state_before_teatime = sv_idle;
double time_accu = 0;
double current_state_dtime = 0;
int move_counter;                 // counts movements of stones
lev::Index *currentIndex = NULL;  // volatile F6 jump back history
int currentLevel;
lev::Index *previousIndex = NULL;
int previousLevel;
ENetAddress network_address;
ENetHost *network_host = 0;

void collect_inputs_for_tick(uint32_t tick, std::array<input::PlayerInput, input::kMaxPlayers> &out,
                             unsigned players) {
    for (auto &pi : out)
        pi = input::PlayerInput();
    for (unsigned player = 0; player < players; ++player) {
        out[player] = input::ConsumeInput(tick, player);
    }
}

}  // namespace

void SimulateOneTick(double timestep) {
    LevelTime += timestep;
    const uint32_t tick = input::CurrentTick();
    unsigned players = input::IsNetworked() ? input::ExpectedPlayers() : player::PlayerCount();
    if (players == 0)
        players = 1;
    std::array<input::PlayerInput, input::kMaxPlayers> inputs;
    collect_inputs_for_tick(tick, inputs, players);

    // Apply discrete actions once per tick; distribute continuous mouse force
    // across smaller physics steps for stable, controllable motion at large tick sizes.
    for (unsigned player = 0; player < players; ++player) {
        const input::PlayerInput &pi = inputs[player];
        player::InhibitPickup(player, pi.activate_count > 0 || pi.rotate_steps != 0);
        if (pi.rotate_steps != 0) {
            int dir = (pi.rotate_steps > 0) ? 1 : -1;
            int steps = (pi.rotate_steps > 0) ? pi.rotate_steps : -pi.rotate_steps;
            for (int i = 0; i < steps; ++i)
                player::RotateInventory(player, dir);
        }
        if (pi.activate_count > 0) {
            for (int i = 0; i < pi.activate_count; ++i)
                Msg_ActivateItem(player);
        }
    }

    constexpr double kMaxPhysicsStep = 0.01;  // original engine tick (10ms)
    int substeps = 1;
    if (timestep > kMaxPhysicsStep)
        substeps = static_cast<int>(std::ceil(timestep / kMaxPhysicsStep));
    if (substeps < 1)
        substeps = 1;
    const double sub_dt = timestep / static_cast<double>(substeps);
    const double inv_substeps = 1.0 / static_cast<double>(substeps);
    for (int step = 0; step < substeps; ++step) {
        for (unsigned player = 0; player < players; ++player) {
            const input::PlayerInput &pi = inputs[player];
            if (pi.mouse_force[0] != 0.0 || pi.mouse_force[1] != 0.0) {
                Msg_MouseForce(player, pi.mouse_force * inv_substeps);
            }
        }
        WorldTick(sub_dt);
    }
    input::AdvanceTick();
}

void load_level(lev::Proxy *levelProxy, bool isRestart) {
    try {
        Uint32 start_tick_time = SDL_GetTicks();  // meassure time for level loading
        if (!CreatingPreview)
            sound::StartLevelLoadMusic();

        LoadedProxy = levelProxy;
        IsLevelRestart = isRestart;
        PrepareLevel();

        // clear inventory before level load and give us 2 extralives
        player::NewGame();

        levelProxy->loadLevel();  // sets the compatibility mode
        multiplayer::PrepareExtraActors();

        game::ResetGameTimer();

        WorldInitLevel();
        if (!CreatingPreview) {
            player::LevelLoaded(isRestart);
            client::Msg_LevelLoaded(isRestart);
            multiplayer::SetupExtraPlayerStartPositions();
        }
        double exectime = (SDL_GetTicks() - start_tick_time) / 1000.0;
        Log << ecl::strf("Server load level did take %g seconds\n", exectime);
    } catch (XLevelLoading &err) {
        std::string levelPathString = (levelProxy->getNormPathType() == lev::Proxy::pt_resource)
                                          ? levelProxy->getAbsLevelPath()
                                          : levelProxy->getNormFilePath();
        std::string msg =
            _("Server Error: could not load level '") + levelPathString + "'\n" + err.what();
        if (!CreatingPreview) {
            client::Msg_Error(msg);
            state = sv_idle;
        } else {
            throw XLevelLoading(msg);
        }
    } catch (XLevelRuntime &err) {
        std::string levelPathString = (levelProxy->getNormPathType() == lev::Proxy::pt_resource)
                                          ? levelProxy->getAbsLevelPath()
                                          : levelProxy->getNormFilePath();
        std::string msg =
            _("Server Error: could not load level '") + levelPathString + "'\n" + err.what();
        if (!CreatingPreview) {
            client::Msg_Error(msg);
            state = sv_idle;
        } else {
            throw XLevelLoading(msg);
        }
    }
}

void RaiseError(const std::string &msg) {
    throw XLevelLoading(msg);
}

void gametick(double dtime) {
    const double timestep = input::TickTimestep();
    int count = 0;

    time_accu += dtime;
    multiplayer::rollback::MaybeRollback(timestep);
    if (input::IsNetworked() && !input::CanAdvanceTick()) {
        // When the simulation is deterministic-networked we stop advancing the world
        // until all per-tick inputs are available. In that case, accumulating wall-clock
        // time as "simulation debt" just triggers misleading overload warnings and can
        // cause large time steps to hit non-deterministic subsystems.
        if (time_accu > timestep)
            time_accu = timestep;
    }
    if (time_accu > 1.0) {
        fprintf(stderr, "Whoa, system overload!\n");
        time_accu = 1.0;
    }
    player::Tick(time_accu);
    for (; time_accu >= timestep; ) {
        if (!input::CanAdvanceTick())
            break;
        time_accu -= timestep;
        multiplayer::rollback::OnBeforeSimTick(input::CurrentTick());
        SimulateOneTick(timestep);
        count++;
    }
    display::GetStatusBar()->set_counter(server::GetMoveCounter());
    player::MessagePlayerPositionsToClient();
    TickFinished(count * timestep);
}

/* -------------------- Functions -------------------- */

void Init() {
}

void Shutdown() {
    lua::ShutdownLevel();
    if (network_host != 0)
        enet_host_destroy(network_host);
}

void InitNewGame() {
    PrepareLevel();
}

bool NetworkStart() {
    return true;
}

void PrepareLevel() {
    state = sv_waiting_for_clients;

    SublevelNumber = 1;
    SublevelTitle = "";
    LastMenuTime = 0.0;
    MenuCount = 0;
    NoCollisions = false;
    WorldInitialized = false;
    LevelTime = 0.0;
    ConserveLevel = true;
    ProvideExtralifes = true;
    InfiniteReincarnation = false;
    SurviveFinish = true;
    AddSecondsToScore = 0;
    TwoPlayerGame = false;
    SingleComputerGame = !multiplayer::IsActive();
    AllowSingleOxyds = false;
    AllowSuicide = true;
    AllowTogglePlayer = !multiplayer::IsActive();
    AutoRespawn = false;
    FollowAction = GridPos(19, 12);  // inner space of a room
    FollowGrid = true;
    FollowMethod = display::FOLLOW_FLIP;  // FLIP
    FollowThreshold = 0.5;
    ShowMoves = false;

    GlassesVisibility = 0;  // nothing
    FlatForce = 0.0;
    ConstantForce = ecl::V2(0, 0);

    // object class specific with Lua access
    Brittleness = 0.5;
    BumperForce = 200.0;
    CrackSpreading = 0.5;
    ElectricForce = 15.0;
    ExtralifeGlasses = 19;  // death + hollow + lightpassenger
    FallenPuzzle = "fl_gray";
    Fragility = 1.0;
    FrictionFactor = 1.0;
    HoleForce = 1.0;
    MagnetForce = 30;
    MagnetRange = 10;
    MaxOxydColor = 7;  // for compatibility
    RubberViolationStrength = 50;
    SlopeForce = 25.0;
    SubSoil = 0;
    SwampSinkSpeed = 4;
    WaterSinkSpeed = 10000;
    WormholeForce = 30;
    WormholeRange = 10;

    move_counter = 0;
    multiplayer::PrimeInputQueueForNewLevel();

    enigma::WorldPrepareLevel();
    WorldSized = false;

    player::PrepareLevel();
}

void PrepareLua() {
    IsDifficult = (GetDifficulty() == DIFFICULTY_HARD);
    // Restart the Lua environment so symbol definitions from
    // different levels do not get in each other's way.
    int api = (EnigmaCompatibility < 1.10) ? 1 : 2;
    lua::ShutdownLevel();
    lua_State *L = lua::InitLevel(api);
    if (api == 1 && lua::DoSysFile(L, "compat.lua") != lua::NO_LUAERROR) {
        throw XLevelLoading("While processing 'compat.lua':\n" + lua::LastError(L));
    }
    if (lua::DoSysFile(L, ecl::strf("api%dinit.lua", api)) != lua::NO_LUAERROR) {
        throw XLevelLoading(ecl::strf("While processing 'api%dinit.lua':\n", api) +
                            lua::LastError(L));
    }
    if (lua::DoSysFile(L, "security.lua") != lua::NO_LUAERROR) {
        throw XLevelLoading("While processing 'security.lua':\n" + lua::LastError(L));
    }
}

void RestartLevel() {
    if (multiplayer::IsActive() && !multiplayer::IsHost())
        return;
    if (state == sv_running || state == sv_finished || state == sv_finishing || state == sv_paused) {
        if (multiplayer::IsActive() && multiplayer::IsHost())
            multiplayer::NotifyRestart(true);
        state = sv_restart_level;
        current_state_dtime = 0;
    }
}

void RestartLevelFromNetwork() {
    if (state == sv_running || state == sv_finished || state == sv_finishing || state == sv_paused) {
        state = sv_restart_level;
        current_state_dtime = 0;
    }
}

bool IsRestartingLevel() {
    return state == sv_restart_level;
}

void Msg_RestartGame() {
    if (multiplayer::IsActive() && !multiplayer::IsHost())
        return;
    if (state == sv_running || state == sv_finished || state == sv_finishing || state == sv_paused) {
        if (multiplayer::IsActive() && multiplayer::IsHost())
            multiplayer::NotifyRestart(false);
        state = sv_restart_game;
        current_state_dtime = 0;
    }
}

void Msg_RestartGameFromNetwork() {
    if (state == sv_running || state == sv_finished || state == sv_finishing || state == sv_paused) {
        state = sv_restart_game;
        current_state_dtime = 0;
    }
}

void FinishLevel() {
    if (state == sv_running) {
        state = sv_finishing;
    }
}

void Tick(double dtime) {
    switch (state) {
    case sv_idle: break;
    case sv_paused: break;
    case sv_teatime: break;
    case sv_waiting_for_clients: break;
    case sv_running: gametick(dtime); break;
    case sv_restart_level:
    case sv_restart_game:
        current_state_dtime += dtime;
        if (current_state_dtime >= 1.0) {
            // In multiplayer, the local "current index position" can differ between peers
            // (e.g. browsing packs/levels while a game is running). Restart must reload the
            // actually loaded level, not whatever the local index currently points to.
            lev::Proxy *to_reload = LoadedProxy;
            if (!to_reload) {
                lev::Index *ind = lev::Index::getCurrentIndex();
                if (ind)
                    to_reload = ind->getCurrent();
            }
            if (to_reload) {
                load_level(to_reload, (state == sv_restart_level));
            } else {
                Log << "Server restart: no loaded level proxy, aborting restart.\n";
                state = sv_idle;
            }
        } else {
            gametick(dtime);
        }
        break;
    case sv_finishing:
        if (SurviveFinish) {
            player::LevelFinished(0);   // mark all shattered actors as dead
            player::CheckDeadActors();  // restart level or game if actors are dead!
        }
        if (state == sv_finishing) {
            state = sv_finished;
            current_state_dtime = 0;
            player::LevelFinished(1);  // remove player-controlled actors
            client::Msg_Command("level_finished");
        }
        break;
    case sv_finished:
        current_state_dtime += dtime;
        if (current_state_dtime <= 2.5)
            gametick(dtime);
        else {
            if (multiplayer::IsActive()) {
                if (multiplayer::IsHost()) {
                    lev::Index *level_index = lev::Index::getCurrentIndex();
                    // Mirror the single-player path: record the finished level in history.
                    lev::PersistentIndex::addCurrentToHistory();
                    if (level_index && level_index->advanceLevel(lev::ADVANCE_NEXT_MODE)) {
                        lev::Proxy *next = level_index->getCurrent();
                        if (next) {
                            multiplayer::NotifyLoadLevel(level_index->getName(),
                                                         next->getNormLevelPath());
                            Msg_LoadLevel(next, false);
                        } else {
                            multiplayer::RequestAbort();
                            client::Msg_Command("abort");
                        }
                    } else {
                        multiplayer::RequestAbort();
                        client::Msg_Command("abort");
                    }
                }
                // Clients do not advance locally. The host will broadcast a fully qualified
                // level load message (pack + level_id) so everyone transitions in lockstep.
                state = sv_waiting_for_clients;
            } else {
                client::Msg_AdvanceLevel(lev::ADVANCE_NEXT_MODE);
                state = sv_waiting_for_clients;
            }
        }
        break;
    }
}

void Msg_SetLevelPack(const std::string &name) {
    lev::Index::setCurrentIndex(name);
}

void Msg_LoadLevel(lev::Proxy *levelProxy, bool isPreview) {
    CreatingPreview = isPreview;
    if (!isPreview) {
        // update F6 jump back history
        if (currentIndex != lev::Index::getCurrentIndex() ||
            currentLevel != currentIndex->getCurrentPosition()) {
            previousIndex = currentIndex;
            previousLevel = currentLevel;
            currentIndex = lev::Index::getCurrentIndex();
            currentLevel = currentIndex->getCurrentPosition();
        }
    }
    load_level(levelProxy, false);
}

void Msg_JumpBack() {
    if (previousIndex != NULL) {
        lev::Index::setCurrentIndex(previousIndex->getName());
        previousIndex->setCurrentPosition(previousLevel);
        currentIndex = previousIndex;
        currentLevel = previousLevel;
        Msg_LoadLevel(currentIndex->getProxy(currentLevel), false);
        Msg_Command("restart");
    }
}

void Msg_StartGame() {
    if (state == sv_waiting_for_clients) {
        bool defer = multiplayer::ShouldDeferStart();
        if (!defer && multiplayer::IsActive() && multiplayer::IsHost() &&
            multiplayer::ExpectedPlayers() > 1 && !multiplayer::HasRemotePeers()) {
            // Guard against cases where the session hasn't observed a remote peer yet
            // (for example VM networking quirks). The host must not start the
            // simulation until at least one peer is connected/ready.
            defer = true;
        }
        if (defer) {
            // In Internet play it can take a moment until all peers have connected and
            // reported "ready". While we wait, the level may already be visible but
            // simulation/input is intentionally deferred to keep everyone in sync.
            display::GetStatusBar()->try_show_text(_("Other players connecting..."), false, 2.0);
            multiplayer::NotifyStartRequested();
            return;
        }
        time_accu = 0;
        state = sv_running;
    } else {
        // Warning << "server: Received unexpected StartGame message.\n";
        // XXX discard message if not waiting for it?
    }
}

void Msg_Command_jumpto(const string &dest) {
    // global level jump
    // e.g.:  dest = "Enigma IV,33" -> jump to level #33 of index "Enigma IV"
    // note: level counter start at 1 (not at 0)

    size_t comma = dest.find_first_of(',');
    string error;

    if (comma != string::npos) {
        std::string name = dest.substr(0, comma);
        Log << "Jumpto '" << name << "'\n";
        int ilevel = atoi(dest.c_str() + comma + 1) - 1;

        if (lev::Index::setCurrentIndex(name)) {

            if (ilevel >= 0 && ilevel < lev::Index::getCurrentIndex()->size()) {
                lev::Index *curInd = lev::Index::getCurrentIndex();
                curInd->setCurrentPosition(ilevel);
                Msg_LoadLevel(curInd->getProxy(ilevel), false);
                Msg_Command("restart");
            } else {
                error = ecl::strf("Illegal level %i (1-%i)", ilevel + 1,
                                  lev::Index::getCurrentIndex()->size());
                // May be we want to reset the current index ?
            }
        } else
            error = ecl::strf("Illegal level pack %s", name.c_str());
    } else if (lev::Index::setCurrentIndex(dest)) {
        lev::Index *curInd = lev::Index::getCurrentIndex();
        Msg_LoadLevel(curInd->getCurrent(), false);
        Msg_Command("restart");
    } else {
        error = "Syntax: jumpto pack[,level]";
    }

    if (!error.empty())
        client::Msg_ShowText(error, false, 2);
}

void Msg_Command_find(const string &text) {
    std::string indName = lev::Proxy::search_shallow(text);
    if (!indName.empty()) {
        lev::Index::setCurrentIndex(indName);
        lev::Index *searchResult = lev::Index::getCurrentIndex();
        searchResult->setCurrentPosition(0);
        if (searchResult->size() == 1) {
            // play unique level directly
            Msg_LoadLevel(searchResult->getProxy(0), false);
            Msg_Command("restart");
        } else {
            // display multiple levels as pack
            Msg_Command("abort");
        }
    } else {
        client::Msg_ShowText(string("Couldn't find '") + text + '\'', false, 2);
    }
}

void Msg_Command(const string &cmd) {
    lev::Index *ind = lev::Index::getCurrentIndex();
    lev::Proxy *curProxy = ind->getCurrent();

    // ------------------------------ normal commands
    if (cmd == "invrotate") {
        player::RotateInventory();
    } else if (cmd == "suicide") {
        player::Suicide();
        if (!AllowSuicide)
            Msg_RestartGame();
    } else if (cmd == "restart") {
        player::Suicide();
        Msg_RestartGame();
    } else if (cmd == "abort") {
        client::Msg_Command(cmd);
    }

    // ------------------------------ cheats
    else if (cmd == "god") {
        BroadcastMessage("_shield", player::CurrentPlayer(), GRID_NONE_BIT, true);
        client::Msg_Command("cheater");
    } else if (cmd == "collision") {
        NoCollisions = !NoCollisions;
        if (NoCollisions)
            client::Msg_ShowText("collision handling disabled", false, 2);
        else
            client::Msg_ShowText("collision handling enabled", false, 2);
        client::Msg_Command("cheater");
        display::GetStatusBar()->setCMode(NoCollisions);
    }

    // ------------------------------ quick options
    else if (cmd == "easy") {
        if (app.state->getInt("Difficulty") == DIFFICULTY_HARD) {
            if (curProxy->hasEasyMode()) {
                client::Msg_ShowText("Restarting in easy mode", false, 2);
                app.state->setProperty("Difficulty", DIFFICULTY_EASY);
                Msg_Command("restart");
            } else
                client::Msg_ShowText("No easy mode available.", false, 2);
        } else
            client::Msg_ShowText("Already in easy mode.", false, 2);
    } else if (cmd == "regular" || cmd == "noeasy") {
        if (app.state->getInt("Difficulty") == DIFFICULTY_EASY) {
            app.state->setProperty("Difficulty", DIFFICULTY_HARD);
            if (curProxy->hasEasyMode()) {
                client::Msg_ShowText("Restarting in regular difficulty mode", false, 2);
                Msg_Command("restart");
            } else {
                client::Msg_ShowText("No difference between easy and regular difficulty.", false,
                                     2);
            }
        } else
            client::Msg_ShowText("Already in normal mode.", false, 2);
    } else if (cmd == "hunt") {
        if (app.state->getInt("NextLevelMode") != lev::NEXT_LEVEL_NOT_BEST) {
            client::Msg_ShowText("Restarting in world record hunt mode", false, 2);
            app.state->setProperty("NextLevelMode", lev::NEXT_LEVEL_NOT_BEST);
            Msg_Command("restart");
        } else
            client::Msg_ShowText("Already in world record hunt mode.", false, 2);
    } else if (cmd == "nohunt") {
        if (app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST) {
            client::Msg_ShowText("Switched back to standard mode", false, 2);
            app.state->setProperty("NextLevelMode", lev::NEXT_LEVEL_STRICTLY);
            client::Msg_Command("easy_going");
        } else
            client::Msg_ShowText("Already in standard mode.", false, 2);
    } else if (cmd == "info") {
        string infotext = ecl::strf("Level #%i of '", ind->getCurrentLevel()) + ind->getName() +
                          "' (" + curProxy->getAbsLevelPath() + ")  -  \"" + curProxy->getTitle() +
                          "\" by " + curProxy->getAuthor() +
                          ecl::strf(" (rev=%i,", curProxy->getReleaseVersion()) + "id=\"" +
                          curProxy->getId() + "\")";

        client::Msg_ShowText(infotext, true);
    } else if (cmd.substr(0, 5) == "find ") {  // global level-jump
        string args = cmd.substr(5);
        Msg_Command_find(args);
    } else if (cmd.substr(0, 7) == "jumpto ") {  // global level-jump
        string args = cmd.substr(7);
        Msg_Command_jumpto(args);
    } else if (cmd == "help") {
        client::Msg_ShowText(
            "suicide, restart, abort, easy, regular, hunt, nohunt, jumpto, find, info", true);
    } else if (cmd == "cheats") {
        client::Msg_ShowText("god, collision  -- Be aware: you'll get no medals!", true);
    } else if (cmd == "April 1st") {
        client::Msg_ShowText("No pizza, and a cloudless sky.", true);
    }

    else {
        enigma::Log << "Warning: Server received unknown command '" << cmd << "'\n";
    }
}

void Msg_Pause(bool onoff) {
    if (onoff && state == sv_running)
        state = sv_paused;
    else if (onoff && state == sv_teatime)
        state = sv_paused;
    else if (!onoff && state == sv_paused)
        state = sv_running;
}

void Msg_Teatime(bool onoff) {
    if (onoff) {
        state_before_teatime = state;
        state = sv_teatime;
    } else
        state = state_before_teatime;
}

void Msg_Panic(bool onoff) {
    if (onoff && state == sv_running)
        state = sv_idle;
    else if (!onoff && state == sv_idle)
        state = sv_running;
}

void Msg_MouseForce(const ecl::V2 &f) {
    Msg_MouseForce(player::CurrentPlayer(), f);
}

void Msg_MouseForce(unsigned player, const ecl::V2 &f) {
    SetMouseForce(player, f);
}

void SetCompatibility(const char *version) {
    GameType type = GetGameType(version);

    if (type == GAMET_UNKNOWN) {
        fprintf(stderr, "Invalid compatibility mode '%s' (ignored. using enigma behavior)\n",
                version);
        fprintf(stderr, "Valid modes:");
        for (int v = 0; v < GAMET_COUNT; ++v)
            fprintf(stderr, " %s", GetGameTypeName((GameType)v).c_str());
        fprintf(stderr, "\n");
        type = GAMET_ENIGMA;
    }

    GameCompatibility = type;
}

void SetCompatibility(lev::Proxy *levelProxy) {
    GameCompatibility = levelProxy->getEngineCompatibility();
    if (GameCompatibility == GAMET_UNKNOWN)
        throw XLevelLoading("unknown engine compatibility");
}

enigma::Difficulty GetDifficulty() {
    if (CreatingPreview)
        return DIFFICULTY_HARD;  // we may not access the current index!

    lev::Index *ind = lev::Index::getCurrentIndex();
    lev::Proxy *curProxy = ind->getCurrent();
    int i = app.state->getInt("Difficulty");
    if (i == DIFFICULTY_EASY && curProxy->hasEasyMode())
        return DIFFICULTY_EASY;
    else
        return DIFFICULTY_HARD;
}

void InitMoveCounter() {
    move_counter = 0;
}

int IncMoveCounter(int increment) {
    move_counter += increment;
    return move_counter;
}

int GetMoveCounter() {
    return move_counter;
}

void Msg_ActivateItem() {
    Msg_ActivateItem(player::CurrentPlayer());
}

void Msg_ActivateItem(unsigned player) {
    player::ActivateFirstItem(player);
}

}  // namespace server
}  // namespace enigma
