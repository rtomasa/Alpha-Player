#include <retro_common_api.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <ass/ass.h>
#include "include/ffmpeg_core.h"
#include "include/ffmpeg_fft.h"

#include <glsym/glsym.h>
#include <features/features_cpu.h>
#include <retro_miscellaneous.h>
#include <rthreads/rthreads.h>
#include <rthreads/tpool.h>
#include <queues/fifo_queue.h>
#include <string/stdstring.h>
#include "include/packet_buffer.h"
#include "include/video_buffer.h"

#include <libretro.h>
#include <unistd.h>

#include <string.h>
#include <ctype.h>
#include <libgen.h> // for dirname()
#include <limits.h> // for PATH_MAX

// Retro callbacks
static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
retro_log_printf_t log_cb;

#define INFO_RESTART "Requires Restart"

#define PRINT_VERSION(s) log_cb(RETRO_LOG_INFO, "[APLAYER] lib%s version:\t%d.%d.%d\n", #s, \
   s ##_version() >> 16 & 0xFF, \
   s ##_version() >> 8 & 0xFF, \
   s ##_version() & 0xFF);

#define MAX_PLAYLIST_ENTRIES 256
static char playlist[MAX_PLAYLIST_ENTRIES][PATH_MAX];
static unsigned playlist_count = 0;
static unsigned playlist_index = 0;  

static bool reset_triggered;
static bool libretro_supports_bitmasks = false;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

/* FFmpeg context data. */
static AVFormatContext *fctx;
static AVCodecContext *vctx;
static int video_stream_index;
static int loopcontent;
static bool is_crt = false;
static const unsigned subtitle_font_base_size = 24;
static unsigned subtitle_font_size = 64;
static bool subtitle_font_auto = false;
static double subtitle_font_scale = 1.0;
static const char *subtitle_font_name = "DejaVu Sans";
static int subtitle_font_bold = 0;
static bool subtitles_enabled = true;
static char subtitle_style_override_font[128];
static char subtitle_style_override_bold[64];
static char *subtitle_style_overrides[] = {
   subtitle_style_override_font,
   subtitle_style_override_bold,
   NULL
};

static double g_current_time = 0.0;  // PTS in seconds
static slock_t *time_lock    = NULL; // Protects g_current_time

static unsigned sw_decoder_threads;
static unsigned sw_sws_threads;
static video_buffer_t *video_buffer;
static tpool_t *tpool;

#define MAX_STREAMS 8
static AVCodecContext *actx[MAX_STREAMS];
static AVCodecContext *sctx[MAX_STREAMS];
static int audio_streams[MAX_STREAMS];
static int audio_streams_num;
static int audio_streams_ptr;
static int subtitle_streams[MAX_STREAMS];
static int subtitle_streams_num;
static int subtitle_streams_ptr;
static bool subtitle_is_ass[MAX_STREAMS];
static bool subtitle_is_external[MAX_STREAMS];

/* ASS/SSA and text-based subtitles via libass. */
static ASS_Library *ass;
static ASS_Renderer *ass_render;
static ASS_Track *ass_track[MAX_STREAMS];
static uint8_t *ass_extra_data[MAX_STREAMS];
static size_t ass_extra_data_size[MAX_STREAMS];
static slock_t *ass_lock;
static bool first_subtitle_event_logged;
static bool first_ass_render_logged;
static bool first_ass_image_logged[MAX_STREAMS];
static bool first_ass_after_sub_logged[MAX_STREAMS];
static int64_t first_subtitle_start_ms[MAX_STREAMS];

static struct attachment *attachments;
static size_t attachments_size;

static fft_t *fft;
unsigned fft_width;
unsigned fft_height;
static bool fft_enabled;

/* A/V timing. */
static uint64_t frame_cnt;
static uint64_t audio_frames;
static double pts_bias;

/* Threaded FIFOs. */
static volatile bool decode_thread_dead;
static fifo_buffer_t *audio_decode_fifo;
static scond_t *fifo_cond;
static scond_t *fifo_decode_cond;
static slock_t *fifo_lock;
static slock_t *decode_thread_lock;
static sthread_t *decode_thread_handle;
static double decode_last_audio_time;
static bool main_sleeping;

/* Seeking, play, pause, loop */
static bool do_seek;
static double seek_time;
static bool paused = false;
static volatile bool audio_switch_requested = false;
static bool seek_supported = true;
static bool time_supported = true;
static bool gme_seek_disabled = false;

/* GL stuff */
static struct frame frames[2];
static unsigned frames_tex_width;
static unsigned frames_tex_height;

static struct retro_hw_render_callback hw_render;
static GLuint prog;
static GLuint vbo;
static GLint vertex_loc;
static GLint tex_loc;
static GLint mix_loc;

static void media_reset_defaults(void)
{
   memset(&media, 0, sizeof(media));
   media.interpolate_fps = 60.0;   // your current default
   media.sample_rate     = 32000.0; // safe default until audio stream is opened
   media.aspect          = 0.0f;    // force recompute
   seek_supported        = true;
   time_supported        = true;
   gme_seek_disabled     = false;
}

static void init_aspect_ratio(void)
{
   if (!vctx || video_stream_index < 0)
      return;
      
   AVStream *video_stream = fctx->streams[video_stream_index];
   AVRational sar = {0, 1}; // Default to square pixels
   
   // Priority order for aspect ratio sources:
   
   // 1. Try codec context sample aspect ratio
   if (vctx->sample_aspect_ratio.num > 0 && vctx->sample_aspect_ratio.den > 0)
      sar = vctx->sample_aspect_ratio;
   
   // 2. Try stream sample aspect ratio
   else if (video_stream->sample_aspect_ratio.num > 0 && 
            video_stream->sample_aspect_ratio.den > 0)
      sar = video_stream->sample_aspect_ratio;
   
   // 3. Try codec parameters sample aspect ratio  
   else if (video_stream->codecpar->sample_aspect_ratio.num > 0 &&
            video_stream->codecpar->sample_aspect_ratio.den > 0)
      sar = video_stream->codecpar->sample_aspect_ratio;
   
   // Check for display aspect ratio in metadata
   AVDictionaryEntry *dar_tag = av_dict_get(video_stream->metadata, "DAR", NULL, 0);
   if (dar_tag) {
      // Parse DAR string (e.g., "16:9", "4:3")
      log_cb(RETRO_LOG_INFO, "[APLAYER] Container DAR: %s\n", dar_tag->value);
   }
   
   // Calculate display aspect ratio
   media.aspect = (float)media.width * av_q2d(sar) / (float)media.height;

   if (media.aspect == 0.0f)
      media.aspect = (float)media.width / (float)media.height;
   
   log_cb(RETRO_LOG_INFO, "[APLAYER] Video aspect ratio: %.3f (SAR: %d:%d)\n", 
          media.aspect, sar.num, sar.den);
}

static bool parse_m3u_playlist(const char* path)
{
   log_cb(RETRO_LOG_INFO, "[APLAYER] Opening M3U playlist: %s\n", path);

   FILE* file = fopen(path, "r");
   if (!file)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Unable to open M3U playlist: %s\n", path);
      return false;
   }

   char line[PATH_MAX];
   char playlist_dir[PATH_MAX];
   strncpy(playlist_dir, path, PATH_MAX - 1);
   playlist_dir[PATH_MAX - 1] = '\0';

   // Get the directory path of the playlist
   char *dir = dirname(playlist_dir);

   playlist_count = 0;

   while (fgets(line, sizeof(line), file) && playlist_count < MAX_PLAYLIST_ENTRIES)
   {
      char* trimmed = line;
      while (isspace(*trimmed)) trimmed++;

      if (*trimmed == '#' || *trimmed == '\0')
         continue; // Skip comments and empty lines

      size_t len = strlen(trimmed);
      while (len > 0 && (trimmed[len-1] == '\r' || trimmed[len-1] == '\n'))
         trimmed[--len] = '\0';

      /* Build absolute path and verify that it exists.        */
      char full_path[PATH_MAX];
      snprintf(full_path, PATH_MAX, "%s/%s", dir, trimmed);

      if (access(full_path, R_OK) == 0)
         strncpy(playlist[playlist_count++], full_path, PATH_MAX - 1);
      else
         log_cb(RETRO_LOG_WARN,
                "[APLAYER] Skipping missing entry: %s\n", full_path);

      log_cb(RETRO_LOG_INFO, "[APLAYER] Found: %s\n", full_path);
   }

   fclose(file);

   if (playlist_count == 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] No valid entries in playlist: %s\n", path);
      return false;
   }

   playlist_index = 0;
   return true;
}

static void ass_msg_cb(int level, const char *fmt, va_list args, void *data)
{
   char buffer[4096];
   (void)data;

   if (level < 6)
   {
      vsnprintf(buffer, sizeof(buffer), fmt, args);
      log_cb(RETRO_LOG_INFO, "[APLAYER] %s\n", buffer);
   }
}

static int get_media_type()
{
   int type;

   if (audio_streams_num > 0 && video_stream_index < 0)
      type = MEDIA_TYPE_AUDIO;
   else if (video_stream_index >= 0)
      type = MEDIA_TYPE_VIDEO;
   else
      type = MEDIA_TYPE_UNKNOWN;

   return type;
}

static bool duration_is_valid(double seconds)
{
   const double max_duration = 365.0 * 24.0 * 60.0 * 60.0;
   return seconds > 0.0 && seconds < max_duration;
}

static bool is_gme_extension(const char *ext)
{
   static const char *const exts[] = {
      ".ay", ".gbs", ".gym", ".hes", ".kss",
      ".nsf", ".nsfe", ".sap", ".spc", ".vgm", ".vgz",
      NULL
   };

   if (!ext || !*ext)
      return false;

   for (int i = 0; exts[i]; i++)
   {
      if (strcasecmp(ext, exts[i]) == 0)
         return true;
   }

   return false;
}

static bool is_gme_path(const char *path)
{
   const char *ext = path ? strrchr(path, '.') : NULL;
   return is_gme_extension(ext);
}

static double stream_duration_seconds(int stream_index)
{
   if (!fctx || stream_index < 0 || stream_index >= (int)fctx->nb_streams)
      return 0.0;

   AVStream *st = fctx->streams[stream_index];
   if (!st || st->duration == AV_NOPTS_VALUE || st->duration <= 0)
      return 0.0;

   return st->duration * av_q2d(st->time_base);
}

static void format_time_hhmmss(double seconds, char *out, size_t out_size)
{
   if (!out || out_size == 0)
      return;

   if (seconds < 0.0)
      seconds = 0.0;

   unsigned total = (unsigned)seconds;
   unsigned hours = total / 3600;
   unsigned minutes = (total % 3600) / 60;
   unsigned secs = total % 60;

   snprintf(out, out_size, "%02u:%02u:%02u", hours, minutes, secs);
}

static void show_not_supported_message(void)
{
   struct retro_message_ext msg_obj = {0};

   msg_obj.msg      = "Not supported";
   msg_obj.duration = 2000;
   msg_obj.priority = 1;
   msg_obj.level    = RETRO_LOG_WARN;
   msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
   msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
   msg_obj.progress = -1;

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
}

static void display_media_title()
{
   if (!fctx || decode_thread_dead)
      return;

   char msg[256];
   struct retro_message_ext msg_obj = {0};
   msg[0] = '\0';

   if (media.title) {
      snprintf(msg, sizeof(msg), "%s", media.title->value);
   } else {
      snprintf(msg, sizeof(msg), "%s", "Title not available");
   }

   msg_obj.msg      = msg;
   msg_obj.duration = 3000;
   msg_obj.priority = 1;
   msg_obj.level    = RETRO_LOG_INFO;
   msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
   msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
   msg_obj.progress = -1;
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
}

static void append_attachment(const uint8_t *data, size_t size)
{
   attachments = (struct attachment*)av_realloc(
         attachments, (attachments_size + 1) * sizeof(*attachments));

   attachments[attachments_size].data = (uint8_t*)av_malloc(size);
   attachments[attachments_size].size = size;
   memcpy(attachments[attachments_size].data, data, size);

   attachments_size++;
}

void retro_init(void)
{
   reset_triggered = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;
}

void retro_deinit(void)
{
   libretro_supports_bitmasks = false;

   if (video_buffer)
   {
      video_buffer_destroy(video_buffer);
      video_buffer = NULL;
   }

   if (tpool)
   {
      tpool_wait(tpool); // Wait for tasks to finish
      tpool_destroy(tpool);
      tpool = NULL;
   }
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Alpha Player";
   info->library_version  = "v2.3.0";
   info->need_fullpath    = true;
   info->valid_extensions = "mkv|avi|f4v|f4f|3gp|ogm|flv|mp4|mp3|flac|ogg|m4a|webm|3g2|mov|wmv|mpg|mpeg|vob|asf|divx|m2p|m2ts|ps|ts|mxf|wma|wav|m3u|s3m|it|xm|mod|ay|gbs|gym|hes|kss|nsf|nsfe|sap|spc|vgm|vgz";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   unsigned width  = vctx ? media.width : 320;
   unsigned height = vctx ? media.height : 240;
   float aspect    = vctx ? media.aspect : (float)width / (float)height;

   if (audio_streams_num > 0 && video_stream_index < 0)
   {
      width = fft_width;
      height = fft_height;
      aspect = (float)fft_width / (float)fft_height;
   }
   info->timing.fps = media.interpolate_fps;
   if (actx[0] && media.sample_rate > 0)
      info->timing.sample_rate = media.sample_rate;
   else
      info->timing.sample_rate = 32000.0;

   info->geometry.base_width   = width;
   info->geometry.base_height  = height;
   info->geometry.max_width    = width;
   info->geometry.max_height   = height;
   info->geometry.aspect_ratio = aspect;
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_core_option_v2_category option_categories[] =
   {
      {"video", "Video", "Video settings"},
      {"music", "Music", "Music Settings"},
      {NULL, NULL, NULL}
   };

   struct retro_core_option_v2_definition option_definitions[] =
   {
      {
         "aplayer_sw_decoder_threads", "Software Decoder Threads (restart)", NULL, INFO_RESTART, NULL, "video",
         {
            {"auto", "Automatic"},
            {"1", NULL},
            {"2", NULL},
            {"3", NULL},
            {"4", NULL},
            {NULL, NULL}
         }, "auto"
      },
      {
         "aplayer_subtitles", "Subtitles", NULL, NULL, NULL, "video",
         {
            {"enabled", "Enabled"},
            {"disabled", "Disabled"},
            {NULL, NULL}
         }, "enabled"
      },
      {
         "aplayer_subtitle_font_size", "Subtitle Font Size", NULL, NULL, NULL, "video",
         {
            {"auto", "Auto"},
            {"24", NULL},
            {"32", NULL},
            {"40", NULL},
            {"48", NULL},
            {"56", NULL},
            {"64", NULL},
            {"72", NULL},
            {"80", NULL},
            {"88", NULL},
            {"96", NULL},
            {"104", NULL},
            {"112", NULL},
            {"120", NULL},
            {"128", NULL},
            {NULL, NULL}
         }, "auto"
      },
      {
         "aplayer_subtitle_font", "Subtitle Font", NULL, NULL, NULL, "video",
         {
            {"dejavu_sans", "DejaVu Sans"},
            {"dejavu_sans_bold", "DejaVu Sans Bold"},
            {"dejavu_sans_mono", "DejaVu Sans Mono"},
            {"dejavu_sans_mono_bold", "DejaVu Sans Mono Bold"},
            {"dejavu_serif", "DejaVu Serif"},
            {"dejavu_serif_bold", "DejaVu Serif Bold"},
            {NULL, NULL}
         }, "dejavu_sans_mono_bold"
      },
      {
         "aplayer_visualizer", "Visualizer", NULL, NULL, NULL, "music",
         {
            {"enabled", "Enabled"},
            {"disabled", "Disabled"},
            {NULL, NULL}
         }, "enabled"
      },
      {
         "aplayer_loop_content", "Loop Mode", NULL, NULL, NULL, NULL,
         {
            {"0", "Play Track"},
            {"1", "Loop Track"},
            {"2", "Loop All"},
            {"3", "Shuffle All"},
            {NULL, NULL}
         }, "0"
      },
      { NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL }
   };

   struct retro_core_options_v2 options =
   {
      .categories = option_categories,
      .definitions = option_definitions
   };

   cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);

   struct retro_log_callback log;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   reset_triggered = true;
}

static void print_ffmpeg_version()
{
   PRINT_VERSION(avformat)
   PRINT_VERSION(avcodec)
   PRINT_VERSION(avutil)
   PRINT_VERSION(swresample)
   PRINT_VERSION(swscale)
}

static unsigned subtitle_font_size_auto(unsigned height)
{
   if (height == 0)
      return subtitle_font_base_size;

   unsigned size = (height * 7 + 99) / 100; /* ~15% of height */
   if (size < 24)
      size = 24;
   if (size > 128)
      size = 128;
   return size;
}

static void ass_scale_track_styles(ASS_Track *track, double scale)
{
   if (!track || scale == 1.0)
      return;

   for (int i = 0; i < track->n_styles; i++)
      track->styles[i].FontSize *= scale;
}

static void ass_scale_all_tracks(double scale)
{
   if (scale == 1.0)
      return;

   for (int i = 0; i < subtitle_streams_num; i++)
      ass_scale_track_styles(ass_track[i], scale);
}

static void update_subtitle_font_scale(void)
{
   double prev_scale = subtitle_font_scale;
   unsigned target_size = subtitle_font_size;
   double target_scale = 1.0;

   if (subtitle_font_auto)
      target_size = subtitle_font_size_auto(media.height);

   if (target_size == 0)
      target_size = subtitle_font_base_size;

   subtitle_font_size = target_size;
   target_scale = (double)target_size / (double)subtitle_font_base_size;
   if (target_scale <= 0.0)
      target_scale = 1.0;

   if (ass_render && ass_lock)
   {
      double scale_ratio = target_scale / (prev_scale > 0.0 ? prev_scale : 1.0);

      slock_lock(ass_lock);
      ass_set_font_scale(ass_render, 1.0);
      ass_scale_all_tracks(scale_ratio);
      slock_unlock(ass_lock);
   }

   subtitle_font_scale = target_scale;
}

static void ass_update_track_fonts(ASS_Track *track, const char *font_name, int bold)
{
   if (!track || !font_name)
      return;

   for (int i = 0; i < track->n_styles; i++)
   {
      if (track->styles[i].FontName)
         free(track->styles[i].FontName);
      track->styles[i].FontName = strdup(font_name);
      track->styles[i].Bold = bold ? -1 : 0;
   }
}

static void ass_update_all_track_fonts(const char *font_name, int bold)
{
   if (!font_name)
      return;

   for (int i = 0; i < subtitle_streams_num; i++)
      ass_update_track_fonts(ass_track[i], font_name, bold);
}

static void update_subtitle_style_overrides(void)
{
   if (!ass)
      return;

   snprintf(subtitle_style_override_font, sizeof(subtitle_style_override_font),
         "FontName=%s", subtitle_font_name);
   snprintf(subtitle_style_override_bold, sizeof(subtitle_style_override_bold),
         "Bold=%d", subtitle_font_bold ? -1 : 0);

   if (ass_lock)
      slock_lock(ass_lock);
   ass_set_style_overrides(ass, subtitle_style_overrides);
   if (ass_render)
      ass_set_fonts(ass_render, subtitle_font_name, NULL, 1, NULL, 1);
   ass_update_all_track_fonts(subtitle_font_name, subtitle_font_bold);
   if (ass_lock)
      slock_unlock(ass_lock);
}

static void check_variables(bool firststart)
{
   struct retro_variable sw_threads_var = {0};
   struct retro_variable loop_content  = {0};
   struct retro_variable replay_is_crt  = {0};
   struct retro_variable fft_toggle_var    = {0};
   struct retro_variable subtitle_toggle_var = {0};
   struct retro_variable subtitle_font_var = {0};
   struct retro_variable subtitle_font_name_var = {0};

   fft_width  = 640;
   fft_height = 480;
   fft_enabled = true;
   fft_toggle_var.key = "aplayer_visualizer";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &fft_toggle_var) &&
         fft_toggle_var.value)
   {
      if (string_is_equal(fft_toggle_var.value, "disabled"))
         fft_enabled = false;
   }

   subtitles_enabled = true;
   subtitle_toggle_var.key = "aplayer_subtitles";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &subtitle_toggle_var) &&
         subtitle_toggle_var.value)
   {
      if (string_is_equal(subtitle_toggle_var.value, "disabled"))
         subtitles_enabled = false;
   }

   subtitle_font_size = 64;
   subtitle_font_auto = false;
   subtitle_font_var.key = "aplayer_subtitle_font_size";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &subtitle_font_var) &&
         subtitle_font_var.value)
   {
      if (string_is_equal(subtitle_font_var.value, "auto"))
      {
         subtitle_font_auto = true;
      }
      else
      {
         subtitle_font_size = strtoul(subtitle_font_var.value, NULL, 0);
         if (subtitle_font_size == 0)
            subtitle_font_size = 64;
      }
   }

   subtitle_font_name = "DejaVu Sans";
   subtitle_font_bold = 0;
   subtitle_font_name_var.key = "aplayer_subtitle_font";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &subtitle_font_name_var) &&
         subtitle_font_name_var.value)
   {
      if (string_is_equal(subtitle_font_name_var.value, "dejavu_sans"))
      {
         subtitle_font_name = "DejaVu Sans";
         subtitle_font_bold = 0;
      }
      else if (string_is_equal(subtitle_font_name_var.value, "dejavu_sans_bold"))
      {
         subtitle_font_name = "DejaVu Sans";
         subtitle_font_bold = 1;
      }
      else if (string_is_equal(subtitle_font_name_var.value, "dejavu_sans_mono"))
      {
         subtitle_font_name = "DejaVu Sans Mono";
         subtitle_font_bold = 0;
      }
      else if (string_is_equal(subtitle_font_name_var.value, "dejavu_sans_mono_bold"))
      {
         subtitle_font_name = "DejaVu Sans Mono";
         subtitle_font_bold = 1;
      }
      else if (string_is_equal(subtitle_font_name_var.value, "dejavu_serif"))
      {
         subtitle_font_name = "DejaVu Serif";
         subtitle_font_bold = 0;
      }
      else if (string_is_equal(subtitle_font_name_var.value, "dejavu_serif_bold"))
      {
         subtitle_font_name = "DejaVu Serif";
         subtitle_font_bold = 1;
      }
   }

   update_subtitle_font_scale();
   update_subtitle_style_overrides();
   /* M3U */
   loop_content.key = "aplayer_loop_content";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &loop_content) && loop_content.value)
   {
      if (string_is_equal(loop_content.value, "0"))
         loopcontent = PLAY_TRACK;
      else if (string_is_equal(loop_content.value, "1"))
         loopcontent = LOOP_TRACK;
      else if (string_is_equal(loop_content.value, "2"))
         loopcontent = LOOP_ALL;
      else if (string_is_equal(loop_content.value, "3"))
         loopcontent = SHUFFLE_ALL;    
   }

   replay_is_crt.key = "replay_is_crt";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &replay_is_crt) && replay_is_crt.value)
   {
      if (string_is_equal(replay_is_crt.value, "true"))
         is_crt = true;
      else
         is_crt = false;
   }

   if (firststart)
   {
      sw_threads_var.key = "aplayer_sw_decoder_threads";
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &sw_threads_var) && sw_threads_var.value)
      {
         if (string_is_equal(sw_threads_var.value, "auto"))
         {
            sw_decoder_threads = cpu_features_get_core_amount();
         }
         else
         {
            sw_decoder_threads = strtoul(sw_threads_var.value, NULL, 0);
         }
         /* Scale the sws threads based on core count but use at least 2 and at most 4 threads */
         sw_sws_threads = MIN(MAX(2, sw_decoder_threads / 2), 4);
      }
   }
}

static void seek_frame(int seek_frames)
{
   char msg[256];
   char seek_time_str[16];
   char total_time_str[16];
   struct retro_message_ext msg_obj = {0};
   int seek_frames_capped           = seek_frames;
   int8_t seek_progress             = -1;
   bool duration_valid              = duration_is_valid(media.duration.time);

   msg[0] = '\0';

   if (!seek_supported && !reset_triggered)
   {
      show_not_supported_message();
      return;
   }

   /* Handle resets + attempts to seek to a location
    * before the start of the video */
   if ((seek_frames < 0 && (unsigned)-seek_frames > frame_cnt) || reset_triggered)
      frame_cnt = 0;
   /* Handle backwards seeking */
   else if (seek_frames < 0)
      frame_cnt += seek_frames;
   /* Handle forwards seeking */
   else
   {
      double current_time     = (double)frame_cnt / media.interpolate_fps;

      if (duration_valid)
      {
         double seek_step_time   = (double)seek_frames / media.interpolate_fps;
         double seek_target_time = current_time + seek_step_time;
         double seek_time_max    = media.duration.time - 1.0;

         seek_time_max = (seek_time_max > 0.0) ?
               seek_time_max : 0.0;

         /* Ensure that we don't attempt to seek past
          * the end of the file */
         if (seek_target_time > seek_time_max)
         {
            seek_step_time = seek_time_max - current_time;

            /* If seek would have taken us to the
             * end of the file, restart it instead
             * (less jarring for the user in case of
             * accidental seeking...) */
            if (seek_step_time < 0.0)
               seek_frames_capped = -1;
            else
               seek_frames_capped = (int)(seek_step_time * media.interpolate_fps);
         }

         if (seek_frames_capped < 0)
            frame_cnt  = 0;
         else
            frame_cnt += seek_frames_capped;
      }
      else
         frame_cnt += seek_frames;
   }

   do_seek = true;
   slock_lock(fifo_lock);
   seek_time      = frame_cnt / media.interpolate_fps;

   /* Convert seek time to a printable format */
   format_time_hhmmss(seek_time, seek_time_str, sizeof(seek_time_str));
   if (duration_valid)
      format_time_hhmmss(media.duration.time, total_time_str, sizeof(total_time_str));
   else
      snprintf(total_time_str, sizeof(total_time_str), "--:--:--");

   /* Get current progress */
   if (duration_valid)
   {
      seek_progress = (int8_t)((100.0 * seek_time / media.duration.time) + 0.5);
      seek_progress = (seek_progress < -1)  ? -1  : seek_progress;
      seek_progress = (seek_progress > 100) ? 100 : seek_progress;
   }

   if (duration_valid)
      snprintf(msg, sizeof(msg), "%s / %s (%d%%)",
            seek_time_str, total_time_str, seek_progress);
   else
      snprintf(msg, sizeof(msg), "%s / %s",
            seek_time_str, total_time_str);

   /* Send message to frontend */
   msg_obj.msg      = msg;
   msg_obj.duration = 3000;
   msg_obj.priority = 3;
   msg_obj.level    = RETRO_LOG_INFO;
   msg_obj.target   = RETRO_MESSAGE_TARGET_OSD;
   msg_obj.type     = RETRO_MESSAGE_TYPE_PROGRESS;
   msg_obj.progress = duration_valid ? seek_progress : -1;
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);

   if (seek_frames_capped < 0)
   {
      log_cb(RETRO_LOG_INFO, "[APLAYER] Resetting PTS.\n");
      frames[0].pts = 0.0;
      frames[1].pts = 0.0;
   }
   audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;

   if (audio_decode_fifo)
      fifo_clear(audio_decode_fifo);
   scond_signal(fifo_decode_cond);

   while (!decode_thread_dead && do_seek)
   {
      main_sleeping = true;
      if (video_stream_index < 0)
      {
         if (!scond_wait_timeout(fifo_cond, fifo_lock, 2000))
         {
            log_cb(RETRO_LOG_WARN, "[APLAYER] Seek timed out, continuing.\n");
            break;
         }
      }
      else
         scond_wait(fifo_cond, fifo_lock);
      main_sleeping = false;
   }

   slock_unlock(fifo_lock);
}

static void dispaly_time(void)
{
   // Early-out checks to avoid doing anything if we are shut down or done
   if (!fctx || decode_thread_dead)
      return;

   // Local buffer for the message, plus the RetroArch message object
   char msg[256];
   char current_str[16];
   char total_str[16];
   struct retro_message_ext msg_obj = {0};
   msg[0] = '\0';

   double total_duration = media.duration.time;
   bool total_valid = duration_is_valid(total_duration);

   double pts_time = 0.0;
   if (time_lock)
   {
      slock_lock(time_lock);
      pts_time = g_current_time;   // The decode thread sets this
      slock_unlock(time_lock);
   }

   double fallback_time = (double)frame_cnt / media.interpolate_fps;
   if (audio_streams_num > 0 && video_stream_index < 0 && media.sample_rate > 0)
      fallback_time = (double)audio_frames / media.sample_rate;

   double current_time = pts_time;
   if (video_stream_index < 0 ||
         current_time < 0.0 ||
         (total_valid && current_time > total_duration + 5.0))
      current_time = fallback_time;

   if (current_time < 0.0)
      current_time = 0.0;

   format_time_hhmmss(current_time, current_str, sizeof(current_str));
   if (total_valid)
      format_time_hhmmss(total_duration, total_str, sizeof(total_str));
   else
      snprintf(total_str, sizeof(total_str), "--:--:--");

   double progress = 0.0;
   int progress_int = -1;
   if (total_valid)
   {
      progress = (current_time / total_duration) * 100.0;
      if (progress < 0.0)   progress = 0.0;
      if (progress > 100.0) progress = 100.0;
      progress_int = (int)(progress + 0.5);
   }

   if (total_valid)
      snprintf(msg, sizeof(msg), "%s / %s (%d%%)",
            current_str, total_str, progress_int);
   else
      snprintf(msg, sizeof(msg), "%s / %s", current_str, total_str);

   // 7. Fill in the retro_message_ext object and send to frontend
   msg_obj.msg      = msg;
   msg_obj.duration = 3000;
   msg_obj.priority = 3;
   msg_obj.level    = RETRO_LOG_INFO;
   msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
   msg_obj.type     = RETRO_MESSAGE_TYPE_PROGRESS;
   msg_obj.progress = progress_int;

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
}

static void render_subtitles_on_buffer(uint32_t *buffer, unsigned width,
      unsigned height, double time_sec);

static void ensure_video_textures_allocated(unsigned width, unsigned height)
{
   unsigned i;

   if (video_stream_index < 0 || width == 0 || height == 0)
      return;

   if (!frames[0].tex || !frames[1].tex)
      return;

   if (frames_tex_width == width && frames_tex_height == height)
      return;

   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   for (i = 0; i < 2; i++)
   {
      if (!frames[i].tex)
         continue;

      glBindTexture(GL_TEXTURE_2D, frames[i].tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
            (GLsizei)width, (GLsizei)height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   }
   glBindTexture(GL_TEXTURE_2D, 0);

   frames_tex_width  = width;
   frames_tex_height = height;
}

void retro_run(void)
{
   static bool last_left;
   static bool last_right;
   static bool last_up;
   static bool last_down;
   static bool last_start;
   static bool last_a;
   static bool last_b;
   static bool last_x;
   static bool last_y;
   static bool last_l;
   static bool last_r;
   static bool last_l2;
   static bool last_r2;
   double min_pts;
   int16_t audio_buffer[2048];
   bool left, right, up, down, start, a, b, x, y, l, r, l2, r2;
   int16_t ret                  = 0;
   size_t to_read_frames        = 0;
   int seek_frames              = 0;
   bool updated                 = false;
   unsigned old_fft_width       = fft_width;
   unsigned old_fft_height      = fft_height;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);

   if (fft_width != old_fft_width || fft_height != old_fft_height)
   {
      struct retro_system_av_info info;
      retro_get_system_av_info(&info);
      if (!environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info))
      {
         fft_width = old_fft_width;
         fft_height = old_fft_height;
      }
   }

   input_poll_cb();

   if (libretro_supports_bitmasks)
      ret = input_state_cb(0, RETRO_DEVICE_JOYPAD,
            0, RETRO_DEVICE_ID_JOYPAD_MASK);
   else
   {
      unsigned i;
      for (i = RETRO_DEVICE_ID_JOYPAD_B; i <= RETRO_DEVICE_ID_JOYPAD_R2; i++)
         if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i))
            ret |= (1 << i);
   }

   left  = ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
   right = ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
   up    = ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP);
   down  = ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
   start = ret & (1 << RETRO_DEVICE_ID_JOYPAD_START);
   a     = ret & (1 << RETRO_DEVICE_ID_JOYPAD_A);
   b     = ret & (1 << RETRO_DEVICE_ID_JOYPAD_B);
   x     = ret & (1 << RETRO_DEVICE_ID_JOYPAD_X);
   y     = ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y);
   l     = ret & (1 << RETRO_DEVICE_ID_JOYPAD_L);
   r     = ret & (1 << RETRO_DEVICE_ID_JOYPAD_R);
   l2    = ret & (1 << RETRO_DEVICE_ID_JOYPAD_L2);
   r2    = ret & (1 << RETRO_DEVICE_ID_JOYPAD_R2);

   if (!decode_thread_dead)
   {
      /* Play/Pause */
      if (start && !last_start) {
         // Toggle the pause state.
         paused = !paused;
         {
            // Send an on-screen message informing the user.
            char msg[32];
            struct retro_message_ext msg_obj = {0};
            
            if (paused) {
               snprintf(msg, sizeof(msg), "Paused");
            } else {
               snprintf(msg, sizeof(msg), "Resumed");
            }
            
            msg_obj.msg      = msg;
            msg_obj.duration = 3000;  // Duration in ms for the message display
            msg_obj.priority = 1;
            msg_obj.level    = RETRO_LOG_INFO;
            msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
            msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
            msg_obj.progress = -1;
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
         }
      }
      /* M3U */
      /* Handle Next/Previous Track with L/R buttons */
      if (playlist_count > 0)
      {
         if (r && !last_r)
         {
            slock_lock(decode_thread_lock);
            unsigned new_index = (playlist_index + 1) % playlist_count;
            if (new_index != playlist_index)
            {
               playlist_index = new_index;
               do_seek = true;
               seek_time = 0.0;
               // Display track change message with file name only
               char msg[256];
               struct retro_message_ext msg_obj = {0};

               // Extract the file name from the full path
               const char *full_path = playlist[playlist_index];
               const char *filename = strrchr(full_path, '/');
               if (filename)
                  filename++;  // Skip the '/' character
               else
                  filename = full_path;

               snprintf(msg, sizeof(msg), "%d/%d %s", playlist_index + 1, playlist_count, filename);
               msg_obj.msg = msg;
               msg_obj.duration = 3000;
               msg_obj.priority = 1;
               msg_obj.level = RETRO_LOG_INFO;
               environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
            }
            slock_unlock(decode_thread_lock);
         }
         else if (l && !last_l)
         {
            slock_lock(decode_thread_lock);
            unsigned new_index = (playlist_index - 1 + playlist_count) % playlist_count;
            if (new_index != playlist_index)
            {
               playlist_index = new_index;
               do_seek = true;
               seek_time = 0.0;
               // Display track change message with file name only
               char msg[256];
               struct retro_message_ext msg_obj = {0};

               // Extract the file name from the full path
               const char *full_path = playlist[playlist_index];
               const char *filename = strrchr(full_path, '/');
               if (filename)
                  filename++;  // Skip the '/' character
               else
                  filename = full_path;

               snprintf(msg, sizeof(msg), "%d/%d %s", playlist_index + 1, playlist_count, filename);
               msg_obj.msg = msg;
               msg_obj.duration = 3000;
               msg_obj.priority = 1;
               msg_obj.level = RETRO_LOG_INFO;
               environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
            }
            slock_unlock(decode_thread_lock);
         }
      }

      /* Display info */

      if (b && !last_b)
         display_media_title();
      if (a && !last_a)
         dispaly_time();

      /* Seek and subtitles */

      if (left && !last_left)
         seek_frames -= 15 * media.interpolate_fps;
      if (right && !last_right)
         seek_frames += 15 * media.interpolate_fps;
      if (up && !last_up)
         seek_frames += 180 * media.interpolate_fps;
      if (down && !last_down)
         seek_frames -= 180 * media.interpolate_fps;
      if (l2 && !last_l2)
         seek_frames -= 300 * media.interpolate_fps;
      if (r2 && !last_r2)
         seek_frames += 300 * media.interpolate_fps;


      int media_type = get_media_type();

      if (media_type == MEDIA_TYPE_VIDEO && y && !last_y && audio_streams_num > 0)
      {
         // Safely update the new audio track index.
         slock_lock(decode_thread_lock);
         audio_streams_ptr = (audio_streams_ptr + 1) % audio_streams_num;
         slock_unlock(decode_thread_lock);

         // In addition to updating the index, trigger a full flush of the pipelines.
         slock_lock(fifo_lock);
         do_seek = true;

         // Set the new seek time to the current playback time.
         // (You can choose to keep the playback time unchanged or compute a new one.)
         slock_lock(time_lock);
         seek_time = g_current_time;  // Alternatively, use 0 if you wish to restart the stream.
         slock_unlock(time_lock);
         scond_signal(fifo_cond);
         slock_unlock(fifo_lock);

         // Display a message for the new audio track.
         {
            char msg[256];
            struct retro_message_ext msg_obj = {0};
            int audio_stream_index = audio_streams[audio_streams_ptr];
            AVDictionaryEntry *tag = av_dict_get(fctx->streams[audio_stream_index]->metadata, "title", NULL, 0);
            if (tag)
               snprintf(msg, sizeof(msg), "%s #%d", tag->value, audio_streams_ptr);
            else
               snprintf(msg, sizeof(msg), "Audio Track #%d", audio_streams_ptr);
            msg_obj.msg      = msg;
            msg_obj.duration = 3000;
            msg_obj.priority = 1;
            msg_obj.level    = RETRO_LOG_INFO;
            msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
            msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
            msg_obj.progress = -1;
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
         }
      }
      else if (media_type == MEDIA_TYPE_VIDEO && x && !last_x)
      {
         char msg[256];
         struct retro_message_ext msg_obj = {0};
         msg[0] = '\0';

         if (subtitle_streams_num > 0)
         {
            slock_lock(decode_thread_lock);
            subtitle_streams_ptr = (subtitle_streams_ptr + 1) % subtitle_streams_num;
            slock_unlock(decode_thread_lock);
            snprintf(msg, sizeof(msg), "Subtitle Track #%d", subtitle_streams_ptr);
         }
         else
         {
            snprintf(msg, sizeof(msg), "Subtitles not available");
         }

         msg_obj.msg      = msg;
         msg_obj.duration = 3000;
         msg_obj.priority = 1;
         msg_obj.level    = RETRO_LOG_INFO;
         msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
         msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
         msg_obj.progress = -1;
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
      }
   }

   last_left  = left;
   last_right = right;
   last_up    = up;
   last_down  = down;
   last_start = start;
   last_a     = a;
   last_b     = b;
   last_x     = x;
   last_y     = y;
   last_l     = l;
   last_r     = r;
   last_l2    = l2;
   last_r2    = r2;

   // If paused, simply display the last rendered video frame and skip further processing.
   if (paused) {
      video_cb(RETRO_HW_FRAME_BUFFER_VALID, media.width, media.height, media.width * sizeof(uint32_t));
      // Do not process audio or advance frames.
      return;
   }
   /* M3U */
   if (do_seek && seek_time == 0.0 && playlist_count > 0)
   {
      retro_unload_game();
      struct retro_game_info next_info = {0};
      next_info.path = playlist[playlist_index];
      unsigned  tries = 0;
      while (tries < playlist_count &&
             !retro_load_game(&next_info))
      {
         /* Advance and try the next entry                       */
         playlist_index = (playlist_index + 1) % playlist_count;
         next_info.path = playlist[playlist_index];
         tries++;
      }

      if (tries == playlist_count)
      {
         struct retro_message_ext m = {
            .msg = "Playlist finished - no playable entries.",
            .duration = 5000, .level = RETRO_LOG_ERROR
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
         return;   /* stop running gracefully */
      }
      do_seek = false;

      struct retro_system_av_info info;
      retro_get_system_av_info(&info);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
   }

   if (reset_triggered)
   {
      seek_frames = -1;
      seek_frame(seek_frames);
      reset_triggered = false;
   }

   /* Push seek request to thread,
    * wait for seek to complete. */
   if (seek_frames)
      seek_frame(seek_frames);

   if (decode_thread_dead)
   {
      video_cb(NULL, 1, 1, sizeof(uint32_t));
      return;
   }

   frame_cnt++;

   /* Have to decode audio before video
    * incase there are PTS fuckups due
    * to seeking. */
   if (audio_streams_num > 0)
   {
      /* Audio */
      double reading_pts;
      double expected_pts;
      double old_pts_bias;
      size_t to_read_bytes;
      size_t bytes_per_frame = sizeof(int16_t) * 2;
      uint64_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;

      to_read_frames = expected_audio_frames - audio_frames;
      to_read_bytes = to_read_frames * bytes_per_frame;

      slock_lock(fifo_lock);
      if (video_stream_index < 0)
      {
         if (!decode_thread_dead && FIFO_READ_AVAIL(audio_decode_fifo) < to_read_bytes)
         {
            main_sleeping = true;
            scond_signal(fifo_decode_cond);
            scond_wait_timeout(fifo_cond, fifo_lock, 2000);
            main_sleeping = false;
         }
      }
      else
      {
         while (!decode_thread_dead && FIFO_READ_AVAIL(audio_decode_fifo) < to_read_bytes)
         {
            main_sleeping = true;
            scond_signal(fifo_decode_cond);
            scond_wait(fifo_cond, fifo_lock);
            main_sleeping = false;
         }
      }
      reading_pts  = decode_last_audio_time -
         (double)FIFO_READ_AVAIL(audio_decode_fifo) / (media.sample_rate * bytes_per_frame);
      expected_pts = (double)audio_frames / media.sample_rate;
      old_pts_bias = pts_bias;
      pts_bias     = reading_pts - expected_pts;

      if (pts_bias < old_pts_bias - 1.0)
      {
         log_cb(RETRO_LOG_INFO, "[APLAYER] Resetting PTS (bias).\n");
         frames[0].pts = 0.0;
         frames[1].pts = 0.0;
      }

      if (!decode_thread_dead)
      {
         if (video_stream_index < 0)
         {
            size_t avail_bytes = FIFO_READ_AVAIL(audio_decode_fifo);
            size_t read_bytes = avail_bytes < to_read_bytes ? avail_bytes : to_read_bytes;
            size_t read_frames = read_bytes / bytes_per_frame;

            if (read_bytes)
               fifo_read(audio_decode_fifo, audio_buffer, read_bytes);
            if (read_frames < to_read_frames)
               memset(audio_buffer + (read_frames * 2), 0,
                     (to_read_frames - read_frames) * bytes_per_frame);
         }
         else
            fifo_read(audio_decode_fifo, audio_buffer, to_read_bytes);
      }
      scond_signal(fifo_decode_cond);

      slock_unlock(fifo_lock);
      audio_frames += to_read_frames;
   }

   min_pts = frame_cnt / media.interpolate_fps + pts_bias;

   if (video_stream_index >= 0)
   {
      /* Video */
      if (min_pts > frames[1].pts)
      {
         struct frame tmp = frames[1];
         frames[1] = frames[0];
         frames[0] = tmp;
      }

      float mix_factor;

      while (!decode_thread_dead && min_pts > frames[1].pts)
      {
         int64_t pts = 0;

         if (!decode_thread_dead)
         {
            video_decoder_context_t *ctx = NULL;
            uint32_t               *pixels = NULL;

            video_buffer_wait_for_finished_slot(video_buffer);

            video_buffer_get_finished_slot(video_buffer, &ctx);
            pts                          = ctx->pts;
            pixels                       = (uint32_t*)ctx->target->data[0];

            double render_time = min_pts;
            if (pts != AV_NOPTS_VALUE)
               render_time = av_q2d(fctx->streams[video_stream_index]->time_base) * pts;
            render_subtitles_on_buffer(pixels, media.width,
                  media.height, render_time);

            ensure_video_textures_allocated(media.width, media.height);
            glBindTexture(GL_TEXTURE_2D, frames[1].tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                  (GLsizei)media.width, (GLsizei)media.height,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glBindTexture(GL_TEXTURE_2D, 0);
            video_buffer_open_slot(video_buffer, ctx);
         }

         frames[1].pts = av_q2d(fctx->streams[video_stream_index]->time_base) * pts;
      }

      mix_factor = 1.0f;

      glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());

      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glViewport(0, 0, media.width, media.height);

      glUseProgram(prog);

      glUniform1f(mix_loc, mix_factor);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, frames[1].tex);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, frames[0].tex);

      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glVertexAttribPointer(vertex_loc, 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(GLfloat), (const GLvoid*)(0 * sizeof(GLfloat)));
      glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(GLfloat), (const GLvoid*)(2 * sizeof(GLfloat)));
      glEnableVertexAttribArray(vertex_loc);
      glEnableVertexAttribArray(tex_loc);
      glBindBuffer(GL_ARRAY_BUFFER, 0);

      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glDisableVertexAttribArray(vertex_loc);
      glDisableVertexAttribArray(tex_loc);

      glUseProgram(0);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, 0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, 0);

      /* Draw video using OGL*/
      video_cb(RETRO_HW_FRAME_BUFFER_VALID,
            media.width, media.height, media.width * sizeof(uint32_t));
   }
   else if (fft)
   {
      if (fft_enabled)
      {
         unsigned       frames = to_read_frames;
         const int16_t *buffer = audio_buffer;

         while (frames)
         {
            unsigned to_read = frames;

            /* FFT size we use (1 << 11). Really shouldn't happen,
             * unless we use a crazy high sample rate. */
            if (to_read > (1 << 11))
               to_read = 1 << 11;

            fft_step_fft(fft, buffer, to_read);
            buffer += to_read * 2;
            frames -= to_read;
         }
         fft_render(fft, hw_render.get_current_framebuffer(), fft_width, fft_height);
      }
      else
      {
         glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
         glViewport(0, 0, fft_width, fft_height);
         glClearColor(0, 0, 0, 1);
         glClear(GL_COLOR_BUFFER_BIT);
      }

      /* Draw music FFT using OGL*/
      video_cb(RETRO_HW_FRAME_BUFFER_VALID,
            fft_width, fft_height, fft_width * sizeof(uint32_t));
   }
   else
   {
      /* Draw music not using FFT and not using OGL */
      video_cb(NULL, 1, 1, sizeof(uint32_t));
   }
   if (to_read_frames)
      audio_batch_cb(audio_buffer, to_read_frames);
}

static bool open_codec(AVCodecContext **ctx, enum AVMediaType type, unsigned index)
{
   int ret              = 0;
   const AVCodec *codec = avcodec_find_decoder(fctx->streams[index]->codecpar->codec_id);
   if (!codec)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Couldn't find suitable decoder\n");
      return false;
   }

   *ctx = avcodec_alloc_context3(codec);
   avcodec_parameters_to_context((*ctx), fctx->streams[index]->codecpar);

   if (type == AVMEDIA_TYPE_VIDEO)
   {
      video_stream_index = index;
      (*ctx)->thread_type  = FF_THREAD_FRAME;
      (*ctx)->thread_count = sw_decoder_threads;
      log_cb(RETRO_LOG_INFO, "[APLAYER] Using SW decoding.\n");
      log_cb(RETRO_LOG_INFO, "[APLAYER] Configured software decoding threads: %d\n", sw_decoder_threads);
   }

   if ((ret = avcodec_open2(*ctx, codec, NULL)) < 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Could not open codec: %s\n", av_err2str(ret));
      return false;
   }

   return true;
}

static bool codec_is_image(enum AVCodecID id)
{
   switch (id)
   {
      case AV_CODEC_ID_MJPEG:
      case AV_CODEC_ID_PNG:
      case AV_CODEC_ID_GIF:
      case AV_CODEC_ID_BMP:
         return true;

      default:
         break;
   }

   return false;
}

static bool codec_id_is_ttf(enum AVCodecID id)
{
   switch (id)
   {
      case AV_CODEC_ID_TTF:
         return true;

      default:
         break;
   }

   return false;
}

static bool codec_id_is_ass(enum AVCodecID id)
{
   switch (id)
   {
      case AV_CODEC_ID_ASS:
      case AV_CODEC_ID_SSA:
         return true;
      default:
         break;
   }

   return false;
}

static bool codec_id_is_text_subtitle(enum AVCodecID id)
{
#ifdef AV_CODEC_PROP_TEXT_SUBTITLE
   const AVCodecDescriptor *desc = avcodec_descriptor_get(id);
   if (desc && (desc->props & AV_CODEC_PROP_TEXT_SUBTITLE))
      return true;
#endif
   switch (id)
   {
#ifdef AV_CODEC_ID_SUBRIP
      case AV_CODEC_ID_SUBRIP:
#endif
#ifdef AV_CODEC_ID_TEXT
      case AV_CODEC_ID_TEXT:
#endif
#ifdef AV_CODEC_ID_WEBVTT
      case AV_CODEC_ID_WEBVTT:
#endif
#ifdef AV_CODEC_ID_MOV_TEXT
      case AV_CODEC_ID_MOV_TEXT:
#endif
         return true;
      default:
         break;
   }

   return false;
}

static bool codec_name_is_text_subtitle(enum AVCodecID id)
{
   const char *name = avcodec_get_name(id);

   if (!name)
      return false;

   if (strcmp(name, "subrip") == 0 ||
         strcmp(name, "srt") == 0 ||
         strcmp(name, "webvtt") == 0 ||
         strcmp(name, "mov_text") == 0 ||
         strcmp(name, "text") == 0)
      return true;

   return false;
}

static void ass_init_default_track(ASS_Track *track)
{
   unsigned play_res_x = media.width ? media.width : 640;
   unsigned play_res_y = media.height ? media.height : 480;
   const char *font_name = subtitle_font_name ? subtitle_font_name : "DejaVu Sans";
   int font_bold = subtitle_font_bold ? -1 : 0;
   unsigned font_size = subtitle_font_base_size;
   char header[1024];
   int len = snprintf(header, sizeof(header),
         "[Script Info]\n"
         "ScriptType: v4.00+\n"
         "PlayResX: %u\n"
         "PlayResY: %u\n"
         "\n"
         "[V4+ Styles]\n"
         "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
         "Style: Default,%s,%u,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,%d,0,0,0,100,100,0,0,1,2,0,2,20,20,20,1\n"
         "\n"
         "[Events]\n"
         "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
         play_res_x, play_res_y, font_name, font_size, font_bold);

   if (len <= 0)
      return;
   if ((size_t)len >= sizeof(header))
      len = (int)sizeof(header) - 1;

   ass_process_codec_private(track, header, len);
}

static int subtitle_slot_for_stream(int stream_index)
{
   for (int i = 0; i < subtitle_streams_num; i++)
   {
      if (subtitle_streams[i] == stream_index)
         return i;
   }

   return -1;
}

static bool open_codecs(void)
{
   unsigned i;
   decode_thread_lock   = slock_new();

   video_stream_index   = -1;
   audio_streams_num    = 0;
   subtitle_streams_num = 0;

   slock_lock(decode_thread_lock);
   audio_streams_ptr    = 0;
   subtitle_streams_ptr = 0;
   slock_unlock(decode_thread_lock);

   memset(audio_streams,    0, sizeof(audio_streams));
   memset(subtitle_streams, 0, sizeof(subtitle_streams));
   memset(subtitle_is_ass,  0, sizeof(subtitle_is_ass));
   memset(subtitle_is_external, 0, sizeof(subtitle_is_external));
   first_subtitle_event_logged = false;
   first_ass_render_logged = false;
   memset(first_ass_image_logged, 0, sizeof(first_ass_image_logged));
   memset(first_ass_after_sub_logged, 0, sizeof(first_ass_after_sub_logged));
   for (i = 0; i < MAX_STREAMS; i++)
      first_subtitle_start_ms[i] = -1;

   for (i = 0; i < fctx->nb_streams; i++)
   {
      enum AVMediaType type = fctx->streams[i]->codecpar->codec_type;
      switch (type)
      {
         case AVMEDIA_TYPE_AUDIO:
            if (audio_streams_num < MAX_STREAMS)
            {
               if (!open_codec(&actx[audio_streams_num], type, i))
                  return false;
               audio_streams[audio_streams_num] = i;
               audio_streams_num++;
            }
            break;

         case AVMEDIA_TYPE_VIDEO:
            if (!vctx
                  && !codec_is_image(fctx->streams[i]->codecpar->codec_id))
            {
               if (!open_codec(&vctx, type, i))
                  return false;
            }
            break;

         case AVMEDIA_TYPE_SUBTITLE:
            if (subtitle_streams_num < MAX_STREAMS)
            {
               int size;
               AVCodecContext **s = &sctx[subtitle_streams_num];
               const enum AVCodecID sub_id = fctx->streams[i]->codecpar->codec_id;
               const char *codec_name = avcodec_get_name(sub_id);
               bool is_ass = codec_id_is_ass(sub_id);
               bool is_text = codec_id_is_text_subtitle(sub_id) ||
                     codec_name_is_text_subtitle(sub_id);

               log_cb(RETRO_LOG_INFO, "[APLAYER] Found subtitle stream %u: %s\n",
                     i, codec_name ? codec_name : "unknown");

               if (!is_ass && !is_text)
               {
                  log_cb(RETRO_LOG_WARN, "[APLAYER] Subtitle codec not supported: %s\n",
                        codec_name ? codec_name : "unknown");
                  break;
               }

               subtitle_streams[subtitle_streams_num] = i;
               subtitle_is_ass[subtitle_streams_num] = is_ass;
               if (!open_codec(s, type, i))
                  return false;

               size = (*s)->extradata ? (*s)->extradata_size : 0;
               ass_extra_data_size[subtitle_streams_num] = size;

               if (size)
               {
                  ass_extra_data[subtitle_streams_num] = (uint8_t*)av_malloc(size);
                  memcpy(ass_extra_data[subtitle_streams_num], (*s)->extradata, size);
               }

               subtitle_streams_num++;
               log_cb(RETRO_LOG_INFO, "[APLAYER] Subtitle stream %u registered: %s (%s)\n",
                     i, codec_name ? codec_name : "unknown",
                     is_ass ? "ass" : "text");
            }
            break;

         case AVMEDIA_TYPE_ATTACHMENT:
            {
               AVCodecParameters *params = fctx->streams[i]->codecpar;
               if (codec_id_is_ttf(params->codec_id))
                  append_attachment(params->extradata, params->extradata_size);
            }
            break;

         default:
            break;
      }
   }

   return actx[0] || vctx;
}

static bool init_media_info(void)
{
   if (actx[0] && actx[0]->sample_rate > 0)
      media.sample_rate = actx[0]->sample_rate;
   else if (actx[0])
      log_cb(RETRO_LOG_WARN,
            "[APLAYER] Invalid audio sample rate (%d), using default %u.\n",
            actx[0]->sample_rate, media.sample_rate);

   media.interpolate_fps = 60.0;

   if (vctx)
   {
      media.width  = vctx->width;
      media.height = vctx->height;
      init_aspect_ratio();
   }

   if (fctx)
   {
      double duration_sec = 0.0;

      if (fctx->duration != AV_NOPTS_VALUE && fctx->duration > 0)
      {
         int64_t duration = fctx->duration + (fctx->duration <= INT64_MAX - 5000 ? 5000 : 0);
         duration_sec = (double)(duration / AV_TIME_BASE);
      }

      if (!duration_is_valid(duration_sec) && audio_streams_num > 0)
         duration_sec = stream_duration_seconds(audio_streams[0]);

      if (!duration_is_valid(duration_sec) && video_stream_index >= 0)
         duration_sec = stream_duration_seconds(video_stream_index);

      if (duration_is_valid(duration_sec))
      {
         media.duration.time     = duration_sec;
         media.duration.seconds  = (unsigned)media.duration.time;
         media.duration.minutes  = media.duration.seconds / 60;
         media.duration.seconds %= 60;
         media.duration.hours    = media.duration.minutes / 60;
         media.duration.minutes %= 60;
      }
      else
      {
         media.duration.time    = 0.0;
         media.duration.hours   = 0;
         media.duration.minutes = 0;
         media.duration.seconds = 0;
         log_cb(RETRO_LOG_WARN, "[APLAYER] Could not determine media duration\n");
      }
   }

   int type = get_media_type();

   switch (type)
   {
      case MEDIA_TYPE_AUDIO:
      {
         for (int i = 0; i < fctx->nb_streams; i++)
         {
            if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            {
               media.title = av_dict_get(fctx->streams[i]->metadata, "title", NULL, 0);
            }
         }
         break;
      }
      case MEDIA_TYPE_VIDEO:
         media.title = av_dict_get(fctx->metadata, "title", media.title, 0);
         break;
      default:
         break;
   }

   display_media_title();

   if (sctx[0])
   {
      unsigned i;

      ass = ass_library_init();
      ass_set_message_cb(ass, ass_msg_cb, NULL);

      for (i = 0; i < attachments_size; i++)
         ass_add_font(ass, (char*)"",
               (char*)attachments[i].data, attachments[i].size);

      ass_render = ass_renderer_init(ass);
      ass_set_frame_size(ass_render, media.width, media.height);
      ass_set_extract_fonts(ass, true);
      ass_set_fonts(ass_render, NULL, NULL, 1, NULL, 1);
      ass_set_hinting(ass_render, ASS_HINTING_LIGHT);
      update_subtitle_style_overrides();
      update_subtitle_font_scale();

      for (i = 0; i < (unsigned)subtitle_streams_num; i++)
      {
         ass_track[i] = ass_new_track(ass);
         if (ass_track[i])
         {
            if (subtitle_is_ass[i] && ass_extra_data_size[i] > 0)
               ass_process_codec_private(ass_track[i], (char*)ass_extra_data[i],
                     ass_extra_data_size[i]);
            else
               ass_init_default_track(ass_track[i]);

            ass_scale_track_styles(ass_track[i], subtitle_font_scale);
         }
      }
   }

   return true;
}

static void set_colorspace(struct SwsContext *sws,
      unsigned width, unsigned height,
      enum AVColorSpace default_color, int in_range)
{
   const int *coeffs = NULL;

   if (default_color != AVCOL_SPC_UNSPECIFIED)
      coeffs = sws_getCoefficients(default_color);
   else if (width >= 1280 || height > 576)
      coeffs = sws_getCoefficients(AVCOL_SPC_BT709);
   else
      coeffs = sws_getCoefficients(AVCOL_SPC_BT470BG);

   if (coeffs)
   {
      int in_full, out_full, brightness, contrast, saturation;
      const int *inv_table, *table;

      sws_getColorspaceDetails(sws, (int**)&inv_table, &in_full,
            (int**)&table, &out_full,
            &brightness, &contrast, &saturation);

      if (in_range != AVCOL_RANGE_UNSPECIFIED)
         in_full = in_range == AVCOL_RANGE_JPEG;

      inv_table = coeffs;
      sws_setColorspaceDetails(sws, inv_table, in_full,
            table, out_full,
            brightness, contrast, saturation);
   }
}

/* Straight CPU alpha blending.
 * Should probably do in GL. */
static void render_ass_img(AVFrame *conv_frame, ASS_Image *img)
{
   uint32_t *frame = (uint32_t*)conv_frame->data[0];
   int      stride = conv_frame->linesize[0] / sizeof(uint32_t);

   for (; img; img = img->next)
   {
      int x, y;
      unsigned r, g, b, a;
      uint32_t *dst         = NULL;
      const uint8_t *bitmap = NULL;

      if (img->w == 0 && img->h == 0)
         continue;

      bitmap = img->bitmap;
      dst    = frame + img->dst_x + img->dst_y * stride;

      r      = (img->color >> 24) & 0xff;
      g      = (img->color >> 16) & 0xff;
      b      = (img->color >>  8) & 0xff;
      a      = 255 - (img->color & 0xff);

      for (y = 0; y < img->h; y++,
            bitmap += img->stride, dst += stride)
      {
         for (x = 0; x < img->w; x++)
         {
            unsigned src_alpha = ((bitmap[x] * (a + 1)) >> 8) + 1;
            unsigned dst_alpha = 256 - src_alpha;

            uint32_t dst_color = dst[x];
            unsigned dst_r     = (dst_color >> 16) & 0xff;
            unsigned dst_g     = (dst_color >>  8) & 0xff;
            unsigned dst_b     = (dst_color >>  0) & 0xff;

            dst_r = (r * src_alpha + dst_r * dst_alpha) >> 8;
            dst_g = (g * src_alpha + dst_g * dst_alpha) >> 8;
            dst_b = (b * src_alpha + dst_b * dst_alpha) >> 8;

            dst[x] = (0xffu << 24) | (dst_r << 16) |
               (dst_g << 8) | (dst_b << 0);
         }
      }
   }
}

static void render_subtitles_on_buffer(uint32_t *buffer, unsigned width,
      unsigned height, double time_sec)
{
   ASS_Track *render_track = NULL;
   int subtitle_ptr = -1;
   int64_t track_start_ms = -1;

   if (!buffer || width == 0 || height == 0)
      return;

   if (!subtitles_enabled)
      return;

   if (!ass_render || subtitle_streams_num <= 0)
      return;

   if (decode_thread_lock)
   {
      slock_lock(decode_thread_lock);
      if (subtitle_streams_num > 0)
      {
         subtitle_ptr = subtitle_streams_ptr;
         render_track = ass_track[subtitle_streams_ptr];
      }
      slock_unlock(decode_thread_lock);
   }

   if (!render_track)
      return;

   if (subtitle_ptr >= 0 && subtitle_ptr < MAX_STREAMS)
      track_start_ms = first_subtitle_start_ms[subtitle_ptr];

   AVFrame tmp_frame;
   memset(&tmp_frame, 0, sizeof(tmp_frame));
   tmp_frame.data[0] = (uint8_t*)buffer;
   tmp_frame.linesize[0] = (int)width * (int)sizeof(uint32_t);

   slock_lock(ass_lock);
   if (ass_render)
   {
      int change = 0;
      long long now_ms = (long long)(time_sec * 1000.0);
      ASS_Image *img = ass_render_frame(ass_render, render_track, now_ms, &change);

      if (!first_ass_render_logged)
      {
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] ass_render_frame first call: images=%s change=%d time_ms=%lld\n",
               img ? "yes" : "no", change, now_ms);
         first_ass_render_logged = true;
      }

      if (subtitle_ptr >= 0 && subtitle_ptr < MAX_STREAMS &&
            img && !first_ass_image_logged[subtitle_ptr])
      {
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] ass_render_frame first non-null image: change=%d time_ms=%lld track=%d first_sub_start_ms=%lld\n",
               change, now_ms, subtitle_ptr, (long long)track_start_ms);
         first_ass_image_logged[subtitle_ptr] = true;
      }
      else if (subtitle_ptr >= 0 && subtitle_ptr < MAX_STREAMS &&
            !img && !first_ass_after_sub_logged[subtitle_ptr] &&
            track_start_ms >= 0 &&
            now_ms >= track_start_ms)
      {
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] ass_render_frame no image at/after first subtitle start: change=%d time_ms=%lld track=%d first_sub_start_ms=%lld\n",
               change, now_ms, subtitle_ptr, (long long)track_start_ms);
         first_ass_after_sub_logged[subtitle_ptr] = true;
      }

      if (img)
         render_ass_img(&tmp_frame, img);
   }
   slock_unlock(ass_lock);
}

static char *ass_escape_text(const char *text)
{
   size_t len = strlen(text);
   char *out = (char*)malloc(len * 2 + 1);
   char *dst = out;

   if (!out)
      return NULL;

   for (const char *src = text; *src; src++)
   {
      if (*src == '\r')
         continue;
      if (*src == '\n')
      {
         *dst++ = '\\';
         *dst++ = 'N';
         continue;
      }

      if (*src == '{' || *src == '}' || *src == '\\')
         *dst++ = '\\';

      *dst++ = *src;
   }

   *dst = '\0';
   return out;
}

static void ass_process_line(ASS_Track *track, const char *line)
{
   if (!line || !*line)
      return;

   const char *prefix = "";
   if (strncmp(line, "Dialogue:", 9) != 0 &&
         strncmp(line, "Comment:", 8) != 0 &&
         strncmp(line, "Style:", 6) != 0 &&
         strncmp(line, "Format:", 7) != 0 &&
         line[0] != '[')
      prefix = "Dialogue: ";

   size_t prefix_len = strlen(prefix);
   size_t len = strlen(line);
   bool has_newline = len > 0 && line[len - 1] == '\n';
   size_t total_len = prefix_len + len + (has_newline ? 0 : 1);

   char *buf = (char*)malloc(total_len + 1);
   if (!buf)
      return;

   memcpy(buf, prefix, prefix_len);
   memcpy(buf + prefix_len, line, len);
   if (!has_newline)
      buf[prefix_len + len] = '\n';
   buf[total_len] = '\0';

   ass_process_data(track, buf, total_len);
   free(buf);
}

static const char *ass_event_payload(const char *ass)
{
   const char *last_comma = NULL;

   if (!ass)
      return NULL;

   for (const char *p = ass; *p; p++)
   {
      if (*p == ',')
         last_comma = p;
   }

   if (!last_comma || !last_comma[1])
      return ass;

   return last_comma + 1;
}

static void ass_format_time(int64_t ms, char *buf, size_t buf_len)
{
   if (!buf || buf_len == 0)
      return;

   if (ms < 0)
      ms = 0;

   int centiseconds = (int)((ms / 10) % 100);
   int seconds = (int)((ms / 1000) % 60);
   int minutes = (int)((ms / 60000) % 60);
   int hours = (int)(ms / 3600000);

   snprintf(buf, buf_len, "%d:%02d:%02d.%02d", hours, minutes, seconds, centiseconds);
}

static void ass_add_text_event_raw(ASS_Track *track, int64_t start_ms, int64_t end_ms, const char *text)
{
   if (!text)
      return;

   if (end_ms <= start_ms)
      end_ms = start_ms + 2000;

   char start_buf[32];
   char end_buf[32];
   ass_format_time(start_ms, start_buf, sizeof(start_buf));
   ass_format_time(end_ms, end_buf, sizeof(end_buf));

   size_t text_len = strlen(text);
   size_t line_cap = text_len + 96;
   char *line = (char*)malloc(line_cap);
   if (!line)
      return;

   snprintf(line, line_cap, "Dialogue: 0,%s,%s,Default,,0,0,0,,%s",
         start_buf, end_buf, text);
   ass_process_line(track, line);
   free(line);
}

static void ass_add_text_event(ASS_Track *track, int64_t start_ms, int64_t end_ms, const char *text)
{
   char *escaped = ass_escape_text(text);

   if (!escaped)
      return;

   if (end_ms <= start_ms)
      end_ms = start_ms + 2000;

   ass_add_text_event_raw(track, start_ms, end_ms, escaped);

   free(escaped);
}

static bool path_replace_extension(const char *path, const char *new_ext, char *out, size_t out_size)
{
   const char *last_slash = NULL;
   const char *last_dot = NULL;
   size_t base_len = 0;
   size_t ext_len = 0;

   if (!path || !new_ext || !out || out_size == 0)
      return false;

   last_slash = strrchr(path, '/');
   last_dot = strrchr(path, '.');

   if (last_dot && (!last_slash || last_dot > last_slash))
      base_len = (size_t)(last_dot - path);
   else
      base_len = strlen(path);

   ext_len = strlen(new_ext);
   if (base_len + ext_len + 1 > out_size)
      return false;

   memcpy(out, path, base_len);
   memcpy(out + base_len, new_ext, ext_len);
   out[base_len + ext_len] = '\0';
   return true;
}

static char *read_entire_file(const char *path, size_t *out_size)
{
   static const size_t max_size = 8u * 1024u * 1024u;
   FILE *f = NULL;
   long file_size = 0;
   size_t to_read = 0;
   size_t read_total = 0;
   char *buf = NULL;

   if (out_size)
      *out_size = 0;

   if (!path)
      return NULL;

   f = fopen(path, "rb");
   if (!f)
      return NULL;

   if (fseek(f, 0, SEEK_END) != 0)
      goto error;

   file_size = ftell(f);
   if (file_size < 0)
      goto error;

   if ((size_t)file_size > max_size)
      goto error;

   if (fseek(f, 0, SEEK_SET) != 0)
      goto error;

   to_read = (size_t)file_size;
   buf = (char*)malloc(to_read + 2);
   if (!buf)
      goto error;

   read_total = fread(buf, 1, to_read, f);
   if (read_total != to_read)
      goto error;

   if (to_read > 0 && buf[to_read - 1] != '\n')
      buf[to_read++] = '\n';
   buf[to_read] = '\0';

   fclose(f);
   if (out_size)
      *out_size = to_read;
   return buf;

error:
   if (f)
      fclose(f);
   free(buf);
   return NULL;
}

static bool buffer_looks_like_ass(const char *buf, size_t buf_size)
{
   const unsigned char *u = (const unsigned char*)buf;
   size_t i = 0;

   if (!buf || buf_size == 0)
      return false;

   if (buf_size >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
      i = 3;

   while (i < buf_size && isspace((unsigned char)buf[i]))
      i++;

   if (i + 13 <= buf_size && strncmp(buf + i, "[Script Info]", 13) == 0)
      return true;

   if (strstr(buf + i, "[V4+ Styles]") || strstr(buf + i, "[V4 Styles]") ||
         strstr(buf + i, "[Events]"))
      return true;

   return false;
}

static char *trim_srt_line(char *line)
{
   unsigned char *u = (unsigned char*)line;
   char *start = line;
   char *end = NULL;

   if (!line)
      return NULL;

   if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
      start += 3;

   while (*start && isspace((unsigned char)*start))
      start++;

   end = start + strlen(start);
   while (end > start && isspace((unsigned char)end[-1]))
      end--;
   *end = '\0';

   return start;
}

static char *next_line_inplace(char **cursor, char *end)
{
   char *line = NULL;
   char *p = NULL;

   if (!cursor || !*cursor || *cursor >= end)
      return NULL;

   line = *cursor;
   p = line;

   while (p < end && *p != '\n' && *p != '\r')
      p++;

   if (p < end)
   {
      if (*p == '\r')
      {
         *p++ = '\0';
         if (p < end && *p == '\n')
            *p++ = '\0';
      }
      else
      {
         *p++ = '\0';
      }
   }

   *cursor = p;
   return line;
}

static bool parse_uint_component(const char *s, int *out_val, const char **out_end)
{
   char *endptr = NULL;
   long v;

   if (!s || !*s || !out_val)
      return false;

   v = strtol(s, &endptr, 10);
   if (endptr == s || v < 0 || v > INT_MAX)
      return false;

   *out_val = (int)v;
   if (out_end)
      *out_end = endptr;
   return true;
}

static bool parse_srt_timestamp_ms(const char *s, int64_t *out_ms)
{
   const char *p = s;
   const char *ms_start = NULL;
   int hours = 0;
   int minutes = 0;
   int seconds = 0;
   int ms = 0;

   if (!s || !out_ms)
      return false;

   while (*p && isspace((unsigned char)*p))
      p++;

   if (!parse_uint_component(p, &hours, &p) || *p != ':')
      return false;
   p++;
   if (!parse_uint_component(p, &minutes, &p) || *p != ':')
      return false;
   p++;
   if (!parse_uint_component(p, &seconds, &p))
      return false;

   if (*p == ',' || *p == '.')
   {
      int ms_val = 0;
      int digits = 0;

      p++;
      ms_start = p;
      if (!parse_uint_component(p, &ms_val, &p))
         ms_val = 0;

      digits = (int)(p - ms_start);
      if (digits == 1)
         ms_val *= 100;
      else if (digits == 2)
         ms_val *= 10;
      else if (digits > 3)
      {
         while (digits > 3)
         {
            ms_val /= 10;
            digits--;
         }
      }
      ms = ms_val;
   }

   *out_ms = ((int64_t)hours * 3600 + (int64_t)minutes * 60 + (int64_t)seconds) * 1000 + ms;
   return true;
}

static bool parse_srt_time_range_ms(const char *line, int64_t *start_ms, int64_t *end_ms)
{
   const char *arrow = NULL;
   const char *p = NULL;

   if (!line || !start_ms || !end_ms)
      return false;

   arrow = strstr(line, "-->");
   if (!arrow)
      return false;

   p = line;
   if (!parse_srt_timestamp_ms(p, start_ms))
      return false;

   p = arrow + 3;
   if (!parse_srt_timestamp_ms(p, end_ms))
      return false;

   return true;
}

static bool srt_add_events_from_buffer(ASS_Track *track, char *buf, size_t buf_size, int64_t *first_start_ms_out)
{
   char *cursor = buf;
   char *end = buf + buf_size;
   int64_t first_start_ms = -1;
   bool any = false;

   if (first_start_ms_out)
      *first_start_ms_out = -1;

   if (!track || !buf || buf_size == 0)
      return false;

   while (true)
   {
      char *line = next_line_inplace(&cursor, end);
      char *trimmed = NULL;
      bool is_index = true;
      int64_t start_ms = 0;
      int64_t end_ms = 0;

      if (!line)
         break;

      trimmed = trim_srt_line(line);
      if (!trimmed || *trimmed == '\0')
         continue;

      /* Optional numeric index line. */
      for (char *p = trimmed; *p; p++)
      {
         if (!isdigit((unsigned char)*p))
         {
            is_index = false;
            break;
         }
      }

      if (is_index)
      {
         line = next_line_inplace(&cursor, end);
         if (!line)
            break;
         trimmed = trim_srt_line(line);
         if (!trimmed || *trimmed == '\0')
            continue;
      }

      if (!parse_srt_time_range_ms(trimmed, &start_ms, &end_ms))
         continue;

      /* Collect text lines until an empty separator line. */
      {
         char *text = NULL;
         size_t text_len = 0;
         size_t text_cap = 0;

         while (true)
         {
            char *tline = next_line_inplace(&cursor, end);
            char *ws = NULL;
            size_t add_len = 0;
            size_t need = 0;

            if (!tline)
               break;

            ws = tline;
            while (*ws && isspace((unsigned char)*ws))
               ws++;

            if (*ws == '\0')
               break;

            add_len = strlen(tline);
            need = text_len + add_len + (text_len ? 1 : 0) + 1;

            if (need > text_cap)
            {
               size_t new_cap = text_cap ? text_cap * 2 : 256;
               char *new_text = NULL;
               while (new_cap < need)
                  new_cap *= 2;
               new_text = (char*)realloc(text, new_cap);
               if (!new_text)
               {
                  free(text);
                  text = NULL;
                  text_cap = 0;
                  text_len = 0;
                  break;
               }
               text = new_text;
               text_cap = new_cap;
            }

            if (text_len)
               text[text_len++] = '\n';
            memcpy(text + text_len, tline, add_len);
            text_len += add_len;
            text[text_len] = '\0';
         }

         if (text && text_len > 0)
         {
            ass_add_text_event(track, start_ms, end_ms, text);
            any = true;
            if (first_start_ms < 0)
               first_start_ms = start_ms;
         }

         free(text);
      }
   }

   if (first_start_ms_out)
      *first_start_ms_out = first_start_ms;

   return any;
}

static bool ensure_ass_context(void)
{
   if (ass && ass_render)
      return true;

   ass = ass_library_init();
   if (!ass)
      return false;

   ass_set_message_cb(ass, ass_msg_cb, NULL);

   for (unsigned i = 0; i < attachments_size; i++)
      ass_add_font(ass, (char*)"", (char*)attachments[i].data, attachments[i].size);

   ass_render = ass_renderer_init(ass);
   if (!ass_render)
   {
      ass_library_done(ass);
      ass = NULL;
      return false;
   }

   ass_set_frame_size(ass_render, media.width, media.height);
   ass_set_extract_fonts(ass, true);
   ass_set_fonts(ass_render, NULL, NULL, 1, NULL, 1);
   ass_set_hinting(ass_render, ASS_HINTING_LIGHT);

   update_subtitle_style_overrides();
   update_subtitle_font_scale();

   return true;
}

static void maybe_load_external_subtitles(const char *media_path)
{
   static const char *exts[] = { ".srt" };
   char sub_path[PATH_MAX];

   if (!media_path)
      return;

   if (get_media_type() != MEDIA_TYPE_VIDEO)
      return;

   if (subtitle_streams_num >= MAX_STREAMS)
      return;

   for (unsigned i = 0; i < (unsigned)(sizeof(exts) / sizeof(exts[0])); i++)
   {
      size_t buf_size = 0;
      char *buf = NULL;
      ASS_Track *track = NULL;
      bool is_ass = false;
      bool any_events = false;
      int64_t first_start_ms = -1;
      int slot = subtitle_streams_num;

      if (!path_replace_extension(media_path, exts[i], sub_path, sizeof(sub_path)))
         continue;

      if (strcmp(sub_path, media_path) == 0)
         continue;

      if (access(sub_path, R_OK) != 0)
         continue;

      buf = read_entire_file(sub_path, &buf_size);
      if (!buf || buf_size == 0)
      {
         free(buf);
         continue;
      }

      if (!ensure_ass_context())
      {
         free(buf);
         return;
      }

      is_ass = buffer_looks_like_ass(buf, buf_size);
      track = ass_new_track(ass);
      if (!track)
      {
         free(buf);
         return;
      }

      if (is_ass)
      {
         ass_process_data(track, buf, (int)buf_size);
      }
      else
      {
         ass_init_default_track(track);
         any_events = srt_add_events_from_buffer(track, buf, buf_size, &first_start_ms);
         if (!any_events)
         {
            ass_free_track(track);
            free(buf);
            continue;
         }
      }

      ass_scale_track_styles(track, subtitle_font_scale);

      sctx[slot] = NULL;
      ass_track[slot] = track;
      subtitle_streams[slot] = -1;
      subtitle_is_ass[slot] = is_ass;
      subtitle_is_external[slot] = true;
      first_subtitle_start_ms[slot] = first_start_ms;
      subtitle_streams_num++;

      update_subtitle_style_overrides();

      log_cb(RETRO_LOG_INFO, "[APLAYER] Loaded external subtitles: %s (%s)\n",
            sub_path, is_ass ? "ass" : "text");

      free(buf);
      break;
   }
}

static void sws_worker_thread(void *arg)
{
   int ret = 0;
   AVFrame *tmp_frame = NULL;
   video_decoder_context_t *ctx = (video_decoder_context_t*) arg;

   tmp_frame = ctx->source;

   ctx->sws = sws_getCachedContext(ctx->sws,
         media.width, media.height, (enum AVPixelFormat)tmp_frame->format,
         media.width, media.height, AV_PIX_FMT_RGB32,
         SWS_FAST_BILINEAR, NULL, NULL, NULL);

   set_colorspace(ctx->sws, media.width, media.height,
         tmp_frame->colorspace,
         tmp_frame->color_range);

   if ((ret = sws_scale(ctx->sws, (const uint8_t *const*)tmp_frame->data,
         tmp_frame->linesize, 0, media.height,
         (uint8_t * const*)ctx->target->data, ctx->target->linesize)) < 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Error while scaling image: %s\n", av_err2str(ret));
   }

   ctx->pts = ctx->source->best_effort_timestamp;

   av_frame_unref(ctx->source);
   video_buffer_finish_slot(video_buffer, ctx);
}

static void decode_video(AVCodecContext *ctx, AVPacket *pkt, size_t frame_size, ASS_Track *ass_track_active)
{
   int ret = 0;
   video_decoder_context_t *decoder_ctx = NULL;

   /* Stop decoding thread until video_buffer is not full again */
   while (!decode_thread_dead && !video_buffer_has_open_slot(video_buffer))
   {
      if (main_sleeping)
      {
         if (!do_seek)
            log_cb(RETRO_LOG_ERROR, "[APLAYER] Thread: Video deadlock detected.\n");
         tpool_wait(tpool);
         video_buffer_clear(video_buffer);
         return;
      }
   }

   /* 1) Send the packet. */
   ret = avcodec_send_packet(ctx, pkt);
   if (ret == AVERROR(EAGAIN))
   {
      /* Means the decoder is full, so we must read (receive) frames first. */
      AVFrame *drain_frame = av_frame_alloc();
      if (!drain_frame)
         return;

      while (true)
      {
         ret = avcodec_receive_frame(ctx, drain_frame);
         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
         {
            break;
         }
         else if (ret < 0)
         {
            /* Real error. */
            log_cb(RETRO_LOG_ERROR,
               "[APLAYER] Error draining frames: %s\n", av_err2str(ret));
            av_frame_free(&drain_frame);
            return;
         }

         /* We got a frame; if needed we can queue it, or just discard. */
         av_frame_unref(drain_frame);
      }
      av_frame_free(&drain_frame);
      /* Now try sending the packet again. */
      ret = avcodec_send_packet(ctx, pkt);
   }

   /* If still an error that is NOT EAGAIN/EOF, we give up. */
   if (ret < 0 && ret != AVERROR_EOF)
   {
      /* Real error. */
      log_cb(RETRO_LOG_ERROR,
         "[APLAYER] Can't decode video packet: %s\n", av_err2str(ret));
      return;
   }

   /* 2) Receive frames in a loop. */
   while (!decode_thread_dead && video_buffer_has_open_slot(video_buffer))
   {
      video_buffer_get_open_slot(video_buffer, &decoder_ctx);

      ret = avcodec_receive_frame(ctx, decoder_ctx->source);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      {
         /* No more frames now, so break. */
         video_buffer_return_open_slot(video_buffer, decoder_ctx);
         break;
      }
      else if (ret < 0)
      {
         /* Real error. */
         log_cb(RETRO_LOG_ERROR,
            "[APLAYER] Error while reading video frame: %s\n", av_err2str(ret));
         video_buffer_return_open_slot(video_buffer, decoder_ctx);
         break;
      }

      decoder_ctx->ass_track_active = ass_track_active;
      /* Submit to worker thread which does sws_scale, etc. */
      tpool_add_work(tpool, sws_worker_thread, decoder_ctx);
   }
}

static int16_t *decode_audio(AVCodecContext *ctx, AVPacket *pkt,
      AVFrame *frame, int16_t *buffer, size_t *buffer_cap,
      SwrContext *swr)
{
   int ret = 0;
   int64_t pts = 0;
   size_t required_buffer = 0;
   int out_samples = 0;
   int max_out_samples = 0;
   size_t bytes_per_frame = sizeof(int16_t) * 2;
   static bool warned_fifo_small = false;

   if ((ret = avcodec_send_packet(ctx, pkt)) < 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Can't decode audio packet: %s\n", av_err2str(ret));
      return buffer;
   }

   for (;;)
   {
      ret = avcodec_receive_frame(ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
         break;
      else if (ret < 0)
      {
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Error while reading audio frame: %s\n", av_err2str(ret));
         break;
      }

      max_out_samples = swr_get_out_samples(swr, frame->nb_samples);
      if (max_out_samples < frame->nb_samples)
         max_out_samples = frame->nb_samples;

      required_buffer = (size_t)max_out_samples * bytes_per_frame;
      if (required_buffer > *buffer_cap)
      {
         buffer      = (int16_t*)av_realloc(buffer, required_buffer);
         *buffer_cap = required_buffer;
      }

      out_samples = swr_convert(swr,
            (uint8_t**)&buffer,
            max_out_samples,
            (const uint8_t**)frame->data,
            frame->nb_samples);
      if (out_samples < 0)
      {
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Error while resampling audio: %s\n",
               av_err2str(out_samples));
         break;
      }
      if (out_samples == 0)
         continue;

      required_buffer = (size_t)out_samples * bytes_per_frame;

      pts = frame->best_effort_timestamp;
      slock_lock(fifo_lock);

      if (!audio_decode_fifo)
      {
         slock_unlock(fifo_lock);
         continue;
      }

      size_t fifo_capacity = audio_decode_fifo->size ? audio_decode_fifo->size - 1 : 0;
      if (fifo_capacity == 0)
      {
         slock_unlock(fifo_lock);
         continue;
      }
      if (required_buffer > fifo_capacity)
      {
         if (!warned_fifo_small)
         {
            log_cb(RETRO_LOG_WARN,
                  "[APLAYER] Audio frame larger than FIFO (%zu > %zu), truncating.\n",
                  required_buffer, fifo_capacity);
            warned_fifo_small = true;
         }
         required_buffer = fifo_capacity;
      }

      while (!decode_thread_dead &&
            FIFO_WRITE_AVAIL(audio_decode_fifo) < required_buffer)
      {
         if (do_seek)
         {
            scond_signal(fifo_cond);
            slock_unlock(fifo_lock);
            return buffer;
         }
         if (!main_sleeping)
            scond_wait(fifo_decode_cond, fifo_lock);
         else if (video_stream_index < 0)
         {
            if (!scond_wait_timeout(fifo_decode_cond, fifo_lock, 2000))
               break;
         }
         else
         {
            log_cb(RETRO_LOG_ERROR, "[APLAYER] Thread: Audio deadlock detected.\n");
            fifo_clear(audio_decode_fifo);
            break;
         }
      }

      decode_last_audio_time = pts * av_q2d(
            fctx->streams[audio_streams[audio_streams_ptr]]->time_base);

      if (!decode_thread_dead &&
            FIFO_WRITE_AVAIL(audio_decode_fifo) >= required_buffer)
         fifo_write(audio_decode_fifo, buffer, required_buffer);

      scond_signal(fifo_cond);
      slock_unlock(fifo_lock);
   }

   return buffer;
}


static void decode_thread_seek(double time)
{
   int64_t seek_to = time * AV_TIME_BASE;
   int i = 0;

   if (seek_to < 0)
      seek_to = 0;

   decode_last_audio_time = time;

   if (avformat_seek_file(fctx, -1, INT64_MIN, seek_to, INT64_MAX, 0) < 0)
      log_cb(RETRO_LOG_ERROR, "[APLAYER] av_seek_frame() failed.\n");

   if (video_stream_index >= 0)
   {
      tpool_wait(tpool);
      video_buffer_clear(video_buffer);
   }

   if (actx[audio_streams_ptr])
      avcodec_flush_buffers(actx[audio_streams_ptr]);
   if (vctx)
      avcodec_flush_buffers(vctx);
   for (i = 0; i < subtitle_streams_num; i++)
   {
      if (sctx[i])
         avcodec_flush_buffers(sctx[i]);
      if (ass_track[i] && !subtitle_is_external[i])
         ass_flush_events(ass_track[i]);
   }
}

/**
 * This function makes sure that we don't decode too many
 * packets and cause stalls in our decoding pipeline.
 * This could happen if we decode too many packets and
 * saturate our buffers. We have a window of "still okay"
 * to decode, that depends on the media fps.
 **/
static bool earlier_or_close_enough(double p1, double p2)
{
   return (p1 <= p2 || (p1-p2) < (1.0 / media.interpolate_fps) );
}

static void decode_thread(void *data)
{
   unsigned i;
   bool eof                = false;
   struct SwrContext *swr[(audio_streams_num > 0) ? audio_streams_num : 1];
   AVFrame *aud_frame      = NULL;
   size_t frame_size       = 0;
   int16_t *audio_buffer   = NULL;
   size_t audio_buffer_cap = 0;
   packet_buffer_t *audio_packet_buffer;
   packet_buffer_t *video_packet_buffer;
   double last_audio_end  = 0;

   (void)data;

   for (i = 0; (int)i < audio_streams_num; i++)
   {
      swr[i] = swr_alloc();

      AVChannelLayout in_default_layout = {0};
      const AVChannelLayout *in_layout = &actx[i]->ch_layout;
      AVChannelLayout out_layout = {0};
      int in_channels = in_layout->nb_channels;

      if (in_channels <= 0)
         in_channels = 2;

      /* Swr expects a concrete channel map; normalize unknown/custom layouts. */
      if (in_layout->order != AV_CHANNEL_ORDER_NATIVE || in_layout->u.mask == 0)
      {
         av_channel_layout_default(&in_default_layout, in_channels);
         in_layout = &in_default_layout;
      }

      av_channel_layout_default(&out_layout, 2);

      av_opt_set_chlayout(swr[i], "in_chlayout", in_layout, 0);
      av_opt_set_chlayout(swr[i], "out_chlayout", &out_layout, 0);
      av_opt_set_int(swr[i], "in_sample_rate",     actx[i]->sample_rate, 0);
      av_opt_set_int(swr[i], "out_sample_rate",    media.sample_rate,    0);
      av_opt_set_sample_fmt(swr[i], "in_sample_fmt",  actx[i]->sample_fmt, 0);
      av_opt_set_sample_fmt(swr[i], "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);

      /* matriz solo si hay 6 canales */
      if (in_layout->nb_channels == 6) {
         double m[12] = {
            1.0, 0.0, 1.2, 0.0, 0.5, 0.5, 
            0.0, 1.0, 1.2, 0.0, 0.5, 0.5
         };
         for (int j=0;j<12;j++) m[j]*=2.0;          /* +6 dB */
         swr_set_matrix(swr[i], m, 0);
      }
      
      int ret;
      if ((ret = swr_init(swr[i])) < 0) {
         log_cb(RETRO_LOG_ERROR, "swr_init fallo: %s\n", av_err2str(ret));
         av_channel_layout_uninit(&out_layout);
         av_channel_layout_uninit(&in_default_layout);
         continue;
     }

      av_channel_layout_uninit(&out_layout);
      av_channel_layout_uninit(&in_default_layout);
   }

   aud_frame = av_frame_alloc();
   audio_packet_buffer = packet_buffer_create();
   video_packet_buffer = packet_buffer_create();

   if (video_stream_index >= 0)
   {
      frame_size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, media.width, media.height, 1);
      video_buffer = video_buffer_create(4, frame_size, media.width, media.height);
      tpool = tpool_create(sw_sws_threads);
      log_cb(RETRO_LOG_INFO, "[APLAYER] Configured worker threads: %d\n", sw_sws_threads);
   }

   AVPacket *pkt_local = av_packet_alloc();
   if (!pkt_local)
      goto end;

   while (!decode_thread_dead)
   {
      av_packet_unref(pkt_local);

      /* If we are paused *and* there is no pending seek we can idle,
      * otherwise we must handle the seek so the main thread unblocks. */
      slock_lock(fifo_lock);
      bool pending_seek = do_seek;
      slock_unlock(fifo_lock);

      if (paused && !pending_seek) {
         usleep(10 * 1000);
         continue;
      }

      bool seek;
      double seek_time_thread;
      int audio_stream_index, audio_stream_ptr;

      double audio_timebase   = 0.0;
      double video_timebase   = 0.0;
      double next_video_end   = 0.0;
      double next_audio_start = 0.0;

      AVCodecContext *actx_active = NULL;
      ASS_Track *ass_track_active = NULL;

      slock_lock(fifo_lock);
      seek             = do_seek;
      seek_time_thread = seek_time;
      slock_unlock(fifo_lock);

      if (seek)
      {
         decode_thread_seek(seek_time_thread);

         slock_lock(fifo_lock);
         do_seek          = false;
         eof              = false;
         seek_time        = 0.0;
         next_video_end   = 0.0;
         next_audio_start = 0.0;
         last_audio_end   = 0.0;

         if (audio_decode_fifo)
            fifo_clear(audio_decode_fifo);

         // Reset packet buffer states
         packet_buffer_clear(&audio_packet_buffer);
         packet_buffer_clear(&video_packet_buffer);

         scond_signal(fifo_cond);
         slock_unlock(fifo_lock);
      }

      slock_lock(decode_thread_lock);
      audio_stream_index          = audio_streams[audio_streams_ptr];
      audio_stream_ptr            = audio_streams_ptr;
      actx_active                 = actx[audio_streams_ptr];
      ass_track_active            = ass_track[subtitle_streams_ptr];
      audio_timebase = av_q2d(fctx->streams[audio_stream_index]->time_base);
      if (video_stream_index >= 0)
         video_timebase = av_q2d(fctx->streams[video_stream_index]->time_base);
      slock_unlock(decode_thread_lock);

      if (!packet_buffer_empty(audio_packet_buffer))
         next_audio_start = audio_timebase * packet_buffer_peek_start_pts(audio_packet_buffer);

      if (!packet_buffer_empty(video_packet_buffer))
         next_video_end = video_timebase * packet_buffer_peek_end_pts(video_packet_buffer);

      /* Decide whether to pull one audio packet from audio_packet_buffer.
       *
       * We do it when:
       *   1. There is no video stream (audio-only file)
       *   2. Audio PTS is not “too far” ahead of the next video PTS
       *      (<= 500 ms tolerance),                              ahead < 0.5
       *   3. The decoder already hit EOF,                        eof == true
       *   4. The main thread is blocked waiting for audio data,  need_audio_now
       *
       * Together these rules guarantee:
       *   – Audio never outruns video by more than half a second during normal
       *     playback.
       *   – The main thread can never dead-lock after a seek: if it is waiting
       *     (`main_sleeping`), we always feed at least one packet even when the
       *     PTS gap is large.
       */

      bool need_audio_now = false;
      slock_lock(fifo_lock);
      need_audio_now = main_sleeping;   /* main thread is waiting for us */
      slock_unlock(fifo_lock);

      double ahead = next_audio_start - next_video_end;   /* may be < 0 */
      bool   okay  = (video_stream_index < 0) ||
                     (ahead < 0.5 /*s*/) || need_audio_now || eof;

      if (okay && !packet_buffer_empty(audio_packet_buffer))
      {
         packet_buffer_get_packet(audio_packet_buffer, pkt_local);
         last_audio_end = audio_timebase * (pkt_local->pts + pkt_local->duration);
         audio_buffer = decode_audio(actx_active, pkt_local, aud_frame,
                                    audio_buffer, &audio_buffer_cap,
                                    swr[audio_stream_ptr]);
         av_packet_unref(pkt_local);
      }

      /*
       * Decode video packet if:
       *  1. we already decoded an audio packet
       *  2. there is no audio stream to play
       *  3. EOF
       **/
      if (!packet_buffer_empty(video_packet_buffer) &&
            (
               (!eof && earlier_or_close_enough(next_video_end, last_audio_end)) ||
               !actx_active ||
               eof
            )
         )
      {
         packet_buffer_get_packet(video_packet_buffer, pkt_local);

         decode_video(vctx, pkt_local, frame_size, ass_track_active);

         av_packet_unref(pkt_local);
      }

      bool break_out_loop = false;

      int media_type = get_media_type();
      bool loop_content = false;

      switch (media_type)
      {
      case MEDIA_TYPE_AUDIO:
         loop_content = packet_buffer_empty(audio_packet_buffer) && packet_buffer_empty(video_packet_buffer) && eof &&
                        (!audio_decode_fifo || FIFO_READ_AVAIL(audio_decode_fifo) <= 1024 * 10);
         break;
      case MEDIA_TYPE_VIDEO:
      default:
         loop_content = packet_buffer_empty(audio_packet_buffer) && packet_buffer_empty(video_packet_buffer) && eof;
         break;
      }

      if (loop_content)
      {
         switch (loopcontent)
         {
         case PLAY_TRACK:
            if (playlist_count > 0)
            {
               // For an M3U playlist, if there is a next track then advance;
               // if already at the last track, finish playback.
               if (playlist_index + 1 < playlist_count)
               {
                  slock_lock(fifo_lock);
                  playlist_index++;
                  do_seek = true;
                  seek_time = 0.0;
                  eof = false;
                  slock_unlock(fifo_lock);
                  log_cb(RETRO_LOG_INFO, "[APLAYER] Advancing to playlist item #%u/%u: %s\n",
                         playlist_index + 1, playlist_count, playlist[playlist_index]);
                  break_out_loop = true; // break out to allow media reload
               }
               else
               {
                  // Last track in playlist: end playback.
                  break_out_loop = true;
               }
            }
            else
            {
               // Single file: do not loop, finish playback.
               break_out_loop = true;
            }
            break;

         case LOOP_TRACK:
            // Always loop the current track, regardless of whether we're part of a playlist.
            slock_lock(fifo_lock);
            do_seek = true;
            seek_time = 0.0;
            eof = false; // Reset the EOF flag.
            slock_unlock(fifo_lock);
            // Continue decoding on the same track.
            continue;

         case LOOP_ALL:
            if (playlist_count > 0)
            {
               // If we have a playlist, advance to the next track (or wrap around).
               slock_lock(fifo_lock);
               if (playlist_index + 1 < playlist_count)
                  playlist_index++;
               else
                  playlist_index = 0;
               do_seek = true;
               seek_time = 0.0;
               eof = false;
               slock_unlock(fifo_lock);
               log_cb(RETRO_LOG_INFO, "[APLAYER] Looping playlist, new track #%u/%u: %s\n",
                      playlist_index + 1, playlist_count, playlist[playlist_index]);
               // Exit the loop so that retro_run() can unload and reload the new media.
               break_out_loop = true;
            }
            else
            {
               // If no playlist (single file), behave like LOOP_TRACK.
               slock_lock(fifo_lock);
               do_seek = true;
               seek_time = 0.0;
               eof = false;
               slock_unlock(fifo_lock);
               continue;
            }
            break;

         case SHUFFLE_ALL:
            if (playlist_count > 0)
            {
               // In a playlist mode, pick a random track different from the current one (if possible).
               unsigned old_index = playlist_index;
               slock_lock(fifo_lock);
               do
               {
                  playlist_index = rand() % playlist_count;
               } while (playlist_count > 1 && playlist_index == old_index);
               do_seek = true;
               seek_time = 0.0;
               eof = false;
               slock_unlock(fifo_lock);
               log_cb(RETRO_LOG_INFO, "[APLAYER] Shuffling playlist, new track #%u/%u: %s\n",
                      playlist_index + 1, playlist_count, playlist[playlist_index]);
               // Exit the loop so that the main thread can reload the newly chosen track.
               break_out_loop = true;
            }
            else
            {
               // If playing a single file, behave like LOOP_TRACK.
               slock_lock(fifo_lock);
               do_seek = true;
               seek_time = 0.0;
               eof = false;
               slock_unlock(fifo_lock);
               continue;
            }
            break;

         default:
            // Fallback: treat as PLAY_TRACK (i.e. finish).
            break_out_loop = true;
            break;
         }

         if (break_out_loop)
            break;
      }

      // Check if an audio track switch has been requested.
      if (audio_switch_requested)
      {
         // Acquire the FIFO lock if necessary (or other audio state lock)
         slock_lock(fifo_lock);

         // Flush FIFO to drop any packets from the old track.
         if (audio_decode_fifo) {
            fifo_clear(audio_decode_fifo);
         }

         // Flush the audio codec buffers for the old track.
         if (actx[audio_streams_ptr]) {
            avcodec_flush_buffers(actx[audio_streams_ptr]);
         }
         
         // Reset timing variables (as an example; adjust to your logic).
         decode_last_audio_time = 0;
         audio_frames = 0;
         
         // Signal any condition variable to re-awaken waiting threads.
         scond_signal(fifo_cond);

         // Clear the switch request flag.
         audio_switch_requested = false;
         
         slock_unlock(fifo_lock);
      }

      // Read the next frame and stage it in case of audio or video frame.
      if (av_read_frame(fctx, pkt_local) < 0)
      {
         eof = true;
      }
      else
      {
         // Update g_current_time if not NOPTS
         if (pkt_local->pts != AV_NOPTS_VALUE)
         {
            double this_pts_seconds = pkt_local->pts * av_q2d(fctx->streams[pkt_local->stream_index]->time_base);

            slock_lock(time_lock);
            g_current_time = this_pts_seconds;
            slock_unlock(time_lock);
         }

         if (pkt_local->stream_index == audio_stream_index && actx_active)
            packet_buffer_add_packet(audio_packet_buffer, pkt_local);
         else if (pkt_local->stream_index == video_stream_index)
            packet_buffer_add_packet(video_packet_buffer, pkt_local);
         else
         {
            int subtitle_slot = subtitle_slot_for_stream(pkt_local->stream_index);
            AVCodecContext *sctx_sub = NULL;
            ASS_Track *ass_track_sub = NULL;

            if (subtitle_slot < 0)
            {
               av_packet_unref(pkt_local);
               continue;
            }

            sctx_sub = sctx[subtitle_slot];
            ass_track_sub = ass_track[subtitle_slot];
            if (!sctx_sub)
            {
               av_packet_unref(pkt_local);
               continue;
            }

            /**
             * Decode subtitle packets right away for ASS/SSA and text subtitles.
             * Bitmap subtitles would need buffering and blending.
             **/
            AVSubtitle sub;
            int finished = 0;
            int64_t base_time_ms = 0;
            int64_t start_ms = 0;
            int64_t end_ms = 0;

            memset(&sub, 0, sizeof(sub));

            while (!finished)
            {
               if (avcodec_decode_subtitle2(sctx_sub, &sub, &finished, pkt_local) < 0)
               {
                  log_cb(RETRO_LOG_ERROR, "[APLAYER] Decode subtitles failed.\n");
                  break;
               }
            }

            if (pkt_local->pts != AV_NOPTS_VALUE)
               base_time_ms = (int64_t)(pkt_local->pts *
                     av_q2d(fctx->streams[pkt_local->stream_index]->time_base) * 1000.0);
            else if (sub.pts != AV_NOPTS_VALUE)
               base_time_ms = sub.pts / 1000;

            start_ms = base_time_ms + sub.start_display_time;
            end_ms = base_time_ms + sub.end_display_time;

            for (i = 0; i < sub.num_rects; i++)
            {
               if (!sub.rects[i])
                  continue;
               slock_lock(ass_lock);
               if (ass_track_sub)
               {
                  if (subtitle_is_ass[subtitle_slot])
                  {
                     if (sub.rects[i]->ass)
                     {
                        if (!first_subtitle_event_logged)
                        {
                           const char *payload = sub.rects[i]->ass;
                           if (first_subtitle_start_ms[subtitle_slot] < 0)
                              first_subtitle_start_ms[subtitle_slot] = start_ms;
                           log_cb(RETRO_LOG_INFO,
                                 "[APLAYER] First subtitle event (ass): stream=%d slot=%d start_ms=%lld end_ms=%lld text=\"%.200s\"\n",
                                 pkt_local->stream_index, subtitle_slot,
                                 (long long)start_ms, (long long)end_ms,
                                 payload ? payload : "(null)");
                           first_subtitle_event_logged = true;
                        }
                        ass_process_line(ass_track_sub, sub.rects[i]->ass);
                     }
                     else if (sub.rects[i]->text)
                     {
                        if (!first_subtitle_event_logged)
                        {
                           const char *payload = sub.rects[i]->text;
                           if (first_subtitle_start_ms[subtitle_slot] < 0)
                              first_subtitle_start_ms[subtitle_slot] = start_ms;
                           log_cb(RETRO_LOG_INFO,
                                 "[APLAYER] First subtitle event (text): stream=%d slot=%d start_ms=%lld end_ms=%lld text=\"%.200s\"\n",
                                 pkt_local->stream_index, subtitle_slot,
                                 (long long)start_ms, (long long)end_ms,
                                 payload ? payload : "(null)");
                           first_subtitle_event_logged = true;
                        }
                        ass_add_text_event(ass_track_sub,
                              start_ms, end_ms, sub.rects[i]->text);
                     }
                  }
                  else
                  {
                     const char *raw_text = sub.rects[i]->text;
                     const char *ass_payload = NULL;

                     if (!raw_text && sub.rects[i]->ass)
                        ass_payload = ass_event_payload(sub.rects[i]->ass);

                     if (!first_subtitle_event_logged)
                     {
                        const char *payload = raw_text ? raw_text : ass_payload;
                        if (first_subtitle_start_ms[subtitle_slot] < 0)
                           first_subtitle_start_ms[subtitle_slot] = start_ms;
                        log_cb(RETRO_LOG_INFO,
                              "[APLAYER] First subtitle event (text): stream=%d slot=%d start_ms=%lld end_ms=%lld text=\"%.200s\"\n",
                              pkt_local->stream_index, subtitle_slot,
                              (long long)start_ms, (long long)end_ms,
                              payload ? payload : "(null)");
                        first_subtitle_event_logged = true;
                     }

                     if (raw_text)
                        ass_add_text_event(ass_track_sub, start_ms, end_ms, raw_text);
                     else if (ass_payload)
                        ass_add_text_event_raw(ass_track_sub, start_ms, end_ms, ass_payload);
                  }
               }
               slock_unlock(ass_lock);
            }
            avsubtitle_free(&sub);
            av_packet_unref(pkt_local);
         }
      }
   }

end:
   av_packet_free(&pkt_local);

   for (i = 0; (int)i < audio_streams_num; i++)
      swr_free(&swr[i]);

   packet_buffer_destroy(audio_packet_buffer);
   packet_buffer_destroy(video_packet_buffer);

   av_frame_free(&aud_frame);
   av_freep(&audio_buffer);

   slock_lock(fifo_lock);
   decode_thread_dead = true;
   scond_signal(fifo_cond);
   slock_unlock(fifo_lock);
}

static void context_destroy(void)
{
   if (fft)
   {
      fft_free(fft);
      fft = NULL;
   }
}

#include "gl_shaders/ffmpeg.glsl.vert.h"

/* OpenGL ES note about main() -  Get format as GL_RGBA/GL_UNSIGNED_BYTE.
 * Assume little endian, so we get ARGB -> BGRA byte order, and
 * we have to swizzle to .BGR. */
#include "gl_shaders/ffmpeg_es.glsl.frag.h"

static void context_reset(void)
{
   static const GLfloat vertex_data[] = {
      -1, -1, 0, 0,
       1, -1, 1, 0,
      -1,  1, 0, 1,
       1,  1, 1, 1,
   };
   GLuint vert, frag;
   unsigned i;

   frames_tex_width  = 0;
   frames_tex_height = 0;

   if (audio_streams_num > 0 && video_stream_index < 0)
   {
      fft = fft_new(11, hw_render.get_proc_address);
      if (fft)
         fft_init_multisample(fft);
   }

   /* Already inits symbols. */
   if (!fft)
      rglgen_resolve_symbols(hw_render.get_proc_address);

   prog = glCreateProgram();
   vert = glCreateShader(GL_VERTEX_SHADER);
   frag = glCreateShader(GL_FRAGMENT_SHADER);

   glShaderSource(vert, 1, &vertex_source, NULL);
   glShaderSource(frag, 1, &fragment_source, NULL);
   glCompileShader(vert);
   glCompileShader(frag);
   glAttachShader(prog, vert);
   glAttachShader(prog, frag);
   glLinkProgram(prog);

   glUseProgram(prog);

   glUniform1i(glGetUniformLocation(prog, "sTex0"), 0);
   glUniform1i(glGetUniformLocation(prog, "sTex1"), 1);
   vertex_loc = glGetAttribLocation(prog, "aVertex");
   tex_loc    = glGetAttribLocation(prog, "aTexCoord");
   mix_loc    = glGetUniformLocation(prog, "uMix");

   glUseProgram(0);

   for (i = 0; i < 2; i++)
   {
      glGenTextures(1, &frames[i].tex);

      glBindTexture(GL_TEXTURE_2D, frames[i].tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER,
         sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);

   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void retro_unload_game(void)
{
   unsigned i;

   if (decode_thread_handle)
   {
      /* Stop the decode thread first */
      slock_lock(fifo_lock);
      decode_thread_dead = true;
      scond_signal(fifo_decode_cond);
      slock_unlock(fifo_lock);

      /* Join the decode thread – no more tasks will be enqueued */
      sthread_join(decode_thread_handle);
      decode_thread_handle = NULL;
   }
   
   /* Now that decode_thread is done, wait for all worker tasks */
   if (tpool)
   {
      tpool_wait(tpool); // Wait for tasks to finish
      tpool_destroy(tpool);
      tpool = NULL;
   }

   /* Safe to clear buffer now, since no thread references it anymore */
   if (video_buffer)
   {
      video_buffer_destroy(video_buffer);
      video_buffer = NULL;
   }

   if (fifo_cond)
      scond_free(fifo_cond);
   if (fifo_decode_cond)
      scond_free(fifo_decode_cond);
   if (fifo_lock)
      slock_free(fifo_lock);
   if (decode_thread_lock)
      slock_free(decode_thread_lock);
   if (ass_lock)
      slock_free(ass_lock);
   if (time_lock)
      slock_free(time_lock);
   if (audio_decode_fifo)
      fifo_free(audio_decode_fifo);

   fifo_cond = NULL;
   fifo_decode_cond = NULL;
   fifo_lock = NULL;
   decode_thread_lock = NULL;
   audio_decode_fifo = NULL;
   ass_lock = NULL;
   time_lock = NULL;

   decode_last_audio_time = 0.0;

   frames[0].pts = frames[1].pts = 0.0;
   pts_bias = 0.0;
   frame_cnt = 0;
   audio_frames = 0;

   for (i = 0; i < MAX_STREAMS; i++)
   {
      if (sctx[i])
         avcodec_free_context(&sctx[i]);
      if (actx[i])
         avcodec_free_context(&actx[i]);
      sctx[i] = NULL;
      actx[i] = NULL;
   }

   if (vctx)
   {
      avcodec_free_context(&vctx);
   }

   if (fctx)
   {
      avformat_close_input(&fctx);
      fctx = NULL;
   }

   for (i = 0; i < attachments_size; i++) {
      av_freep(&attachments[i].data);
   }
   av_freep(&attachments); // Free the array itself
   attachments_size = 0;

   for (i = 0; i < MAX_STREAMS; i++)
   {
      if (ass_track[i])
         ass_free_track(ass_track[i]);
      ass_track[i] = NULL;

      av_freep(&ass_extra_data[i]);
      ass_extra_data_size[i] = 0;
   }
   if (ass_render) {
      ass_renderer_done(ass_render);
      ass_render = NULL;
   }
   if (ass) {
      ass_library_done(ass);
      ass = NULL;
   }

   ass_render = NULL;
   ass = NULL;

   media_reset_defaults();
}

bool retro_load_game(const struct retro_game_info *info)
{
   if (!info)
      return false;

   media_reset_defaults();

   const char* ext = strrchr(info->path, '.');
   // Local mutable retro_game_info
   struct retro_game_info local_info;

   if (ext && strcasecmp(ext, ".m3u") == 0)
   {
      if (!parse_m3u_playlist(info->path))
         return false;

      // Load first media file in playlist
      local_info.path = playlist[playlist_index];
      local_info.size = info->size;
      local_info.data = info->data;
   }
   else
   {
      // If not an M3U, check if we're in the middle of a playlist
      if (playlist_count == 0)
      {
         // Not part of a playlist; load as single file
         playlist_count = 0;
         playlist_index = 0;
         local_info.path = info->path;
         local_info.size = info->size;
         local_info.data = info->data;
      }
      else
      {
         // Part of an existing playlist; use the current playlist entry
         local_info.path = playlist[playlist_index];
         local_info.size = info->size;
         local_info.data = info->data;
      }
   }

   gme_seek_disabled = is_gme_path(local_info.path);

   log_cb(RETRO_LOG_INFO, "[APLAYER] Loading %s", local_info.path);

   /*
      AV_LOG_QUIET: No messages are printed.
      AV_LOG_PANIC: Only panics, which are very serious errors like an application crash, are printed.
      AV_LOG_FATAL: Fatal errors are printed.
      AV_LOG_ERROR: Errors are printed.
      AV_LOG_WARNING: Warnings are printed.
      AV_LOG_INFO: Informational messages are printed (default level).
      AV_LOG_VERBOSE: Verbose messages are printed.
      AV_LOG_DEBUG: Debugging messages are printed.
   */
#ifdef DEBUG
   av_log_set_level(AV_LOG_DEBUG);
#else
   av_log_set_level(AV_LOG_QUIET);
#endif
   int ret = 0;
   bool is_fft = false;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Play/Pause" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Display Time" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Display Title" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Toggle Subtitles"},
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Cycle Audio Track"},
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Previous (M3U)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Next (M3U)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Seek -15 sec" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Seek +15 sec" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Seek -3 min" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Seek +3 min" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Seek -5 min" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Seek +5 min" },
      { 0 },
   };

   check_variables(true);

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Cannot set pixel format.");
      goto error;
   }

   if ((ret = avformat_open_input(&fctx, local_info.path, NULL, NULL)) < 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to open input: %s. %s\n", av_err2str(ret));
      goto error;
   }

   print_ffmpeg_version();

   if ((ret = avformat_find_stream_info(fctx, NULL)) < 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to find stream info: %s\n", av_err2str(ret));
      goto error;
   }

   log_cb(RETRO_LOG_INFO, "[APLAYER] Media information:\n");
   av_dump_format(fctx, 0, local_info.path, 0);

   if (!open_codecs())
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to find codec.");
      goto error;
   }

   if (!init_media_info())
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to init media info.");
      goto error;
   }

   time_supported = duration_is_valid(media.duration.time);
   seek_supported = time_supported && !gme_seek_disabled;

   maybe_load_external_subtitles(local_info.path);

   is_fft = video_stream_index < 0 && audio_streams_num > 0;

   if (video_stream_index >= 0 || is_fft)
   {
      hw_render.context_reset      = context_reset;
      hw_render.context_destroy    = context_destroy;
      hw_render.bottom_left_origin = is_fft;
      hw_render.depth              = is_fft;
      hw_render.stencil            = is_fft;
      hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      {
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Cannot initialize HW render.\n");
      }
   }

   /* NEW: advertise geometry/timing to the frontend right after we know them */
   {
      struct retro_system_av_info av;
      retro_get_system_av_info(&av);
      if (!environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av))
         log_cb(RETRO_LOG_WARN, "[APLAYER] Frontend did not accept SYSTEM_AV_INFO.\n");
   }

   if (audio_streams_num > 0)
   {
      /* audio fifo is 2 seconds deep */
      audio_decode_fifo = fifo_new(
         media.sample_rate * sizeof(int16_t) * 2 * 2
      );
   }

   fifo_cond        = scond_new();
   fifo_decode_cond = scond_new();
   fifo_lock        = slock_new();
   ass_lock         = slock_new();
   time_lock        = slock_new();

   slock_lock(fifo_lock);
   decode_thread_dead = false;
   slock_unlock(fifo_lock);

   decode_thread_handle = sthread_create(decode_thread, NULL);

   pts_bias = 0.0;

   return true;

error:
   retro_unload_game();
   return false;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
