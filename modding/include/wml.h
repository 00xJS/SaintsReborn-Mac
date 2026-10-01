/*
 * Whompay's Mod Loader (WML) - API for native (DLL) mods.
 *
 * A native mod is a DLL in its mod folder that exports:
 *
 *   WML_EXPORT int wml_mod_init(const WmlApi* api, const WmlMod* mod);
 *
 * It is called once at startup, after the game executable has been loaded into
 * memory and before the game starts running. Return 0 on success; any other
 * value unloads the mod.
 *
 * Guest addresses are Xbox 360 addresses (e.g. 0x82000000 for code). Values in
 * guest memory are big-endian; the read/write helpers convert for you.
 *
 * Part of Saints Reborn (MIT License).
 */
#ifndef WML_H_
#define WML_H_

#include <stdint.h>

#define WML_API_VERSION 1

#ifdef _WIN32
#define WML_EXPORT __declspec(dllexport)
#else
#define WML_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Guest CPU state of the thread running a hooked function. */
typedef struct WmlContext WmlContext;

/* Signature of every recompiled game function (and of hook functions). */
typedef void (*WmlGuestFunction)(WmlContext* ctx, uint8_t* base);

typedef void (*WmlFrameCallback)(void* user);

/* Describes the mod being initialised. */
typedef struct WmlMod {
  const char* name;   /* from mod.ini */
  const char* folder; /* full path of the mod folder, UTF-8 */
} WmlMod;

typedef struct WmlApi {
  uint32_t version; /* WML_API_VERSION implemented by the loader */
  uint32_t size;    /* sizeof(WmlApi) of the loader */

  /* Writes a line to mods/wml.log and the game log. */
  void (*log)(const WmlMod* mod, const char* message);

  /* Guest memory access. Reading or writing unmapped memory crashes the game. */
  uint8_t (*read_u8)(uint32_t address);
  uint16_t (*read_u16)(uint32_t address);
  uint32_t (*read_u32)(uint32_t address);
  float (*read_f32)(uint32_t address);
  void (*write_u8)(uint32_t address, uint8_t value);
  void (*write_u16)(uint32_t address, uint16_t value);
  void (*write_u32)(uint32_t address, uint32_t value);
  void (*write_f32)(uint32_t address, float value);
  /* Host pointer to guest memory (data stays big-endian). */
  void* (*guest_pointer)(uint32_t address);

  /* Replaces the game function at guest address `function`. `*original`
     receives a function that runs the previous implementation. Several mods
     may hook the same function; they are chained. Returns 0 on success.
     Must be called from wml_mod_init. */
  int (*hook)(uint32_t function, WmlGuestFunction hook, WmlGuestFunction* original);

  /* Calls the game function at `function` with the given context (only valid
     inside a hook, using the context it received). Returns 0 on success. */
  int (*call)(WmlContext* ctx, uint32_t function);

  /* Registers: r0-r31 (integer), f0-f31 (floating point), lr. */
  uint64_t (*get_r)(WmlContext* ctx, int index);
  void (*set_r)(WmlContext* ctx, int index, uint64_t value);
  double (*get_f)(WmlContext* ctx, int index);
  void (*set_f)(WmlContext* ctx, int index, double value);
  uint32_t (*get_lr)(WmlContext* ctx);

  /* Called once per rendered frame, on the game's render thread. */
  void (*on_frame)(WmlFrameCallback callback, void* user);

  /* Keyboard state (Windows virtual-key codes). Always 0 while the game window
     is not focused. key_pressed is 1 only on the frame the key went down. */
  int (*key_down)(int virtual_key);
  int (*key_pressed)(int virtual_key);

  /* Optional tail extension: plain text drawn over the game, with a shadow.
     Pass an empty string to hide. No focus or mouse capture. Copies the text.
     Each mod has its own text; texts from several mods are shown one below
     the other.
     Check size >= sizeof(WmlApi) before using this extended API. */
  void (*overlay_text)(const char* text);

  /* Optional tail extension: registers a callback at the game-loop boundary,
     before simulation/render. Useful for state that must be applied before
     the frame is built. New functions are only ever added at the end, so
     mods built against an older wml.h keep working. */
  void (*on_game_frame)(WmlFrameCallback callback, void* user);

  /* Optional tail extension: glowing lines in the 3D world, drawn over the
     game (no depth test), e.g. beams. data holds count beams of 10 floats:
     start x y z, end x y z (world), width in pixels at 1080p, red, green, blue
     (0-1). Replaces the beams sent before; count 0 clears them. Beams not
     sent again for 150 ms disappear. Copies the data. */
  void (*overlay_beams)(const float* data, int count);

  /* Optional tail extension: the finished picture's look (the Modern Look
     mod). data holds count floats: sharpen (0-1), bloom strength, bloom
     threshold (0-1), exposure (stops), contrast (1 = unchanged), saturation
     (1 = unchanged), vibrance, filmic curve (0-1), temperature (-1..1, +warm),
     tint (-1..1, +magenta), vignette (0-1), black level (0-0.2), FXAA (0/1),
     anisotropic filtering (1, 2, 4, 8 or 16; 0 = the game's). count 0 (or
     data NULL) switches the look off: the original picture. */
  void (*set_look)(const float* data, int count);
} WmlApi;

typedef int (*WmlModInitFunction)(const WmlApi* api, const WmlMod* mod);

#ifdef __cplusplus
}
#endif

#endif /* WML_H_ */
