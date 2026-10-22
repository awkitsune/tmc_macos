// Force-included (-include) into the sources whose writable globals are game
// state (CMakeLists.txt): their data and bss go to the __TMCSTATE segment, which
// save states snapshot (port/include/port/state.h). Variables with an explicit
// section (EWRAM_DATA, STATE_EXCLUDE) are placed by that instead.
#pragma clang section bss = "__TMCSTATE,__bss" data = "__TMCSTATE,__data"
