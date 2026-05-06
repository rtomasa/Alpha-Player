#include "shaders_common.h"

static const char *fragment_source = GLSL(
      varying vec2 vTex;
      uniform sampler2D sFrame0Y;
      uniform sampler2D sFrame0U;
      uniform sampler2D sFrame0V;
      uniform sampler2D sFrame1Y;
      uniform sampler2D sFrame1U;
      uniform sampler2D sFrame1V;
      uniform sampler2D sSubtitle;
      uniform float uMix;
      uniform float uSubtitleEnabled;
      uniform vec4 uYuvParams0;
      uniform vec4 uYuvParams1;
      uniform vec2 uYuvGreen0;
      uniform vec2 uYuvGreen1;

      vec3 yuv_to_rgb(float yRaw, float cbRaw, float crRaw,
            vec4 params, vec2 green) {
         float y = (yRaw - params.x) * params.y;
         float cb = cbRaw - 0.5;
         float cr = crRaw - 0.5;
         return clamp(vec3(
               y + params.z * cr,
               y + green.x * cb + green.y * cr,
               y + params.w * cb), 0.0, 1.0);
      }

      void main() {
         vec3 frame1 = yuv_to_rgb(texture2D(sFrame1Y, vTex).r,
               texture2D(sFrame1U, vTex).r, texture2D(sFrame1V, vTex).r,
               uYuvParams1, uYuvGreen1);
         vec3 color = frame1;

         if (uMix < 0.999) {
            vec3 frame0 = yuv_to_rgb(texture2D(sFrame0Y, vTex).r,
                  texture2D(sFrame0U, vTex).r, texture2D(sFrame0V, vTex).r,
                  uYuvParams0, uYuvGreen0);
            color = pow(mix(pow(frame0, vec3(2.2)),
                  pow(frame1, vec3(2.2)), uMix), vec3(1.0 / 2.2));
         }

         if (uSubtitleEnabled > 0.5) {
            vec4 subtitle = texture2D(sSubtitle, vTex);
            color = subtitle.rgb + color * (1.0 - subtitle.a);
         }

         gl_FragColor = vec4(color, 1.0);
     }
);
