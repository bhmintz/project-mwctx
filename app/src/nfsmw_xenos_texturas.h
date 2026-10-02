#pragma once

// Xbox 360 (Xenos) texture layout arithmetic: mip levels, the packed tail, the 32x32-block tiling and the
// byte swap. Pure functions shared by the native renderer (nfsmw_nativo_dibujos.cpp), which reads textures
// from guest memory, and the ETC2 cache prefill (nfsmw_precarga_etc2.cpp), which reads the same bytes from
// the game files. Moved here unchanged from nfsmw_nativo_dibujos.cpp.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace nfsmw::xenos_tex {

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
inline uint32_t Log2Techo(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

inline uint32_t Log2Suelo(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

// First level of the packed tail: once the short side is 16 texels or less.
inline uint32_t NivelEmpaquetado(uint32_t ancho, uint32_t alto) {
  const uint32_t l = Log2Techo(std::min(ancho, alto));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
inline void DesplazamientoEmpaquetado(uint32_t ancho, uint32_t alto, uint32_t bloque, uint32_t nivel, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_ancho = Log2Techo(ancho);
  const uint32_t l2_alto = Log2Techo(alto);
  const uint32_t l2 = std::min(l2_ancho, l2_alto);
  x = 0;
  y = 0;
  if (l2 > 4 + nivel) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = nivel - base;
  if (m < 3) {
    if (l2_ancho > l2_alto) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_ancho > l2_alto) {
    x = (1u << (l2_ancho - base)) >> (m - 2);
  } else {
    y = (1u << (l2_alto - base)) >> (m - 2);
  }
  x /= bloque;
  y /= bloque;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
inline int32_t DesplazamientoMosaico2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During race stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): LeerNivel called DesplazamientoMosaico2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as DesplazamientoMosaico2D (checked block by block on PC).
template <uint32_t kLog2>
inline void DesenmosaicarNivel(const uint8_t* origen, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* destino) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGrupo = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_fila = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t fila = 0; fila < by; ++fila) {
    const int32_t y = int32_t(oy + fila);
    const int32_t macro_y = (y >> 5) * macros_fila;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* salida = destino + size_t(fila) * bx_host * kBytes;
    uint32_t columna = 0;
    while (columna < bx) {
      const int32_t x = int32_t(ox + columna);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t desplazamiento = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGrupo > 1 && (uint32_t(x) % kGrupo) == 0 && columna + kGrupo <= bx) {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kGrupo * kBytes);
        columna += kGrupo;
      } else {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kBytes);
        ++columna;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void CambiarOrdenBytes(uint8_t* datos, size_t bytes, uint32_t unidad, uint32_t orden) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unidad == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unidad == 2 && orden != k8in16) || (unidad != 2 && unidad != 4) || orden == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(datos + i);
    uint8x16_t r;
    if (unidad == 2 || orden == k8in16) {
      r = vrev16q_u8(v);
    } else if (orden == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(datos + i, r);
  }
#endif
  if (unidad == 2 || orden == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(datos[i], datos[i + 1]);
    }
  } else if (orden == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 3]);
      std::swap(datos[i + 1], datos[i + 2]);
    }
  } else if (orden == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 2]);
      std::swap(datos[i + 1], datos[i + 3]);
    }
  }
}

}  // namespace nfsmw::xenos_tex
