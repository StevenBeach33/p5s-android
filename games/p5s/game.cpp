// Persona 5 Strikers (Switch; Koei Tecmo/Omega Force engine, Nintendo SDK 8.3.2, NVN): what the shared runtime needs to know about the game. Nothing in here is
// patched or hooked yet: the first runs on a phone show what the runtime still lacks (missing services and NVN calls
// are in the log, docs/RUNTIME.md), and each fix goes in the runtime or, when only this game needs it, here.
//
// Unknown until a dump has been looked at (so left to the player's own game file, not guessed):
//  - the program (title) ID (the ticket of a dump of the base game is named 010087701b092000...): 0 here means "the one in the game file" (runtime svc_process.cpp, svcGetInfo ProgramId);
//  - the system resource size: all ones means "main.npdm's" (same place);
//  - the build ID: null, any build is accepted. Once a version is the reference, put the start of its module ID here
//    (log line "main .tdata" is preceded by the build in a refusal message) so a different one is refused.
// The shaders: games/p5s/import scans every RomFS file for NVN (GLSLC) program blocks, whatever the container.
// Strikers' SDK is 8.3.2 (the runtime was written against 16.2): expect missing/older nn:: service versions in the log.
#include <cstdlib>

#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"p5s", "Persona 5 Strikers", 0 /* from the player's game file */,
                                "io.github.p5sandroid", "private/p5s", ~0ull /* from main.npdm */,
                                nullptr /* any build */};

void game::after_init() {
    // The build's own time (build_apk_nce.sh touches this file, so it is current): a log says which build ran.
    log("build: p5s %s %s", __DATE__, __TIME__);
    // Sends the SDK module's own libc file calls ("rom:/...") to the same nn::fs HLE as the game's imports (harmless
    // if Strikers never uses them; it also traces opens in the log).
    hook_sdk_fs();
    // Shaders the import did not cover (nx.log "no .../spirv/<hash>.spv"): their code and control go to <data>/shader_misses/.
    setenv("NX_DUMP_SHADER_MISSES", "1", 0);
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
