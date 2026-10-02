// PacketPause - Raff
// Hook em NetworkSystem::_sendInternal (0xc642bf4 nesta build) do libminecraftpe.so.
// Assinatura obtida com o Zaphkiel. Argumentos (leitura do assembly):
//   x0 = this (NetworkSystem), x1 = NetworkIdentifier const&,
//   x2 = Packet const&,        x3 = std::string const* (bytes ja serializados)
//
// Modos (menu ImGui, ver ImGuiOverlay.cpp):
//   Delay-Packets              - segura os pacotes numa fila; ao desligar, envia tudo de uma vez, em ordem
//   Cancel-Packets             - descarta os pacotes
//   Close without send packets - descarta so o ContainerClose (fechar menu sem avisar o servidor)
//   Flush                      - envia a fila agora, sem desligar o Delay-Packets

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <link.h>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

#include "Shared.hpp"

namespace pp {

// ------------------------------------------------------------- estado publico
std::atomic_bool gPause{false};
std::atomic_bool gCancel{false};
std::atomic_bool gNoClose{false};
std::atomic_bool gFlushReq{false};
std::atomic_bool gKeepMove{false};
std::atomic_bool gLogIds{true};
std::atomic<size_t> gQueued{0};
ll::mod::NativeMod *gMod = nullptr;

namespace {

// Assinatura de 0xc642bf4 (gerada pelo Zaphkiel, unica na build analisada).
constexpr const char *kSignature =
    "FF 43 02 D1 FD 7B 03 A9 FB ?? ?? F9 FA 67 05 A9 F8 5F 06 A9 F6 57 07 A9 "
    "F4 4F 08 A9 FD C3 00 91 59 D0 3B D5 F3 03 03 AA F4 03 02 AA 28 ?? ?? F9 "
    "F5 03 00 AA F6 03 01 AA";
constexpr uintptr_t kExpectedRva = 0xc642bf4; // so para o caminho rapido

constexpr size_t kNetIdSize = 0xB0; // NetworkIdentifier copiado cru
constexpr size_t kPktSize = 0x30;   // inicio do Packet (vtable, +0xc, +0x28)
constexpr size_t kMaxQueue = 4096;

// IDs de pacote (a conferir no log: "first packet id=...").
constexpr uint32_t kIdContainerClose = 47;
constexpr uint32_t kIdPlayerAuthInput = 144;
constexpr uint32_t kIdNetworkStackLatency = 115;

using SendFn = void (*)(void *, const void *, const void *, const std::string *);

SendFn gOrig = nullptr;
std::atomic<uint8_t> gSeen[1024];

struct Item {
  void *self{};
  alignas(16) uint8_t netId[kNetIdSize]{};
  alignas(16) uint8_t pkt[kPktSize]{};
  std::string data;
};

std::mutex gMutex;
std::deque<Item> gQueue;

// ----------------------------------------------------------------- utilitarios
uint32_t packetIdOf(const std::string &d) {
  uint32_t v = 0;
  int shift = 0;
  for (size_t i = 0; i < d.size() && i < 5; ++i) {
    const auto b = static_cast<uint8_t>(d[i]);
    v |= static_cast<uint32_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      break;
    }
    shift += 7;
  }
  return v & 0x3FF; // cabecalho: id | (sender << 10) | (recipient << 12)
}

void drainQueue(void *self) {
  std::deque<Item> local;
  {
    std::lock_guard lock(gMutex);
    local.swap(gQueue);
    gQueued = 0;
  }
  size_t sent = 0;
  size_t skipped = 0;
  for (auto &it : local) {
    if (it.self != self) { // sessao diferente: nao reenviar
      ++skipped;
      continue;
    }
    gOrig(self, it.netId, it.pkt, &it.data);
    ++sent;
  }
  if (gMod && (sent || skipped)) {
    gMod->getLogger().info("flush: enviados={} descartados={}", sent, skipped);
  }
}

// ---------------------------------------------------------------------- detour
void detour(void *self, const void *netId, const void *pkt,
            const std::string *data) {
  if (!gOrig) {
    return; // janela minuscula logo apos instalar o hook
  }

  if (gFlushReq.exchange(false)) {
    drainQueue(self);
  }

  if (!data || data->empty()) {
    gOrig(self, netId, pkt, data);
    return;
  }

  const uint32_t id = packetIdOf(*data);

  if (gLogIds.load() && id < 1024 && !gSeen[id].exchange(1) && gMod) {
    gMod->getLogger().info("first packet id={} len={}", id, data->size());
  }

  const bool whitelisted =
      id == kIdNetworkStackLatency ||
      (gKeepMove.load() && id == kIdPlayerAuthInput);

  if (!whitelisted) {
    if (gNoClose.load() && id == kIdContainerClose) {
      return;
    }
    if (gCancel.load()) {
      return;
    }
    if (gPause.load()) {
      Item item;
      item.self = self;
      std::memcpy(item.netId, netId, kNetIdSize);
      std::memcpy(item.pkt, pkt, kPktSize);
      item.data = *data;
      bool overflow = false;
      {
        std::lock_guard lock(gMutex);
        gQueue.push_back(std::move(item));
        gQueued = gQueue.size();
        overflow = gQueue.size() >= kMaxQueue;
      }
      if (overflow) {
        drainQueue(self);
      }
      return;
    }
  }

  gOrig(self, netId, pkt, data);
}

// -------------------------------------------------------- busca por assinatura
std::vector<int> parseSignature(const char *text) {
  std::vector<int> out;
  std::istringstream stream(text);
  std::string token;
  while (stream >> token) {
    if (token == "??" || token == "?") {
      out.push_back(-1);
    } else {
      out.push_back(static_cast<int>(std::strtol(token.c_str(), nullptr, 16)));
    }
  }
  return out;
}

struct LibInfo {
  uintptr_t base{};
  std::vector<std::pair<const uint8_t *, size_t>> exec;
};

int phdrCallback(dl_phdr_info *info, size_t, void *data) {
  auto *lib = static_cast<LibInfo *>(data);
  if (!info->dlpi_name || !std::strstr(info->dlpi_name, "libminecraftpe.so")) {
    return 0;
  }
  lib->base = info->dlpi_addr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const auto &ph = info->dlpi_phdr[i];
    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) && (ph.p_flags & PF_R)) {
      lib->exec.emplace_back(
          reinterpret_cast<const uint8_t *>(info->dlpi_addr + ph.p_vaddr),
          static_cast<size_t>(ph.p_memsz));
    }
  }
  return 1;
}

bool matchAt(const uint8_t *p, const std::vector<int> &pat) {
  for (size_t i = 0; i < pat.size(); ++i) {
    if (pat[i] >= 0 && p[i] != static_cast<uint8_t>(pat[i])) {
      return false;
    }
  }
  return true;
}

const uint8_t *scanSegment(const uint8_t *begin, size_t len,
                           const std::vector<int> &pat) {
  const size_t n = pat.size();
  if (n == 0 || pat[0] < 0 || len < n) {
    return nullptr;
  }
  const auto first = static_cast<uint8_t>(pat[0]);
  const uint8_t *p = begin;
  const uint8_t *last = begin + (len - n);
  while (p <= last) {
    p = static_cast<const uint8_t *>(
        std::memchr(p, first, static_cast<size_t>(last - p) + 1));
    if (!p) {
      return nullptr;
    }
    if (matchAt(p, pat)) {
      return p;
    }
    ++p;
  }
  return nullptr;
}

const uint8_t *findTarget(const LibInfo &lib, const std::vector<int> &pat) {
  const auto *guess = reinterpret_cast<const uint8_t *>(lib.base + kExpectedRva);
  for (const auto &seg : lib.exec) {
    if (guess >= seg.first && guess + pat.size() <= seg.first + seg.second &&
        matchAt(guess, pat)) {
      return guess;
    }
  }
  for (const auto &seg : lib.exec) {
    if (const auto *hit = scanSegment(seg.first, seg.second, pat)) {
      return hit;
    }
  }
  return nullptr;
}

} // namespace
} // namespace pp

// ------------------------------------------------------------------------- mod
namespace {

using namespace pp;

class PacketPause {
public:
  static PacketPause &instance() {
    static PacketPause mod;
    return mod;
  }

  PacketPause() : mSelf(*ll::mod::NativeMod::current()) { gMod = &mSelf; }

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load() {
    getSelf().getLogger().info("PacketPause carregado");
    return true;
  }

  bool enable() {
    mStop = false;
    mInstallThread = std::thread([this] { installLoop(); });
    ui::install();
    return true;
  }

  bool disable() {
    ui::uninstall();
    mStop = true;
    if (mInstallThread.joinable()) {
      mInstallThread.join();
    }
    resetModes();
    if (mHookInstalled) {
      pl::memory::unhook(mTarget, reinterpret_cast<pl::memory::FuncPtr>(&detour));
      mHookInstalled = false;
    }
    {
      std::lock_guard lock(gMutex);
      gQueue.clear();
      gQueued = 0;
    }
    getSelf().getLogger().info("PacketPause desativado");
    return true;
  }

  bool unload() { return true; }

private:
  ll::mod::NativeMod &mSelf;
  std::thread mInstallThread;
  std::atomic_bool mStop{false};
  bool mHookInstalled{false};
  pl::memory::FuncPtr mTarget{};

  static void resetModes() {
    gPause = false;
    gCancel = false;
    gNoClose = false;
    gFlushReq = false;
  }

  void installLoop() {
    auto &log = getSelf().getLogger();
    const auto pattern = parseSignature(kSignature);

    for (int attempt = 0; attempt < 240 && !mStop; ++attempt) { // ~60 s
      LibInfo lib;
      dl_iterate_phdr(phdrCallback, &lib);

      if (lib.base != 0 && !lib.exec.empty()) {
        const uint8_t *target = findTarget(lib, pattern);
        if (!target) {
          log.error("assinatura nao encontrada (versao do jogo diferente?)");
          return;
        }
        log.info("alvo em base+0x{:x}",
                 reinterpret_cast<uintptr_t>(target) - lib.base);

        gOrig = nullptr;
        mTarget = reinterpret_cast<pl::memory::FuncPtr>(const_cast<uint8_t *>(target));
        const int rc = pl::memory::hook(
            mTarget, reinterpret_cast<pl::memory::FuncPtr>(&detour),
            reinterpret_cast<pl::memory::FuncPtr *>(&gOrig));
        if (rc == 0) {
          mHookInstalled = true;
          log.info("hook instalado");
        } else {
          log.error("falha ao instalar o hook (rc={})", rc);
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    log.error("libminecraftpe.so nao encontrada");
  }
};

} // namespace

PL_REGISTER_MOD(PacketPause, PacketPause::instance())
