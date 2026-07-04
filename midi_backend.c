#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/midi_backend.h"

#define TML_IMPLEMENTATION
#include "include/third_party/tml.h"
#define TSF_IMPLEMENTATION
#include "include/third_party/tsf.h"

#define MIDI_BACKEND_PATH_MAX 4096
#define MIDI_OUTPUT_RAW "raw"
#define MIDI_TAIL_SECONDS 1
#define FLUID_OK 0
#define FLUID_PLAYER_DONE 3

typedef struct fluid_settings_t fluid_settings_t;
typedef struct fluid_synth_t fluid_synth_t;
typedef struct fluid_player_t fluid_player_t;

struct fluid_api
{
   void *handle;
   fluid_settings_t *(*new_settings)(void);
   void (*delete_settings)(fluid_settings_t *settings);
   int (*settings_setnum)(fluid_settings_t *settings, const char *name, double value);
   int (*settings_setint)(fluid_settings_t *settings, const char *name, int value);
   int (*settings_setstr)(fluid_settings_t *settings, const char *name, const char *value);
   fluid_synth_t *(*new_synth)(fluid_settings_t *settings);
   void (*delete_synth)(fluid_synth_t *synth);
   int (*synth_sfload)(fluid_synth_t *synth, const char *filename, int reset_presets);
   int (*synth_write_s16)(fluid_synth_t *synth, int len,
         void *left, int left_offset, int left_increment,
         void *right, int right_offset, int right_increment);
   int (*synth_system_reset)(fluid_synth_t *synth);
   fluid_player_t *(*new_player)(fluid_synth_t *synth);
   void (*delete_player)(fluid_player_t *player);
   int (*player_add)(fluid_player_t *player, const char *midifile);
   int (*player_play)(fluid_player_t *player);
   int (*player_stop)(fluid_player_t *player);
   int (*player_join)(fluid_player_t *player);
   int (*player_get_status)(fluid_player_t *player);
};

struct midi_backend_state
{
   struct fluid_api api;
   fluid_settings_t *settings;
   fluid_synth_t *fluid_synth;
   fluid_player_t *player;
   tsf *tiny_synth;
   tml_message *messages;
   tml_message *next_message;
   struct retro_midi_interface midi_interface;
   char midi_path[MIDI_BACKEND_PATH_MAX];
   char output_name[128];
   unsigned sample_rate;
   uint64_t rendered_frames;
   uint64_t completion_frame;
   bool raw;
   bool active;
   bool using_fluid;
   bool done;
};

static struct midi_backend_state midi;

static bool midi_path_has_extension(const char *path, const char *extension)
{
   const char *dot;

   if (!path || !extension)
      return false;

   dot = strrchr(path, '.');
   if (!dot)
      return false;

   dot++;
   while (*dot && *extension)
   {
      if (tolower((unsigned char)*dot) != tolower((unsigned char)*extension))
         return false;
      dot++;
      extension++;
   }

   return *dot == '\0' && *extension == '\0';
}

bool midi_backend_is_path(const char *path)
{
   return midi_path_has_extension(path, "mid") ||
         midi_path_has_extension(path, "midi") ||
         midi_path_has_extension(path, "kar");
}

static bool fluid_load_symbol(void **symbol, const char *name)
{
   *symbol = dlsym(midi.api.handle, name);
   return *symbol != NULL;
}

#define LOAD_FLUID_SYMBOL(field, name) \
   fluid_load_symbol((void**)&midi.api.field, name)

static bool fluid_api_load(void)
{
   static const char *const library_names[] = {
      "libfluidsynth.so.3",
      "libfluidsynth.so.2",
      "libfluidsynth.so.1",
      "libfluidsynth.so",
      NULL
   };
   unsigned i;

   if (midi.api.handle)
      return true;

   for (i = 0; library_names[i]; i++)
   {
      midi.api.handle = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
      if (midi.api.handle)
         break;
   }

   if (!midi.api.handle)
      return false;

   if (!LOAD_FLUID_SYMBOL(new_settings, "new_fluid_settings") ||
       !LOAD_FLUID_SYMBOL(delete_settings, "delete_fluid_settings") ||
       !LOAD_FLUID_SYMBOL(settings_setnum, "fluid_settings_setnum") ||
       !LOAD_FLUID_SYMBOL(settings_setint, "fluid_settings_setint") ||
       !LOAD_FLUID_SYMBOL(settings_setstr, "fluid_settings_setstr") ||
       !LOAD_FLUID_SYMBOL(new_synth, "new_fluid_synth") ||
       !LOAD_FLUID_SYMBOL(delete_synth, "delete_fluid_synth") ||
       !LOAD_FLUID_SYMBOL(synth_sfload, "fluid_synth_sfload") ||
       !LOAD_FLUID_SYMBOL(synth_write_s16, "fluid_synth_write_s16") ||
       !LOAD_FLUID_SYMBOL(synth_system_reset, "fluid_synth_system_reset") ||
       !LOAD_FLUID_SYMBOL(new_player, "new_fluid_player") ||
       !LOAD_FLUID_SYMBOL(delete_player, "delete_fluid_player") ||
       !LOAD_FLUID_SYMBOL(player_add, "fluid_player_add") ||
       !LOAD_FLUID_SYMBOL(player_play, "fluid_player_play") ||
       !LOAD_FLUID_SYMBOL(player_stop, "fluid_player_stop") ||
       !LOAD_FLUID_SYMBOL(player_join, "fluid_player_join") ||
       !LOAD_FLUID_SYMBOL(player_get_status, "fluid_player_get_status"))
   {
      dlclose(midi.api.handle);
      memset(&midi.api, 0, sizeof(midi.api));
      return false;
   }

   return true;
}

static bool midi_file_readable(const char *path)
{
   return path && path[0] && access(path, R_OK) == 0;
}

static bool midi_build_soundfont_path(char *path, size_t path_size,
      const char *system_dir, const char *subdir, const char *filename)
{
   int written;

   if (!path || path_size == 0 || !system_dir || !system_dir[0] || !filename)
      return false;

   written = snprintf(path, path_size, "%s%s%s%s%s",
         system_dir,
         system_dir[strlen(system_dir) - 1] == '/' ? "" : "/",
         subdir ? subdir : "",
         subdir && subdir[0] ? "/" : "",
         filename);

   return written > 0 && (size_t)written < path_size && midi_file_readable(path);
}

static bool midi_find_named_soundfont(char *path, size_t path_size,
      const char *system_dir, const char *filename)
{
   static const char *const system_subdirs[] = {
      "scummvm/soundfonts",
      "scummvm/extra",
      "",
      NULL
   };
   unsigned i;

   for (i = 0; system_subdirs[i]; i++)
      if (midi_build_soundfont_path(path, path_size, system_dir,
               system_subdirs[i], filename))
         return true;

   if (snprintf(path, path_size, "/usr/share/sounds/sf2/%s", filename) > 0 &&
       midi_file_readable(path))
      return true;

   path[0] = '\0';
   return false;
}

static const char *midi_output_filename(const char *output)
{
   if (!output || !output[0] || strcmp(output, "default") == 0)
      return NULL;
   if (strcmp(output, "roland_sc55") == 0)
      return "Roland_SC-55.sf2";
   if (strcmp(output, "gm_roland") == 0)
      return "GM_Roland.sf2";
   if (strcmp(output, "fluidr3_gm") == 0)
      return "FluidR3_GM.sf2";
   if (strcmp(output, "uhd3") == 0)
      return "UHD3.sf2";

   return NULL;
}

static bool midi_find_soundfont(char *path, size_t path_size,
      const char *system_dir, const char *output)
{
   static const char *const default_fonts[] = {
      "Roland_SC-55.sf2",
      "GM_Roland.sf2",
      "FluidR3_GM.sf2",
      "UHD3.sf2",
      NULL
   };
   const char *filename = midi_output_filename(output);
   unsigned i;

   if (filename &&
       midi_find_named_soundfont(path, path_size, system_dir, filename))
      return true;

   for (i = 0; default_fonts[i]; i++)
      if (midi_find_named_soundfont(path, path_size, system_dir, default_fonts[i]))
         return true;

   return false;
}

static bool midi_raw_write(const uint8_t *data, size_t size)
{
   size_t i;

   if (!midi.midi_interface.write)
      return false;

   for (i = 0; i < size; i++)
      if (!midi.midi_interface.write(data[i], 0))
         return false;

   return !midi.midi_interface.flush || midi.midi_interface.flush();
}

static void midi_raw_reset(void)
{
   unsigned channel;
   uint8_t message[3];

   if (!midi.raw || !midi.midi_interface.write)
      return;

   for (channel = 0; channel < 16; channel++)
   {
      message[0] = (uint8_t)(0xb0 | channel);
      message[1] = TML_ALL_NOTES_OFF;
      message[2] = 0;
      midi_raw_write(message, sizeof(message));
      message[1] = TML_ALL_SOUND_OFF;
      midi_raw_write(message, sizeof(message));
   }
}

static void midi_raw_event(const tml_message *event)
{
   uint8_t message[3];
   size_t size;

   if (!midi.raw || !event)
      return;

   message[0] = (uint8_t)(event->type | (event->channel & 0x0f));
   size = 3;

   switch (event->type)
   {
      case TML_NOTE_OFF:
      case TML_NOTE_ON:
         message[1] = (uint8_t)event->key;
         message[2] = (uint8_t)event->velocity;
         break;
      case TML_KEY_PRESSURE:
         message[1] = (uint8_t)event->key;
         message[2] = (uint8_t)event->key_pressure;
         break;
      case TML_CONTROL_CHANGE:
         message[1] = (uint8_t)event->control;
         message[2] = (uint8_t)event->control_value;
         break;
      case TML_PROGRAM_CHANGE:
         message[1] = (uint8_t)event->program;
         size = 2;
         break;
      case TML_CHANNEL_PRESSURE:
         message[1] = (uint8_t)event->channel_pressure;
         size = 2;
         break;
      case TML_PITCH_BEND:
         message[1] = (uint8_t)(event->pitch_bend & 0x7f);
         message[2] = (uint8_t)((event->pitch_bend >> 7) & 0x7f);
         break;
      default:
         return;
   }

   midi_raw_write(message, size);
}

static void midi_tiny_event(const tml_message *event)
{
   unsigned channel;

   if (!midi.tiny_synth || !event)
      return;

   channel = event->channel & 0x0f;

   switch (event->type)
   {
      case TML_NOTE_OFF:
         tsf_channel_note_off(midi.tiny_synth, channel, (uint8_t)event->key);
         break;
      case TML_NOTE_ON:
         if (event->velocity)
            tsf_channel_note_on(midi.tiny_synth, channel, (uint8_t)event->key,
                  (uint8_t)event->velocity / 127.0f);
         else
            tsf_channel_note_off(midi.tiny_synth, channel, (uint8_t)event->key);
         break;
      case TML_CONTROL_CHANGE:
         tsf_channel_midi_control(midi.tiny_synth, channel,
               (uint8_t)event->control, (uint8_t)event->control_value);
         break;
      case TML_PROGRAM_CHANGE:
         tsf_channel_set_presetnumber(midi.tiny_synth, channel,
               (uint8_t)event->program, channel == 9);
         break;
      case TML_PITCH_BEND:
         tsf_channel_set_pitchwheel(midi.tiny_synth, channel, event->pitch_bend);
         break;
      default:
         break;
   }
}

static void midi_fluid_player_destroy(void)
{
   if (!midi.player)
      return;

   midi.api.player_stop(midi.player);
   midi.api.player_join(midi.player);
   midi.api.delete_player(midi.player);
   midi.player = NULL;
}

static void midi_fluid_destroy(void)
{
   midi_fluid_player_destroy();

   if (midi.fluid_synth)
   {
      midi.api.delete_synth(midi.fluid_synth);
      midi.fluid_synth = NULL;
   }
   if (midi.settings)
   {
      midi.api.delete_settings(midi.settings);
      midi.settings = NULL;
   }
   midi.using_fluid = false;
}

static bool midi_fluid_player_create(void)
{
   midi.player = midi.api.new_player(midi.fluid_synth);
   if (!midi.player)
      return false;

   if (midi.api.player_add(midi.player, midi.midi_path) != FLUID_OK ||
       midi.api.player_play(midi.player) != FLUID_OK)
   {
      midi_fluid_player_destroy();
      return false;
   }

   return true;
}

static bool midi_fluid_create(const char *soundfont_path)
{
   if (!fluid_api_load())
      return false;

   midi.settings = midi.api.new_settings();
   if (!midi.settings)
      goto error;

   midi.api.settings_setnum(midi.settings, "synth.sample-rate", midi.sample_rate);
   midi.api.settings_setstr(midi.settings, "player.timing-source", "sample");
   midi.api.settings_setint(midi.settings, "player.reset-synth", 1);

   midi.fluid_synth = midi.api.new_synth(midi.settings);
   if (!midi.fluid_synth ||
       midi.api.synth_sfload(midi.fluid_synth, soundfont_path, 1) < 0 ||
       !midi_fluid_player_create())
      goto error;

   midi.using_fluid = true;
   return true;

error:
   midi_fluid_destroy();
   return false;
}

static bool midi_tiny_create(const char *soundfont_path)
{
   midi.messages = tml_load_filename(midi.midi_path);
   if (!midi.messages)
      return false;

   if (soundfont_path && soundfont_path[0])
      midi.tiny_synth = tsf_load_filename(soundfont_path);

   if (!midi.raw && !midi.tiny_synth)
      return false;

   if (midi.tiny_synth)
      tsf_set_output(midi.tiny_synth, TSF_STEREO_INTERLEAVED,
            (int)midi.sample_rate, 0.0f);

   midi.next_message = midi.messages;
   midi.completion_frame = UINT64_MAX;
   return true;
}

static bool midi_raw_available(const struct retro_midi_interface *midi_interface)
{
   return midi_interface && midi_interface->write &&
         (!midi_interface->output_enabled || midi_interface->output_enabled());
}

bool midi_backend_load(const char *midi_path, const char *output,
      const char *system_dir, unsigned sample_rate,
      const struct retro_midi_interface *midi_interface)
{
   char soundfont_path[MIDI_BACKEND_PATH_MAX] = {0};
   const char *filename;

   midi_backend_unload();

   if (!midi_backend_is_path(midi_path) || sample_rate == 0)
      return false;

   midi.raw = output && strcmp(output, MIDI_OUTPUT_RAW) == 0;
   midi.sample_rate = sample_rate;
   snprintf(midi.midi_path, sizeof(midi.midi_path), "%s", midi_path);

   if (midi.raw && midi_raw_available(midi_interface))
   {
      midi.midi_interface = *midi_interface;
      midi_find_soundfont(soundfont_path, sizeof(soundfont_path),
            system_dir, "default");
      snprintf(midi.output_name, sizeof(midi.output_name), "Frontend MIDI (Raw)");
   }
   else
   {
      const char *soundfont_output = midi.raw ? "default" : output;
      midi.raw = false;

      if (!midi_find_soundfont(soundfont_path, sizeof(soundfont_path),
               system_dir, soundfont_output))
         goto error;

      filename = strrchr(soundfont_path, '/');
      filename = filename ? filename + 1 : soundfont_path;

      if (midi_fluid_create(soundfont_path))
      {
         snprintf(midi.output_name, sizeof(midi.output_name), "%.*s (FluidSynth)",
               (int)sizeof(midi.output_name) - 14, filename);
         midi.active = true;
         return true;
      }
   }

   if (!midi_tiny_create(soundfont_path))
      goto error;

   if (!midi.raw)
   {
      filename = strrchr(soundfont_path, '/');
      filename = filename ? filename + 1 : soundfont_path;
      snprintf(midi.output_name, sizeof(midi.output_name), "%.*s (embedded)",
            (int)sizeof(midi.output_name) - 12, filename);
   }

   midi.active = true;
   return true;

error:
   midi_backend_unload();
   return false;
}

bool midi_backend_restart(void)
{
   if (!midi.active)
      return false;

   midi.done = false;
   midi.rendered_frames = 0;
   midi.completion_frame = UINT64_MAX;

   if (midi.using_fluid)
   {
      midi_fluid_player_destroy();
      midi.api.synth_system_reset(midi.fluid_synth);
      return midi_fluid_player_create();
   }

   midi.next_message = midi.messages;
   if (midi.tiny_synth)
   {
      tsf_reset(midi.tiny_synth);
      tsf_set_output(midi.tiny_synth, TSF_STEREO_INTERLEAVED,
            (int)midi.sample_rate, 0.0f);
   }
   midi_raw_reset();
   return true;
}

void midi_backend_pause(bool paused)
{
   if (paused)
      midi_raw_reset();
}

void midi_backend_unload(void)
{
   midi_raw_reset();
   midi_fluid_destroy();

   if (midi.tiny_synth)
      tsf_close(midi.tiny_synth);
   if (midi.messages)
      tml_free(midi.messages);

   midi.tiny_synth = NULL;
   midi.messages = NULL;
   midi.next_message = NULL;
   memset(&midi.midi_interface, 0, sizeof(midi.midi_interface));
   midi.midi_path[0] = '\0';
   midi.output_name[0] = '\0';
   midi.sample_rate = 0;
   midi.rendered_frames = 0;
   midi.completion_frame = UINT64_MAX;
   midi.raw = false;
   midi.active = false;
   midi.done = false;
}

bool midi_backend_active(void)
{
   return midi.active;
}

bool midi_backend_done(void)
{
   if (!midi.active)
      return false;
   if (midi.using_fluid)
      return midi.player &&
            midi.api.player_get_status(midi.player) == FLUID_PLAYER_DONE;
   return midi.done;
}

bool midi_backend_is_raw(void)
{
   return midi.active && midi.raw;
}

const char *midi_backend_output_name(void)
{
   return midi.output_name;
}

static uint64_t midi_event_frame(const tml_message *event)
{
   return (uint64_t)event->time * midi.sample_rate / 1000;
}

size_t midi_backend_render(int16_t *buffer, size_t frames)
{
   size_t rendered = 0;

   if (!midi.active || !buffer || frames == 0)
      return 0;

   if (midi.using_fluid)
   {
      memset(buffer, 0, frames * sizeof(int16_t) * 2);
      if (midi.api.synth_write_s16(midi.fluid_synth, (int)frames,
               buffer, 0, 2, buffer, 1, 2) != FLUID_OK)
         return 0;
      return frames;
   }

   while (rendered < frames)
   {
      uint64_t current_frame = midi.rendered_frames + rendered;
      size_t segment = frames - rendered;

      while (midi.next_message &&
             midi_event_frame(midi.next_message) <= current_frame)
      {
         midi_raw_event(midi.next_message);
         midi_tiny_event(midi.next_message);
         midi.next_message = midi.next_message->next;

         if (!midi.next_message)
            midi.completion_frame =
               current_frame + (uint64_t)midi.sample_rate * MIDI_TAIL_SECONDS;
      }

      if (midi.next_message)
      {
         uint64_t event_frame = midi_event_frame(midi.next_message);
         uint64_t distance = event_frame - current_frame;
         if (distance < segment)
            segment = (size_t)distance;
      }

      if (segment == 0)
         continue;

      if (midi.tiny_synth)
         tsf_render_short(midi.tiny_synth, buffer + rendered * 2,
               (int)segment, 0);
      else
         memset(buffer + rendered * 2, 0, segment * sizeof(int16_t) * 2);
      rendered += segment;
   }

   midi.rendered_frames += rendered;
   if (midi.completion_frame != UINT64_MAX &&
       midi.rendered_frames >= midi.completion_frame)
      midi.done = true;

   return rendered;
}
