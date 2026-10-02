#pragma once
// Estado compartilhado entre o nucleo (hook de pacotes) e a interface ImGui.

#include <atomic>
#include <cstddef>

#include <pl/Mod.hpp>

namespace pp {

extern std::atomic_bool gPause;
extern std::atomic_bool gCancel;
extern std::atomic_bool gNoClose;
extern std::atomic_bool gFlushReq;
extern std::atomic_bool gKeepMove;
extern std::atomic_bool gLogIds;
extern std::atomic<size_t> gQueued; // pacotes na fila
extern ll::mod::NativeMod *gMod;

namespace ui {
void install();   // hook do eglSwapBuffers + callback de toque
void uninstall(); // desliga o overlay
} // namespace ui

} // namespace pp
