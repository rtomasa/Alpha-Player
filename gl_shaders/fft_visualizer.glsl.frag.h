#include "shaders_common.h"

static const char *fft_fragment_program_visualizer = GLSL_300(
   precision highp float;
   precision highp int;
   in vec2 vTex;
   out vec4 FragColor;
   uniform sampler2D sHeight;
   uniform vec2 uResolution;
   uniform vec2 uHeightmapSize;
   uniform float uRow;
   uniform float uTime;

   float hash11(float p)
   {
      return fract(sin(p * 12.9898) * 43758.5453);
   }

   float mapRange(float v, float inMin, float inMax, float outMin, float outMax)
   {
      float t = (v - inMin) / (inMax - inMin);
      t = clamp(t, 0.0, 1.0);
      return mix(outMin, outMax, t);
   }

   float fftValue(float idx)
   {
      idx = clamp(idx, 0.0, uHeightmapSize.x - 1.0);
      vec2 uv = vec2((idx + 0.5) / uHeightmapSize.x,
                     (uRow + 0.5) / uHeightmapSize.y);
      return textureLod(sHeight, uv, 0.0).g * 40.0;
   }

   float groundY(float x, float unit, float groundLineY, float time)
   {
      float angle = 1.1 * x / unit * 10.24;
      return sin(radians(angle + time * 2.0)) * unit * 1.25 + groundLineY - unit * 1.25;
   }

   void main()
   {
      vec2 fragCoord = vec2(vTex.x, 1.0 - vTex.y) * uResolution;
      float aspect = uResolution.x / uResolution.y;
      float scale = 1.0;
      if (aspect >= 1.3333334)
         scale = min(1.0, aspect / 2.1);
      float unit = (uResolution.y / 100.0) * scale;
      float sphereRadius = 15.0 * floor(unit + 0.5);
      float groundLineY = uResolution.y * 0.75;
      float yOffset = sin(radians(150.0)) * sphereRadius;
      vec2 origin = vec2(uResolution.x * 0.5, groundLineY - yOffset);

      vec2 pos = fragCoord - origin;
      float r = length(pos);
      float angle = degrees(atan(pos.y, pos.x));
      if (angle < 0.0)
         angle += 360.0;

      float localAngle = angle - 150.0;
      if (localAngle < 0.0)
         localAngle += 360.0;

      float arcMask = step(localAngle, 240.0);
      float lineWidth = max(1.0, unit / 10.24);
      float angleTol = lineWidth * 180.0 / (3.14159265 * max(r, 1.0));

      float gY = groundY(fragCoord.x, unit, groundLineY, uTime);
      float aboveGround = step(fragCoord.y, gY);

      float color = 0.0;

      /* Audio-driven radial lines. */
      float extendingLinesMin = sphereRadius * 1.3;
      float extendingLinesMax = sphereRadius * 3.5;
      float baseLen = mix(extendingLinesMin, extendingLinesMax,
            hash11(floor(localAngle) * 0.3 + 1.0));
      float lineLen = baseLen;

      if (localAngle <= 240.0)
      {
         float idx;
         float sumVal;

         if (localAngle <= 30.0)
         {
            idx = 240.0 - round(mapRange(localAngle, 0.0, 30.0, 0.0, 80.0));
            sumVal = fftValue(idx);
            lineLen = mapRange(sumVal, 0.0, 0.8,
                  baseLen - baseLen / 8.0, extendingLinesMax * 1.5);
         }
         else if (localAngle <= 90.0)
         {
            idx = 160.0 - round(mapRange(localAngle - 30.0, 0.0, 60.0, 0.0, 80.0));
            sumVal = fftValue(idx);
            lineLen = mapRange(sumVal, 0.0, 3.0,
                  baseLen - baseLen / 8.0, extendingLinesMax * 1.5);
         }
         else if (localAngle <= 120.0)
         {
            idx = 80.0 - round(mapRange(localAngle - 90.0, 0.0, 30.0, 65.0, 80.0));
            sumVal = fftValue(idx);
            lineLen = mapRange(sumVal, 0.0, 40.0,
                  baseLen - baseLen / 8.0, extendingLinesMax * 1.5);
         }
         else if (localAngle <= 150.0)
         {
            idx = round(mapRange(localAngle - 120.0, 0.0, 30.0, 0.0, 15.0));
            sumVal = fftValue(idx);
            lineLen = mapRange(sumVal, 0.0, 40.0,
                  baseLen - baseLen / 8.0, extendingLinesMax * 1.5);
         }
         else if (localAngle <= 210.0)
         {
            idx = 80.0 + round(mapRange(localAngle - 150.0, 0.0, 60.0, 0.0, 80.0));
            sumVal = fftValue(idx);
            lineLen = mapRange(sumVal, 0.0, 3.0,
                  baseLen - baseLen / 8.0, extendingLinesMax * 1.5);
         }
         else
         {
            idx = 160.0 + round(mapRange(localAngle - 210.0, 0.0, 30.0, 0.0, 80.0));
            sumVal = fftValue(idx);
            lineLen = mapRange(sumVal, 0.0, 0.8,
                  baseLen - baseLen / 8.0, extendingLinesMax * 1.5);
         }
      }

      float angleMask = 1.0 - smoothstep(0.0, angleTol, abs(localAngle - round(localAngle)));
      float radialMask = step(sphereRadius, r) * step(r, lineLen);
      float audioLine = arcMask * angleMask * radialMask * aboveGround;
      color = max(color, audioLine);

      /* Static long lines. */
      float staticStep = 4.0;
      float staticAngle = round(localAngle / staticStep) * staticStep;
      float staticAngleMask = 1.0 - smoothstep(0.0, angleTol, abs(localAngle - staticAngle));
      float staticLen = mix(sphereRadius, sphereRadius * 7.0,
            hash11(staticAngle * 0.31 + 2.0));
      float staticRadial = step(sphereRadius, r) * step(r, staticLen);
      float staticLine = arcMask * staticAngleMask * staticRadial * aboveGround;
      color = max(color, staticLine * 0.8);

      /* Dotted rings around the sphere. */
      float dotRadius = 1.5 * unit / 10.24;
      float dotSpacing = 1.5;
      float dotPhase = abs(fract(localAngle / dotSpacing) - 0.5);
      float dotMask = step(dotPhase, 0.35);

      for (int i = 0; i < 6; i++)
      {
         float count = float(i + 1);
         float surrRadMin = sphereRadius + sphereRadius * 0.5 * count;
         float surrRadMax = surrRadMin + surrRadMin * 0.125;
         float dir = mod(count, 2.0);
         float addon = uTime * 1.5 * mix(1.0, 1.5, dir);
         float ringRadius = mix(surrRadMin, surrRadMax,
               0.5 + 0.5 * sin(radians(localAngle * 7.0 + addon)));

         float ringMask = 1.0 - smoothstep(dotRadius, dotRadius * 1.5,
               abs(r - ringRadius));
         float ringBright = mapRange(ringRadius, surrRadMin, surrRadMax, 0.4, 1.0);
         float ring = arcMask * dotMask * ringMask * ringBright;

         if (r > sphereRadius + dotRadius)
            color = max(color, ring);
      }

      /* Ground dotted line. */
      float groundDotRadius = 0.9 * unit / 10.24;
      float groundDist = abs(fragCoord.y - gY);
      float groundMask = 1.0 - smoothstep(groundDotRadius, groundDotRadius * 1.5, groundDist);
      float groundSpacing = max(groundDotRadius * 2.2, 1.0);
      float groundPhase = abs(fract(fragCoord.x / groundSpacing) - 0.5);
      float groundDots = step(groundPhase, 0.4);
      color = max(color, groundMask * groundDots);

      /* Keep center empty for the sphere silhouette. */
      color *= step(sphereRadius, r);

      FragColor = vec4(vec3(color), 1.0);
   }
);
