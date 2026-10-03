# Documentation

## Start here

These documents explain how the port works, how to build it, and how to use it as a guide to port another Xbox 360
game. They are written for someone who does not know the project yet. If a word is new to you, look it up in the
[glossary](glossary.md).

## How the whole port fits together

This is the path the game follows, from the Xbox 360 disc to the Switch screen:

1. **The game's code is translated.** The game's program, `default.xex`, is written for the Xbox 360's processor. It
   is translated to C++ with [ReXGlue](glossary.md#rexglue) and compiled into an NRO for the Switch.
   See [toolchain.md](toolchain.md).
2. **The game gets a fake Xbox 360 around it.** When the game runs, ReXGlue gives it what it would find on an Xbox 360:
   files, memory, threads, audio and controllers. The Switch part of that is in `sdk/`.
   See [platform-notes.md](platform-notes.md).
3. **The game draws as usual, and the port draws the same thing on the Switch.** The game writes its list of GPU
   commands, as it always did. The port's renderer reads that list and draws the same frame with Vulkan.
   See [native-renderer.md](native-renderer.md).
4. **The shaders are translated ahead of time.** The game's small GPU programs are converted before playing. The
   installer page does it from the user's own disc. See [shaders.md](shaders.md).
5. **Vulkan runs on a driver inside the NRO.** The driver is NVK, from the Mesa project, with this port's changes.
   See [mesa.md](mesa.md).
6. **Audio and cutscenes are decoded on the Switch.** See [audio-and-video.md](audio-and-video.md).

## What to read, depending on what you want

| You want to... | Read |
|---|---|
| Build the port yourself | [building.md](building.md) |
| Port another Xbox 360 game | [porting-another-game.md](porting-another-game.md), then [native-renderer.md](native-renderer.md), [shaders.md](shaders.md) and [platform-notes.md](platform-notes.md) |
| Make a port faster | [measuring.md](measuring.md) first, then [performance-history.md](performance-history.md), [toolchain.md](toolchain.md) and [mesa.md](mesa.md) |
| Support another edition or language of the game | [editions.md](editions.md) |

## All documents

| Document | What it explains |
|---|---|
| [glossary.md](glossary.md) | Every technical word, in plain words |
| [building.md](building.md) | How to build the NRO, the driver and the shader library, step by step |
| [porting-another-game.md](porting-another-game.md) | What you can reuse for another game, and in which order to work |
| [native-renderer.md](native-renderer.md) | How the port draws the game with Vulkan |
| [backend-mali.md](backend-mali.md) | The native renderer's Mali mode for low-end Android GPUs (Mali-G52): architecture, frame and texture flow, every hack (in Spanish) |
| [mali-g52-vulkan.md](mali-g52-vulkan.md) | What the Mali-G52 MC2 exposes in Vulkan: features, extensions, limits, memory and formats (in Spanish) |
| [plan-optimizacion-mali.md](plan-optimizacion-mali.md) | The optimization to-do list for the Mali mode (in Spanish) |
| [mejoras-mali-vk11.md](mejoras-mali-vk11.md) | Shader and backend improvements for the Mali mode on Samsung's Vulkan 1.1 driver, from what ARM's compiler and the driver captures showed (in Spanish) |
| [herramientas-mali.md](herramientas-mali.md) | The tools to capture the Mali driver, read ARM's compiled shaders and build our PanVK, with how to build them and what they need (in Spanish) |
| [shaders.md](shaders.md) | How the game's shaders are translated, and what had to be fixed |
| [toolchain.md](toolchain.md) | How the game's code is translated and compiled, and the build options that make it faster |
| [mesa.md](mesa.md) | The graphics driver and this port's changes to it |
| [platform-notes.md](platform-notes.md) | Things about the Switch system that cost a lot of time to find out |
| [audio-and-video.md](audio-and-video.md) | The game's audio and cutscenes on the Switch |
| [editions.md](editions.md) | How every edition and language of the game is supported |
| [measuring.md](measuring.md) | How to measure performance on the console without being misled |
| [performance-history.md](performance-history.md) | How the frame rate went from a few FPS to about 30, step by step |
