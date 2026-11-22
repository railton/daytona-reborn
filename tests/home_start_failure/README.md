# Home startup failure control-flow fixture

The build extracts the actual begin_solo callback from src/app/main.cpp and
compiles it with mock Game, Launcher, Audio and start_game collaborators.
Extraction markers are checked; changes to the callback are rebuild dependencies.
The six scenarios cover ROM preflight failure, loading failure, rejected fresh
session, accepted selection, invalid course and invalid transmission.

    cmake -S tests/home_start_failure -B build/home-start-failure
    cmake --build build/home-start-failure --config Release
    ctest --test-dir build/home-start-failure -C Release --output-on-failure

The rejected-session check failed before game cleanup was added and passes now.
This is actual callback control-flow validation, not a full GUI/SDL fault injection,
thread/audio-device test, gamepad test or guest gearbox validation. The real
start_game catch already destroys a failed partial game; the mock reflects that
contract. Keep independent source review of the caller when its contract changes.
