// Overlay ImGui do PacketPause.
//  - desenha dentro do eglSwapBuffers (hook)
//  - recebe toque pela Input API do LeviLauncher (consome so o que cai no menu)

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <dlfcn.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <pl/Input.hpp>
#include <pl/memory/Hook.hpp>

#include "Shared.hpp"

namespace pp::ui {
namespace {

using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);

SwapFn gOrigSwap = nullptr;
pl::memory::FuncPtr gSwapTarget = nullptr;
bool gHooked = false;
std::atomic_bool gActive{false};
bool gTouchRegistered = false;

// Estado de ImGui (somente a thread de render mexe aqui).
ImGuiContext *gImCtx = nullptr;
EGLContext gGlCtx = EGL_NO_CONTEXT;
bool gBackendReady = false;
std::chrono::steady_clock::time_point gLastFrame{};
bool gOpen = false;
float gScaleSetting = 0.0f; // 0 = automatico
float gAppliedScale = 0.0f;
bool gFirstFrameLogged = false;

// Retangulos do menu (para decidir se o toque e do menu ou do jogo).
struct Rect {
  float x{}, y{}, w{}, h{};
  bool valid{};
};
std::mutex gRectMutex;
Rect gRects[2];

// Fila de eventos de toque (callback de toque -> thread de render).
struct Ev {
  int type; // 0 = down, 1 = move, 2 = up
  float x, y;
};
std::mutex gEvMutex;
std::vector<Ev> gEvents;
std::atomic_int gPointer{-1};
std::atomic_int gTouchLogs{0};

constexpr int kActionDown = 0;
constexpr int kActionUp = 1;
constexpr int kActionMove = 2;
constexpr int kActionCancel = 3;
constexpr int kActionPointerDown = 5;
constexpr int kActionPointerUp = 6;

// ------------------------------------------------------------------ toque
bool insideMenu(float x, float y) {
  std::lock_guard lock(gRectMutex);
  constexpr float kMargin = 8.0f;
  for (const auto &r : gRects) {
    if (r.valid && x >= r.x - kMargin && x <= r.x + r.w + kMargin &&
        y >= r.y - kMargin && y <= r.y + r.h + kMargin) {
      return true;
    }
  }
  return false;
}

void pushEvent(int type, float x, float y) {
  std::lock_guard lock(gEvMutex);
  if (gEvents.size() < 256) {
    gEvents.push_back({type, x, y});
  }
}

bool onTouch(const pl::input::TouchEvent &e) {
  if (!gActive.load()) {
    return false;
  }
  const int action = e.action & 0xFF;

  if (gTouchLogs.load() < 12 && gMod) {
    ++gTouchLogs;
    gMod->getLogger().info("touch action={} id={} x={} y={}", e.action, e.pointerId,
                           e.x, e.y);
  }

  switch (action) {
  case kActionDown:
  case kActionPointerDown:
    if (gPointer.load() == -1 && insideMenu(e.x, e.y)) {
      gPointer = e.pointerId;
      pushEvent(0, e.x, e.y);
      return true;
    }
    return false;
  case kActionMove:
    if (e.pointerId == gPointer.load()) {
      pushEvent(1, e.x, e.y);
      return true;
    }
    return false;
  case kActionUp:
  case kActionPointerUp:
  case kActionCancel:
    if (e.pointerId == gPointer.load()) {
      pushEvent(2, e.x, e.y);
      gPointer = -1;
      return true;
    }
    return false;
  default:
    return false;
  }
}

void processEvents(ImGuiIO &io) {
  std::vector<Ev> local;
  {
    std::lock_guard lock(gEvMutex);
    local.swap(gEvents);
  }
  for (const auto &e : local) {
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(e.x, e.y);
    if (e.type == 0) {
      io.AddMouseButtonEvent(0, true);
    } else if (e.type == 2) {
      io.AddMouseButtonEvent(0, false);
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // tira o "hover" apos soltar
    }
  }
}

// -------------------------------------------------------------------- UI
void storeRect(int index) {
  const ImVec2 p = ImGui::GetWindowPos();
  const ImVec2 s = ImGui::GetWindowSize();
  std::lock_guard lock(gRectMutex);
  gRects[index] = {p.x, p.y, s.x, s.y, true};
}

void clearRect(int index) {
  std::lock_guard lock(gRectMutex);
  gRects[index].valid = false;
}

bool toggleButton(const std::string &text, const char *id, bool highlight,
                  float width) {
  // O ID do botao fica fixo (##id); so o texto e a cor mudam com o estado.
  const std::string label = text + "##" + id;

  if (highlight) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.45f, 0.20f, 1.0f));
  }
  const bool pressed = ImGui::Button(label.c_str(), ImVec2(width, 0.0f));
  if (highlight) {
    ImGui::PopStyleColor(3);
  }
  return pressed;
}

void drawUi() {
  const float scale = gAppliedScale > 0.0f ? gAppliedScale : 1.0f;

  // Botao pequeno que abre/fecha o painel (sempre visivel).
  ImGui::SetNextWindowPos(ImVec2(12.0f * scale, 70.0f * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.65f);
  ImGui::Begin("##pp_chip", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
  if (ImGui::Button(gOpen ? "PP  -" : "PP")) {
    gOpen = !gOpen;
  }
  storeRect(0);
  ImGui::End();

  if (!gOpen) {
    clearRect(1);
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(12.0f * scale, 150.0f * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 21.0f, 0.0f),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGui::Begin("Packet Pause", &gOpen,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

  const float full = ImGui::GetContentRegionAvail().x;

  // Delay Packets: false | true
  const bool delay = gPause.load();
  if (toggleButton(delay ? "Delay Packets: true" : "Delay Packets: false", "delay", delay,
                   full)) {
    const bool was = gPause.exchange(!delay);
    if (was && delay) {
      gFlushReq = true; // desligou o delay: libera tudo de uma vez
    }
  }

  // Send Packets: true | false  (false = descarta os pacotes)
  const bool cancel = gCancel.load();
  if (toggleButton(cancel ? "Send Packets: false" : "Send Packets: true", "send", cancel,
                   full)) {
    gCancel = !cancel;
  }

  // Libera a fila agora, sem desligar o Delay Packets
  if (ImGui::Button("Flush (releases delay packets)##flush", ImVec2(full, 0.0f))) {
    gFlushReq = true;
  }

  // Close without Packet: fechar o menu sem avisar o servidor
  const bool noClose = gNoClose.load();
  if (toggleButton(noClose ? "Close without Packet  [ON]" : "Close without Packet",
                   "noclose", noClose, full)) {
    gNoClose = !noClose;
  }

  ImGui::Separator();
  ImGui::Text("Fila: %zu pacotes", static_cast<size_t>(gQueued.load()));

  bool keepMove = gKeepMove.load();
  if (ImGui::Checkbox("Manter movimento", &keepMove)) {
    gKeepMove = keepMove;
  }
  bool logIds = gLogIds.load();
  if (ImGui::Checkbox("Logar IDs de pacote", &logIds)) {
    gLogIds = logIds;
  }

  float s = gScaleSetting > 0.0f ? gScaleSetting : gAppliedScale;
  if (ImGui::SliderFloat("Escala", &s, 1.0f, 4.0f, "%.1f")) {
    gScaleSetting = s;
  }

  storeRect(1);
  ImGui::End();
}

void applyScale(float s) {
  ImGuiStyle style;
  ImGui::StyleColorsDark(&style);
  style.ScaleAllSizes(s);
  style.WindowRounding = 6.0f * s;
  style.FrameRounding = 4.0f * s;
  ImGui::GetStyle() = style;
  ImGui::GetIO().FontGlobalScale = s;
  gAppliedScale = s;
}

// ---------------------------------------------------------------- render
void renderFrame(EGLDisplay dpy, EGLSurface surf) {
  const EGLContext ctx = eglGetCurrentContext();
  if (ctx == EGL_NO_CONTEXT) {
    return;
  }
  EGLint w = 0;
  EGLint h = 0;
  if (!eglQuerySurface(dpy, surf, EGL_WIDTH, &w) ||
      !eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) || w <= 0 || h <= 0) {
    return;
  }

  if (!gImCtx) {
    IMGUI_CHECKVERSION();
    gImCtx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    gLastFrame = std::chrono::steady_clock::now();
  }
  ImGui::SetCurrentContext(gImCtx);

  if (ctx != gGlCtx) { // primeiro frame, ou o jogo recriou o contexto EGL
    if (gBackendReady) {
      ImGui_ImplOpenGL3_Shutdown();
    }
    gBackendReady = ImGui_ImplOpenGL3_Init("#version 300 es");
    gGlCtx = ctx;
    if (!gBackendReady && gMod) {
      gMod->getLogger().error("ImGui_ImplOpenGL3_Init falhou");
      return;
    }
  }

  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));

  const float wanted = gScaleSetting > 0.0f
                           ? gScaleSetting
                           : std::clamp(static_cast<float>(h) / 600.0f, 1.5f, 4.0f);
  if (wanted != gAppliedScale) {
    applyScale(wanted);
  }

  const auto now = std::chrono::steady_clock::now();
  const float dt = std::chrono::duration<float>(now - gLastFrame).count();
  gLastFrame = now;
  io.DeltaTime = std::clamp(dt, 0.001f, 0.25f);

  if (!gFirstFrameLogged && gMod) {
    gFirstFrameLogged = true;
    gMod->getLogger().info("overlay: primeiro frame {}x{}", w, h);
  }

  processEvents(io);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  drawUi();
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

EGLBoolean hookedSwap(EGLDisplay dpy, EGLSurface surf) {
  if (gActive.load()) {
    renderFrame(dpy, surf);
  }
  return gOrigSwap ? gOrigSwap(dpy, surf) : EGL_FALSE;
}

} // namespace

// ------------------------------------------------------------- publico
void install() {
  gActive = true;

  if (!gTouchRegistered) {
    pl::input::registerTouchCallback(onTouch); // nao ha "unregister": gActive protege
    gTouchRegistered = true;
  }
  if (gHooked) {
    return;
  }

  void *sym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  if (!sym) {
    if (gMod) {
      gMod->getLogger().error("eglSwapBuffers nao encontrado");
    }
    return;
  }

  gSwapTarget = reinterpret_cast<pl::memory::FuncPtr>(sym);
  const int rc = pl::memory::hook(
      gSwapTarget, reinterpret_cast<pl::memory::FuncPtr>(&hookedSwap),
      reinterpret_cast<pl::memory::FuncPtr *>(&gOrigSwap));
  if (rc == 0) {
    gHooked = true;
    if (gMod) {
      gMod->getLogger().info("hook do eglSwapBuffers instalado");
    }
  } else if (gMod) {
    gMod->getLogger().error("falha ao hookar eglSwapBuffers (rc={})", rc);
  }
}

void uninstall() {
  gActive = false;
  gPointer = -1;
  if (gHooked) {
    pl::memory::unhook(gSwapTarget, reinterpret_cast<pl::memory::FuncPtr>(&hookedSwap));
    gHooked = false;
  }
}

} // namespace pp::ui
