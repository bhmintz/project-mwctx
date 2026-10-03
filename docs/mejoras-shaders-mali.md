# Mejoras de shaders del modo Mali (pendientes)

Encontradas con `malioc` (Arm Performance Studio 2026.5, `D:/arm/mali_offline_compiler/malioc.exe`), que compila el
SPIR-V del `.nfsp` Mali con el compilador real de ARM para el Mali-G52. Sirven con cualquier driver (Samsung y PanVK)
porque cambian el shader, no el driver. Cómo medir: ver "Compilador de ARM (malioc) y variantes" en
[herramientas-mali.md](herramientas-mali.md). Más mejoras, con datos de todos los fragmentos, en
[mejoras-mali-vk11.md](mejoras-mali-vk11.md).

## 1. `remapInput` usa la pila (vértices)

`android/app/src/main/assets/shaders/shader_common.h`, `remapInput(float4 value, uint code)`.

- El código de remapeo de cada entrada de vértices llega por dibujo en el UBO compartido (`g_InputRemap`, bytes
  296+). La función lo aplica con un `for` de 4 vueltas que escribe `result[i]` y lee `value[source]` con índices
  variables.
- Indexar un vector con una variable hace que el compilador (ARM y Mesa) lo baje a memoria de pila. `malioc` lo
  muestra como "Stack size: 16 bytes / Alloca region" en la variante de varyings. En el SPIR-V aparecen como
  `OpVariable ... Function` más `OpAccessChain %_ptr_Function_float %x %i` dentro de un loop.
- Peor shader de vértices (módulo 010 del `.nfsp`, con skinning): variante de varyings con 72.8 ciclos de
  load/store contra 19.5 de aritmética, 62 registros y ocupación al 50%.
- Arreglo propuesto, solo con `NFSMW_MALI`: desenrollar en 4 selecciones sin array, todo en registros.

  ```hlsl
  float remapComponente(float4 v, float propio, uint s)
  {
      return s == 0 ? v.x : s == 1 ? v.y : s == 2 ? v.z : s == 3 ? v.w :
             s == 4 ? 0.0 : s == 5 ? 1.0 : propio;
  }
  // result = float4(remapComponente(value, value.x, code & 7), ... (code >> 3) & 7, ... >> 6, ... >> 9)
  ```

- Verificar antes de regenerar el `.nfsp`: compilar un par de vértices con DXC (WSL) y comparar con `malioc` antes
  y después (ciclos de load/store, stack, registros).

## 2. Skinning: lecturas con índice dinámico (no evitables)

Las matrices de huesos se leen del UBO de vértices con un índice que sale del vértice (`9 + índice`, hasta 246).
Eso va a memoria con cualquier compilador. Queda anotado para no buscarle arreglo por ahí.
