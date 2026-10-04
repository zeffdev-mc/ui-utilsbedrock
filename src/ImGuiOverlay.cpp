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
#include <unistd.h>

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
bool gCollapsed = false; // menu contraido (so a barra de cima)
float gScaleSetting = 0.0f; // 0 = automatico
float gAppliedScale = 0.0f;
bool gFirstFrameLogged = false;

// ------------------------------------------------------------ tema e fonte
// Cores de destaque (RGBA). Edite aqui para trocar as cores do menu.
struct Theme {
  const char *name;
  ImVec4 accent;
};
const Theme kThemes[] = {
    {"Azul", ImVec4(0.20f, 0.55f, 0.95f, 1.0f)},
    {"Verde", ImVec4(0.15f, 0.70f, 0.35f, 1.0f)},
    {"Roxo", ImVec4(0.60f, 0.35f, 0.95f, 1.0f)},
    {"Laranja", ImVec4(0.95f, 0.55f, 0.15f, 1.0f)},
    {"Rosa", ImVec4(0.95f, 0.35f, 0.60f, 1.0f)},
};
constexpr int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));
int gTheme = 0;
bool gThemeDirty = true;
ImVec4 gAccent = kThemes[0].accent;

// Fonte: tenta fontes do sistema Android; se nao achar, usa a padrao do ImGui.
constexpr float kBakePx = 48.0f;  // tamanho em que a fonte e rasterizada
constexpr float kTextPx = 14.0f;  // tamanho "base" do texto (multiplicado pela escala)
bool gFontTtf = false;

// Retangulos do menu (para decidir se o toque e do menu ou do jogo).
struct Rect {
  float x{}, y{}, w{}, h{};
  bool valid{};
};
std::mutex gRectMutex;
Rect gRects[1];

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

ImVec4 tint(const ImVec4 &c, float k) {
  return ImVec4(std::min(c.x * k, 1.0f), std::min(c.y * k, 1.0f),
                std::min(c.z * k, 1.0f), c.w);
}

bool toggleButton(const std::string &text, const char *id, bool highlight,
                  float width) {
  // O ID do botao fica fixo (##id); so o texto e a cor mudam com o estado.
  const std::string label = text + "##" + id;

  if (highlight) {
    ImGui::PushStyleColor(ImGuiCol_Button, tint(gAccent, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(gAccent, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, tint(gAccent, 0.7f));
  }
  const bool pressed = ImGui::Button(label.c_str(), ImVec2(width, 0.0f));
  if (highlight) {
    ImGui::PopStyleColor(3);
  }
  return pressed;
}

void drawUi() {
  const float scale = gAppliedScale > 0.0f ? gAppliedScale : 1.0f;
  const float fontSize = ImGui::GetFontSize();
  const float full = fontSize * 20.0f; // largura util do painel

  ImGui::SetNextWindowPos(ImVec2(12.0f * scale, 70.0f * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGui::Begin("##pp_main", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);

  // Barra de cima: arraste o menu por ela; a seta no canto contrai/expande.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(fontSize * 0.5f, fontSize * 0.45f));
  const float barH = ImGui::GetFrameHeight();
  const ImVec2 barPos = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
      barPos, ImVec2(barPos.x + full, barPos.y + barH),
      ImGui::GetColorU32(ImGuiCol_TitleBgActive), ImGui::GetStyle().WindowRounding * 0.5f);
  ImGui::AlignTextToFramePadding();
  ImGui::Text("  Ui Utils");
  ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + full - barH);
  if (ImGui::ArrowButton("##collapse", gCollapsed ? ImGuiDir_Right : ImGuiDir_Down)) {
    gCollapsed = !gCollapsed;
  }
  ImGui::PopStyleVar();

  if (gCollapsed) {
    storeRect(0);
    ImGui::End();
    return;
  }

  ImGui::Spacing();

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

  float sc = gScaleSetting > 0.0f ? gScaleSetting : gAppliedScale;
  ImGui::SetNextItemWidth(full * 0.6f);
  if (ImGui::SliderFloat("Escala", &sc, 1.0f, 4.0f, "%.1f")) {
    gScaleSetting = sc;
  }

  ImGui::Text("Tema");
  const float swatch = ImGui::GetFrameHeight();
  for (int i = 0; i < kThemeCount; ++i) {
    ImGui::SameLine();
    const std::string id = "##theme" + std::to_string(i);
    if (ImGui::ColorButton(id.c_str(), kThemes[i].accent,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                               ImGuiColorEditFlags_NoBorder,
                           ImVec2(swatch, swatch))) {
      gTheme = i;
      gThemeDirty = true;
    }
  }

  storeRect(0);
  ImGui::End();
}

void applyStyle(float sc, int themeIndex) {
  themeIndex = std::clamp(themeIndex, 0, kThemeCount - 1);
  const ImVec4 a = kThemes[themeIndex].accent;
  gAccent = a;

  ImGuiStyle style;
  ImGui::StyleColorsDark(&style);

  style.WindowRounding = 12.0f;
  style.FrameRounding = 8.0f;
  style.GrabRounding = 8.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(12.0f, 12.0f);
  style.FramePadding = ImVec2(10.0f, 8.0f);
  style.ItemSpacing = ImVec2(8.0f, 8.0f);
  style.GrabMinSize = 18.0f;
  style.ScaleAllSizes(sc);

  ImVec4 *c = style.Colors;
  c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.97f, 1.0f);
  c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.11f, 0.92f);
  c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.20f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = tint(a, 0.45f);
  c[ImGuiCol_FrameBgActive] = tint(a, 0.60f);
  c[ImGuiCol_TitleBgActive] = tint(a, 0.55f); // barra de cima do menu
  c[ImGuiCol_Button] = ImVec4(0.17f, 0.19f, 0.26f, 1.0f);
  c[ImGuiCol_ButtonHovered] = tint(a, 0.65f);
  c[ImGuiCol_ButtonActive] = tint(a, 0.85f);
  c[ImGuiCol_CheckMark] = a;
  c[ImGuiCol_SliderGrab] = a;
  c[ImGuiCol_SliderGrabActive] = tint(a, 1.2f);
  c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);

  ImGui::GetStyle() = style;
  // Com fonte TTF (rasterizada em kBakePx) a escala e relativa a kTextPx.
  ImGui::GetIO().FontGlobalScale = sc * (gFontTtf ? kTextPx / kBakePx : 1.0f);
  gAppliedScale = sc;
}

void loadFont(ImGuiIO &io) {
  static const char *kFontPaths[] = {
      "/system/fonts/Roboto-Regular.ttf",
      "/system/fonts/NotoSans-Regular.ttf",
      "/system/fonts/DroidSans.ttf",
  };
  for (const char *path : kFontPaths) {
    if (access(path, R_OK) != 0) {
      continue;
    }
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    if (io.Fonts->AddFontFromFileTTF(path, kBakePx, &cfg)) {
      gFontTtf = true;
      if (gMod) {
        gMod->getLogger().info("fonte carregada: {}", path);
      }
      return;
    }
  }
  if (gMod) {
    gMod->getLogger().info("fonte do sistema nao encontrada, usando a padrao do ImGui");
  }
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
    loadFont(io);
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
  if (wanted != gAppliedScale || gThemeDirty) {
    applyStyle(wanted, gTheme);
    gThemeDirty = false;
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
