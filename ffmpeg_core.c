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
#include <libavutil/pixdesc.h>
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
#ifndef _WIN32
#include <dlfcn.h>
#endif

typedef struct AVFilter AVFilter;
typedef struct AVFilterContext AVFilterContext;
typedef struct AVFilterGraph AVFilterGraph;

#ifndef AV_BUFFERSRC_FLAG_KEEP_REF
#define AV_BUFFERSRC_FLAG_KEEP_REF 8
#endif

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
#define APLAYER_MAX_PORTS 4
#define APLAYER_TARGET_REFRESH_RETRY_FRAMES 1
#define APLAYER_BOOKMARK_MAGIC 0x4150424dU
#define APLAYER_BOOKMARK_VERSION 1U
#define APLAYER_BOOKMARK_MIN_TIME 1.0
#define APLAYER_VIDEO_ZOOM_MIN 0.75f
#define APLAYER_VIDEO_ZOOM_MAX 1.35f
#define APLAYER_AUDIO_PACKET_BUFFER_LIMIT 128
#define APLAYER_AUDIO_SWITCH_PREROLL_SECONDS 0.15

enum aplayer_deinterlace_mode
{
   APLAYER_DEINTERLACE_DISABLED = 0,
   APLAYER_DEINTERLACE_AUTO,
   APLAYER_DEINTERLACE_FORCED
};

struct aplayer_avfilter_api
{
   bool load_attempted;
   bool available;
   void *handle;
   const AVFilter *(*avfilter_get_by_name)(const char *name);
   AVFilterGraph *(*avfilter_graph_alloc)(void);
   int (*avfilter_graph_create_filter)(AVFilterContext **filt_ctx,
         const AVFilter *filt, const char *name, const char *args,
         void *opaque, AVFilterGraph *graph_ctx);
   int (*avfilter_link)(AVFilterContext *src, unsigned srcpad,
         AVFilterContext *dst, unsigned dstpad);
   int (*avfilter_graph_config)(AVFilterGraph *graphctx, void *log_ctx);
   void (*avfilter_graph_free)(AVFilterGraph **graph);
   int (*av_buffersrc_add_frame_flags)(AVFilterContext *buffer_src,
         AVFrame *frame, int flags);
   int (*av_buffersink_get_frame)(AVFilterContext *ctx, AVFrame *frame);
};

struct aplayer_video_filter_state
{
   AVFilterGraph *graph;
   AVFilterContext *buffer_src_ctx;
   AVFilterContext *yadif_ctx;
   AVFilterContext *buffer_sink_ctx;
   unsigned width;
   unsigned height;
   enum AVPixelFormat pix_fmt;
   AVRational time_base;
   AVRational sample_aspect_ratio;
   enum aplayer_deinterlace_mode mode;
   bool use_yadif;
   bool enabled_logged;
   bool unavailable_logged;
};

static char playlist[MAX_PLAYLIST_ENTRIES][PATH_MAX];
static unsigned playlist_count = 0;
static unsigned playlist_index = 0;
static char current_content_path[PATH_MAX];
static char current_media_path[PATH_MAX];
static char playlist_source_path[PATH_MAX];

static bool reset_triggered;
static bool libretro_supports_bitmasks = false;
static unsigned input_ports = 1;
static unsigned controller_port_devices[APLAYER_MAX_PORTS];
static bool auto_resume_enabled = false;
static bool content_loaded = false;
static bool playlist_source_active = false;
static bool internal_playlist_reload_pending = false;
struct aplayer_bookmark
{
   uint32_t magic;
   uint32_t version;
   double playback_time;
   uint32_t playlist_index;
   int32_t audio_stream_ptr;
   int32_t subtitle_stream_ptr;
   char media_path[PATH_MAX];
};

struct aplayer_input_binding
{
   unsigned id;
   const char *description;
};

static const struct aplayer_input_binding aplayer_input_bindings[] =
{
   { RETRO_DEVICE_ID_JOYPAD_START, "Play/Pause" },
   { RETRO_DEVICE_ID_JOYPAD_A,     "Display Time" },
   { RETRO_DEVICE_ID_JOYPAD_B,     "Display Title" },
   { RETRO_DEVICE_ID_JOYPAD_X,     "Toggle Subtitles" },
   { RETRO_DEVICE_ID_JOYPAD_Y,     "Cycle Audio Track" },
   { RETRO_DEVICE_ID_JOYPAD_L,     "Previous (M3U)" },
   { RETRO_DEVICE_ID_JOYPAD_R,     "Next (M3U)" },
   { RETRO_DEVICE_ID_JOYPAD_LEFT,  "Seek -15 sec" },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, "Seek +15 sec" },
   { RETRO_DEVICE_ID_JOYPAD_DOWN,  "Seek -3 min" },
   { RETRO_DEVICE_ID_JOYPAD_UP,    "Seek +3 min" },
   { RETRO_DEVICE_ID_JOYPAD_L2,    "Seek -5 min" },
   { RETRO_DEVICE_ID_JOYPAD_R2,    "Seek +5 min" },
};

static struct retro_input_descriptor
   aplayer_input_descriptors[(APLAYER_MAX_PORTS * ARRAY_SIZE(aplayer_input_bindings)) + 1];

static const struct retro_controller_description aplayer_controller_types[] =
{
   { "RetroPad", RETRO_DEVICE_JOYPAD },
};

static struct retro_controller_info aplayer_controller_info[APLAYER_MAX_PORTS + 1];

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static bool aplayer_device_is_joypad(unsigned device)
{
   return (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD;
}

static void aplayer_reset_controller_ports(void)
{
   unsigned i;

   input_ports = 1;

   for (i = 0; i < APLAYER_MAX_PORTS; i++)
      controller_port_devices[i] = RETRO_DEVICE_JOYPAD;
}

static unsigned aplayer_get_input_ports(void)
{
   unsigned max_users = 0;

   if (environ_cb &&
       environ_cb(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS, &max_users) &&
       max_users > 0)
   {
      if (max_users > APLAYER_MAX_PORTS)
         max_users = APLAYER_MAX_PORTS;
      return max_users;
   }

   return 1;
}

static void aplayer_set_controller_info(unsigned ports)
{
   unsigned i;

   for (i = 0; i < ports; i++)
   {
      aplayer_controller_info[i].types     = aplayer_controller_types;
      aplayer_controller_info[i].num_types = ARRAY_SIZE(aplayer_controller_types);
   }

   aplayer_controller_info[ports].types     = NULL;
   aplayer_controller_info[ports].num_types = 0;

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, aplayer_controller_info);
}

static void aplayer_set_input_descriptors(unsigned ports)
{
   unsigned port;
   size_t desc = 0;

   for (port = 0; port < ports; port++)
   {
      size_t i;

      for (i = 0; i < ARRAY_SIZE(aplayer_input_bindings); i++)
      {
         aplayer_input_descriptors[desc].port        = port;
         aplayer_input_descriptors[desc].device      = RETRO_DEVICE_JOYPAD;
         aplayer_input_descriptors[desc].index       = 0;
         aplayer_input_descriptors[desc].id          = aplayer_input_bindings[i].id;
         aplayer_input_descriptors[desc].description = aplayer_input_bindings[i].description;
         desc++;
      }
   }

   memset(&aplayer_input_descriptors[desc], 0,
         sizeof(aplayer_input_descriptors[desc]));
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, aplayer_input_descriptors);
}

static void aplayer_register_input_layout(void)
{
   input_ports = aplayer_get_input_ports();
   aplayer_set_controller_info(input_ports);
   aplayer_set_input_descriptors(input_ports);
}

static uint16_t aplayer_poll_input_mask(unsigned port)
{
   uint16_t ret = 0;

   if (libretro_supports_bitmasks)
      return (uint16_t)input_state_cb(port, RETRO_DEVICE_JOYPAD,
            0, RETRO_DEVICE_ID_JOYPAD_MASK);

   {
      unsigned i;
      for (i = RETRO_DEVICE_ID_JOYPAD_B; i <= RETRO_DEVICE_ID_JOYPAD_R2; i++)
         if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, i))
            ret |= (1U << i);
   }

   return ret;
}

/* FFmpeg context data. */
static AVFormatContext *fctx;
static AVCodecContext *vctx;
static int video_stream_index;
static int loopcontent;
static bool is_crt = false;
static const double subtitle_font_size = 38.0;
static const double subtitle_outline_size = 1.65;
static const int subtitle_margin_x = 19;
static const int subtitle_margin_y = 34;
static const int subtitle_font_scale_base = 720;
static const int subtitle_text_playres_x = 384;
static const int subtitle_text_playres_y = 288;
static const char *subtitle_font_name = "sans-serif";

static double g_current_time = 0.0;  // PTS in seconds
static slock_t *time_lock    = NULL; // Protects g_current_time

static unsigned sw_decoder_threads;
static unsigned sw_sws_threads;
static video_buffer_t *video_buffer;
static tpool_t *tpool;

#define MAX_STREAMS 8
#define SUBTITLE_STREAM_DISABLED (-1)
#define SUBTITLE_UNKNOWN_DURATION_MS (((INT_MAX / 1000) * 1000))
#define SUBTITLE_FIX_TIMING_THRESHOLD_MS 210
#define BITMAP_SUBTITLE_MAX_EVENTS 64
#define BITMAP_SUBTITLE_TRAILING_LIMIT_MS (60 * 1000)
#define APLAYER_AUDIO_LANGUAGE_DEFAULT "default"
static AVCodecContext *actx[MAX_STREAMS];
static AVCodecContext *sctx[MAX_STREAMS];
static int audio_streams[MAX_STREAMS];
static int audio_streams_num;
static int audio_streams_ptr;
static int subtitle_streams[MAX_STREAMS];
static int subtitle_streams_num;
static int subtitle_streams_ptr;
static bool subtitle_is_ass[MAX_STREAMS];
static bool subtitle_is_bitmap[MAX_STREAMS];
static bool subtitle_is_external[MAX_STREAMS];
static bool subtitle_uses_native_text_header[MAX_STREAMS];

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

struct bitmap_subtitle_rect
{
   int x;
   int y;
   int w;
   int h;
   uint32_t *pixels;
};

struct bitmap_subtitle_event
{
   int64_t start_ms;
   int64_t end_ms;
   bool end_known;
   int canvas_w;
   int canvas_h;
   unsigned rect_count;
   struct bitmap_subtitle_rect *rects;
};

static struct bitmap_subtitle_event *bitmap_subtitle_events[MAX_STREAMS];
static size_t bitmap_subtitle_event_count[MAX_STREAMS];
static size_t bitmap_subtitle_event_cap[MAX_STREAMS];

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
static float video_blend_strength = 1.0f;
static float video_zoom = 1.0f;
static float video_fill_target_aspect = 0.0f;
static unsigned video_crop_left = 0;
static unsigned video_crop_top = 0;
static unsigned video_crop_right = 0;
static unsigned video_crop_bottom = 0;
static bool target_refresh_retry_active = false;
static unsigned target_refresh_retry_frames = 0;
static char preferred_audio_language[16] = APLAYER_AUDIO_LANGUAGE_DEFAULT;
static enum aplayer_deinterlace_mode video_deinterlace_mode = APLAYER_DEINTERLACE_AUTO;
static volatile bool video_filter_reset_pending = false;
static volatile bool playback_restart_request = false;
static volatile bool playback_restart_pending = false;
static struct aplayer_avfilter_api avfilter_api = {0};
static struct aplayer_video_filter_state video_filter = {0};

static const char *const spanish_latam_language_tags[] =
{
   "es-419", "es-mx", "es-ar", "es-cl", "es-co", "es-pe", "es-ve", "es-uy",
   "es-py", "es-bo", "es-ec", "es-gt", "es-hn", "es-ni", "es-sv", "es-cr",
   "es-pa", "es-do", "es-pr", "es-cu", "es-us", NULL
};

static const char *const portuguese_brazil_language_tags[] =
{
   "pt-br", "pob", NULL
};

static const char *const chinese_simplified_language_tags[] =
{
   "zh-hans", "zh-cn", "zh-sg", "cmn-hans", "cmn-cn", "cmn-sg", NULL
};

static const char *const chinese_traditional_language_tags[] =
{
   "zh-hant", "zh-tw", "zh-hk", "zh-mo", "cmn-hant", "cmn-tw", NULL
};

static const char *const cantonese_language_tags[] =
{
   "yue", "zh-yue", NULL
};

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
   media.interpolate_fps = 60.0;   // fallback until target refresh is known
   media.sample_rate     = 32000.0; // safe default until audio stream is opened
   media.aspect          = 0.0f;    // force recompute
   video_fill_target_aspect = 0.0f;
   video_crop_left       = 0;
   video_crop_top        = 0;
   video_crop_right      = 0;
   video_crop_bottom     = 0;
   seek_supported        = true;
   time_supported        = true;
   gme_seek_disabled     = false;
   paused                = false;
   do_seek               = false;
   seek_time             = 0.0;
   audio_switch_requested = false;
   g_current_time        = 0.0;
   frames[0].pts         = 0.0;
   frames[1].pts         = 0.0;
   frames[0].valid       = false;
   frames[1].valid       = false;
   target_refresh_retry_active = false;
   target_refresh_retry_frames = 0;
   video_filter_reset_pending = true;
   playback_restart_request = false;
   playback_restart_pending = false;
   video_filter.enabled_logged = false;
   video_filter.unavailable_logged = false;
}

static void aplayer_reset_playback_timing_to_start(void)
{
   frame_cnt = 0;
   audio_frames = 0;
   pts_bias = 0.0;
   frames[0].pts = 0.0;
   frames[1].pts = 0.0;
   frames[0].valid = false;
   frames[1].valid = false;

   if (time_lock)
      slock_lock(time_lock);
   g_current_time = 0.0;
   if (time_lock)
      slock_unlock(time_lock);
}

static bool aplayer_consume_playback_restart_pending(void)
{
   if (!playback_restart_pending)
      return false;

   playback_restart_pending = false;
   aplayer_reset_playback_timing_to_start();
   return true;
}

static void sws_worker_thread(void *arg);

static const char *video_deinterlace_mode_name(enum aplayer_deinterlace_mode mode)
{
   switch (mode)
   {
      case APLAYER_DEINTERLACE_DISABLED:
         return "Off";
      case APLAYER_DEINTERLACE_FORCED:
         return "YADIF Always";
      case APLAYER_DEINTERLACE_AUTO:
      default:
         return "YADIF Auto";
   }
}

static const char *video_deinterlace_filter_args(enum aplayer_deinterlace_mode mode)
{
   if (mode == APLAYER_DEINTERLACE_FORCED)
      return "mode=send_frame:parity=auto:deint=all";

   return "mode=send_frame:parity=auto:deint=interlaced";
}

static bool rationals_differ(AVRational lhs, AVRational rhs)
{
   return lhs.num != rhs.num || lhs.den != rhs.den;
}

static AVRational video_deinterlace_frame_sar(const AVFrame *frame)
{
   AVRational sar = {1, 1};

   if (frame)
      sar = frame->sample_aspect_ratio;

   if ((sar.num <= 0 || sar.den <= 0) && fctx && video_stream_index >= 0)
      sar = av_guess_sample_aspect_ratio(fctx, fctx->streams[video_stream_index],
            (AVFrame*)frame);

   if (sar.num <= 0 || sar.den <= 0)
      sar = (AVRational){1, 1};

   return sar;
}

static bool video_frame_is_interlaced(const AVFrame *frame)
{
   return frame && (frame->flags & AV_FRAME_FLAG_INTERLACED);
}

static bool video_stream_is_interlaced(void)
{
   return vctx &&
         vctx->field_order != AV_FIELD_PROGRESSIVE &&
         vctx->field_order != AV_FIELD_UNKNOWN;
}

static bool playback_rates_differ(double lhs, double rhs)
{
   return lhs < rhs - 0.001 || lhs > rhs + 0.001;
}

static bool video_filter_should_use_yadif(const AVFrame *frame)
{
   if (video_deinterlace_mode == APLAYER_DEINTERLACE_DISABLED)
      return false;

   if (video_deinterlace_mode == APLAYER_DEINTERLACE_FORCED)
      return true;

   if (video_filter.graph && video_filter.use_yadif)
      return true;

   return video_frame_is_interlaced(frame) || video_stream_is_interlaced();
}

static bool video_filter_should_process(const AVFrame *frame)
{
   return video_filter_should_use_yadif(frame);
}

static void video_filter_build_pipeline_label(bool use_yadif,
      char *label, size_t label_size)
{
   if (!label || label_size == 0)
      return;

   if (use_yadif)
      snprintf(label, label_size, "%s",
            video_deinterlace_mode_name(video_deinterlace_mode));
   else
      snprintf(label, label_size, "Off");
}

static void video_filter_close(void)
{
   if (avfilter_api.available && avfilter_api.avfilter_graph_free &&
         video_filter.graph)
      avfilter_api.avfilter_graph_free(&video_filter.graph);
   else
      video_filter.graph = NULL;

   video_filter.buffer_src_ctx = NULL;
   video_filter.yadif_ctx = NULL;
   video_filter.buffer_sink_ctx = NULL;
   video_filter.width = 0;
   video_filter.height = 0;
   video_filter.pix_fmt = AV_PIX_FMT_NONE;
   video_filter.time_base = (AVRational){0, 1};
   video_filter.sample_aspect_ratio = (AVRational){0, 1};
   video_filter.mode = APLAYER_DEINTERLACE_DISABLED;
   video_filter.use_yadif = false;
}

static bool avfilter_api_load(void)
{
   if (avfilter_api.load_attempted)
      return avfilter_api.available;

   avfilter_api.load_attempted = true;

#ifndef _WIN32
   {
      static const char *const candidates[] =
      {
         "libavfilter.so.10",
         "libavfilter.so.9",
         "libavfilter.so.8",
         "libavfilter.so.7",
         "libavfilter.so",
         NULL
      };
      unsigned i;

      for (i = 0; candidates[i]; i++)
      {
         avfilter_api.handle = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
         if (avfilter_api.handle)
            break;
      }

      if (!avfilter_api.handle)
         return false;

#define LOAD_AVFILTER_SYMBOL(name) \
      do { \
         *(void**)(&avfilter_api.name) = dlsym(avfilter_api.handle, #name); \
         if (!avfilter_api.name) \
            goto error; \
      } while (0)

      LOAD_AVFILTER_SYMBOL(avfilter_get_by_name);
      LOAD_AVFILTER_SYMBOL(avfilter_graph_alloc);
      LOAD_AVFILTER_SYMBOL(avfilter_graph_create_filter);
      LOAD_AVFILTER_SYMBOL(avfilter_link);
      LOAD_AVFILTER_SYMBOL(avfilter_graph_config);
      LOAD_AVFILTER_SYMBOL(avfilter_graph_free);
      LOAD_AVFILTER_SYMBOL(av_buffersrc_add_frame_flags);
      LOAD_AVFILTER_SYMBOL(av_buffersink_get_frame);

#undef LOAD_AVFILTER_SYMBOL

      avfilter_api.available = true;
      return true;

error:
      dlclose(avfilter_api.handle);
      avfilter_api.handle = NULL;
   }
#endif

   return false;
}

static void video_submit_frame_to_worker(video_decoder_context_t *ctx,
      ASS_Track *ass_track_active)
{
   if (!ctx)
      return;

   ctx->ass_track_active = ass_track_active;
   tpool_add_work(tpool, sws_worker_thread, ctx);
}

static bool video_filter_drain_to_buffer(ASS_Track *ass_track_active)
{
   bool drained = false;

   if (!video_filter.graph || !video_filter.buffer_sink_ctx)
      return false;

   while (!decode_thread_dead && video_buffer_has_open_slot(video_buffer))
   {
      int ret;
      video_decoder_context_t *ctx = NULL;

      video_buffer_get_open_slot(video_buffer, &ctx);
      if (!ctx)
         break;

      av_frame_unref(ctx->source);
      av_frame_unref(ctx->filtered);

      ret = avfilter_api.av_buffersink_get_frame(
            video_filter.buffer_sink_ctx, ctx->filtered);
      if (ret == AVERROR(EAGAIN))
      {
         video_buffer_return_open_slot(video_buffer, ctx);
         break;
      }
      if (ret == AVERROR_EOF)
      {
         video_buffer_return_open_slot(video_buffer, ctx);
         break;
      }
      if (ret < 0)
      {
         log_cb(RETRO_LOG_ERROR,
               "[APLAYER] Failed to receive a filtered video frame: %s\n",
               av_err2str(ret));
         av_frame_unref(ctx->filtered);
         video_buffer_return_open_slot(video_buffer, ctx);
         video_filter_close();
         break;
      }

      drained = true;
      video_submit_frame_to_worker(ctx, ass_track_active);
   }

   return drained;
}

static bool video_filter_ensure_graph(const AVFrame *frame)
{
   char buffer_args[256];
   char pipeline_label[128];
   int ret;
   unsigned width;
   unsigned height;
   AVRational time_base;
   AVRational sar;
   bool use_yadif;
   const char *pix_fmt_name = NULL;
   AVFilterContext *current_ctx = NULL;
   const AVFilter *buffer = NULL;
   const AVFilter *yadif = NULL;
   const AVFilter *buffersink = NULL;

   if (!frame)
      return false;

   use_yadif = video_filter_should_use_yadif(frame);

   if (!avfilter_api_load())
   {
      if (!video_filter.unavailable_logged)
      {
         video_filter_build_pipeline_label(use_yadif,
               pipeline_label, sizeof(pipeline_label));
         log_cb(RETRO_LOG_WARN,
               "[APLAYER] %s requested but libavfilter could not be loaded; video will remain untouched.\n",
               pipeline_label);
         video_filter.unavailable_logged = true;
      }
      return false;
   }

   width = frame->width > 0 ? (unsigned)frame->width : media.width;
   height = frame->height > 0 ? (unsigned)frame->height : media.height;
   time_base = (fctx && video_stream_index >= 0) ?
         fctx->streams[video_stream_index]->time_base : (AVRational){1, AV_TIME_BASE};
   if (time_base.num <= 0 || time_base.den <= 0)
      time_base = (AVRational){1, AV_TIME_BASE};
   sar = video_deinterlace_frame_sar(frame);
   pix_fmt_name = av_get_pix_fmt_name((enum AVPixelFormat)frame->format);

   if (video_filter_reset_pending ||
       (video_filter.graph &&
        (video_filter.width != width ||
         video_filter.height != height ||
         video_filter.pix_fmt != (enum AVPixelFormat)frame->format ||
         rationals_differ(video_filter.time_base, time_base) ||
         rationals_differ(video_filter.sample_aspect_ratio, sar) ||
         video_filter.mode != video_deinterlace_mode ||
         video_filter.use_yadif != use_yadif)))
   {
      video_filter_close();
      video_filter_reset_pending = false;
   }

   if (video_filter.graph)
      return true;

   if (!use_yadif)
      return false;

   buffer = avfilter_api.avfilter_get_by_name("buffer");
   yadif = avfilter_api.avfilter_get_by_name("yadif");
   buffersink = avfilter_api.avfilter_get_by_name("buffersink");

   if (!buffer || !yadif || !buffersink)
   {
      if (!video_filter.unavailable_logged)
      {
         video_filter_build_pipeline_label(use_yadif,
               pipeline_label, sizeof(pipeline_label));
         log_cb(RETRO_LOG_WARN,
               "[APLAYER] libavfilter is present but required filters for %s are unavailable.\n",
               pipeline_label);
         video_filter.unavailable_logged = true;
      }
      return false;
   }

   snprintf(buffer_args, sizeof(buffer_args),
         "video_size=%ux%u:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
         width, height, frame->format,
         time_base.num, time_base.den,
         sar.num, sar.den);

   video_filter.graph = avfilter_api.avfilter_graph_alloc();
   if (!video_filter.graph)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to allocate the video filter graph.\n");
      return false;
   }

   ret = avfilter_api.avfilter_graph_create_filter(
         &video_filter.buffer_src_ctx,
         buffer, "aplayer_buffer", buffer_args, NULL,
         video_filter.graph);
   if (ret < 0)
      goto error;

   current_ctx = video_filter.buffer_src_ctx;

   ret = avfilter_api.avfilter_graph_create_filter(
         &video_filter.yadif_ctx,
         yadif, "aplayer_yadif",
         video_deinterlace_filter_args(video_deinterlace_mode), NULL,
         video_filter.graph);
   if (ret < 0)
      goto error;

   ret = avfilter_api.avfilter_link(current_ctx, 0, video_filter.yadif_ctx, 0);
   if (ret < 0)
      goto error;

   current_ctx = video_filter.yadif_ctx;

   ret = avfilter_api.avfilter_graph_create_filter(
         &video_filter.buffer_sink_ctx,
         buffersink, "aplayer_buffersink", NULL, NULL,
         video_filter.graph);
   if (ret < 0)
      goto error;

   ret = avfilter_api.avfilter_link(current_ctx, 0, video_filter.buffer_sink_ctx, 0);
   if (ret < 0)
      goto error;

   ret = avfilter_api.avfilter_graph_config(video_filter.graph, NULL);
   if (ret < 0)
      goto error;

   video_filter.width = width;
   video_filter.height = height;
   video_filter.pix_fmt = (enum AVPixelFormat)frame->format;
   video_filter.time_base = time_base;
   video_filter.sample_aspect_ratio = sar;
   video_filter.mode = video_deinterlace_mode;
   video_filter.use_yadif = use_yadif;
   video_filter.unavailable_logged = false;

   if (!video_filter.enabled_logged)
   {
      video_filter_build_pipeline_label(use_yadif,
            pipeline_label, sizeof(pipeline_label));
      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Video filter graph enabled: %s (%ux%u %s)\n",
            pipeline_label,
            width, height,
            pix_fmt_name ? pix_fmt_name : "unknown");
      video_filter.enabled_logged = true;
   }

   return true;

error:
   log_cb(RETRO_LOG_ERROR,
         "[APLAYER] Failed to initialize the video filter graph: %s\n",
         av_err2str(ret));
   video_filter_close();
   return false;
}

static bool video_filter_queue_frame(video_decoder_context_t *ctx,
      ASS_Track *ass_track_active)
{
   int ret;

   if (!ctx || !ctx->source || !ctx->filtered)
      return false;

   av_frame_unref(ctx->filtered);

   if (video_filter_reset_pending)
   {
      if (video_filter.graph)
         video_filter_close();
      video_filter_reset_pending = false;
   }

   if (!video_filter_should_process(ctx->source))
      return false;

   if (!video_filter_ensure_graph(ctx->source))
      return false;

   ret = avfilter_api.av_buffersrc_add_frame_flags(
         video_filter.buffer_src_ctx,
         ctx->source,
         AV_BUFFERSRC_FLAG_KEEP_REF);
   if (ret < 0)
   {
      log_cb(RETRO_LOG_ERROR,
            "[APLAYER] Failed to queue a frame into the video filter graph: %s\n",
            av_err2str(ret));
      video_filter_close();
      return false;
   }

   av_frame_unref(ctx->source);
   video_buffer_return_open_slot(video_buffer, ctx);
   video_filter_drain_to_buffer(ass_track_active);
   return true;
}

static const char *stream_metadata_value(AVStream *stream, const char *key)
{
   AVDictionaryEntry *tag = NULL;

   if (!stream || string_is_empty(key))
      return NULL;

   tag = av_dict_get(stream->metadata, key, NULL, 0);
   if (tag && !string_is_empty(tag->value))
      return tag->value;

   while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
   {
      if (string_is_equal_case_insensitive(tag->key, key) &&
            !string_is_empty(tag->value))
         return tag->value;
   }

   return NULL;
}

static bool audio_selection_is_valid(int audio_ptr)
{
   return audio_ptr >= 0 && audio_ptr < audio_streams_num;
}

static bool audio_stream_is_default(int audio_ptr)
{
   int stream_index;

   if (!fctx || !audio_selection_is_valid(audio_ptr))
      return false;

   stream_index = audio_streams[audio_ptr];
   if (stream_index < 0 || stream_index >= (int)fctx->nb_streams)
      return false;

   return (fctx->streams[stream_index]->disposition & AV_DISPOSITION_DEFAULT) != 0;
}

static const char *audio_stream_language_tag(int audio_ptr)
{
   int stream_index;

   if (!fctx || !audio_selection_is_valid(audio_ptr))
      return NULL;

   stream_index = audio_streams[audio_ptr];
   if (stream_index < 0 || stream_index >= (int)fctx->nb_streams)
      return NULL;

   return stream_metadata_value(fctx->streams[stream_index], "language");
}

static void audio_language_normalize_tag(const char *input, char *output,
      size_t output_size)
{
   size_t len = 0;
   bool last_was_dash = false;

   if (!output || output_size == 0)
      return;

   output[0] = '\0';

   if (!input)
      return;

   while (*input && len + 1 < output_size)
   {
      unsigned char c = (unsigned char)*input++;

      if (isalnum(c))
      {
         output[len++] = (char)tolower(c);
         last_was_dash = false;
      }
      else if ((c == '-' || c == '_') && len > 0 && !last_was_dash)
      {
         output[len++] = '-';
         last_was_dash = true;
      }
   }

   if (len > 0 && output[len - 1] == '-')
      len--;

   output[len] = '\0';
}

static bool audio_language_tag_matches_prefix(const char *tag, const char *prefix)
{
   size_t prefix_len;

   if (string_is_empty(tag) || string_is_empty(prefix))
      return false;

   if (string_is_equal(tag, prefix))
      return true;

   prefix_len = strlen(prefix);
   return string_starts_with(tag, prefix) && tag[prefix_len] == '-';
}

static int audio_language_base_match_score(const char *tag, const char *iso639_1,
      const char *iso639_2_term, const char *iso639_2_bibl)
{
   int score = 0;

   if (!string_is_empty(iso639_1) && audio_language_tag_matches_prefix(tag, iso639_1))
      score = string_is_equal(tag, iso639_1) ? 120 : 110;

   if (!string_is_empty(iso639_2_term) &&
         audio_language_tag_matches_prefix(tag, iso639_2_term) &&
         score < (string_is_equal(tag, iso639_2_term) ? 120 : 110))
      score = string_is_equal(tag, iso639_2_term) ? 120 : 110;

   if (!string_is_empty(iso639_2_bibl) &&
         audio_language_tag_matches_prefix(tag, iso639_2_bibl) &&
         score < (string_is_equal(tag, iso639_2_bibl) ? 120 : 110))
      score = string_is_equal(tag, iso639_2_bibl) ? 120 : 110;

   return score;
}

static int audio_language_prefix_list_match_score(const char *tag,
      const char *const *prefixes)
{
   unsigned i;

   if (!tag || !prefixes)
      return 0;

   for (i = 0; prefixes[i]; i++)
      if (audio_language_tag_matches_prefix(tag, prefixes[i]))
         return string_is_equal(tag, prefixes[i]) ? 140 : 130;

   return 0;
}

static int audio_language_match_score(const char *preferred_language,
      const char *track_language)
{
   char normalized_tag[32];

   if (string_is_empty(preferred_language) ||
       string_is_equal(preferred_language, APLAYER_AUDIO_LANGUAGE_DEFAULT))
      return 0;

   audio_language_normalize_tag(track_language, normalized_tag, sizeof(normalized_tag));

   if (string_is_empty(normalized_tag) ||
       string_is_equal(normalized_tag, "und") ||
       string_is_equal(normalized_tag, "unk"))
      return 0;

   if (string_is_equal(preferred_language, "en"))
      return audio_language_base_match_score(normalized_tag, "en", "eng", NULL);
   if (string_is_equal(preferred_language, "ja"))
      return audio_language_base_match_score(normalized_tag, "ja", "jpn", NULL);
   if (string_is_equal(preferred_language, "es"))
      return audio_language_base_match_score(normalized_tag, "es", "spa", NULL);
   if (string_is_equal(preferred_language, "es-419"))
      return audio_language_prefix_list_match_score(normalized_tag,
            spanish_latam_language_tags);
   if (string_is_equal(preferred_language, "fr"))
      return audio_language_base_match_score(normalized_tag, "fr", "fra", "fre");
   if (string_is_equal(preferred_language, "de"))
      return audio_language_base_match_score(normalized_tag, "de", "deu", "ger");
   if (string_is_equal(preferred_language, "it"))
      return audio_language_base_match_score(normalized_tag, "it", "ita", NULL);
   if (string_is_equal(preferred_language, "pt"))
      return audio_language_base_match_score(normalized_tag, "pt", "por", NULL);
   if (string_is_equal(preferred_language, "pt-br"))
      return audio_language_prefix_list_match_score(normalized_tag,
            portuguese_brazil_language_tags);
   if (string_is_equal(preferred_language, "nl"))
      return audio_language_base_match_score(normalized_tag, "nl", "nld", "dut");
   if (string_is_equal(preferred_language, "ru"))
      return audio_language_base_match_score(normalized_tag, "ru", "rus", NULL);
   if (string_is_equal(preferred_language, "uk"))
      return audio_language_base_match_score(normalized_tag, "uk", "ukr", NULL);
   if (string_is_equal(preferred_language, "pl"))
      return audio_language_base_match_score(normalized_tag, "pl", "pol", NULL);
   if (string_is_equal(preferred_language, "cs"))
      return audio_language_base_match_score(normalized_tag, "cs", "ces", "cze");
   if (string_is_equal(preferred_language, "hu"))
      return audio_language_base_match_score(normalized_tag, "hu", "hun", NULL);
   if (string_is_equal(preferred_language, "ro"))
      return audio_language_base_match_score(normalized_tag, "ro", "ron", "rum");
   if (string_is_equal(preferred_language, "tr"))
      return audio_language_base_match_score(normalized_tag, "tr", "tur", NULL);
   if (string_is_equal(preferred_language, "ar"))
      return audio_language_base_match_score(normalized_tag, "ar", "ara", NULL);
   if (string_is_equal(preferred_language, "he"))
      return audio_language_base_match_score(normalized_tag, "he", "heb", "iw");
   if (string_is_equal(preferred_language, "hi"))
      return audio_language_base_match_score(normalized_tag, "hi", "hin", NULL);
   if (string_is_equal(preferred_language, "ko"))
      return audio_language_base_match_score(normalized_tag, "ko", "kor", NULL);
   if (string_is_equal(preferred_language, "zh-hans"))
      return audio_language_prefix_list_match_score(normalized_tag,
            chinese_simplified_language_tags);
   if (string_is_equal(preferred_language, "zh-hant"))
      return audio_language_prefix_list_match_score(normalized_tag,
            chinese_traditional_language_tags);
   if (string_is_equal(preferred_language, "yue"))
      return audio_language_prefix_list_match_score(normalized_tag,
            cantonese_language_tags);
   if (string_is_equal(preferred_language, "th"))
      return audio_language_base_match_score(normalized_tag, "th", "tha", NULL);
   if (string_is_equal(preferred_language, "vi"))
      return audio_language_base_match_score(normalized_tag, "vi", "vie", NULL);

   return 0;
}

static int audio_default_stream_ptr(void)
{
   int i;

   if (audio_streams_num <= 0)
      return -1;

   for (i = 0; i < audio_streams_num; i++)
      if (audio_stream_is_default(i))
         return i;

   return 0;
}

static int audio_select_initial_stream_ptr(void)
{
   int best_ptr = -1;
   int best_score = 0;
   int default_ptr = audio_default_stream_ptr();
   int i;

   if (audio_streams_num <= 0)
      return -1;

   if (string_is_equal(preferred_audio_language, APLAYER_AUDIO_LANGUAGE_DEFAULT))
      return default_ptr;

   for (i = 0; i < audio_streams_num; i++)
   {
      int score = audio_language_match_score(preferred_audio_language,
            audio_stream_language_tag(i));

      if (score <= 0)
         continue;

      if (audio_stream_is_default(i))
         score += 10;

      if (score > best_score)
      {
         best_score = score;
         best_ptr = i;
      }
   }

   if (best_ptr >= 0)
      return best_ptr;

   return default_ptr;
}

static void audio_selection_label(int audio_ptr, char *label, size_t label_size)
{
   const char *title = NULL;
   const char *language = NULL;
   int stream_index = -1;

   if (!label || label_size == 0)
      return;

   if (!audio_selection_is_valid(audio_ptr))
   {
      snprintf(label, label_size, "Audio Track");
      return;
   }

   stream_index = audio_streams[audio_ptr];
   if (fctx && stream_index >= 0 && stream_index < (int)fctx->nb_streams)
   {
      AVStream *stream = fctx->streams[stream_index];
      title = stream_metadata_value(stream, "title");
      language = stream_metadata_value(stream, "language");
   }

   if (title && language)
      snprintf(label, label_size, "%s [%s] #%d", title, language, audio_ptr);
   else if (title)
      snprintf(label, label_size, "%s #%d", title, audio_ptr);
   else if (language)
      snprintf(label, label_size, "%s #%d", language, audio_ptr);
   else
      snprintf(label, label_size, "Audio Track #%d", audio_ptr);
}

static void init_aspect_ratio(void)
{
   if (!vctx || video_stream_index < 0)
      return;

   AVStream *video_stream = fctx->streams[video_stream_index];
   AVRational sar = av_guess_sample_aspect_ratio(fctx, video_stream, NULL);
   AVDictionaryEntry *dar_tag = av_dict_get(video_stream->metadata, "DAR", NULL, 0);

   if (dar_tag)
      log_cb(RETRO_LOG_INFO, "[APLAYER] Container DAR: %s\n", dar_tag->value);

   media.aspect = (float)media.width * av_q2d(sar) / (float)media.height;
   if (media.aspect <= 0.0f && media.width > 0 && media.height > 0)
      media.aspect = (float)media.width / (float)media.height;

   log_cb(RETRO_LOG_INFO,
         "[APLAYER] Video aspect ratio: %.3f (SAR guess: %d:%d)\n",
         media.aspect, sar.num, sar.den);
}

static void update_video_presentation_from_frame(const AVFrame *frame)
{
   unsigned frame_width;
   unsigned frame_height;
   unsigned crop_left;
   unsigned crop_top;
   unsigned crop_right;
   unsigned crop_bottom;
   unsigned visible_width;
   unsigned visible_height;
   float aspect;
   AVRational sar = {0, 1};

   if (!frame || !fctx || video_stream_index < 0)
      return;

   frame_width = frame->width > 0 ? (unsigned)frame->width : media.width;
   frame_height = frame->height > 0 ? (unsigned)frame->height : media.height;
   if (frame_width == 0 || frame_height == 0)
      return;

   crop_left = (unsigned)frame->crop_left;
   crop_top = (unsigned)frame->crop_top;
   crop_right = (unsigned)frame->crop_right;
   crop_bottom = (unsigned)frame->crop_bottom;

   if (crop_left + crop_right >= frame_width)
      crop_left = crop_right = 0;
   if (crop_top + crop_bottom >= frame_height)
      crop_top = crop_bottom = 0;

   visible_width = frame_width - crop_left - crop_right;
   visible_height = frame_height - crop_top - crop_bottom;
   if (visible_width == 0 || visible_height == 0)
   {
      crop_left = crop_top = crop_right = crop_bottom = 0;
      visible_width = frame_width;
      visible_height = frame_height;
   }

   sar = av_guess_sample_aspect_ratio(fctx, fctx->streams[video_stream_index],
         (AVFrame*)frame);

   aspect = (float)visible_width * av_q2d(sar) / (float)visible_height;
   if (aspect <= 0.0f)
      aspect = (float)visible_width / (float)visible_height;

   if (crop_left != video_crop_left ||
       crop_top != video_crop_top ||
       crop_right != video_crop_right ||
       crop_bottom != video_crop_bottom)
   {
      video_crop_left = crop_left;
      video_crop_top = crop_top;
      video_crop_right = crop_right;
      video_crop_bottom = crop_bottom;
      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Video presentation crop: %ux%u+%u+%u\n",
            visible_width, visible_height, crop_left, crop_top);
   }

   if (!(media.aspect > aspect - 0.0005f && media.aspect < aspect + 0.0005f))
   {
      media.aspect = aspect;
      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Video aspect ratio updated from frame: %.3f (SAR guess: %d:%d)\n",
            media.aspect, sar.num, sar.den);
   }
}

static float get_video_native_aspect(void)
{
   if (media.aspect > 0.0f)
      return media.aspect;

   if (media.width > 0 && media.height > 0)
      return (float)media.width / (float)media.height;

   return 4.0f / 3.0f;
}

static float get_video_zoom(void);

static bool aspect_values_differ(float lhs, float rhs)
{
   return lhs < rhs - 0.0005f || lhs > rhs + 0.0005f;
}

static float get_display_target_aspect(const struct retro_display_info *display_info)
{
   if (!display_info)
      return 0.0f;

   if ((display_info->flags & RETRO_DISPLAY_INFO_VALID_DISPLAY_AR) &&
       display_info->display_aspect_ratio > 0.0f)
      return display_info->display_aspect_ratio;

   if ((display_info->flags & RETRO_DISPLAY_INFO_VALID_DISPLAY_SIZE) &&
       display_info->display_width > 0 &&
       display_info->display_height > 0)
      return (float)display_info->display_width /
         (float)display_info->display_height;

   if ((display_info->flags & RETRO_DISPLAY_INFO_VALID_VIEWPORT_AR) &&
       display_info->viewport_aspect_ratio > 0.0f)
      return display_info->viewport_aspect_ratio;

   if ((display_info->flags & RETRO_DISPLAY_INFO_VALID_VIEWPORT) &&
       display_info->viewport_width > 0 &&
       display_info->viewport_height > 0)
      return (float)display_info->viewport_width /
         (float)display_info->viewport_height;

   return 0.0f;
}

static void refresh_video_fill_target_aspect(void)
{
   struct retro_display_info display_info;
   float target_aspect = 0.0f;

   if (!environ_cb)
      return;

   memset(&display_info, 0, sizeof(display_info));
   display_info.struct_size = sizeof(display_info);

   if (environ_cb(RETRO_ENVIRONMENT_GET_DISPLAY_INFO, &display_info))
      target_aspect = get_display_target_aspect(&display_info);

   if (target_aspect <= 0.0f)
   {
      if (log_cb && video_fill_target_aspect > 0.0f)
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Automatic zoom fill target unavailable.\n");

      video_fill_target_aspect = 0.0f;
      return;
   }

   if (log_cb && aspect_values_differ(video_fill_target_aspect, target_aspect))
   {
      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Automatic zoom fill target aspect: %.3f\n",
            target_aspect);
   }

   video_fill_target_aspect = target_aspect;
}

static bool use_fill_zoom(void)
{
   return video_fill_target_aspect > 0.0f &&
      get_video_zoom() > 1.0f &&
      aspect_values_differ(video_fill_target_aspect, get_video_native_aspect());
}

static float get_fill_zoom_blend(void)
{
   float zoom = get_video_zoom();

   if (zoom <= 1.0f)
      return 0.0f;

   return (zoom - 1.0f) / (APLAYER_VIDEO_ZOOM_MAX - 1.0f);
}

static float get_fill_zoom_output_aspect(float native_aspect)
{
   float target_aspect = video_fill_target_aspect;
   float blend = get_fill_zoom_blend();

   if (target_aspect <= 0.0f || blend <= 0.0f)
      return native_aspect;

   if (blend > 1.0f)
      blend = 1.0f;

   return native_aspect + (target_aspect - native_aspect) * blend;
}

static float get_video_output_aspect(void)
{
   float aspect = get_video_native_aspect();

   if (use_fill_zoom())
      aspect = get_fill_zoom_output_aspect(aspect);

   return aspect;
}

static float clamp_video_zoom(float zoom)
{
   if (zoom < APLAYER_VIDEO_ZOOM_MIN)
      return APLAYER_VIDEO_ZOOM_MIN;
   if (zoom > APLAYER_VIDEO_ZOOM_MAX)
      return APLAYER_VIDEO_ZOOM_MAX;
   return zoom;
}

static float parse_video_zoom_value(const char *value)
{
   char *end = NULL;
   float zoom;

   if (!value || !*value)
      return 1.0f;

   zoom = strtof(value, &end);
   if (end == value)
      return 1.0f;

   return clamp_video_zoom(zoom);
}

static float get_video_zoom(void)
{
   return clamp_video_zoom(video_zoom);
}

static void get_video_texture_window(float *u_min, float *u_max,
      float *v_min, float *v_max)
{
   float base_u_min = 0.0f;
   float base_u_max = 1.0f;
   float base_v_min = 0.0f;
   float base_v_max = 1.0f;
   float visible_width;
   float visible_height;
   float base_width;
   float base_height;
   float zoom = get_video_zoom();
   if (media.width > 0)
   {
      unsigned crop_left = video_crop_left;
      unsigned crop_right = video_crop_right;

      if (crop_left + crop_right >= media.width)
         crop_left = crop_right = 0;

      base_u_min = (float)crop_left / (float)media.width;
      base_u_max = (float)(media.width - crop_right) / (float)media.width;
   }

   if (media.height > 0)
   {
      unsigned crop_top = video_crop_top;
      unsigned crop_bottom = video_crop_bottom;

      if (crop_top + crop_bottom >= media.height)
         crop_top = crop_bottom = 0;

      base_v_min = (float)crop_top / (float)media.height;
      base_v_max = (float)(media.height - crop_bottom) / (float)media.height;
   }

   base_width = base_u_max - base_u_min;
   base_height = base_v_max - base_v_min;
   if (base_width <= 0.0f)
      base_width = 1.0f;
   if (base_height <= 0.0f)
      base_height = 1.0f;

   visible_width = base_width;
   visible_height = base_height;

   if (zoom > 1.0f && use_fill_zoom())
   {
      float native_aspect = get_video_native_aspect();
      float desired_aspect = get_fill_zoom_output_aspect(native_aspect);

      if (desired_aspect < native_aspect)
         visible_width *= desired_aspect / native_aspect;
      else if (desired_aspect > native_aspect)
         visible_height *= native_aspect / desired_aspect;
   }

   if (visible_width < 0.0f)
      visible_width = 0.0f;
   else if (visible_width > base_width)
      visible_width = base_width;

   if (visible_height < 0.0f)
      visible_height = 0.0f;
   else if (visible_height > base_height)
      visible_height = base_height;

   *u_min = base_u_min + (base_width - visible_width) * 0.5f;
   *u_max = *u_min + visible_width;
   *v_min = base_v_min + (base_height - visible_height) * 0.5f;
   *v_max = *v_min + visible_height;
}

static void update_video_quad(void)
{
   GLfloat vertex_data[16];
   float u_min;
   float u_max;
   float v_min;
   float v_max;
   float quad_scale = get_video_zoom();

   if (use_fill_zoom())
      quad_scale = 1.0f;

   get_video_texture_window(&u_min, &u_max, &v_min, &v_max);

   vertex_data[0]  = -quad_scale;
   vertex_data[1]  = -quad_scale;
   vertex_data[2]  = u_min;
   vertex_data[3]  = v_min;
   vertex_data[4]  = quad_scale;
   vertex_data[5]  = -quad_scale;
   vertex_data[6]  = u_max;
   vertex_data[7]  = v_min;
   vertex_data[8]  = -quad_scale;
   vertex_data[9]  = quad_scale;
   vertex_data[10] = u_min;
   vertex_data[11] = v_max;
   vertex_data[12] = quad_scale;
   vertex_data[13] = quad_scale;
   vertex_data[14] = u_max;
   vertex_data[15] = v_max;

   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertex_data), vertex_data);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
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

static bool aplayer_query_target_refresh_rate(double *refresh_rate_out)
{
   float refresh_rate = 0.0f;

   if (environ_cb &&
       environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &refresh_rate) &&
       refresh_rate > 10.0f && refresh_rate < 1000.0f)
   {
      if (refresh_rate_out)
         *refresh_rate_out = refresh_rate;
      return true;
   }

   return false;
}

static double aplayer_get_target_refresh_rate(bool *used_fallback)
{
   double refresh_rate = 60.0;
   bool valid = aplayer_query_target_refresh_rate(&refresh_rate);

   if (used_fallback)
      *used_fallback = !valid;

   return valid ? refresh_rate : 60.0;
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

static const char *aplayer_filename_from_path(const char *path)
{
   const char *filename_unix = NULL;
   const char *filename_windows = NULL;

   if (!path || !path[0])
      return NULL;

   filename_unix = strrchr(path, '/');
   filename_windows = strrchr(path, '\\');

   if (filename_unix && filename_windows)
      return (filename_unix > filename_windows ? filename_unix : filename_windows) + 1;
   if (filename_unix)
      return filename_unix + 1;
   if (filename_windows)
      return filename_windows + 1;

   return path;
}

static void aplayer_copy_path(char *dst, size_t size, const char *src)
{
   if (!dst || size == 0)
      return;

   if (src)
      snprintf(dst, size, "%s", src);
   else
      dst[0] = '\0';
}

static bool aplayer_path_is_m3u(const char *path)
{
   const char *ext = path ? strrchr(path, '.') : NULL;
   return ext && strcasecmp(ext, ".m3u") == 0;
}

static unsigned long long aplayer_bookmark_hash_path(const char *path)
{
   const unsigned char *ptr = (const unsigned char*)path;
   unsigned long long hash = 1469598103934665603ULL;

   while (ptr && *ptr)
   {
      hash ^= (unsigned long long)(*ptr++);
      hash *= 1099511628211ULL;
   }

   return hash;
}

static bool aplayer_bookmark_get_dir(char *out_dir, size_t out_dir_size)
{
   const char *dir = NULL;

   if (!out_dir || out_dir_size == 0)
      return false;

   out_dir[0] = '\0';

   if (environ_cb &&
       environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) &&
       !string_is_empty(dir))
   {
      aplayer_copy_path(out_dir, out_dir_size, dir);
      return true;
   }

   if (environ_cb &&
       environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) &&
       !string_is_empty(dir))
   {
      aplayer_copy_path(out_dir, out_dir_size, dir);
      return true;
   }

   return false;
}

static bool aplayer_bookmark_build_path(const char *content_path,
      char *out_path, size_t out_path_size)
{
   char base_dir[PATH_MAX];
   char filename[64];
   size_t dir_len;

   if (!out_path || out_path_size == 0 || string_is_empty(content_path))
      return false;

   out_path[0] = '\0';

   if (!aplayer_bookmark_get_dir(base_dir, sizeof(base_dir)))
      return false;

   snprintf(filename, sizeof(filename), "alpha_player_resume_%016llx.dat",
         aplayer_bookmark_hash_path(content_path));

   dir_len = strlen(base_dir);
   snprintf(out_path, out_path_size, "%s%s%s",
         base_dir,
         (dir_len > 0 && base_dir[dir_len - 1] != '/' &&
               base_dir[dir_len - 1] != '\\') ? "/" : "",
         filename);

   return out_path[0] != '\0';
}

static double aplayer_get_current_playback_time(void)
{
   double total_duration = media.duration.time;
   bool total_valid = duration_is_valid(total_duration);
   double pts_time = 0.0;
   double fallback_time = (double)frame_cnt / media.interpolate_fps;
   double current_time;

   if (time_lock)
   {
      slock_lock(time_lock);
      pts_time = g_current_time;
      slock_unlock(time_lock);
   }

   if (audio_streams_num > 0 && video_stream_index < 0 && media.sample_rate > 0)
      fallback_time = (double)audio_frames / media.sample_rate;

   current_time = pts_time;
   if (video_stream_index < 0 ||
         current_time < 0.0 ||
         (total_valid && current_time > total_duration + 5.0))
      current_time = fallback_time;

   if (current_time < 0.0)
      current_time = 0.0;

   return current_time;
}

static void aplayer_show_resume_message(double playback_time)
{
   char msg[64];
   char time_str[16];
   struct retro_message_ext msg_obj = {0};

   format_time_hhmmss(playback_time, time_str, sizeof(time_str));
   snprintf(msg, sizeof(msg), "Resumed from %s", time_str);

   msg_obj.msg      = msg;
   msg_obj.duration = 3000;
   msg_obj.priority = 2;
   msg_obj.level    = RETRO_LOG_INFO;
   msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
   msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
   msg_obj.progress = -1;
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
}

static void aplayer_bookmark_clear_path(const char *content_path)
{
   char bookmark_path[PATH_MAX];

   if (!aplayer_bookmark_build_path(content_path, bookmark_path, sizeof(bookmark_path)))
      return;

   remove(bookmark_path);
}

static void aplayer_bookmark_clear_current(void)
{
   aplayer_bookmark_clear_path(current_content_path);
}

static bool aplayer_bookmark_load(const char *content_path,
      struct aplayer_bookmark *bookmark)
{
   FILE *fp = NULL;
   char bookmark_path[PATH_MAX];
   size_t read_elems = 0;

   if (!bookmark)
      return false;

   memset(bookmark, 0, sizeof(*bookmark));

   if (!aplayer_bookmark_build_path(content_path, bookmark_path, sizeof(bookmark_path)))
      return false;

   fp = fopen(bookmark_path, "rb");
   if (!fp)
      return false;

   read_elems = fread(bookmark, sizeof(*bookmark), 1, fp);
   fclose(fp);

   if (read_elems != 1 ||
       bookmark->magic != APLAYER_BOOKMARK_MAGIC ||
       bookmark->version != APLAYER_BOOKMARK_VERSION ||
       (bookmark->playback_time != 0.0 &&
        !duration_is_valid(bookmark->playback_time)))
   {
      aplayer_bookmark_clear_path(content_path);
      memset(bookmark, 0, sizeof(*bookmark));
      return false;
   }

   return true;
}

static bool aplayer_bookmark_save_current(void)
{
   struct aplayer_bookmark bookmark;
   FILE *fp = NULL;
   char bookmark_path[PATH_MAX];
   double playback_time = 0.0;

   if (!content_loaded || !seek_supported || string_is_empty(current_content_path))
      return false;

   playback_time = aplayer_get_current_playback_time();
   if (!duration_is_valid(playback_time) ||
       playback_time < APLAYER_BOOKMARK_MIN_TIME)
   {
      aplayer_bookmark_clear_current();
      return false;
   }

   if (!aplayer_bookmark_build_path(current_content_path, bookmark_path, sizeof(bookmark_path)))
      return false;

   memset(&bookmark, 0, sizeof(bookmark));
   bookmark.magic              = APLAYER_BOOKMARK_MAGIC;
   bookmark.version            = APLAYER_BOOKMARK_VERSION;
   bookmark.playback_time      = playback_time;
   bookmark.playlist_index     = playlist_index;
   bookmark.audio_stream_ptr   = audio_streams_ptr;
   bookmark.subtitle_stream_ptr = subtitle_streams_ptr;
   aplayer_copy_path(bookmark.media_path, sizeof(bookmark.media_path), current_media_path);

   fp = fopen(bookmark_path, "wb");
   if (!fp)
      return false;

   if (fwrite(&bookmark, sizeof(bookmark), 1, fp) != 1)
   {
      fclose(fp);
      return false;
   }

   fclose(fp);
   return true;
}

static void aplayer_bookmark_apply_stream_selection(
      const struct aplayer_bookmark *bookmark)
{
   if (!bookmark || !decode_thread_lock)
      return;

   slock_lock(decode_thread_lock);
   if (bookmark->audio_stream_ptr >= 0 &&
       bookmark->audio_stream_ptr < audio_streams_num)
      audio_streams_ptr = bookmark->audio_stream_ptr;

   if (bookmark->subtitle_stream_ptr == SUBTITLE_STREAM_DISABLED)
      subtitle_streams_ptr = SUBTITLE_STREAM_DISABLED;
   else if (bookmark->subtitle_stream_ptr >= 0 &&
       bookmark->subtitle_stream_ptr < subtitle_streams_num)
      subtitle_streams_ptr = bookmark->subtitle_stream_ptr;
   slock_unlock(decode_thread_lock);
}

static void aplayer_bookmark_restore_playlist_index(
      const struct aplayer_bookmark *bookmark)
{
   unsigned i;

   if (!bookmark || playlist_count == 0)
      return;

   if (!string_is_empty(bookmark->media_path))
   {
      for (i = 0; i < playlist_count; i++)
      {
         if (string_is_equal(playlist[i], bookmark->media_path))
         {
            playlist_index = i;
            return;
         }
      }
   }

   if (bookmark->playlist_index < playlist_count)
      playlist_index = bookmark->playlist_index;
}

static void display_media_title()
{
   const char *filename = NULL;

   if (!fctx || decode_thread_dead)
      return;

   char msg[256];
   struct retro_message_ext msg_obj = {0};
   msg[0] = '\0';
   filename = aplayer_filename_from_path(current_media_path);

   if (media.title) {
      snprintf(msg, sizeof(msg), "%s", media.title->value);
   } else if (filename) {
      snprintf(msg, sizeof(msg), "%s", filename);
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
   content_loaded = false;
   playlist_source_active = false;
   internal_playlist_reload_pending = false;
   current_content_path[0] = '\0';
   current_media_path[0] = '\0';
   playlist_source_path[0] = '\0';
   aplayer_reset_controller_ports();

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;
}

void retro_deinit(void)
{
   libretro_supports_bitmasks = false;
   video_filter_close();
   video_filter_reset_pending = false;
   playback_restart_request = false;
   playback_restart_pending = false;

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
   if (port < APLAYER_MAX_PORTS)
   {
      controller_port_devices[port] = device;
      if (port + 1 > input_ports)
         input_ports = port + 1;
   }
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Alpha Player";
   info->library_version  = "v2.5.0";
   info->need_fullpath    = true;
   info->valid_extensions = "mkv|avi|f4v|f4f|3gp|ogm|flv|mp4|mp3|flac|ogg|m4a|webm|3g2|mov|wmv|mpg|mpeg|vob|asf|divx|m2p|m2ts|ps|ts|mxf|wma|wav|m3u|s3m|it|xm|mod|ay|gbs|gym|hes|kss|nsf|nsfe|sap|spc|vgm|vgz";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   if (vctx)
      refresh_video_fill_target_aspect();

   unsigned width  = vctx ? media.width : 320;
   unsigned height = vctx ? media.height : 240;
   float aspect    = vctx ? get_video_output_aspect() : (float)width / (float)height;

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
      {"audio", "Audio", "Audio settings"},
      {"music", "Music", "Music Settings"},
      {NULL, NULL, NULL}
   };

   struct retro_core_option_v2_definition option_definitions[] =
   {
      {
         "aplayer_video_blending", "Frame Blending", "Crossfades adjacent frames to smooth motion when content and display refresh do not match.",
         NULL, NULL, "video",
         {
            {"off", "Off"},
            {"low", "Low"},
            {"medium", "Medium"},
            {"high", "High"},
            {"full", "Full"},
            {NULL, NULL}
         }, "off"
      },
      {
         "aplayer_video_zoom", "Zoom", "Scales the displayed video image from 0.75x to 1.35x. Above 1.00x, the image progressively crops toward the current frontend display aspect, falling back to the viewport aspect when display information is incomplete.",
         NULL, NULL, "video",
         {
            {"0.75", "0.75x"},
            {"0.80", "0.80x"},
            {"0.85", "0.85x"},
            {"0.90", "0.90x"},
            {"0.95", "0.95x"},
            {"1.00", "1.00x"},
            {"1.05", "1.05x"},
            {"1.10", "1.10x"},
            {"1.15", "1.15x"},
            {"1.20", "1.20x"},
            {"1.25", "1.25x"},
            {"1.30", "1.30x"},
            {"1.35", "1.35x"},
            {NULL, NULL}
         }, "1.00"
      },
      {
         "aplayer_video_deinterlace", "Deinterlace", "Uses FFmpeg YADIF deinterlacing for interlaced video. Auto only deinterlaces frames marked as interlaced, while YADIF Always forces the filter on every frame.",
         NULL, NULL, "video",
         {
            {"disabled", "Off"},
            {"auto", "Auto"},
            {"forced", "Always"},
            {NULL, NULL}
         }, "auto"
      },
      {
         "aplayer_audio_language", "Preferred Language", "Selects the default audio track language when matching streams are tagged in the media file. Falls back to the file default track, or the first audio track when no default is flagged.",
         NULL, NULL, "audio",
         {
            {"default", "Default"},
            {"en", "English"},
            {"ja", "Japanese"},
            {"es", "Spanish"},
            {"es-419", "Spanish (Latin America)"},
            {"fr", "French"},
            {"de", "German"},
            {"it", "Italian"},
            {"pt", "Portuguese"},
            {"pt-br", "Portuguese (Brazil)"},
            {"nl", "Dutch"},
            {"ru", "Russian"},
            {"uk", "Ukrainian"},
            {"pl", "Polish"},
            {"cs", "Czech"},
            {"hu", "Hungarian"},
            {"ro", "Romanian"},
            {"tr", "Turkish"},
            {"ar", "Arabic"},
            {"he", "Hebrew"},
            {"hi", "Hindi"},
            {"ko", "Korean"},
            {"zh-hans", "Chinese (Simplified)"},
            {"zh-hant", "Chinese (Traditional)"},
            {"yue", "Cantonese"},
            {"th", "Thai"},
            {"vi", "Vietnamese"},
            {NULL, NULL}
         }, "default"
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
         "aplayer_auto_resume", "Auto Resume", NULL, NULL, NULL, NULL,
         {
            {"disabled", "OFF"},
            {"enabled", "ON"},
            {NULL, NULL}
         }, "disabled"
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
   aplayer_bookmark_clear_current();
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

static bool subtitle_track_is_text(unsigned slot)
{
   return slot < MAX_STREAMS &&
         !subtitle_is_ass[slot] &&
         !subtitle_is_bitmap[slot];
}

static bool subtitle_track_is_bitmap(unsigned slot)
{
   return slot < MAX_STREAMS && subtitle_is_bitmap[slot];
}

static void bitmap_subtitle_clear_event(struct bitmap_subtitle_event *event)
{
   unsigned i = 0;

   if (!event)
      return;

   for (i = 0; i < event->rect_count; i++)
      free(event->rects[i].pixels);
   free(event->rects);

   memset(event, 0, sizeof(*event));
}

static void bitmap_subtitle_clear_slot(unsigned slot)
{
   size_t i = 0;

   if (slot >= MAX_STREAMS)
      return;

   for (i = 0; i < bitmap_subtitle_event_count[slot]; i++)
      bitmap_subtitle_clear_event(&bitmap_subtitle_events[slot][i]);

   free(bitmap_subtitle_events[slot]);
   bitmap_subtitle_events[slot] = NULL;
   bitmap_subtitle_event_count[slot] = 0;
   bitmap_subtitle_event_cap[slot] = 0;
}

static void bitmap_subtitle_prune_locked(unsigned slot, int64_t now_ms)
{
   size_t remove_count = 0;

   if (slot >= MAX_STREAMS)
      return;

   while (remove_count < bitmap_subtitle_event_count[slot])
   {
      struct bitmap_subtitle_event *event = &bitmap_subtitle_events[slot][remove_count];
      bool expired = false;

      if (event->end_known)
         expired = event->end_ms < now_ms;
      else
         expired = now_ms >= event->start_ms + BITMAP_SUBTITLE_TRAILING_LIMIT_MS;

      if (!expired)
         break;

      bitmap_subtitle_clear_event(event);
      remove_count++;
   }

   if (remove_count == 0)
      return;

   if (remove_count < bitmap_subtitle_event_count[slot])
   {
      memmove(bitmap_subtitle_events[slot],
            bitmap_subtitle_events[slot] + remove_count,
            (bitmap_subtitle_event_count[slot] - remove_count) *
            sizeof(bitmap_subtitle_events[slot][0]));
   }

   bitmap_subtitle_event_count[slot] -= remove_count;
}

static void bitmap_subtitle_end_previous_locked(unsigned slot, int64_t end_ms)
{
   struct bitmap_subtitle_event *prev = NULL;

   if (slot >= MAX_STREAMS || bitmap_subtitle_event_count[slot] == 0)
      return;

   prev = &bitmap_subtitle_events[slot][bitmap_subtitle_event_count[slot] - 1];
   if (!prev->end_known || prev->end_ms > end_ms)
   {
      prev->end_ms = end_ms;
      prev->end_known = true;
   }
}

static bool bitmap_subtitle_append_locked(unsigned slot,
      const struct bitmap_subtitle_event *event)
{
   struct bitmap_subtitle_event *events = NULL;
   size_t new_cap = 0;

   if (slot >= MAX_STREAMS || !event)
      return false;

   if (bitmap_subtitle_event_count[slot] >= bitmap_subtitle_event_cap[slot])
   {
      new_cap = bitmap_subtitle_event_cap[slot] ? bitmap_subtitle_event_cap[slot] * 2 : 8;
      events = (struct bitmap_subtitle_event*)realloc(bitmap_subtitle_events[slot],
            new_cap * sizeof(*events));
      if (!events)
         return false;

      bitmap_subtitle_events[slot] = events;
      bitmap_subtitle_event_cap[slot] = new_cap;
   }

   bitmap_subtitle_events[slot][bitmap_subtitle_event_count[slot]++] = *event;

   if (bitmap_subtitle_event_count[slot] > BITMAP_SUBTITLE_MAX_EVENTS)
   {
      bitmap_subtitle_clear_event(&bitmap_subtitle_events[slot][0]);
      memmove(bitmap_subtitle_events[slot],
            bitmap_subtitle_events[slot] + 1,
            (bitmap_subtitle_event_count[slot] - 1) *
            sizeof(bitmap_subtitle_events[slot][0]));
      bitmap_subtitle_event_count[slot]--;
   }

   return true;
}

static struct bitmap_subtitle_event *bitmap_subtitle_current_locked(unsigned slot,
      int64_t now_ms)
{
   size_t i = 0;

   if (slot >= MAX_STREAMS)
      return NULL;

   for (i = bitmap_subtitle_event_count[slot]; i > 0; i--)
   {
      struct bitmap_subtitle_event *event = &bitmap_subtitle_events[slot][i - 1];
      bool active = false;

      if (now_ms < event->start_ms)
         continue;

      if (event->end_known)
         active = now_ms < event->end_ms;
      else
         active = now_ms < event->start_ms + BITMAP_SUBTITLE_TRAILING_LIMIT_MS;

      if (active)
         return event;
   }

   return NULL;
}

static double subtitle_scale_for_playres(double value, int play_res_y)
{
   if (play_res_y <= 0)
      play_res_y = subtitle_text_playres_y;

   return value * (double)play_res_y / (double)subtitle_font_scale_base;
}

static int subtitle_scale_int_for_playres(double value, int play_res_y)
{
   double scaled = subtitle_scale_for_playres(value, play_res_y);

   if (scaled <= 0.0)
      return 0;

   return (int)(scaled + 0.5);
}

static void subtitle_set_text_track_playres(ASS_Track *track)
{
   int play_res_y = 0;
   int play_res_x = 0;

   if (!track)
      return;

   play_res_y = track->PlayResY > 0 ? track->PlayResY : subtitle_text_playres_y;
   play_res_x = track->PlayResX;

   if (media.width > 0 && media.height > 0)
   {
      double scaled_x = (double)play_res_y * (double)media.width / (double)media.height;

      if (scaled_x > 0.0)
         play_res_x = (int)(scaled_x + 0.5);
   }

   if (play_res_x <= 0)
      play_res_x = subtitle_text_playres_x;

   track->PlayResX = play_res_x;
   track->PlayResY = play_res_y;
   track->Kerning = true;
}

static void subtitle_apply_mpv_text_style(ASS_Style *style, int play_res_y)
{
   const char *font_name = subtitle_font_name ? subtitle_font_name : "sans-serif";

   if (!style)
      return;

   /* Match mpv's default text subtitle sizing in scaled pixels at 720p. */
   if (style->FontName)
      free(style->FontName);
   style->FontName = strdup(font_name);
   style->Bold = 0;
   style->FontSize = subtitle_scale_for_playres(subtitle_font_size, play_res_y);
   style->Outline = subtitle_scale_for_playres(subtitle_outline_size, play_res_y);
   style->Shadow = 0.0;
   style->MarginL = subtitle_scale_int_for_playres(subtitle_margin_x, play_res_y);
   style->MarginR = subtitle_scale_int_for_playres(subtitle_margin_x, play_res_y);
   style->MarginV = subtitle_scale_int_for_playres(subtitle_margin_y, play_res_y);
}

static void ass_update_text_track_styles(ASS_Track *track)
{
   if (!track)
      return;

   subtitle_set_text_track_playres(track);

   for (int i = 0; i < track->n_styles; i++)
      subtitle_apply_mpv_text_style(&track->styles[i], track->PlayResY);
}

static void ass_update_all_text_track_styles(void)
{
   for (int i = 0; i < subtitle_streams_num; i++)
   {
      if (!subtitle_track_is_text(i))
         continue;
      ass_update_text_track_styles(ass_track[i]);
   }
}

static void update_subtitle_font_settings(void)
{
   if (!ass)
      return;

   if (ass_lock)
      slock_lock(ass_lock);
   ass_update_all_text_track_styles();
   if (ass_lock)
      slock_unlock(ass_lock);
}

static void check_variables(bool firststart)
{
   struct retro_variable loop_content = {0};
   struct retro_variable auto_resume_var = {0};
   struct retro_variable audio_language_var = {0};
   struct retro_variable replay_is_crt = {0};
   struct retro_variable fft_toggle_var = {0};
   struct retro_variable video_blending_var = {0};
   struct retro_variable video_zoom_var = {0};
   struct retro_variable video_deinterlace_var = {0};
   enum aplayer_deinterlace_mode old_deinterlace_mode = video_deinterlace_mode;

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

   subtitle_font_name = "sans-serif";

   video_blend_strength = 1.0f;
   video_blending_var.key = "aplayer_video_blending";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &video_blending_var) &&
         video_blending_var.value)
   {
      if (string_is_equal(video_blending_var.value, "off"))
         video_blend_strength = 0.0f;
      else if (string_is_equal(video_blending_var.value, "low"))
         video_blend_strength = 0.35f;
      else if (string_is_equal(video_blending_var.value, "medium"))
         video_blend_strength = 0.60f;
      else if (string_is_equal(video_blending_var.value, "high"))
         video_blend_strength = 0.80f;
      else
         video_blend_strength = 1.0f;
   }

   video_zoom = 1.0f;
   video_zoom_var.key = "aplayer_video_zoom";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &video_zoom_var) &&
         video_zoom_var.value)
      video_zoom = parse_video_zoom_value(video_zoom_var.value);

   video_deinterlace_mode = APLAYER_DEINTERLACE_AUTO;
   video_deinterlace_var.key = "aplayer_video_deinterlace";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &video_deinterlace_var) &&
         video_deinterlace_var.value)
   {
      if (string_is_equal(video_deinterlace_var.value, "disabled"))
         video_deinterlace_mode = APLAYER_DEINTERLACE_DISABLED;
      else if (string_is_equal(video_deinterlace_var.value, "forced"))
         video_deinterlace_mode = APLAYER_DEINTERLACE_FORCED;
   }

   if (!firststart && old_deinterlace_mode != video_deinterlace_mode)
   {
      if (decode_thread_lock)
         slock_lock(decode_thread_lock);
      video_filter_reset_pending = true;
      video_filter.enabled_logged = false;
      if (decode_thread_lock)
         slock_unlock(decode_thread_lock);

      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Deinterlace updated: %s -> %s\n",
            video_deinterlace_mode_name(old_deinterlace_mode),
            video_deinterlace_mode_name(video_deinterlace_mode));
   }

   snprintf(preferred_audio_language, sizeof(preferred_audio_language), "%s",
         APLAYER_AUDIO_LANGUAGE_DEFAULT);
   audio_language_var.key = "aplayer_audio_language";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &audio_language_var) &&
         !string_is_empty(audio_language_var.value))
      audio_language_normalize_tag(audio_language_var.value,
            preferred_audio_language, sizeof(preferred_audio_language));

   update_subtitle_font_settings();
   auto_resume_enabled = false;
   auto_resume_var.key = "aplayer_auto_resume";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &auto_resume_var) &&
         auto_resume_var.value)
   {
      if (string_is_equal(auto_resume_var.value, "enabled"))
         auto_resume_enabled = true;
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
      unsigned available_cores = cpu_features_get_core_amount();

      sw_decoder_threads = available_cores > 1 ? available_cores - 1 : 1;
      /* Scale the sws threads based on core count but use at least 2 and at most 4 threads */
      sw_sws_threads = MIN(MAX(2, sw_decoder_threads / 2), 4);
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
      log_cb(RETRO_LOG_INFO, "[APLAYER] Resetting PTS.\n");

   frames[0].pts = 0.0;
   frames[1].pts = 0.0;
   frames[0].valid = false;
   frames[1].valid = false;
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
   double current_time = aplayer_get_current_playback_time();

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

static bool subtitle_selection_is_valid(int subtitle_ptr)
{
   return subtitle_ptr >= 0 && subtitle_ptr < subtitle_streams_num;
}

static int subtitle_next_selection(int subtitle_ptr)
{
   if (subtitle_streams_num <= 0)
      return SUBTITLE_STREAM_DISABLED;

   if (!subtitle_selection_is_valid(subtitle_ptr))
      return 0;

   subtitle_ptr++;
   if (subtitle_ptr >= subtitle_streams_num)
      return SUBTITLE_STREAM_DISABLED;

   return subtitle_ptr;
}

static void subtitle_selection_label(int subtitle_ptr, char *label, size_t label_size)
{
   AVDictionaryEntry *tag = NULL;
   const char *title = NULL;
   int stream_index = -1;

   if (!label || label_size == 0)
      return;

   if (!subtitle_selection_is_valid(subtitle_ptr))
   {
      snprintf(label, label_size, "Disabled subtitles");
      return;
   }

   stream_index = subtitle_streams[subtitle_ptr];
   if (fctx && stream_index >= 0 && stream_index < (int)fctx->nb_streams)
   {
      tag = av_dict_get(fctx->streams[stream_index]->metadata, "title", NULL, 0);
      if (tag && !string_is_empty(tag->value))
         title = tag->value;
   }

   if (title)
      snprintf(label, label_size, "%s #%d", title, subtitle_ptr);
   else if (subtitle_is_external[subtitle_ptr])
      snprintf(label, label_size, "External Subtitle #%d", subtitle_ptr);
   else
      snprintf(label, label_size, "Subtitle Track #%d", subtitle_ptr);
}

static bool ass_event_has_layout_overrides(const char *text)
{
   if (!text)
      return false;

   return strstr(text, "\\pos") || strstr(text, "\\move") ||
         strstr(text, "\\clip") || strstr(text, "\\iclip") ||
         strstr(text, "\\org") || strstr(text, "\\p");
}

static long long subtitle_adjust_render_time_ms(ASS_Track *track,
      int subtitle_ptr, long long now_ms)
{
   ASS_Event *overlap[2] = {0};
   int count = 0;

   if (!track || subtitle_ptr < 0 || subtitle_ptr >= MAX_STREAMS ||
         !subtitle_track_is_text((unsigned)subtitle_ptr))
      return now_ms;

   for (int i = 0; i < track->n_events; i++)
   {
      ASS_Event *event = &track->events[i];
      long long start = event->Start;
      long long end = event->Start + event->Duration;

      if (now_ms >= start - SUBTITLE_FIX_TIMING_THRESHOLD_MS &&
            now_ms <= end + SUBTITLE_FIX_TIMING_THRESHOLD_MS)
      {
         if (count >= 2)
            return now_ms;
         overlap[count++] = event;
      }
   }

   if (count != 2)
      return now_ms;

   if (overlap[0]->Style != overlap[1]->Style ||
         ass_event_has_layout_overrides(overlap[0]->Text) ||
         ass_event_has_layout_overrides(overlap[1]->Text))
      return now_ms;

   if (overlap[0]->Start > overlap[1]->Start)
   {
      ASS_Event *tmp = overlap[0];
      overlap[0] = overlap[1];
      overlap[1] = tmp;
   }

   {
      long long first_end = overlap[0]->Start + overlap[0]->Duration;
      long long second_start = overlap[1]->Start;
      long long second_end = overlap[1]->Start + overlap[1]->Duration;

      if (first_end >= second_end)
         return now_ms;

      if (now_ms >= first_end &&
            now_ms < second_start &&
            first_end < second_start &&
            first_end + SUBTITLE_FIX_TIMING_THRESHOLD_MS >= second_start)
         return first_end - 1;

      if (now_ms >= second_start &&
            now_ms <= first_end &&
            first_end > second_start &&
            first_end <= second_start + SUBTITLE_FIX_TIMING_THRESHOLD_MS)
         return first_end;
   }

   return now_ms;
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
   float old_video_aspect       = video_stream_index >= 0 ? get_video_output_aspect() : 0.0f;
   float old_video_zoom         = get_video_zoom();
   bool geometry_changed        = false;
   double old_interpolate_fps   = media.interpolate_fps;
   uint64_t old_frame_cnt       = frame_cnt;
   bool timing_changed          = false;
   bool refresh_updated         = false;
   double target_refresh_rate   = 0.0;

   refresh_video_fill_target_aspect();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);

   if (old_video_zoom < get_video_zoom() - 0.0005f ||
       old_video_zoom > get_video_zoom() + 0.0005f)
   {
      if (use_fill_zoom())
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Zoom updated: %.2f -> %.2f (auto fill %.3f)\n",
               old_video_zoom, get_video_zoom(), video_fill_target_aspect);
      else
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Zoom updated: %.2f -> %.2f\n",
               old_video_zoom, get_video_zoom());
   }

   timing_changed = playback_rates_differ(old_interpolate_fps, media.interpolate_fps);

   if (target_refresh_retry_active && target_refresh_retry_frames > 0)
   {
      if (aplayer_query_target_refresh_rate(&target_refresh_rate) &&
          (target_refresh_rate < old_interpolate_fps - 0.001 ||
           target_refresh_rate > old_interpolate_fps + 0.001))
      {
         double current_time = old_interpolate_fps > 0.0 ?
               (double)frame_cnt / old_interpolate_fps : 0.0;
         media.interpolate_fps = target_refresh_rate;
         frame_cnt = (uint64_t)(current_time * media.interpolate_fps + 0.5);
         refresh_updated = true;
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Target refresh update detected during playback: %.3f -> %.3f Hz\n",
               old_interpolate_fps, media.interpolate_fps);
      }

      target_refresh_retry_frames--;
      if (!refresh_updated && target_refresh_retry_frames == 0)
      {
         target_refresh_retry_active = false;
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Target refresh monitoring finished at %.3f Hz\n",
               media.interpolate_fps);
      }
   }

   geometry_changed = fft_width != old_fft_width ||
      fft_height != old_fft_height ||
      (video_stream_index >= 0 &&
       aspect_values_differ(old_video_aspect, get_video_output_aspect()));

   timing_changed = timing_changed ||
         playback_rates_differ(old_interpolate_fps, media.interpolate_fps);

   if (geometry_changed || refresh_updated || timing_changed)
   {
      struct retro_system_av_info info;
      retro_get_system_av_info(&info);

      if (geometry_changed)
      {
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Runtime geometry update: %ux%u aspect %.3f zoom %.2f\n",
               info.geometry.base_width, info.geometry.base_height,
               info.geometry.aspect_ratio, get_video_zoom());
         environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
      }

      if (!environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info))
      {
         fft_width = old_fft_width;
         fft_height = old_fft_height;
         media.interpolate_fps = old_interpolate_fps;
         frame_cnt = old_frame_cnt;
         timing_changed = false;
         refresh_updated = false;
      }
      else if (refresh_updated)
      {
         target_refresh_retry_active = false;
         target_refresh_retry_frames = 0;
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Applied updated target refresh rate during playback: %.3f -> %.3f Hz\n",
               old_interpolate_fps, media.interpolate_fps);
      }
      else if (timing_changed)
      {
         log_cb(RETRO_LOG_INFO,
               "[APLAYER] Applied updated playback timing during playback: %.3f -> %.3f Hz\n",
               old_interpolate_fps, media.interpolate_fps);
      }
   }

   input_poll_cb();
   {
      unsigned port;
      unsigned ports = input_ports;

      if (ports > APLAYER_MAX_PORTS)
         ports = APLAYER_MAX_PORTS;

      for (port = 0; port < ports; port++)
      {
         if (!aplayer_device_is_joypad(controller_port_devices[port]))
            continue;

         ret |= aplayer_poll_input_mask(port);
      }
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

      if (media_type == MEDIA_TYPE_VIDEO && y && !last_y)
      {
         char msg[256];
         struct retro_message_ext msg_obj = {0};

         msg[0] = '\0';

         if (audio_streams_num > 1)
         {
            int next_audio_stream_ptr;

            /* Select the new audio track without disturbing video decode state.
             * A full seek here can restart H.264 between reference frames. */
            slock_lock(decode_thread_lock);
            audio_streams_ptr = (audio_streams_ptr + 1) % audio_streams_num;
            next_audio_stream_ptr = audio_streams_ptr;
            audio_switch_requested = true;
            slock_unlock(decode_thread_lock);

            slock_lock(fifo_lock);
            scond_signal(fifo_cond);
            scond_signal(fifo_decode_cond);
            slock_unlock(fifo_lock);
            audio_selection_label(next_audio_stream_ptr, msg, sizeof(msg));
         }
         else
            snprintf(msg, sizeof(msg), "No alternate audio tracks");

         msg_obj.msg      = msg;
         msg_obj.duration = 3000;
         msg_obj.priority = 1;
         msg_obj.level    = RETRO_LOG_INFO;
         msg_obj.target   = RETRO_MESSAGE_TARGET_ALL;
         msg_obj.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
         msg_obj.progress = -1;
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
      }
      else if (media_type == MEDIA_TYPE_VIDEO && x && !last_x)
      {
         char msg[256];
         struct retro_message_ext msg_obj = {0};
         msg[0] = '\0';

         if (subtitle_streams_num > 0)
         {
            int next_subtitle_stream_ptr;

            slock_lock(decode_thread_lock);
            subtitle_streams_ptr = subtitle_next_selection(subtitle_streams_ptr);
            next_subtitle_stream_ptr = subtitle_streams_ptr;
            slock_unlock(decode_thread_lock);
            subtitle_selection_label(next_subtitle_stream_ptr, msg, sizeof(msg));
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

   aplayer_consume_playback_restart_pending();

   /* M3U */
   if (do_seek && seek_time == 0.0 && playlist_count > 0)
   {
      internal_playlist_reload_pending = true;
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
         internal_playlist_reload_pending = false;
         struct retro_message_ext m = {
            .msg = "Playlist finished - no playable entries.",
            .duration = 5000, .level = RETRO_LOG_ERROR
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
         return;   /* stop running gracefully */
      }
      do_seek = false;
      internal_playlist_reload_pending = false;

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
      float mix_factor;

      while (!decode_thread_dead && (!frames[1].valid || min_pts > frames[1].pts))
      {
         int64_t pts = 0;

         if (frames[1].valid)
         {
            struct frame tmp = frames[1];
            frames[1] = frames[0];
            frames[0] = tmp;
         }

         if (!decode_thread_dead)
         {
            video_decoder_context_t *ctx = NULL;
            uint32_t               *pixels = NULL;

            if (!video_buffer_wait_for_finished_slot(video_buffer))
            {
               if (aplayer_consume_playback_restart_pending())
                  min_pts = frame_cnt / media.interpolate_fps + pts_bias;
               continue;
            }

            if (aplayer_consume_playback_restart_pending())
               min_pts = frame_cnt / media.interpolate_fps + pts_bias;

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

         if (pts != AV_NOPTS_VALUE)
            frames[1].pts = av_q2d(fctx->streams[video_stream_index]->time_base) * pts;
         else if (frames[0].valid)
            frames[1].pts = frames[0].pts + (1.0 / media.interpolate_fps);
         else
            frames[1].pts = min_pts;
         frames[1].valid = true;
      }

      mix_factor = 1.0f;
      if (frames[0].valid && frames[1].valid && frames[1].pts > frames[0].pts)
      {
         double mix = (min_pts - frames[0].pts) / (frames[1].pts - frames[0].pts);
         if (mix < 0.0)
            mix = 0.0;
         else if (mix > 1.0)
            mix = 1.0;
         mix_factor = 1.0f - (video_blend_strength * (1.0f - (float)mix));
      }

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

      update_video_quad();
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
      case AV_CODEC_ID_SUBRIP:
      case AV_CODEC_ID_TEXT:
      case AV_CODEC_ID_WEBVTT:
      case AV_CODEC_ID_MOV_TEXT:
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

static bool codec_id_is_bitmap_subtitle(enum AVCodecID id)
{
   switch (id)
   {
      case AV_CODEC_ID_DVD_SUBTITLE:
      case AV_CODEC_ID_DVB_SUBTITLE:
      case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
      case AV_CODEC_ID_XSUB:
         return true;
      default:
         break;
   }

   return false;
}

static void subtitle_store_codec_private(unsigned slot, AVCodecContext *ctx, bool is_text)
{
   const uint8_t *src = NULL;
   size_t size = 0;
   bool has_native_header = false;

   if (slot >= MAX_STREAMS || !ctx)
      return;

   av_freep(&ass_extra_data[slot]);
   ass_extra_data_size[slot] = 0;
   subtitle_uses_native_text_header[slot] = false;

   if (is_text && ctx->subtitle_header && ctx->subtitle_header_size > 0)
   {
      src = (const uint8_t*)ctx->subtitle_header;
      size = (size_t)ctx->subtitle_header_size;
      has_native_header = true;
   }
   else if (ctx->extradata && ctx->extradata_size > 0)
   {
      src = ctx->extradata;
      size = (size_t)ctx->extradata_size;
   }

   if (!src || size == 0)
      return;

   ass_extra_data[slot] = (uint8_t*)av_malloc(size);
   if (!ass_extra_data[slot])
      return;

   memcpy(ass_extra_data[slot], src, size);
   ass_extra_data_size[slot] = size;
   subtitle_uses_native_text_header[slot] = has_native_header;
}

static bool open_subtitle_codec(AVCodecContext **ctx, unsigned index, bool is_text)
{
   bool use_text_opts = is_text;
   const AVCodec *codec = avcodec_find_decoder(fctx->streams[index]->codecpar->codec_id);

   if (!codec)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Couldn't find suitable subtitle decoder\n");
      return false;
   }

   do
   {
      int ret = 0;
      AVDictionary *opts = NULL;

      *ctx = avcodec_alloc_context3(codec);
      if (!*ctx)
         return false;

      ret = avcodec_parameters_to_context(*ctx, fctx->streams[index]->codecpar);
      if (ret < 0)
      {
         avcodec_free_context(ctx);
         return false;
      }

      if (use_text_opts)
      {
         av_dict_set(&opts, "sub_text_format", "ass", 0);
         av_dict_set(&opts, "flags2", "+ass_ro_flush_noop", 0);
#ifdef FF_SUB_CHARENC_MODE_IGNORE
         (*ctx)->sub_charenc_mode = FF_SUB_CHARENC_MODE_IGNORE;
#endif
      }

      ret = avcodec_open2(*ctx, codec, &opts);
      av_dict_free(&opts);

      if (ret >= 0)
         return true;

      avcodec_free_context(ctx);
      if (!use_text_opts)
      {
         log_cb(RETRO_LOG_ERROR, "[APLAYER] Could not open subtitle codec: %s\n",
               av_err2str(ret));
         return false;
      }

      log_cb(RETRO_LOG_WARN,
            "[APLAYER] Retrying text subtitle decoder without ASS conversion options: %s\n",
            av_err2str(ret));
      use_text_opts = false;
   } while (true);

   return false;
}

static AVCodecContext *open_text_subtitle_converter(enum AVCodecID codec_id)
{
   const AVCodec *codec = avcodec_find_decoder(codec_id);
   bool use_text_opts = true;

   if (!codec)
      return NULL;

   do
   {
      int ret = 0;
      AVDictionary *opts = NULL;
      AVCodecContext *ctx = avcodec_alloc_context3(codec);

      if (!ctx)
         return NULL;

      if (use_text_opts)
      {
         av_dict_set(&opts, "sub_text_format", "ass", 0);
         av_dict_set(&opts, "flags2", "+ass_ro_flush_noop", 0);
#ifdef FF_SUB_CHARENC_MODE_IGNORE
         ctx->sub_charenc_mode = FF_SUB_CHARENC_MODE_IGNORE;
#endif
      }

      ret = avcodec_open2(ctx, codec, &opts);
      av_dict_free(&opts);

      if (ret >= 0)
         return ctx;

      avcodec_free_context(&ctx);
      if (!use_text_opts)
         return NULL;

      use_text_opts = false;
   } while (true);

   return NULL;
}

static void ass_init_default_track(ASS_Track *track)
{
   unsigned play_res_x = subtitle_text_playres_x;
   unsigned play_res_y = subtitle_text_playres_y;
   const char *font_name = subtitle_font_name ? subtitle_font_name : "sans-serif";
   int font_bold = 0;
   unsigned font_size = 16;
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

static void ass_init_subtitle_track(ASS_Track *track, unsigned slot)
{
   if (!track || slot >= MAX_STREAMS)
      return;

   if (ass_extra_data_size[slot] > 0)
      ass_process_codec_private(track, (char*)ass_extra_data[slot], ass_extra_data_size[slot]);

   if (track->n_styles == 0)
      ass_init_default_track(track);

   if (subtitle_track_is_text(slot))
      ass_update_text_track_styles(track);

#ifdef LIBASS_VERSION
#if LIBASS_VERSION >= 0x01302000
   ass_set_check_readorder(track, 1);
#endif
#endif
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

static int audio_slot_for_stream(int stream_index)
{
   for (int i = 0; i < audio_streams_num; i++)
   {
      if (audio_streams[i] == stream_index)
         return i;
   }

   return -1;
}

static void audio_packet_buffer_drop_stale(packet_buffer_t *buffer,
      double timebase, double target_time)
{
   if (!buffer || timebase <= 0.0)
      return;

   while (!packet_buffer_empty(buffer))
   {
      int64_t end_pts = packet_buffer_peek_end_pts(buffer);
      if (end_pts == AV_NOPTS_VALUE || end_pts < 0)
         break;
      if ((double)end_pts * timebase >= target_time)
         break;
      packet_buffer_drop_packet(buffer);
   }
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
   subtitle_streams_ptr = SUBTITLE_STREAM_DISABLED;
   slock_unlock(decode_thread_lock);

   memset(audio_streams,    0, sizeof(audio_streams));
   memset(subtitle_streams, 0, sizeof(subtitle_streams));
   memset(subtitle_is_ass,  0, sizeof(subtitle_is_ass));
   memset(subtitle_is_bitmap, 0, sizeof(subtitle_is_bitmap));
   memset(subtitle_is_external, 0, sizeof(subtitle_is_external));
   memset(subtitle_uses_native_text_header, 0, sizeof(subtitle_uses_native_text_header));
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
               unsigned slot = subtitle_streams_num;
               AVCodecContext **s = &sctx[slot];
               const enum AVCodecID sub_id = fctx->streams[i]->codecpar->codec_id;
               const char *codec_name = avcodec_get_name(sub_id);
               bool is_ass = codec_id_is_ass(sub_id);
               bool is_text = codec_id_is_text_subtitle(sub_id) ||
                     codec_name_is_text_subtitle(sub_id);
               bool is_bitmap = codec_id_is_bitmap_subtitle(sub_id);

               log_cb(RETRO_LOG_INFO, "[APLAYER] Found subtitle stream %u: %s\n",
                     i, codec_name ? codec_name : "unknown");

               if (!is_ass && !is_text && !is_bitmap)
               {
                  log_cb(RETRO_LOG_WARN, "[APLAYER] Subtitle codec not supported: %s\n",
                        codec_name ? codec_name : "unknown");
                  break;
               }

               subtitle_streams[slot] = i;
               subtitle_is_ass[slot] = is_ass;
               subtitle_is_bitmap[slot] = is_bitmap;
               if (!open_subtitle_codec(s, i, is_text))
                  return false;

               if (!is_bitmap)
                  subtitle_store_codec_private(slot, *s, is_text);

               subtitle_streams_num++;
               log_cb(RETRO_LOG_INFO, "[APLAYER] Subtitle stream %u registered: %s (%s)\n",
                     i, codec_name ? codec_name : "unknown",
                     is_ass ? "ass" : (is_bitmap ? "bitmap" : "text"));
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

   if (audio_streams_num > 0)
      audio_streams_ptr = audio_select_initial_stream_ptr();

   return actx[0] || vctx;
}

static bool init_media_info(void)
{
   bool used_fallback = false;

   if (actx[0] && actx[0]->sample_rate > 0)
      media.sample_rate = actx[0]->sample_rate;
   else if (actx[0])
      log_cb(RETRO_LOG_WARN,
            "[APLAYER] Invalid audio sample rate (%d), using default %u.\n",
            actx[0]->sample_rate, media.sample_rate);

   media.interpolate_fps = aplayer_get_target_refresh_rate(&used_fallback);
   target_refresh_retry_active = true;
   target_refresh_retry_frames = APLAYER_TARGET_REFRESH_RETRY_FRAMES;
   if (used_fallback)
      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Target refresh rate unavailable, using fallback %.3f Hz and retrying during early playback.\n",
            media.interpolate_fps);
   else
      log_cb(RETRO_LOG_INFO,
            "[APLAYER] Using target refresh rate: %.3f Hz and monitoring for early updates.\n",
            media.interpolate_fps);

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
      bool need_ass_context = false;

      for (i = 0; i < (unsigned)subtitle_streams_num; i++)
      {
         if (!subtitle_track_is_bitmap(i))
         {
            need_ass_context = true;
            break;
         }
      }

      if (!need_ass_context)
         return true;

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
      update_subtitle_font_settings();

      for (i = 0; i < (unsigned)subtitle_streams_num; i++)
      {
         if (subtitle_track_is_bitmap(i))
            continue;

         ass_track[i] = ass_new_track(ass);
         if (ass_track[i])
            ass_init_subtitle_track(ass_track[i], i);
      }
   }

   return true;
}

static void set_colorspace(struct SwsContext *sws,
      unsigned width, unsigned height,
      enum AVColorSpace default_color, int in_range)
{
   const int *coeffs = NULL;
   const int *fallback_coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
   int in_full = 0;
   int out_full = 0;
   int brightness = 0;
   int contrast = 1 << 16;
   int saturation = 1 << 16;
   const int *inv_table = fallback_coeffs;
   const int *table = fallback_coeffs;
   int ret;

   if (!sws || !fallback_coeffs)
      return;

   if (default_color != AVCOL_SPC_UNSPECIFIED)
      coeffs = sws_getCoefficients(default_color);
   else if (width >= 1280 || height > 576)
      coeffs = sws_getCoefficients(AVCOL_SPC_BT709);
   else
      coeffs = sws_getCoefficients(AVCOL_SPC_BT470BG);

   if (!coeffs)
      coeffs = fallback_coeffs;

   ret = sws_getColorspaceDetails(sws, (int**)&inv_table, &in_full,
         (int**)&table, &out_full,
         &brightness, &contrast, &saturation);
   if (ret < 0)
   {
      inv_table = coeffs;
      table = fallback_coeffs;
      in_full = 0;
      out_full = 0;
      brightness = 0;
      contrast = 1 << 16;
      saturation = 1 << 16;
   }
   else
      inv_table = coeffs;

   if (in_range != AVCOL_RANGE_UNSPECIFIED)
      in_full = in_range == AVCOL_RANGE_JPEG;

   if (sws_setColorspaceDetails(sws, inv_table, in_full,
         table ? table : fallback_coeffs, out_full,
         brightness, contrast, saturation) < 0)
   {
      log_cb(RETRO_LOG_WARN,
            "[APLAYER] Failed to apply video colorspace details; using swscale defaults.\n");
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

static int bitmap_subtitle_scale_coord(int value, int src_size, int dst_size)
{
   if (src_size <= 0)
      return value;

   return (int)(((int64_t)value * dst_size + src_size / 2) / src_size);
}

static void blend_bitmap_subtitle_pixel(uint32_t *dst, uint32_t src)
{
   unsigned src_a = (src >> 24) & 0xff;
   unsigned src_r = (src >> 16) & 0xff;
   unsigned src_g = (src >>  8) & 0xff;
   unsigned src_b = (src >>  0) & 0xff;
   unsigned inv_a = 255 - src_a;
   uint32_t dst_color = *dst;
   unsigned dst_r = (dst_color >> 16) & 0xff;
   unsigned dst_g = (dst_color >>  8) & 0xff;
   unsigned dst_b = (dst_color >>  0) & 0xff;

   if (src_a == 0)
      return;

   if (src_a == 255)
   {
      *dst = src;
      return;
   }

   dst_r = src_r + ((dst_r * inv_a + 127) / 255);
   dst_g = src_g + ((dst_g * inv_a + 127) / 255);
   dst_b = src_b + ((dst_b * inv_a + 127) / 255);

   *dst = (0xffu << 24) | (dst_r << 16) | (dst_g << 8) | dst_b;
}

static void render_bitmap_subtitle_event(uint32_t *buffer, unsigned width,
      unsigned height, const struct bitmap_subtitle_event *event)
{
   unsigned rect_index = 0;
   int canvas_w = 0;
   int canvas_h = 0;

   if (!buffer || width == 0 || height == 0 || !event)
      return;

   canvas_w = event->canvas_w > 0 ? event->canvas_w : (int)width;
   canvas_h = event->canvas_h > 0 ? event->canvas_h : (int)height;

   for (rect_index = 0; rect_index < event->rect_count; rect_index++)
   {
      const struct bitmap_subtitle_rect *rect = &event->rects[rect_index];
      int full_x0 = 0;
      int full_y0 = 0;
      int full_x1 = 0;
      int full_y1 = 0;
      int dst_x0 = 0;
      int dst_y0 = 0;
      int dst_x1 = 0;
      int dst_y1 = 0;
      int dst_w = 0;
      int dst_h = 0;
      int full_w = 0;
      int full_h = 0;
      int src_y = 0;

      if (!rect->pixels || rect->w <= 0 || rect->h <= 0)
         continue;

      full_x0 = bitmap_subtitle_scale_coord(rect->x, canvas_w, (int)width);
      full_y0 = bitmap_subtitle_scale_coord(rect->y, canvas_h, (int)height);
      full_x1 = bitmap_subtitle_scale_coord(rect->x + rect->w, canvas_w, (int)width);
      full_y1 = bitmap_subtitle_scale_coord(rect->y + rect->h, canvas_h, (int)height);

      dst_x0 = full_x0;
      dst_y0 = full_y0;
      dst_x1 = full_x1;
      dst_y1 = full_y1;

      if (dst_x1 <= dst_x0)
         dst_x1 = dst_x0 + 1;
      if (dst_y1 <= dst_y0)
         dst_y1 = dst_y0 + 1;

      full_w = full_x1 - full_x0;
      full_h = full_y1 - full_y0;
      if (full_w <= 0)
         full_w = 1;
      if (full_h <= 0)
         full_h = 1;

      if (dst_x0 >= (int)width || dst_y0 >= (int)height || dst_x1 <= 0 || dst_y1 <= 0)
         continue;

      if (dst_x0 < 0)
         dst_x0 = 0;
      if (dst_y0 < 0)
         dst_y0 = 0;
      if (dst_x1 > (int)width)
         dst_x1 = (int)width;
      if (dst_y1 > (int)height)
         dst_y1 = (int)height;

      dst_w = dst_x1 - dst_x0;
      dst_h = dst_y1 - dst_y0;
      if (dst_w <= 0 || dst_h <= 0)
         continue;

      for (int dy = 0; dy < dst_h; dy++)
      {
         uint32_t *dst_row = buffer + (dst_y0 + dy) * width + dst_x0;
         int full_dy = (dst_y0 - full_y0) + dy;
         src_y = ((int64_t)full_dy * rect->h) / full_h;
         if (src_y >= rect->h)
            src_y = rect->h - 1;
         if (src_y < 0)
            src_y = 0;

         for (int dx = 0; dx < dst_w; dx++)
         {
            int full_dx = (dst_x0 - full_x0) + dx;
            int src_x = ((int64_t)full_dx * rect->w) / full_w;
            uint32_t src = 0;

            if (src_x >= rect->w)
               src_x = rect->w - 1;
            if (src_x < 0)
               src_x = 0;

            src = rect->pixels[src_y * rect->w + src_x];
            blend_bitmap_subtitle_pixel(&dst_row[dx], src);
         }
      }
   }
}

static void render_subtitles_on_buffer(uint32_t *buffer, unsigned width,
      unsigned height, double time_sec)
{
   ASS_Track *render_track = NULL;
   struct bitmap_subtitle_event *bitmap_event = NULL;
   int subtitle_ptr = -1;
   int64_t track_start_ms = -1;

   if (!buffer || width == 0 || height == 0)
      return;

   if (subtitle_streams_num <= 0)
      return;

   if (decode_thread_lock)
   {
      slock_lock(decode_thread_lock);
      if (subtitle_selection_is_valid(subtitle_streams_ptr))
      {
         subtitle_ptr = subtitle_streams_ptr;
         render_track = ass_track[subtitle_ptr];
      }
      slock_unlock(decode_thread_lock);
   }

   if (subtitle_ptr < 0 || subtitle_ptr >= MAX_STREAMS)
      return;

   if (subtitle_ptr >= 0 && subtitle_ptr < MAX_STREAMS)
      track_start_ms = first_subtitle_start_ms[subtitle_ptr];

   AVFrame tmp_frame;
   memset(&tmp_frame, 0, sizeof(tmp_frame));
   tmp_frame.data[0] = (uint8_t*)buffer;
   tmp_frame.linesize[0] = (int)width * (int)sizeof(uint32_t);

   slock_lock(ass_lock);
   if (subtitle_track_is_bitmap((unsigned)subtitle_ptr))
   {
      long long now_ms = subtitle_adjust_render_time_ms(render_track,
            subtitle_ptr, (long long)(time_sec * 1000.0));
      bitmap_subtitle_prune_locked((unsigned)subtitle_ptr, now_ms);
      bitmap_event = bitmap_subtitle_current_locked((unsigned)subtitle_ptr, now_ms);
      if (bitmap_event)
         render_bitmap_subtitle_event(buffer, width, height, bitmap_event);
   }
   else if (render_track && ass_render)
   {
      int change = 0;
      long long now_ms = subtitle_adjust_render_time_ms(render_track,
            subtitle_ptr, (long long)(time_sec * 1000.0));
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

static void bitmap_subtitle_convert_palette(uint32_t *colors, size_t count)
{
   size_t i = 0;

   if (!colors)
      return;

   for (i = 0; i < count; i++)
   {
      uint32_t c = colors[i];
      uint32_t b = c & 0xff;
      uint32_t g = (c >> 8) & 0xff;
      uint32_t r = (c >> 16) & 0xff;
      uint32_t a = (c >> 24) & 0xff;

      b = b * a / 255;
      g = g * a / 255;
      r = r * a / 255;

      colors[i] = b | (g << 8) | (r << 16) | (a << 24);
   }
}

static bool bitmap_subtitle_build_event(AVCodecContext *ctx, const AVSubtitle *sub,
      int64_t start_ms, int64_t end_ms, bool end_known,
      struct bitmap_subtitle_event *event)
{
   int canvas_w = 0;
   int canvas_h = 0;
   unsigned rect_count = 0;

   if (!sub || !event || sub->num_rects <= 0)
      return false;

   memset(event, 0, sizeof(*event));
   event->start_ms = start_ms;
   event->end_ms = end_ms;
   event->end_known = end_known;

   if (ctx)
   {
      canvas_w = ctx->width;
      canvas_h = ctx->height;
   }

   event->rects = (struct bitmap_subtitle_rect*)calloc((size_t)sub->num_rects,
         sizeof(*event->rects));
   if (!event->rects)
      return false;

   for (int i = 0; i < sub->num_rects; i++)
   {
      const AVSubtitleRect *rect = sub->rects[i];
      struct bitmap_subtitle_rect *dst = NULL;
      uint32_t palette[256] = {0};
      uint32_t *pixels = NULL;
      int palette_size = 0;

      if (!rect || rect->type != SUBTITLE_BITMAP ||
            rect->w <= 0 || rect->h <= 0 ||
            !rect->data[0] || !rect->data[1] || rect->linesize[0] <= 0)
      {
         continue;
      }

      palette_size = rect->nb_colors;
      if (palette_size <= 0 || palette_size > 256)
         continue;

      pixels = (uint32_t*)malloc((size_t)rect->w * (size_t)rect->h * sizeof(*pixels));
      if (!pixels)
         continue;

      memcpy(palette, rect->data[1], (size_t)palette_size * sizeof(palette[0]));
      bitmap_subtitle_convert_palette(palette, 256);

      for (int y = 0; y < rect->h; y++)
      {
         const uint8_t *src_row = rect->data[0] + y * rect->linesize[0];
         uint32_t *dst_row = pixels + (size_t)y * (size_t)rect->w;

         for (int x = 0; x < rect->w; x++)
            dst_row[x] = palette[src_row[x]];
      }

      dst = &event->rects[rect_count++];
      dst->x = rect->x;
      dst->y = rect->y;
      dst->w = rect->w;
      dst->h = rect->h;
      dst->pixels = pixels;

      if (dst->x + dst->w > canvas_w)
         canvas_w = dst->x + dst->w;
      if (dst->y + dst->h > canvas_h)
         canvas_h = dst->y + dst->h;
   }

   if (rect_count == 0)
   {
      bitmap_subtitle_clear_event(event);
      return false;
   }

   event->rect_count = rect_count;
   event->canvas_w = canvas_w > 0 ? canvas_w : media.width;
   event->canvas_h = canvas_h > 0 ? canvas_h : media.height;
   return true;
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

static void ass_add_embedded_event(ASS_Track *track, int64_t start_ms,
      int64_t end_ms, const char *ass_line)
{
   const char *chunk = ass_line;
   int64_t duration_ms = 0;

   if (!track || !ass_line || !*ass_line)
      return;

   while (*chunk && isspace((unsigned char)*chunk))
      chunk++;

   if (strncmp(chunk, "Dialogue:", 9) == 0)
   {
      chunk += 9;
      while (*chunk && isspace((unsigned char)*chunk))
         chunk++;
   }
   else if (strncmp(chunk, "Comment:", 8) == 0)
   {
      return;
   }

   if (!*chunk)
      return;

   if (start_ms < 0)
      start_ms = 0;

   duration_ms = end_ms - start_ms;
   if (duration_ms <= 0)
      duration_ms = 2000;

   ass_process_chunk(track, (char*)chunk, (int)strlen(chunk), start_ms, duration_ms);
}

static void ass_backfill_unknown_event_durations(ASS_Track *track)
{
   int last = 0;

   if (!track || track->n_events <= 1)
      return;

   last = track->n_events - 1;
   for (int i = last - 1; i >= 0; i--)
   {
      ASS_Event *event = &track->events[i];
      ASS_Event *next = &track->events[i + 1];

      if (track->events[last].Start == event->Start)
         continue;

      if (event->Duration == SUBTITLE_UNKNOWN_DURATION_MS)
      {
         if (event->Start < next->Start)
            event->Duration = next->Start - event->Start;
         else if (event->Start == next->Start)
            event->Duration = next->Duration;
      }

      if (i > 0 && event->Start != track->events[i - 1].Start)
         break;
   }
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

static bool srt_add_events_from_buffer(ASS_Track *track, char *buf, size_t buf_size,
      int64_t *first_start_ms_out, AVCodecContext *converter)
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
            bool added = false;

            if (converter)
            {
               AVPacket pkt;
               AVSubtitle sub;
               int got_sub = 0;

               memset(&pkt, 0, sizeof(pkt));
               memset(&sub, 0, sizeof(sub));
               pkt.data = (uint8_t*)text;
               pkt.size = (int)text_len;

               if (avcodec_decode_subtitle2(converter, &sub, &got_sub, &pkt) >= 0 &&
                     got_sub)
               {
                  for (int i = 0; i < sub.num_rects; i++)
                  {
                     if (!sub.rects[i])
                        continue;

                     if (sub.rects[i]->ass)
                     {
                        ass_add_embedded_event(track, start_ms, end_ms, sub.rects[i]->ass);
                        added = true;
                     }
                     else if (sub.rects[i]->text)
                     {
                        ass_add_text_event(track, start_ms, end_ms, sub.rects[i]->text);
                        added = true;
                     }
                  }
               }

               avsubtitle_free(&sub);
            }

            if (!added)
            {
               ass_add_text_event(track, start_ms, end_ms, text);
               added = true;
            }

            if (added)
            {
               any = true;
               if (first_start_ms < 0)
                  first_start_ms = start_ms;
            }
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

   update_subtitle_font_settings();

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
      AVCodecContext *converter = NULL;
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
#ifdef AV_CODEC_ID_SUBRIP
         converter = open_text_subtitle_converter(AV_CODEC_ID_SUBRIP);
#endif
         if (converter)
            subtitle_store_codec_private(slot, converter, true);

         ass_init_subtitle_track(track, slot);
         any_events = srt_add_events_from_buffer(track, buf, buf_size,
               &first_start_ms, converter);
         if (!any_events)
         {
            ass_free_track(track);
            av_freep(&ass_extra_data[slot]);
            ass_extra_data_size[slot] = 0;
            subtitle_uses_native_text_header[slot] = false;
            avcodec_free_context(&converter);
            free(buf);
            continue;
         }
      }

#ifdef LIBASS_VERSION
#if LIBASS_VERSION >= 0x01302000
      ass_set_check_readorder(track, 1);
#endif
#endif

      sctx[slot] = NULL;
      ass_track[slot] = track;
      subtitle_streams[slot] = -1;
      subtitle_is_ass[slot] = is_ass;
      subtitle_is_bitmap[slot] = false;
      subtitle_is_external[slot] = true;
      first_subtitle_start_ms[slot] = first_start_ms;
      subtitle_streams_num++;

      update_subtitle_font_settings();

      log_cb(RETRO_LOG_INFO, "[APLAYER] Loaded external subtitles: %s (%s)\n",
            sub_path, is_ass ? "ass" : "text");

      avcodec_free_context(&converter);
      free(buf);
      break;
   }
}

static void sws_worker_thread(void *arg)
{
   int ret = 0;
   unsigned src_width;
   unsigned src_height;
   enum AVPixelFormat src_fmt;
   AVFrame *tmp_frame = NULL;
   video_decoder_context_t *ctx = (video_decoder_context_t*) arg;

   if (ctx)
      ctx->pts = AV_NOPTS_VALUE;

   tmp_frame = ctx->filtered && ctx->filtered->data[0] ?
         ctx->filtered : ctx->source;

   if (!tmp_frame || !tmp_frame->data[0])
      goto done;

   src_width = tmp_frame->width > 0 ? (unsigned)tmp_frame->width : media.width;
   src_height = tmp_frame->height > 0 ? (unsigned)tmp_frame->height : media.height;
   src_fmt = (enum AVPixelFormat)tmp_frame->format;

   if (src_width == 0 || src_height == 0 || src_fmt == AV_PIX_FMT_NONE || src_fmt < 0)
   {
      log_cb(RETRO_LOG_WARN,
            "[APLAYER] Dropping video frame with invalid swscale source parameters (%ux%u fmt=%d).\n",
            src_width, src_height, (int)src_fmt);
      goto done;
   }

   ctx->sws = sws_getCachedContext(ctx->sws,
         (int)src_width,
         (int)src_height,
         src_fmt,
         media.width, media.height, AV_PIX_FMT_RGB32,
         SWS_FAST_BILINEAR, NULL, NULL, NULL);
   if (!ctx->sws)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Failed to acquire swscale context.\n");
      goto done;
   }

   set_colorspace(ctx->sws,
         src_width,
         src_height,
         tmp_frame->colorspace,
         tmp_frame->color_range);

   if ((ret = sws_scale(ctx->sws, (const uint8_t *const*)tmp_frame->data,
         tmp_frame->linesize, 0,
         (int)src_height,
         (uint8_t * const*)ctx->target->data, ctx->target->linesize)) < 0)
   {
      log_cb(RETRO_LOG_ERROR, "[APLAYER] Error while scaling image: %s\n", av_err2str(ret));
   }

   ctx->pts = tmp_frame->best_effort_timestamp != AV_NOPTS_VALUE ?
         tmp_frame->best_effort_timestamp : tmp_frame->pts;

done:
   av_frame_unref(ctx->source);
   av_frame_unref(ctx->filtered);
   video_buffer_finish_slot(video_buffer, ctx);
}

static void decode_video(AVCodecContext *ctx, AVPacket *pkt, ASS_Track *ass_track_active)
{
   int ret = 0;
   video_decoder_context_t *decoder_ctx = NULL;

   video_filter_drain_to_buffer(ass_track_active);

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

      update_video_presentation_from_frame(decoder_ctx->source);
      if (video_filter_queue_frame(decoder_ctx, ass_track_active))
         continue;

      video_submit_frame_to_worker(decoder_ctx, ass_track_active);
   }

   video_filter_drain_to_buffer(ass_track_active);
}

static int16_t *decode_audio(AVCodecContext *ctx, AVPacket *pkt,
      AVFrame *frame, int16_t *buffer, size_t *buffer_cap,
      SwrContext *swr, bool *clock_rebase_pending)
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
         if (audio_switch_requested)
         {
            scond_signal(fifo_cond);
            break;
         }
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

      if (audio_switch_requested)
      {
         scond_signal(fifo_cond);
         slock_unlock(fifo_lock);
         break;
      }

      if (clock_rebase_pending && *clock_rebase_pending)
      {
         double queued_seconds = (double)(FIFO_READ_AVAIL(audio_decode_fifo) +
               required_buffer) / (media.sample_rate * bytes_per_frame);
         decode_last_audio_time = (double)audio_frames / media.sample_rate +
               pts_bias + queued_seconds;
         *clock_rebase_pending = false;
      }
      else
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

   video_filter_close();
   video_filter_reset_pending = false;
   playback_restart_request = false;
   playback_restart_pending = false;

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
      if (subtitle_track_is_bitmap((unsigned)i))
         bitmap_subtitle_clear_slot((unsigned)i);
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
   bool audio_clock_rebase_pending = false;
   packet_buffer_t *audio_packet_buffers[MAX_STREAMS] = {0};
   packet_buffer_t *audio_packet_buffer = NULL;
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
   for (i = 0; (int)i < audio_streams_num; i++)
      audio_packet_buffers[i] = packet_buffer_create();
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
         bool restart_request = playback_restart_request;

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
         for (i = 0; (int)i < audio_streams_num; i++)
            packet_buffer_clear(&audio_packet_buffers[i]);
         packet_buffer_clear(&video_packet_buffer);

         if (restart_request)
         {
            playback_restart_pending = true;
            playback_restart_request = false;
         }

         scond_signal(fifo_cond);
         slock_unlock(fifo_lock);
      }

      slock_lock(decode_thread_lock);
      if (audio_switch_requested)
      {
         int ret;
         int switched_audio_stream_ptr = audio_streams_ptr;
         for (i = 0; (int)i < audio_streams_num; i++)
         {
            if (actx[i])
               avcodec_flush_buffers(actx[i]);
            if (swr[i])
            {
               swr_close(swr[i]);
               ret = swr_init(swr[i]);
               if (ret < 0)
                  log_cb(RETRO_LOG_ERROR,
                        "[APLAYER] Failed to reset audio resampler: %s\n",
                        av_err2str(ret));
            }
         }
         audio_switch_requested = false;
         if (aud_frame)
            av_frame_unref(aud_frame);
         slock_unlock(decode_thread_lock);

         slock_lock(fifo_lock);
         if (audio_decode_fifo)
            fifo_clear(audio_decode_fifo);
         audio_packet_buffer = audio_packet_buffers[switched_audio_stream_ptr];
         audio_packet_buffer_drop_stale(audio_packet_buffer,
               av_q2d(fctx->streams[audio_streams[switched_audio_stream_ptr]]->time_base),
               (double)audio_frames / media.sample_rate + pts_bias -
                  APLAYER_AUDIO_SWITCH_PREROLL_SECONDS);
         decode_last_audio_time = (double)audio_frames / media.sample_rate + pts_bias;
         last_audio_end = decode_last_audio_time;
         audio_clock_rebase_pending = true;
         scond_signal(fifo_cond);
         scond_signal(fifo_decode_cond);
         slock_unlock(fifo_lock);
      }
      else
         slock_unlock(decode_thread_lock);

      slock_lock(decode_thread_lock);
      audio_stream_index          = audio_streams[audio_streams_ptr];
      audio_stream_ptr            = audio_streams_ptr;
      actx_active                 = actx[audio_streams_ptr];
      if (subtitle_selection_is_valid(subtitle_streams_ptr))
         ass_track_active         = ass_track[subtitle_streams_ptr];
      audio_timebase = av_q2d(fctx->streams[audio_stream_index]->time_base);
      if (video_stream_index >= 0)
         video_timebase = av_q2d(fctx->streams[video_stream_index]->time_base);
      slock_unlock(decode_thread_lock);
      audio_packet_buffer = audio_packet_buffers[audio_stream_ptr];

      video_filter_drain_to_buffer(ass_track_active);

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
                                    swr[audio_stream_ptr],
                                    &audio_clock_rebase_pending);
         av_packet_unref(pkt_local);
      }

      if (audio_switch_requested)
         continue;

      /*
       * Decode video packet if:
       *  1. we already decoded an audio packet
       *  2. there is no audio stream to play
       *  3. EOF
       **/
      if (!audio_clock_rebase_pending &&
            !packet_buffer_empty(video_packet_buffer) &&
            (
               (!eof && earlier_or_close_enough(next_video_end, last_audio_end)) ||
               !actx_active ||
               eof
            )
         )
      {
         packet_buffer_get_packet(video_packet_buffer, pkt_local);

         decode_video(vctx, pkt_local, ass_track_active);

         av_packet_unref(pkt_local);
      }

      if (eof && packet_buffer_empty(video_packet_buffer))
         video_filter_drain_to_buffer(ass_track_active);

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
         loop_content = packet_buffer_empty(audio_packet_buffer) &&
                        packet_buffer_empty(video_packet_buffer) &&
                        eof &&
                        (!audio_decode_fifo || FIFO_READ_AVAIL(audio_decode_fifo) <= 1024 * 10) &&
                        (!video_buffer || !video_buffer_has_finished_slot(video_buffer));
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
            playback_restart_request = true;
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
               playback_restart_request = true;
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
               playback_restart_request = true;
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

         int audio_slot = audio_slot_for_stream(pkt_local->stream_index);
         if (audio_slot >= 0 && actx[audio_slot])
         {
            packet_buffer_add_packet(audio_packet_buffers[audio_slot], pkt_local);
            if (audio_slot != audio_stream_ptr)
               packet_buffer_trim(audio_packet_buffers[audio_slot],
                     APLAYER_AUDIO_PACKET_BUFFER_LIMIT);
         }
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

            /* Decode subtitle packets immediately. Text/ASS are appended to
             * libass tracks; bitmap subtitles are queued for later blending. */
            AVSubtitle sub;
            int finished = 0;
            int64_t base_time_ms = 0;
            int64_t start_ms = 0;
            int64_t end_ms = 0;
            int64_t duration_ms = 0;
            bool end_known = false;

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

            start_ms = base_time_ms + (int64_t)sub.start_display_time;
            if (sub.end_display_time != UINT32_MAX &&
                  sub.end_display_time > sub.start_display_time)
            {
               duration_ms = (int64_t)sub.end_display_time - (int64_t)sub.start_display_time;
               end_known = true;
            }

            if (duration_ms <= 0 && pkt_local->duration > 0)
            {
               double packet_duration_ms = pkt_local->duration *
                     av_q2d(fctx->streams[pkt_local->stream_index]->time_base) * 1000.0;
               if (packet_duration_ms > 0.0)
               {
                  duration_ms = (int64_t)(packet_duration_ms + 0.5);
                  end_known = true;
               }
            }

            if (end_known)
               end_ms = start_ms + duration_ms;
            else
               end_ms = start_ms;

            if (subtitle_track_is_bitmap((unsigned)subtitle_slot))
            {
               slock_lock(ass_lock);
               bitmap_subtitle_end_previous_locked((unsigned)subtitle_slot, start_ms);

               if (sub.num_rects > 0)
               {
                  struct bitmap_subtitle_event bitmap_event;

                  if (bitmap_subtitle_build_event(sctx_sub, &sub, start_ms, end_ms,
                           end_known, &bitmap_event))
                  {
                     if (!first_subtitle_event_logged)
                     {
                        if (first_subtitle_start_ms[subtitle_slot] < 0)
                           first_subtitle_start_ms[subtitle_slot] = start_ms;
                        log_cb(RETRO_LOG_INFO,
                              "[APLAYER] First subtitle event (bitmap): stream=%d slot=%d start_ms=%lld end_ms=%s%lld rects=%u\n",
                              pkt_local->stream_index, subtitle_slot,
                              (long long)start_ms,
                              end_known ? "" : "unknown/",
                              (long long)end_ms,
                              bitmap_event.rect_count);
                        first_subtitle_event_logged = true;
                     }

                     if (!bitmap_subtitle_append_locked((unsigned)subtitle_slot,
                              &bitmap_event))
                     {
                        bitmap_subtitle_clear_event(&bitmap_event);
                     }
                  }
               }

               slock_unlock(ass_lock);
               avsubtitle_free(&sub);
               av_packet_unref(pkt_local);
               continue;
            }

            if (duration_ms <= 0)
               duration_ms = SUBTITLE_UNKNOWN_DURATION_MS;

            end_ms = start_ms + duration_ms;

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
                        ass_add_embedded_event(ass_track_sub,
                              start_ms, end_ms, sub.rects[i]->ass);
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
                     const char *ass_payload = sub.rects[i]->ass;

                     if (!first_subtitle_event_logged)
                     {
                        const char *payload = ass_payload ? ass_payload : raw_text;
                        if (first_subtitle_start_ms[subtitle_slot] < 0)
                           first_subtitle_start_ms[subtitle_slot] = start_ms;
                        log_cb(RETRO_LOG_INFO,
                              "[APLAYER] First subtitle event (text): stream=%d slot=%d start_ms=%lld end_ms=%lld text=\"%.200s\"\n",
                              pkt_local->stream_index, subtitle_slot,
                              (long long)start_ms, (long long)end_ms,
                              payload ? payload : "(null)");
                        first_subtitle_event_logged = true;
                     }

                     if (ass_payload)
                        ass_add_embedded_event(ass_track_sub, start_ms, end_ms, ass_payload);
                     else if (raw_text)
                        ass_add_text_event(ass_track_sub, start_ms, end_ms, raw_text);
                  }
               }
               slock_unlock(ass_lock);
            }
            if (ass_track_sub && !subtitle_is_ass[subtitle_slot])
            {
               slock_lock(ass_lock);
               ass_backfill_unknown_event_durations(ass_track_sub);
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

   for (i = 0; (int)i < audio_streams_num; i++)
      packet_buffer_destroy(audio_packet_buffers[i]);
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
   frames[0].pts     = 0.0;
   frames[1].pts     = 0.0;
   frames[0].valid   = false;
   frames[1].valid   = false;

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
         sizeof(vertex_data), vertex_data, GL_DYNAMIC_DRAW);

   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void retro_unload_game(void)
{
   unsigned i;

   if (auto_resume_enabled)
      aplayer_bookmark_save_current();

   content_loaded = false;

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

   video_filter_close();
   video_filter_reset_pending = false;

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
   frames[0].valid = frames[1].valid = false;
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
      subtitle_is_bitmap[i] = false;
      subtitle_uses_native_text_header[i] = false;

      bitmap_subtitle_clear_slot(i);

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

   current_media_path[0] = '\0';

   media_reset_defaults();
}

bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_game_info local_info;
   struct aplayer_bookmark bookmark;
   bool have_bookmark = false;
   bool internal_playlist_reload = internal_playlist_reload_pending;
   bool requested_playlist = false;
   int resume_frames = 0;

   internal_playlist_reload_pending = false;

   if (!info || string_is_empty(info->path))
      return false;

   memset(&local_info, 0, sizeof(local_info));
   memset(&bookmark, 0, sizeof(bookmark));

   media_reset_defaults();
   aplayer_reset_controller_ports();
   aplayer_register_input_layout();
   current_media_path[0] = '\0';
   content_loaded = false;
   check_variables(true);

   requested_playlist = aplayer_path_is_m3u(info->path);

   if (requested_playlist)
   {
      if (!parse_m3u_playlist(info->path))
         return false;

      playlist_source_active = true;
      aplayer_copy_path(playlist_source_path, sizeof(playlist_source_path), info->path);
      aplayer_copy_path(current_content_path, sizeof(current_content_path), playlist_source_path);

      if (auto_resume_enabled &&
          aplayer_bookmark_load(current_content_path, &bookmark))
      {
         have_bookmark = true;
         aplayer_bookmark_restore_playlist_index(&bookmark);
      }

      local_info.path = playlist[playlist_index];
      local_info.size = info->size;
      local_info.data = info->data;
   }
   else if (internal_playlist_reload && playlist_source_active && playlist_count > 0)
   {
      aplayer_copy_path(current_content_path, sizeof(current_content_path), playlist_source_path);
      local_info.path = playlist[playlist_index];
      local_info.size = info->size;
      local_info.data = info->data;
   }
   else
   {
      playlist_source_active = false;
      playlist_source_path[0] = '\0';
      playlist_count = 0;
      playlist_index = 0;
      aplayer_copy_path(current_content_path, sizeof(current_content_path), info->path);

      if (auto_resume_enabled &&
          aplayer_bookmark_load(current_content_path, &bookmark))
      {
         have_bookmark = true;
      }

      local_info.path = info->path;
      local_info.size = info->size;
      local_info.data = info->data;
   }

   gme_seek_disabled = is_gme_path(local_info.path);
   aplayer_copy_path(current_media_path, sizeof(current_media_path), local_info.path);

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
   if (have_bookmark)
      aplayer_bookmark_apply_stream_selection(&bookmark);

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
   frames[0].valid = false;
   frames[1].valid = false;
   content_loaded = true;

   if (!internal_playlist_reload &&
       have_bookmark &&
       seek_supported &&
       duration_is_valid(bookmark.playback_time) &&
       bookmark.playback_time >= APLAYER_BOOKMARK_MIN_TIME &&
       bookmark.playback_time <= ((double)INT_MAX / media.interpolate_fps))
   {
      resume_frames = (int)(bookmark.playback_time * media.interpolate_fps + 0.5);
      if (resume_frames > 0)
      {
         seek_frame(resume_frames);
         aplayer_show_resume_message(bookmark.playback_time);
      }
   }

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
