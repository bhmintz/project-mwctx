// See nfsmw_etc2_codificar.h. Moved out of nfsmw_cache_etc2.cpp unchanged, so the launcher's prefill library
// (which does not load the game engine) writes the same files.

#include "nfsmw_etc2_codificar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

// Same xxHash setup as nfsmw_nativo_dibujos.cpp (reads through memcpy; see the note there).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../sdk/thirdparty/etcpak/ProcessRGB.hpp"

namespace nfsmw::nativo::etc2 {

namespace {

constexpr char kMagia[8] = {'N', 'F', 'S', 'E', 'T', 'C', '2', 0};
constexpr uint32_t kVersion = 1;

// --- BC decode to BGRA8 (etcpak's input: 0xAARRGGBB) ------------------------------------------------------

void ColorBc1(const uint8_t* b, bool forzar_4, uint8_t rgba[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
  const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t paleta[4][4];
  const auto expandir = [](uint16_t c, uint8_t* p) {
    p[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    p[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    p[2] = uint8_t((c & 31) * 255 / 31);
    p[3] = 255;
  };
  expandir(c0, paleta[0]);
  expandir(c1, paleta[1]);
  const bool cuatro = c0 > c1 || forzar_4;
  for (int k = 0; k < 3; ++k) {
    if (cuatro) {
      paleta[2][k] = uint8_t((2 * paleta[0][k] + paleta[1][k]) / 3);
      paleta[3][k] = uint8_t((paleta[0][k] + 2 * paleta[1][k]) / 3);
    } else {
      paleta[2][k] = uint8_t((paleta[0][k] + paleta[1][k]) / 2);
      paleta[3][k] = 0;
    }
  }
  paleta[2][3] = 255;
  paleta[3][3] = cuatro ? 255 : 0;
  const uint32_t indices = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(rgba[i], paleta[(indices >> (2 * i)) & 3], 4);
  }
}

void CanalBc4(const uint8_t* b, uint8_t salida[16]) {
  const uint32_t a0 = b[0], a1 = b[1];
  uint8_t paleta[8];
  paleta[0] = uint8_t(a0);
  paleta[1] = uint8_t(a1);
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) paleta[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (uint32_t i = 1; i < 5; ++i) paleta[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    paleta[6] = 0;
    paleta[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) indices |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) salida[i] = paleta[(indices >> (3 * i)) & 7];
}

// One 4x4 block into `destino` (row stride `ancho` pixels). False for BC1 with a transparent texel.
bool BloqueABgra(VkFormat bc, const uint8_t* b, uint32_t* destino, size_t ancho) {
  uint8_t rgba[16][4];
  uint8_t c0[16], c1[16];
  switch (bc) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      ColorBc1(b, false, rgba);
      for (int i = 0; i < 16; ++i) {
        if (rgba[i][3] < 128) {
          return false;
        }
      }
      break;
    case VK_FORMAT_BC2_UNORM_BLOCK:
      ColorBc1(b + 8, true, rgba);
      for (int i = 0; i < 16; ++i) rgba[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 0xF) * 17);
      break;
    case VK_FORMAT_BC3_UNORM_BLOCK:
      ColorBc1(b + 8, true, rgba);
      CanalBc4(b, c0);
      for (int i = 0; i < 16; ++i) rgba[i][3] = c0[i];
      break;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      CanalBc4(b, c0);
      for (int i = 0; i < 16; ++i) {
        rgba[i][0] = c0[i];
        rgba[i][1] = rgba[i][2] = 0;
        rgba[i][3] = 255;
      }
      break;
    default:  // BC5
      CanalBc4(b, c0);
      CanalBc4(b + 8, c1);
      for (int i = 0; i < 16; ++i) {
        rgba[i][0] = c0[i];
        rgba[i][1] = c1[i];
        rgba[i][2] = 0;
        rgba[i][3] = 255;
      }
      break;
  }
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const uint8_t* p = rgba[y * 4 + x];
      destino[size_t(y) * ancho + x] =
          (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
    }
  }
  return true;
}

}  // namespace

bool Codificar(const Trabajo& t, VkFormat& formato, std::vector<uint8_t>& salida) {
  formato = FormatoEtc2De(t.bc);
  if (formato == VK_FORMAT_UNDEFINED) {
    return false;
  }
  const uint32_t bytes_bloque =
      t.bc == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || t.bc == VK_FORMAT_BC4_UNORM_BLOCK ? 8 : 16;
  salida.assign(t.datos.size(), 0);
  std::vector<uint32_t> bgra;
  for (uint32_t n = 0; n < t.niveles; ++n) {
    const uint32_t w = std::max(t.ancho >> n, 1u), h = std::max(t.alto >> n, 1u);
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    const size_t bytes_rebanada = size_t(bx) * by * bytes_bloque;
    const size_t ancho_px = size_t(bx) * 4;
    bgra.assign(ancho_px * by * 4, 0);
    for (uint32_t r = 0; r < t.rebanadas; ++r) {
      const size_t inicio = size_t(t.desplazamientos[n]) + bytes_rebanada * r;
      if (inicio + bytes_rebanada > t.datos.size()) {
        return false;
      }
      const uint8_t* bloques = t.datos.data() + inicio;
      for (uint32_t y = 0; y < by; ++y) {
        for (uint32_t x = 0; x < bx; ++x) {
          if (!BloqueABgra(t.bc, bloques + (size_t(y) * bx + x) * bytes_bloque,
                           bgra.data() + size_t(y) * 4 * ancho_px + size_t(x) * 4, ancho_px)) {
            return false;  // BC1 with transparency: not stored
          }
        }
      }
      uint64_t* destino = reinterpret_cast<uint64_t*>(salida.data() + inicio);
      const uint32_t bloques_n = bx * by;
      switch (formato) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
          CompressEtc2Rgb(bgra.data(), destino, bloques_n, ancho_px, true);
          break;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
          CompressEtc2Rgba(bgra.data(), destino, bloques_n, ancho_px, true);
          break;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
          CompressEacR(bgra.data(), destino, bloques_n, ancho_px);
          break;
        default:  // EAC RG11
          CompressEacRg(bgra.data(), destino, bloques_n, ancho_px);
          break;
      }
    }
  }
  return true;
}

VkFormat FormatoEtc2De(VkFormat bc) {
  switch (bc) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK:
      return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

uint64_t Suma(const std::vector<uint8_t>& datos) {
  return XXH3_64bits(datos.data(), datos.size());
}

bool AbrirIndice(const std::filesystem::path& carpeta, std::unordered_map<uint64_t, Registro>& registros,
                 uint64_t& tam_datos, uint64_t& descartados) {
  std::error_code ec;
  std::filesystem::create_directories(carpeta, ec);
  const std::string ruta_datos = (carpeta / "datos.bin").string();
  const std::string ruta_indice = (carpeta / "indice.bin").string();
  registros.clear();
  descartados = 0;
  // The data file's real size: records pointing past it (a crash mid-write) are dropped.
  tam_datos = 0;
  if (FILE* f = std::fopen(ruta_datos.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    tam_datos = uint64_t(std::ftell(f));
    std::fclose(f);
  }
  bool indice_valido = false;
  if (FILE* f = std::fopen(ruta_indice.c_str(), "rb")) {
    char magia[8] = {};
    uint32_t version = 0, reservado = 0;
    if (std::fread(magia, 1, 8, f) == 8 && std::fread(&version, 4, 1, f) == 1 && std::fread(&reservado, 4, 1, f) == 1 &&
        std::memcmp(magia, kMagia, 8) == 0 && version == kVersion) {
      indice_valido = true;
      Registro r;
      while (std::fread(&r, sizeof(r), 1, f) == 1) {
        if (r.offset + r.bytes <= tam_datos && r.bytes > 0) {
          registros[r.clave] = r;
        } else {
          ++descartados;
        }
      }
    }
    std::fclose(f);
  }
  if (!indice_valido) {
    // New (or from another version): start from scratch.
    registros.clear();
    tam_datos = 0;
    FILE* fi = std::fopen(ruta_indice.c_str(), "wb");
    if (fi) {
      const uint32_t cabecera[2] = {kVersion, 0};
      std::fwrite(kMagia, 1, 8, fi);
      std::fwrite(cabecera, 4, 2, fi);
      std::fclose(fi);
    }
    FILE* fd = std::fopen(ruta_datos.c_str(), "wb");
    if (fd) {
      std::fclose(fd);
    }
    return fi && fd;
  }
  return true;
}

namespace {
constexpr char kMagiaMundo[8] = {'N', 'F', 'S', 'M', 'U', 'N', 'D', '1'};
}

bool LeerClaves(const std::filesystem::path& carpeta, const char* archivo, std::unordered_set<uint64_t>& claves) {
  claves.clear();
  FILE* f = std::fopen((carpeta / archivo).string().c_str(), "rb");
  if (!f) {
    return false;
  }
  char magia[8] = {};
  uint64_t n = 0;
  bool ok = std::fread(magia, 1, 8, f) == 8 && std::memcmp(magia, kMagiaMundo, 8) == 0 &&
            std::fread(&n, 8, 1, f) == 1 && n < (1u << 24);
  for (uint64_t i = 0; ok && i < n; ++i) {
    uint64_t clave = 0;
    ok = std::fread(&clave, 8, 1, f) == 1;
    if (ok) {
      claves.insert(clave);
    }
  }
  std::fclose(f);
  return ok;
}

bool EscribirClaves(const std::filesystem::path& carpeta, const char* archivo,
                    const std::unordered_set<uint64_t>& claves) {
  // Written to a temporary file and renamed, so a crash never leaves half a list.
  const std::string ruta = (carpeta / archivo).string();
  const std::string temporal = ruta + ".tmp";
  FILE* f = std::fopen(temporal.c_str(), "wb");
  if (!f) {
    return false;
  }
  const uint64_t n = claves.size();
  bool ok = std::fwrite(kMagiaMundo, 1, 8, f) == 8 && std::fwrite(&n, 8, 1, f) == 1;
  for (const uint64_t clave : claves) {
    ok = ok && std::fwrite(&clave, 8, 1, f) == 1;
  }
  ok = std::fclose(f) == 0 && ok;
  return ok && std::rename(temporal.c_str(), ruta.c_str()) == 0;
}

bool LeerMundo(const std::filesystem::path& carpeta, std::unordered_set<uint64_t>& claves) {
  return LeerClaves(carpeta, "mundo.bin", claves);
}

bool EscribirMundo(const std::filesystem::path& carpeta, const std::unordered_set<uint64_t>& claves) {
  return EscribirClaves(carpeta, "mundo.bin", claves);
}

}  // namespace nfsmw::nativo::etc2
