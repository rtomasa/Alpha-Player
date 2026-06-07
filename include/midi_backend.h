#ifndef APLAYER_MIDI_BACKEND_H
#define APLAYER_MIDI_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

bool midi_backend_is_path(const char *path);
bool midi_backend_load(const char *midi_path, const char *output,
      const char *system_dir, unsigned sample_rate,
      const struct retro_midi_interface *midi_interface);
bool midi_backend_restart(void);
void midi_backend_pause(bool paused);
void midi_backend_unload(void);
bool midi_backend_active(void);
bool midi_backend_done(void);
bool midi_backend_is_raw(void);
const char *midi_backend_output_name(void);
size_t midi_backend_render(int16_t *buffer, size_t frames);

#endif
