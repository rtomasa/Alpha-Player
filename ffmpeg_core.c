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
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
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

static double g_current_time = 0.0;  // PTS in seconds
static slock_t *time_lock    = NULL; // Protects g_current_time

static unsigned sw_decoder_threads;
static unsigned sw_sws_threads;
static video_buffer_t *video_buffer;
static tpool_t *tpool;

static enum AVHWDeviceType hw_decoder;
static bool hw_decoding_enabled;
static enum AVPixelFormat pix_fmt;
static bool force_sw_decoder;

#define MAX_STREAMS 8
static AVCodecContext *actx[MAX_STREAMS];
static AVCodecContext *sctx[MAX_STREAMS];
static int audio_streams[MAX_STREAMS];
static int audio_streams_num;
static int audio_streams_ptr;
static int subtitle_streams[MAX_STREAMS];
static int subtitle_streams_num;
static int subtitle_streams_ptr;

/* AAS/SSA subtitles. */
static ASS_Library *ass;
static ASS_Renderer *ass_render;
static ASS_Track *ass_track[MAX_STREAMS];
static uint8_t *ass_extra_data[MAX_STREAMS];
static size_t ass_extra_data_size[MAX_STREAMS];
static slock_t *ass_lock;

static struct attachment *attachments;
static size_t attachments_size;

static fft_t *fft;
unsigned fft_width;
unsigned fft_height;

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

static uint32_t *video_frame_temp_buffer;

/* Seeking, play, pause, loop */
static bool do_seek;
static double seek_time;
static bool paused = false;
static volatile bool audio_switch_requested = false;

/* GL stuff */
static struct frame frames[2];

static struct retro_hw_render_callback hw_render;
static GLuint prog;
static GLuint vbo;
static GLint vertex_loc;
static GLint tex_loc;
static GLint mix_loc;

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

      // Build full path
      char full_path[PATH_MAX];
      snprintf(full_path, PATH_MAX, "%s/%s", dir, trimmed);

      // Copy the full path into playlist
      strncpy(playlist[playlist_count++], full_path, PATH_MAX - 1);

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
   info->library_version  = "v2.0.5";
   info->need_fullpath    = true;
   info->valid_extensions = "mkv|avi|f4v|f4f|3gp|ogm|flv|mp4|mp3|flac|ogg|m4a|webm|3g2|mov|wmv|mpg|mpeg|vob|asf|divx|m2p|m2ts|ps|ts|mxf|wma|wav|m3u";
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
   info->timing.sample_rate = actx[0] ? media.sample_rate : 32000.0;

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
      {"music", "Music", "Music Settings"},
      {"video", "Video", "Video settings"},
      {NULL, NULL, NULL}
   };

   struct retro_core_option_v2_definition option_definitions[] =
   {
      {
         "aplayer_hw_decoder", "Hardware Decoder (restart)", NULL, INFO_RESTART, NULL, "video",
         {
            {"auto", "Automatic"},
            {"off", "Disabled"},
            {"drm", "DRM"},
            {NULL, NULL}
         }, "auto"
      },
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
         "aplayer_fft_resolution", "Visualizer Resolution", NULL, NULL, NULL, "music",
         {
            {"320x240", NULL},
            {"320x180", NULL},
            {NULL, NULL}
         }, "320x240"
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

static void check_variables(bool firststart)
{
   struct retro_variable hw_var  = {0};
   struct retro_variable sw_threads_var = {0};
   struct retro_variable loop_content  = {0};
   struct retro_variable replay_is_crt  = {0};
   struct retro_variable fft_var    = {0};

   fft_var.key = "aplayer_fft_resolution";

   fft_width       = 320;
   fft_height      = 240;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &fft_var) && fft_var.value)
   {
      unsigned w, h;
      if (sscanf(fft_var.value, "%ux%u", &w, &h) == 2)
      {
         fft_width = w;
         fft_height = h;
      }
   }
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
      hw_var.key = "aplayer_hw_decoder";

      force_sw_decoder = false;
      hw_decoder = AV_HWDEVICE_TYPE_NONE;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &hw_var) && hw_var.value)
      {
         if (string_is_equal(hw_var.value, "off"))
            force_sw_decoder = true;
         else if (string_is_equal(hw_var.value, "drm"))
            hw_decoder = AV_HWDEVICE_TYPE_DRM;
      }
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
   struct retro_message_ext msg_obj = {0};
   int seek_frames_capped           = seek_frames;
   unsigned seek_hours              = 0;
   unsigned seek_minutes            = 0;
   unsigned seek_seconds            = 0;
   int8_t seek_progress             = -1;

   msg[0] = '\0';

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

   do_seek = true;
   slock_lock(fifo_lock);
   seek_time      = frame_cnt / media.interpolate_fps;

   /* Convert seek time to a printable format */
   seek_seconds  = (unsigned)seek_time;
   seek_minutes  = seek_seconds / 60;
   seek_seconds %= 60;
   seek_hours    = seek_minutes / 60;
   seek_minutes %= 60;

   /* Get current progress */
   if (media.duration.time > 0.0)
   {
      seek_progress = (int8_t)((100.0 * seek_time / media.duration.time) + 0.5);
      seek_progress = (seek_progress < -1)  ? -1  : seek_progress;
      seek_progress = (seek_progress > 100) ? 100 : seek_progress;
   }

   snprintf(msg, sizeof(msg), "%02d:%02d:%02d / %02d:%02d:%02d (%d%%)",
         seek_hours, seek_minutes, seek_seconds,
         media.duration.hours, media.duration.minutes, media.duration.seconds,
         seek_progress);

   /* Send message to frontend */
   msg_obj.msg      = msg;
   msg_obj.duration = 3000;
   msg_obj.priority = 3;
   msg_obj.level    = RETRO_LOG_INFO;
   msg_obj.target   = RETRO_MESSAGE_TARGET_OSD;
   msg_obj.type     = RETRO_MESSAGE_TYPE_PROGRESS;
   msg_obj.progress = seek_progress;
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
   struct retro_message_ext msg_obj = {0};
   msg[0] = '\0';

   // 1. Read the shared "current time" in seconds from a protected variable
   double current_time = 0.0;
   slock_lock(time_lock);
   current_time = g_current_time;   // The decode thread sets this
   slock_unlock(time_lock);

   // 2. Get total duration of the file (already in fctx->duration)
   double total_duration = (double)fctx->duration / AV_TIME_BASE;

   // (Optional) If current_time <= 0 or uninitialized, you could decide to return
   // or skip the rest. That is up to your design.
   if (current_time < 0.0)
      current_time = 0.0;

   // 3. Convert current_time to HH:MM:SS
   int current_hours   = (int)(current_time / 3600);
   int current_minutes = ((int)current_time % 3600) / 60;
   int current_seconds = (int)current_time % 60;

   // 4. Convert total_duration to HH:MM:SS
   int total_hours     = (int)(total_duration / 3600);
   int total_minutes   = ((int)total_duration % 3600) / 60;
   int total_seconds   = (int)total_duration % 60;

   // 5. Compute progress percentage (0..100)
   double progress = 0.0;
   if (total_duration > 0.0)
   {
      progress = (current_time / total_duration) * 100.0;
      if (progress < 0.0)   progress = 0.0;
      if (progress > 100.0) progress = 100.0;
   }

   // 6. Build the message string
   snprintf(msg, sizeof(msg),
         "%02d:%02d:%02d / %02d:%02d:%02d (%d%%)",
         current_hours, current_minutes, current_seconds,
         total_hours,   total_minutes,   total_seconds,
         (int)progress);

   // 7. Fill in the retro_message_ext object and send to frontend
   msg_obj.msg      = msg;
   msg_obj.duration = 3000;
   msg_obj.priority = 3;
   msg_obj.level    = RETRO_LOG_INFO;
   msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
   msg_obj.type     = RETRO_MESSAGE_TYPE_PROGRESS;
   msg_obj.progress = progress;

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
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
      if (!retro_load_game(&next_info))
      {
         // Handle error
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
      uint64_t expected_audio_frames = frame_cnt * media.sample_rate / media.interpolate_fps;

      to_read_frames = expected_audio_frames - audio_frames;
      to_read_bytes = to_read_frames * sizeof(int16_t) * 2;

      slock_lock(fifo_lock);
      while (!decode_thread_dead && FIFO_READ_AVAIL(audio_decode_fifo) < to_read_bytes)
      {
         main_sleeping = true;
         scond_signal(fifo_decode_cond);
         scond_wait(fifo_cond, fifo_lock);
         main_sleeping = false;
      }
      reading_pts  = decode_last_audio_time -
         (double)FIFO_READ_AVAIL(audio_decode_fifo) / (media.sample_rate * sizeof(int16_t) * 2);
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
         fifo_read(audio_decode_fifo, audio_buffer, to_read_bytes);
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
            unsigned y;
            int stride, width;
            const uint8_t *src           = NULL;
            video_decoder_context_t *ctx = NULL;
            uint32_t               *data = NULL;

            video_buffer_wait_for_finished_slot(video_buffer);

            video_buffer_get_finished_slot(video_buffer, &ctx);
            pts                          = ctx->pts;
            data                         = video_frame_temp_buffer;
            src                          = ctx->target->data[0];
            stride                       = ctx->target->linesize[0];
            width                        = media.width * sizeof(uint32_t);
            for (y = 0; y < media.height; y++, src += stride, data += width/4)
               memcpy(data, src, width);

            glBindTexture(GL_TEXTURE_2D, frames[1].tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                  media.width, media.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, video_frame_temp_buffer);
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

/*
 * Try to initialize a specific HW decoder defined by type.
 * Optionaly tests the pixel format list for a compatible pixel format.
 */
static enum AVPixelFormat init_hw_decoder(struct AVCodecContext *ctx,
                                    const enum AVHWDeviceType type,
                                    const enum AVPixelFormat *pix_fmts)
{
   int ret = 0;
   enum AVPixelFormat decoder_pix_fmt = AV_PIX_FMT_NONE;
   const AVCodec *codec = avcodec_find_decoder(fctx->streams[video_stream_index]->codecpar->codec_id);

   for (int i = 0;; i++)
   {
      const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
      if (!config)
      {
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Codec %s is not supported by HW video decoder %s.\n",
                  codec->name, av_hwdevice_get_type_name(type));
         break;
      }
      if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
         config->device_type == type)
      {
         enum AVPixelFormat device_pix_fmt = config->pix_fmt;

         log_cb(RETRO_LOG_INFO, "[APLAYER] Selected HW decoder %s.\n",
                  av_hwdevice_get_type_name(type));
         log_cb(RETRO_LOG_INFO, "[APLAYER] Selected HW pixel format %s.\n",
                  av_get_pix_fmt_name(device_pix_fmt));

         if (pix_fmts != NULL)
         {
            /* Look if codec can supports the pix format of the device */
            for (size_t i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++)
               if (pix_fmts[i] == device_pix_fmt)
               {
                  decoder_pix_fmt = pix_fmts[i];
                  goto exit;
               }
            log_cb(RETRO_LOG_ERROR, "[APLAYER] Codec %s does not support device pixel format %s.\n",
                  codec->name, av_get_pix_fmt_name(device_pix_fmt));
         }
         else
         {
            decoder_pix_fmt = device_pix_fmt;
            goto exit;
         }
      }
   }

exit:
   if (decoder_pix_fmt != AV_PIX_FMT_NONE)
   {
      AVBufferRef *hw_device_ctx;
      if ((ret = av_hwdevice_ctx_create(&hw_device_ctx,
                                       type, NULL, NULL, 0)) < 0)
      {
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to create specified HW device: %s\n", av_err2str(ret));
         decoder_pix_fmt = AV_PIX_FMT_NONE;
      }
      else
         ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
   }

   return decoder_pix_fmt;
}

/* Automatically try to find a suitable HW decoder */
static enum AVPixelFormat auto_hw_decoder(AVCodecContext *ctx,
                                    const enum AVPixelFormat *pix_fmts)
{
   enum AVPixelFormat decoder_pix_fmt = AV_PIX_FMT_NONE;
   enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

   while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
   {
      decoder_pix_fmt = init_hw_decoder(ctx, type, pix_fmts);
      if (decoder_pix_fmt != AV_PIX_FMT_NONE)
         break;
   }

   return decoder_pix_fmt;
}

static enum AVPixelFormat select_decoder(AVCodecContext *ctx,
                                    const enum AVPixelFormat *pix_fmts)
{
   enum AVPixelFormat format = AV_PIX_FMT_NONE;

   if (!force_sw_decoder)
   {
      if (hw_decoder == AV_HWDEVICE_TYPE_NONE)
         format              = auto_hw_decoder(ctx, pix_fmts);
      else
         format              = init_hw_decoder(ctx, hw_decoder, pix_fmts);
   }

   /* Fallback to SW rendering */
   if (format == AV_PIX_FMT_NONE)
   {
      log_cb(RETRO_LOG_INFO, "[APLAYER] Using SW decoding.\n");

      ctx->thread_type       = FF_THREAD_FRAME;
      ctx->thread_count      = sw_decoder_threads;
      log_cb(RETRO_LOG_INFO, "[APLAYER] Configured software decoding threads: %d\n", sw_decoder_threads);
      format                 = (enum AVPixelFormat)fctx->streams[video_stream_index]->codecpar->format;
      hw_decoding_enabled    = false;
   }
   else
      hw_decoding_enabled    = true;

   return format;
}

/* Callback used by ffmpeg to configure the pixelformat to use. */
static enum AVPixelFormat get_format(AVCodecContext *ctx,
                                     const enum AVPixelFormat *pix_fmts)
{
   /* Look if we can reuse the current decoder */
   for (size_t i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++)
   {
      if (pix_fmts[i] == pix_fmt)
         return pix_fmt;
   }

   pix_fmt = select_decoder(ctx, pix_fmts);

   return pix_fmt;
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
      vctx->get_format  = get_format;
      pix_fmt = select_decoder((*ctx), NULL);
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
            if (subtitle_streams_num < MAX_STREAMS
                  && codec_id_is_ass(fctx->streams[i]->codecpar->codec_id))
            {
               int size;
               AVCodecContext **s = &sctx[subtitle_streams_num];

               subtitle_streams[subtitle_streams_num] = i;
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
   if (actx[0])
      media.sample_rate = actx[0]->sample_rate;

   media.interpolate_fps = 60.0;

   if (vctx)
   {
      media.width  = vctx->width;
      media.height = vctx->height;
      //media.aspect = (float)vctx->width *
      //   av_q2d(vctx->sample_aspect_ratio) / vctx->height;
      init_aspect_ratio();
   }

   if (fctx)
   {
      if (fctx->duration != AV_NOPTS_VALUE)
      {
         int64_t duration        = fctx->duration + (fctx->duration <= INT64_MAX - 5000 ? 5000 : 0);
         media.duration.time     = (double)(duration / AV_TIME_BASE);
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
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Could not determine media duration\n");
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

      for (i = 0; i < (unsigned)subtitle_streams_num; i++)
      {
         ass_track[i] = ass_new_track(ass);
         ass_process_codec_private(ass_track[i], (char*)ass_extra_data[i],
               ass_extra_data_size[i]);
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

static void sws_worker_thread(void *arg)
{
   int ret = 0;
   AVFrame *tmp_frame = NULL;
   video_decoder_context_t *ctx = (video_decoder_context_t*) arg;

   if (hw_decoding_enabled)
      tmp_frame = ctx->hw_source;
   else
      tmp_frame = ctx->source;

   ctx->sws = sws_getCachedContext(ctx->sws,
         media.width, media.height, (enum AVPixelFormat)tmp_frame->format,
         media.width, media.height, AV_PIX_FMT_RGB32,
         SWS_POINT, NULL, NULL, NULL);

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

   double video_time = ctx->pts * av_q2d(fctx->streams[video_stream_index]->time_base);
   slock_lock(ass_lock);
   if (ass_render && ctx->ass_track_active)
   {
      int change     = 0;
      ASS_Image *img = ass_render_frame(ass_render, ctx->ass_track_active,
            1000 * video_time, &change);
      render_ass_img(ctx->target, img);
   }
   slock_unlock(ass_lock);

   av_frame_unref(ctx->source);
   av_frame_unref(ctx->hw_source);
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
      while (true)
      {
         AVFrame *tmpframe = av_frame_alloc();
         ret = avcodec_receive_frame(ctx, tmpframe);
         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
         {
            av_frame_free(&tmpframe);
            break;
         }
         else if (ret < 0)
         {
            /* Real error. */
            log_cb(RETRO_LOG_ERROR,
               "[APLAYER] Error draining frames: %s\n", av_err2str(ret));
            av_frame_free(&tmpframe);
            return;
         }

         /* We got a frame; if needed we can queue it, or just discard. */
         av_frame_free(&tmpframe);
      }
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

      required_buffer = frame->nb_samples * sizeof(int16_t) * 2;
      if (required_buffer > *buffer_cap)
      {
         buffer      = (int16_t*)av_realloc(buffer, required_buffer);
         *buffer_cap = required_buffer;
      }

      swr_convert(swr,
            (uint8_t**)&buffer,
            frame->nb_samples,
            (const uint8_t**)frame->data,
            frame->nb_samples);

      pts = frame->best_effort_timestamp;
      slock_lock(fifo_lock);

      while (!decode_thread_dead &&
            FIFO_WRITE_AVAIL(audio_decode_fifo) < required_buffer)
      {
         if (!main_sleeping)
            scond_wait(fifo_decode_cond, fifo_lock);
         else
         {
            log_cb(RETRO_LOG_ERROR, "[APLAYER] Thread: Audio deadlock detected.\n");
            fifo_clear(audio_decode_fifo);
            break;
         }
      }

      decode_last_audio_time = pts * av_q2d(
            fctx->streams[audio_streams[audio_streams_ptr]]->time_base);

      if (!decode_thread_dead)
         fifo_write(audio_decode_fifo, buffer, required_buffer);

      scond_signal(fifo_cond);
      slock_unlock(fifo_lock);
   }

   return buffer;
}


static void decode_thread_seek(double time)
{
   int64_t seek_to = time * AV_TIME_BASE;

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
   if (sctx[subtitle_streams_ptr])
      avcodec_flush_buffers(sctx[subtitle_streams_ptr]);
   if (ass_track[subtitle_streams_ptr])
      ass_flush_events(ass_track[subtitle_streams_ptr]);
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

      uint64_t in_layout  = actx[i]->channel_layout ?
                      actx[i]->channel_layout :
                      av_get_default_channel_layout(actx[i]->channels);
      uint64_t out_layout = AV_CH_LAYOUT_STEREO;

      av_opt_set_int(swr[i], "in_channel_layout",  in_layout,  0);
      av_opt_set_int(swr[i], "out_channel_layout", out_layout, 0);
      av_opt_set_int(swr[i], "in_sample_rate",     actx[i]->sample_rate, 0);
      av_opt_set_int(swr[i], "out_sample_rate",    media.sample_rate,    0);
      av_opt_set_sample_fmt(swr[i], "in_sample_fmt",  actx[i]->sample_fmt, 0);
      av_opt_set_sample_fmt(swr[i], "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);

      /* matriz solo si hay 6 canales */
      if (av_get_channel_layout_nb_channels(in_layout) == 6) {
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
         continue;
     }
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

   while (!decode_thread_dead)
   {
      // If paused, check again.
      if (paused) {
         usleep(10 * 1000);
         continue;
      }

      bool seek;
      int subtitle_stream;
      double seek_time_thread;
      int audio_stream_index, audio_stream_ptr;

      double audio_timebase   = 0.0;
      double video_timebase   = 0.0;
      double next_video_end   = 0.0;
      double next_audio_start = 0.0;

      AVPacket *pkt_local = av_packet_alloc();  // <--- local AVPacket pointer
      if (!pkt_local)
      {
         // handle error or break
         break;
      }
      AVCodecContext *actx_active = NULL;
      AVCodecContext *sctx_active = NULL;
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
      subtitle_stream             = subtitle_streams[subtitle_streams_ptr];
      actx_active                 = actx[audio_streams_ptr];
      sctx_active                 = sctx[subtitle_streams_ptr];
      ass_track_active            = ass_track[subtitle_streams_ptr];
      audio_timebase = av_q2d(fctx->streams[audio_stream_index]->time_base);
      if (video_stream_index >= 0)
         video_timebase = av_q2d(fctx->streams[video_stream_index]->time_base);
      slock_unlock(decode_thread_lock);

      if (!packet_buffer_empty(audio_packet_buffer))
         next_audio_start = audio_timebase * packet_buffer_peek_start_pts(audio_packet_buffer);

      if (!packet_buffer_empty(video_packet_buffer))
         next_video_end = video_timebase * packet_buffer_peek_end_pts(video_packet_buffer);

      /*
       * Decode audio packet if:
       *  1. it's the start of file or it's audio only media
       *  2. there is a video packet for in the buffer
       *  3. EOF
       **/
      if (!packet_buffer_empty(audio_packet_buffer) &&
            (
               next_video_end == 0.0 ||
               (!eof && earlier_or_close_enough(next_audio_start, next_video_end)) ||
               eof
            )
         )
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

      if (packet_buffer_empty(audio_packet_buffer) &&
         packet_buffer_empty(video_packet_buffer) && eof)
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
                     av_packet_free(&pkt_local);
                     break_out_loop = true;  // break out to allow media reload
                  }
                  else
                  {
                     // Last track in playlist: end playback.
                     av_packet_free(&pkt_local);
                     break_out_loop = true;
                  }
               }
               else
               {
                  // Single file: do not loop, finish playback.
                  av_packet_free(&pkt_local);
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
               av_packet_free(&pkt_local);
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
                  av_packet_free(&pkt_local);
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
                  av_packet_free(&pkt_local);
                  continue;
               }
               break;

            case SHUFFLE_ALL:
               if (playlist_count > 0)
               {
                  // In a playlist mode, pick a random track different from the current one (if possible).
                  unsigned old_index = playlist_index;
                  slock_lock(fifo_lock);
                  do {
                     playlist_index = rand() % playlist_count;
                  } while (playlist_count > 1 && playlist_index == old_index);
                  do_seek = true;
                  seek_time = 0.0;
                  eof = false;
                  slock_unlock(fifo_lock);
                  log_cb(RETRO_LOG_INFO, "[APLAYER] Shuffling playlist, new track #%u/%u: %s\n",
                        playlist_index + 1, playlist_count, playlist[playlist_index]);
                  av_packet_free(&pkt_local);
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
                  av_packet_free(&pkt_local);
                  continue;
               }
               break;

            default:
               // Fallback: treat as PLAY_TRACK (i.e. finish).
               av_packet_free(&pkt_local);
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
         else if (pkt_local->stream_index == subtitle_stream && sctx_active)
         {
            /**
             * Decode subtitle packets right away, since SSA/ASS can operate this way.
             * If we ever support other subtitles, we need to handle this with a
             * buffer too
             **/
            AVSubtitle sub;
            int finished = 0;

            memset(&sub, 0, sizeof(sub));

            while (!finished)
            {
               if (avcodec_decode_subtitle2(sctx_active, &sub, &finished, pkt_local) < 0)
               {
                  log_cb(RETRO_LOG_ERROR, "[APLAYER] Decode subtitles failed.\n");
                  break;
               }
            }
            for (i = 0; i < sub.num_rects; i++)
            {
               slock_lock(ass_lock);
               if (sub.rects[i]->ass && ass_track_active)
                  ass_process_data(ass_track_active,
                        sub.rects[i]->ass, strlen(sub.rects[i]->ass));
               slock_unlock(ass_lock);
            }
            avsubtitle_free(&sub);
            av_packet_unref(pkt_local);
         }
      }
      av_packet_free(&pkt_local);
   }

   for (i = 0; (int)i < audio_streams_num; i++)
      swr_free(&swr[i]);

   if (vctx && vctx->hw_device_ctx)
      av_buffer_unref(&vctx->hw_device_ctx);

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

   av_freep(&video_frame_temp_buffer);
}

bool retro_load_game(const struct retro_game_info *info)
{
   if (!info)
      return false;

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

   video_frame_temp_buffer = (uint32_t*)
      av_malloc(media.width * media.height * sizeof(uint32_t));

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
