// See nfsmw_precarga_etc2.h.
//
// The fetch constant of a pack texture (worked out on 2026-10-02 from 1,098 [diag etc2] lines of the A32,
// 951 of them found byte for byte in the packs by the hash of their first 4 KB):
//   f[0]: bit 31 = tiled (bit 8 of the D3DFORMAT); bits 22-30 = pitch / 32 texels, the width aligned to 32
//         blocks (128 texels for BC); sign bits 0. Bits 10-21 (clamp) are not in the key.
//   f[1]: low byte of the D3DFORMAT (format in bits 0-5, byte order in 6-7); bits 8-10 are 0.
//   f[2]: (width - 1) | (height - 1) << 13.
//   f[4]: levels 0 to N-1, N being the pack's mip count: (f[4] >> 2) & 0xFF = (N - 1) << 4.
//   f[5]: 2D (1 in bits 9-10) and, with mips, the packed tail bit (11); the mips start right after the
//         base level region (bytes_base).
// Everything else below is PrepararTextura's arithmetic (nfsmw_nativo_dibujos.cpp) for a 2D texture with
// one layer, on the pack's bytes instead of guest memory. Keep both in step: the key must be the same.

#include "nfsmw_precarga_etc2.h"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Same xxHash setup as nfsmw_nativo_dibujos.cpp (reads through memcpy).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "nfsmw_etc2_codificar.h"
#include "nfsmw_xenos_texturas.h"

namespace nfsmw::nativo::precarga {
namespace {

using namespace nfsmw::xenos_tex;

struct FormatoBc {
  VkFormat formato;
  uint32_t bytes;  // per 4x4 block
};

// The BC formats of FormatoTexturaDe (nfsmw_nativo_dibujos.cpp); all swap in 16-bit units.
bool FormatoBcDe(uint32_t xenos, FormatoBc& f) {
  switch (xenos) {
    case 18:  // k_DXT1
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8};
      return true;
    case 19:  // k_DXT2_3
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 16};
      return true;
    case 20:  // k_DXT4_5
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 16};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 16};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 8};
      return true;
    default:
      return false;
  }
}

struct RegionMip {
  uint64_t desplazamiento = 0;
  uint64_t fila_bytes = 0;
  uint64_t zancada = 0;
  uint32_t pitch_bloques = 0;
};

// What PrepararTextura works out from the fetch constant, for a 2D texture with one layer.
struct Disposicion {
  FormatoBc bc{};
  uint32_t ancho = 0, alto = 0;
  uint32_t bloques_x = 0, bloques_y = 0;
  uint32_t pitch_bloques = 0;
  uint32_t log2_bloque = 0;
  bool mosaico = false;
  uint32_t nivel_empaquetado = UINT32_MAX;
  uint32_t nivel_max = 0;
  std::array<RegionMip, 16> regiones{};
  uint64_t extension = 0;
  uint64_t extension_mips = 0;
  uint32_t base_ox = 0, base_oy = 0;
  uint32_t ancho_host = 0, alto_host = 0;
  uint32_t niveles = 1;
};

bool Disponer(const uint32_t* f, uint64_t bytes_mips_disponibles, Disposicion& d) {
  if (!FormatoBcDe(f[1] & 0x3F, d.bc)) {
    return false;
  }
  const uint32_t dimension = (f[5] >> 9) & 0x3;
  if (dimension != 1) {
    return false;  // only 2D (cubes and volumes are not in the packs read here)
  }
  constexpr uint32_t kBloque = 4;
  d.ancho = (f[2] & 0x1FFF) + 1;
  d.alto = ((f[2] >> 13) & 0x1FFF) + 1;
  d.bloques_x = (d.ancho + kBloque - 1) / kBloque;
  d.bloques_y = (d.alto + kBloque - 1) / kBloque;
  d.pitch_bloques = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / kBloque, 1);
  d.log2_bloque = d.bc.bytes >= 16 ? 4 : 3;
  d.mosaico = (f[0] >> 31) & 0x1;
  const bool hay_mips = ((f[5] >> 12) & 0x1FFFF) != 0 || bytes_mips_disponibles != 0;
  d.nivel_empaquetado = ((f[5] >> 11) & 0x1) ? NivelEmpaquetado(d.ancho, d.alto) : UINT32_MAX;
  d.nivel_max = 0;
  if (hay_mips) {
    const uint32_t tam_max = Log2Suelo(std::max(d.ancho, d.alto));
    const uint32_t nivel_min = std::min((f[4] >> 2) & 0xFu, tam_max);
    d.nivel_max = std::max(std::min((f[4] >> 6) & 0xFu, tam_max), nivel_min);
    if (nivel_min != 0) {
      return false;  // the renderer would not read the base: not a cache candidate
    }
  }
  d.extension_mips = 0;
  if (d.nivel_max != 0) {
    const uint32_t ultima = d.nivel_empaquetado == 0 ? 0 : std::min(d.nivel_max, d.nivel_empaquetado);
    for (uint32_t s = d.nivel_empaquetado == 0 ? 0 : 1; s <= ultima; ++s) {
      RegionMip& region = d.regiones[s];
      const uint32_t fila_texels = std::max(std::bit_ceil(d.ancho) >> s, 1u);
      const uint32_t filas_texels = std::max(std::bit_ceil(d.alto) >> s, 1u);
      region.pitch_bloques = ((fila_texels + kBloque - 1) / kBloque + 31) & ~31u;
      region.fila_bytes = uint64_t(region.pitch_bloques) * d.bc.bytes;
      if (!d.mosaico) {
        region.fila_bytes = (region.fila_bytes + 255) & ~uint64_t(255);
      }
      const uint64_t filas_bloques = uint64_t(((filas_texels + kBloque - 1) / kBloque + 31) & ~31u);
      region.zancada = (region.fila_bytes * filas_bloques + 4095) & ~uint64_t(4095);
      region.desplazamiento = d.extension_mips;
      d.extension_mips += region.zancada;
    }
  }
  d.base_ox = 0;
  d.base_oy = 0;
  if (d.nivel_empaquetado == 0) {
    DesplazamientoEmpaquetado(d.ancho, d.alto, kBloque, 0, d.base_ox, d.base_oy);
  }
  const uint32_t bx_leidos = d.bloques_x + d.base_ox;
  const uint32_t by_leidos = d.bloques_y + d.base_oy;
  d.extension = d.mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((bx_leidos - 1) & ~31u),
                                                                                int32_t((by_leidos - 1) & ~31u),
                                                                                d.pitch_bloques, d.log2_bloque),
                                                        0)) +
                                (0x400u << d.log2_bloque)
                          : uint64_t(std::max(d.pitch_bloques, bx_leidos)) * d.bc.bytes * (by_leidos - 1) +
                                uint64_t(bx_leidos) * d.bc.bytes;
  d.ancho_host = (d.ancho + 3) & ~uint32_t(3);
  d.alto_host = (d.alto + 3) & ~uint32_t(3);
  d.niveles = std::min(d.nivel_max + 1, Log2Suelo(std::max(d.ancho_host, d.alto_host)) + 1);
  return true;
}

// The blocks of one level from a region of the pack (LeerNivel, without the fast path's self-check).
bool LeerNivel(const uint8_t* origen, uint64_t bytes_origen, bool mosaico, uint32_t pitch_bloques,
               uint64_t fila_bytes, const Disposicion& d, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
               uint32_t bx_host, uint8_t* destino) {
  if (!bx || !by) {
    return true;
  }
  const uint32_t bytes = d.bc.bytes;
  // The end of the last 32x32-block macro tile read (the extent PrepararTextura hashes). The renderer's
  // LeerNivel bounds against the next tile instead, which is fine against the whole guest memory but runs past
  // a level's own bytes here.
  const uint64_t extremo =
      mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx - 1) & ~31u),
                                                                   int32_t((oy + by - 1) & ~31u), pitch_bloques,
                                                                   d.log2_bloque),
                                           0)) +
                    (0x400u << d.log2_bloque)
              : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * bytes;
  if (extremo > bytes_origen) {
    return false;
  }
  if (!mosaico) {
    for (uint32_t y = 0; y < by; ++y) {
      std::memcpy(destino + size_t(y) * bx_host * bytes, origen + uint64_t(oy + y) * fila_bytes + uint64_t(ox) * bytes,
                  size_t(bx) * bytes);
    }
    return true;
  }
  if (d.log2_bloque == 4) {
    DesenmosaicarNivel<4>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino);
  } else {
    DesenmosaicarNivel<3>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino);
  }
  return true;
}

uint64_t ClaveEtc2(const uint32_t* f, const Disposicion& d, uint64_t huella) {
  // clave_etc2_de in PrepararTextura (claves[0], f[1] & 0x7FF, claves[2], claves[3], dimension and packed
  // bits, host size, levels, layers, version).
  const uint32_t forma[10] = {f[0] & 0xFFC003FC, f[1] & 0x7FF, f[2], (f[4] >> 2) & 0xFF, (f[5] >> 9) & 0x7,
                              d.ancho_host, d.alto_host, d.niveles, 1, 0xE7C20001u};
  return XXH3_64bits_withSeed(forma, sizeof(forma), huella) | 1;
}

}  // namespace

bool FetchDe(const tpk::Textura& t, Fetch& fetch) {
  const uint32_t d3d = t.formato_d3d;
  FormatoBc bc{};
  if (!FormatoBcDe(d3d & 0x3F, bc) || !t.ancho || !t.alto || t.ancho > 8192 || t.alto > 8192) {
    return false;
  }
  const bool mosaico = (d3d >> 8) & 0x1;
  const uint32_t pitch = (t.ancho + 127) & ~127u;  // 32 blocks of 4 texels
  uint32_t* f = fetch.f;
  f[0] = (mosaico ? 0x80000000u : 0) | ((pitch >> 5) << 22) | 0x2u;
  f[1] = d3d & 0xFF;
  f[2] = (t.ancho - 1) | ((t.alto - 1) << 13);
  f[3] = 0;
  const uint32_t niveles = std::max(t.niveles, 1u);
  f[4] = ((niveles - 1) << 6) | 0x3;
  f[5] = (1u << 9) | (niveles > 1 ? (1u << 11) : 0);
  return true;
}

bool ClaveDe(const tpk::Textura& t, uint64_t& clave, uint64_t& huella, uint64_t& extension,
             uint64_t& extension_mips) {
  Fetch fetch;
  if (!FetchDe(t, fetch)) {
    return false;
  }
  const uint64_t bytes_mips = t.bytes_imagen > t.bytes_base ? t.bytes_imagen - t.bytes_base : 0;
  Disposicion d;
  if (!Disponer(fetch.f, t.niveles > 1 ? bytes_mips : 0, d) || d.extension > t.bytes_base ||
      d.extension_mips > bytes_mips || t.datos.size() < t.bytes_imagen) {
    return false;
  }
  huella = XXH3_64bits(t.datos.data(), size_t(d.extension));
  if (d.extension_mips) {
    huella = XXH3_64bits_withSeed(t.datos.data() + t.bytes_base, size_t(d.extension_mips), huella);
  }
  clave = ClaveEtc2(fetch.f, d, huella);
  extension = d.extension;
  extension_mips = d.extension_mips;
  return true;
}

bool TrabajoDe(const tpk::Textura& t, etc2::Trabajo& trabajo) {
  uint64_t huella = 0;
  uint64_t extension = 0;
  uint64_t extension_mips = 0;
  if (!ClaveDe(t, trabajo.clave, huella, extension, extension_mips)) {
    return false;
  }
  Fetch fetch;
  FetchDe(t, fetch);
  const uint32_t* f = fetch.f;
  const uint64_t bytes_mips = t.bytes_imagen - t.bytes_base;
  Disposicion d;
  Disponer(f, t.niveles > 1 ? bytes_mips : 0, d);
  // Levels back to back, tight blocks (SubirTextura's layout).
  size_t total = 0;
  for (uint32_t n = 0; n < d.niveles; ++n) {
    trabajo.desplazamientos[n] = uint32_t(total);
    total += size_t((std::max(d.ancho_host >> n, 1u) + 3) / 4) * ((std::max(d.alto_host >> n, 1u) + 3) / 4) *
             d.bc.bytes;
  }
  trabajo.datos.assign(total, 0);
  const uint32_t bx_host = (d.ancho_host + 3) / 4;
  if (!LeerNivel(t.datos.data(), t.bytes_base, d.mosaico, d.pitch_bloques,
                 uint64_t(std::max(d.pitch_bloques, d.bloques_x)) * d.bc.bytes, d, d.base_ox, d.base_oy, d.bloques_x,
                 d.bloques_y, bx_host, trabajo.datos.data())) {
    return false;
  }
  const uint8_t* mips = t.datos.data() + t.bytes_base;
  for (uint32_t n = 1; n < d.niveles; ++n) {
    const uint32_t s = d.nivel_empaquetado == 0 ? 0 : std::min(n, d.nivel_empaquetado);
    uint32_t ox = 0;
    uint32_t oy = 0;
    if (n >= d.nivel_empaquetado) {
      DesplazamientoEmpaquetado(d.ancho, d.alto, 4, n, ox, oy);
    }
    const uint32_t bx_invitado = (std::max(d.ancho >> n, 1u) + 3) / 4;
    const uint32_t by_invitado = (std::max(d.alto >> n, 1u) + 3) / 4;
    const uint32_t bx_n = (std::max(d.ancho_host >> n, 1u) + 3) / 4;
    const uint32_t by_n = (std::max(d.alto_host >> n, 1u) + 3) / 4;
    const RegionMip& r = d.regiones[s];
    if (r.desplazamiento > bytes_mips ||
        !LeerNivel(mips + r.desplazamiento, bytes_mips - r.desplazamiento, d.mosaico, r.pitch_bloques, r.fila_bytes,
                   d, ox, oy, std::min(bx_invitado, bx_n), std::min(by_invitado, by_n), bx_n,
                   trabajo.datos.data() + trabajo.desplazamientos[n])) {
      return false;
    }
  }
  CambiarOrdenBytes(trabajo.datos.data(), trabajo.datos.size(), 2, (f[1] >> 6) & 0x3);
  trabajo.bc = d.bc.formato;
  trabajo.ancho = d.ancho_host;
  trabajo.alto = d.alto_host;
  trabajo.rebanadas = 1;
  trabajo.niveles = d.niveles;
  return true;
}

/*
 * The fixed reflection (nfsmw_cubemap_contenido = 4): the game's own panorama of Rockport that it puts in the
 * windows of the buildings (WINDOWREFLECTIONCOL, 256x128 BC1, in the "Region" pack), decoded to RGBA and
 * saved as reflejo_fijo.bin ("NFSPANO1", width, height, then RGBA rows). The renderer wraps it around the
 * horizon of a cubemap (nfsmw_nativo_dibujos.cpp, CuboFijo) that the car reflects instead of the dynamic one.
 */
static bool GuardarPanorama(const tpk::Textura& t, const std::filesystem::path& carpeta) {
  etc2::Trabajo trabajo;
  if (!TrabajoDe(t, trabajo) || trabajo.bc != VK_FORMAT_BC1_RGBA_UNORM_BLOCK) {
    return false;
  }
  const uint32_t w = trabajo.ancho, h = trabajo.alto;
  const uint32_t bx = (w + 3) / 4;
  std::vector<uint8_t> rgba(size_t(w) * h * 4, 255);
  for (uint32_t y = 0; y < h; y += 4) {
    for (uint32_t x = 0; x < w; x += 4) {
      const uint8_t* b = trabajo.datos.data() + (size_t(y / 4) * bx + x / 4) * 8;
      const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
      const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
      uint8_t paleta[4][3];
      const auto expandir = [](uint16_t c, uint8_t* q) {
        q[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        q[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        q[2] = uint8_t((c & 31) * 255 / 31);
      };
      expandir(c0, paleta[0]);
      expandir(c1, paleta[1]);
      for (int k = 0; k < 3; ++k) {
        paleta[2][k] = c0 > c1 ? uint8_t((2 * paleta[0][k] + paleta[1][k]) / 3)
                               : uint8_t((paleta[0][k] + paleta[1][k]) / 2);
        paleta[3][k] = c0 > c1 ? uint8_t((paleta[0][k] + 2 * paleta[1][k]) / 3) : 0;
      }
      const uint32_t indices = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t px = x + i % 4, py = y + i / 4;
        if (px < w && py < h) {
          std::memcpy(&rgba[(size_t(py) * w + px) * 4], paleta[(indices >> (2 * i)) & 3], 3);
        }
      }
    }
  }
  const std::string ruta = (carpeta / "reflejo_fijo.bin").string();
  FILE* f = std::fopen((ruta + ".tmp").c_str(), "wb");
  if (!f) {
    return false;
  }
  const uint32_t cabecera[2] = {w, h};
  bool ok = std::fwrite("NFSPANO1", 1, 8, f) == 8 && std::fwrite(cabecera, 4, 2, f) == 2 &&
            std::fwrite(rgba.data(), 1, rgba.size(), f) == rgba.size();
  ok = std::fclose(f) == 0 && ok;
  return ok && std::rename((ruta + ".tmp").c_str(), ruta.c_str()) == 0;
}

/*
 * The prefill.
 * One reader (this thread) walks the archive in order, decompressing the packs; the textures go into a
 * bounded queue and the other cores untile and transcode them. The files are appended under one lock, in the
 * same format the game's cache writes (nfsmw_etc2_codificar.cpp), so the game finds them on its next start.
 * A texture that appears in several sections (the world repeats many) is transcoded once.
 *
 * Self-check: the first textures that were already in the cache (stored by the game while playing) are
 * transcoded again and compared byte by byte with what is stored. Same key and same bytes means the pack
 * reader, the key and the untiling agree with the renderer; any difference is counted (Progreso::distintas).
 */
bool Precargar(const std::string& carpeta_nfs, const std::filesystem::path& carpeta_cache, Progreso& progreso,
               const std::atomic<bool>& cancelar) {
  std::vector<tpk::Entrada> entradas;
  if (!tpk::LeerIndice(carpeta_nfs + "/ZDIR.BIN", entradas)) {
    return false;
  }
  std::unordered_map<uint64_t, etc2::Registro> registros;
  uint64_t tam_datos = 0;
  uint64_t descartados = 0;
  if (!etc2::AbrirIndice(carpeta_cache, registros, tam_datos, descartados)) {
    return false;
  }
  std::unordered_set<uint64_t> mundo;
  etc2::LeerMundo(carpeta_cache, mundo);
  std::unordered_set<uint64_t> sin_etc2;
  etc2::LeerClaves(carpeta_cache, "sin_etc2.bin", sin_etc2);
  FILE* datos = std::fopen((carpeta_cache / "datos.bin").string().c_str(), "ab");
  FILE* indice = std::fopen((carpeta_cache / "indice.bin").string().c_str(), "ab");
  FILE* lectura = std::fopen((carpeta_cache / "datos.bin").string().c_str(), "rb");
  if (!datos || !indice || !lectura) {
    for (FILE* f : {datos, indice, lectura}) {
      if (f) {
        std::fclose(f);
      }
    }
    return false;
  }
  std::fseek(datos, 0, SEEK_END);
  tam_datos = uint64_t(std::ftell(datos));

  uint64_t total = 0;
  for (const auto& e : entradas) {
    total += e.bytes;
  }
  progreso.bytes_total = total;

  constexpr uint64_t kColaMax = 160ull << 20;  // bytes of texture data waiting for a core
  constexpr uint64_t kComprobarMax = 64;
  std::mutex m;
  std::condition_variable hay_trabajo, hay_sitio;
  std::deque<tpk::Textura> cola;
  uint64_t bytes_cola = 0;
  bool fin_lectura = false;
  std::unordered_set<uint64_t> vistas;  // keys already handled in this run
  std::mutex m_archivos;                // datos/indice/lectura and registros

  const auto trabajar = [&]() {
    std::vector<uint8_t> salida;
    std::vector<uint8_t> guardado;
    for (;;) {
      tpk::Textura t;
      {
        std::unique_lock<std::mutex> l(m);
        hay_trabajo.wait(l, [&] { return !cola.empty() || fin_lectura; });
        if (cola.empty()) {
          return;
        }
        t = std::move(cola.front());
        cola.pop_front();
        bytes_cola -= std::min<uint64_t>(bytes_cola, t.datos.size());
      }
      hay_sitio.notify_one();
      if (cancelar.load(std::memory_order_relaxed)) {
        continue;
      }
      etc2::Trabajo trabajo;
      if (!TrabajoDe(t, trabajo)) {
        progreso.sin_guardar.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      progreso.texturas.fetch_add(1, std::memory_order_relaxed);
      bool ya_estaba = false;
      etc2::Registro guardada{};
      {
        std::lock_guard<std::mutex> l(m);
        if (t.del_mundo && mundo.insert(trabajo.clave).second) {
          progreso.del_mundo.fetch_add(1, std::memory_order_relaxed);
        }
        // The map (see sin_etc2.bin in nfsmw_etc2_codificar.h): listed, never stored.
        if (t.paquete == "TRACKMAPS" || t.nombre.rfind("MINIMAP", 0) == 0 ||
            t.nombre.find("WORLDMAP") != std::string::npos) {
          sin_etc2.insert(trabajo.clave);
          continue;
        }
        if (!vistas.insert(trabajo.clave).second) {
          continue;  // a copy of a texture already handled
        }
      }
      {
        std::lock_guard<std::mutex> l(m_archivos);
        const auto it = registros.find(trabajo.clave);
        if (it != registros.end()) {
          ya_estaba = true;
          guardada = it->second;
        }
      }
      if (ya_estaba) {
        progreso.ya_estaban.fetch_add(1, std::memory_order_relaxed);
        if (progreso.comprobadas.load(std::memory_order_relaxed) >= kComprobarMax) {
          continue;
        }
        VkFormat formato = VK_FORMAT_UNDEFINED;
        if (!etc2::Codificar(trabajo, formato, salida)) {
          continue;
        }
        guardado.resize(guardada.bytes);
        bool leido = false;
        {
          std::lock_guard<std::mutex> l(m_archivos);
          std::fflush(datos);
          leido = fseeko(lectura, off_t(guardada.offset), SEEK_SET) == 0 &&
                  std::fread(guardado.data(), 1, guardado.size(), lectura) == guardado.size();
          std::clearerr(lectura);
        }
        if (progreso.comprobadas.fetch_add(1, std::memory_order_relaxed) < kComprobarMax) {
          if (!leido || guardado != salida || uint32_t(formato) != guardada.formato) {
            progreso.distintas.fetch_add(1, std::memory_order_relaxed);
            __android_log_print(ANDROID_LOG_WARN, "NFSMW-precarga",
                                "comprobacion: %s/%s (%ux%u) clave %016llX NO coincide con la cache del juego",
                                t.paquete.c_str(), t.nombre.c_str(), t.ancho, t.alto,
                                (unsigned long long)trabajo.clave);
          }
        }
        continue;
      }
      VkFormat formato = VK_FORMAT_UNDEFINED;
      if (!etc2::Codificar(trabajo, formato, salida)) {
        progreso.sin_guardar.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      etc2::Registro r{trabajo.clave, 0, uint32_t(salida.size()), uint32_t(formato), etc2::Suma(salida)};
      {
        std::lock_guard<std::mutex> l(m_archivos);
        r.offset = tam_datos;
        if (std::fwrite(salida.data(), 1, salida.size(), datos) != salida.size()) {
          continue;
        }
        tam_datos += salida.size();
        // The data before its record (a crash can leave data without a record, never the other way round).
        std::fflush(datos);
        std::fwrite(&r, sizeof(r), 1, indice);
        registros[r.clave] = r;
      }
      progreso.nuevas.fetch_add(1, std::memory_order_relaxed);
    }
  };

  const unsigned nucleos = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> hilos;
  for (unsigned i = 0; i + 1 < nucleos; ++i) {
    hilos.emplace_back(trabajar);
  }

  // The reader.
  FILE* archivos[16] = {};
  tpk::Estadisticas estadisticas;
  bool panorama_guardado = false;
  for (const auto& en : entradas) {
    if (cancelar.load(std::memory_order_relaxed)) {
      break;
    }
    if (en.archivo >= 16) {
      progreso.bytes_hechos.fetch_add(en.bytes, std::memory_order_relaxed);
      continue;
    }
    FILE*& f = archivos[en.archivo];
    if (!f) {
      f = std::fopen((carpeta_nfs + "/ZZDATA" + std::to_string(en.archivo) + ".BIN").c_str(), "rb");
      if (!f) {
        progreso.bytes_hechos.fetch_add(en.bytes, std::memory_order_relaxed);
        continue;
      }
    }
    tpk::RecorrerEntrada(
        f, en,
        [&](tpk::Textura& t) {
          if (!panorama_guardado && t.nombre == "WINDOWREFLECTIONCOL") {  // in the "Region" pack
            panorama_guardado = GuardarPanorama(t, carpeta_cache);
            __android_log_print(ANDROID_LOG_INFO, "NFSMW-precarga", "panorama del reflejo fijo (%s, %ux%u): %s",
                                t.nombre.c_str(), t.ancho, t.alto, panorama_guardado ? "guardado" : "NO");
          }
          // Only BC: the rest (the vinyls are ARGB) never goes through the cache.
          FormatoBc bc{};
          if (!FormatoBcDe(t.formato_d3d & 0x3F, bc)) {
            return !cancelar.load(std::memory_order_relaxed);
          }
          std::unique_lock<std::mutex> l(m);
          hay_sitio.wait(l, [&] { return bytes_cola < kColaMax || cancelar.load(std::memory_order_relaxed); });
          bytes_cola += t.datos.size();
          cola.push_back(std::move(t));
          l.unlock();
          hay_trabajo.notify_one();
          return !cancelar.load(std::memory_order_relaxed);
        },
        estadisticas, [&](uint64_t bytes) { progreso.bytes_hechos.fetch_add(bytes, std::memory_order_relaxed); });
  }
  for (FILE*& f : archivos) {
    if (f) {
      std::fclose(f);
    }
  }
  {
    std::lock_guard<std::mutex> l(m);
    fin_lectura = true;
  }
  hay_trabajo.notify_all();
  for (auto& h : hilos) {
    h.join();
  }
  std::fclose(datos);
  std::fclose(indice);
  std::fclose(lectura);
  etc2::EscribirMundo(carpeta_cache, mundo);
  etc2::EscribirClaves(carpeta_cache, "sin_etc2.bin", sin_etc2);
  __android_log_print(ANDROID_LOG_INFO, "NFSMW-precarga", "%zu texturas del mapa sin ETC2", sin_etc2.size());
  __android_log_print(ANDROID_LOG_INFO, "NFSMW-precarga",
                      "fin: %llu texturas BC, %llu nuevas, %llu ya estaban, %llu sin guardar, %llu comprobadas (%llu "
                      "distintas), %llu del mundo; paquetes %llu, fallos de lectura %llu%s",
                      (unsigned long long)progreso.texturas.load(), (unsigned long long)progreso.nuevas.load(),
                      (unsigned long long)progreso.ya_estaban.load(), (unsigned long long)progreso.sin_guardar.load(),
                      (unsigned long long)progreso.comprobadas.load(), (unsigned long long)progreso.distintas.load(),
                      (unsigned long long)mundo.size(), (unsigned long long)estadisticas.paquetes,
                      (unsigned long long)estadisticas.fallos, cancelar.load() ? " (cancelada)" : "");
  return true;
}

}  // namespace nfsmw::nativo::precarga
