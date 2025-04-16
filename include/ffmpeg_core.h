#ifndef FFMPEG_CORE_H_
#define FFMPEG_CORE_H_

#include <glsym/glsym.h>

struct attachment
{
   uint8_t *data;
   size_t size;
};

struct frame
{
   GLuint tex;
   double pts;
};

enum media_type {
    MEDIA_TYPE_UNKNOWN = -1,
    MEDIA_TYPE_VIDEO,
    MEDIA_TYPE_AUDIO
};

enum loop_mode {
    PLAY_TRACK,
    LOOP_TRACK,
    LOOP_ALL,
    SHUFFLE_ALL
};

static struct
{
   double interpolate_fps;
   unsigned width;
   unsigned height;
   unsigned sample_rate;
   AVDictionaryEntry *title;

   float aspect;

   struct
   {
      double time;
      unsigned hours;
      unsigned minutes;
      unsigned seconds;
   } duration;

} media;

#endif