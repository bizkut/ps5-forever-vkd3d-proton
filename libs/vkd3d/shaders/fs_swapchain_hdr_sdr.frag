#version 450

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D Tex;

layout(push_constant) uniform HDRPresent
{
  float paperNits;
  float peakNits;
  float expansion;
  float saturation;
  float gamma;
  float kneeNits;
  float kneePq;
  float capPq;
  float useLuminance;
} hdr;

const float PQ_M1 = 0.1593017578125;
const float PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375;
const float PQ_C2 = 18.8515625;
const float PQ_C3 = 18.6875;

float pqInverseEotf(float nits)
{
  float p = pow(clamp(nits / 10000.0, 0.0, 1.0), PQ_M1);
  return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

void main()
{
  vec4 color = textureLod(Tex, vUV, 0.0);
  vec3 display709 = clamp(color.rgb, 0.0, 1.0);

  if (max(display709.r, max(display709.g, display709.b)) == 0.0)
  {
    FragColor = vec4(0.0, 0.0, 0.0, color.a);
    return;
  }

  /* PS5: WoW's low-preset output pass is a display-referred power-law path,
   * not an sRGB OETF, so the default inverse is gamma 2.2. gamma=srgb is
   * represented by zero and selects the standard piecewise sRGB EOTF. */
  vec3 linear709;
  if (hdr.gamma == 0.0)
  {
    vec3 low = display709 / 12.92;
    vec3 high = pow((display709 + 0.055) / 1.055, vec3(2.4));
    linear709 = mix(low, high, greaterThan(display709, vec3(0.04045)));
  }
  else
  {
    linear709 = pow(display709, vec3(hdr.gamma));
  }

  /* ITU-R BT.2087 linear-light BT.709 -> BT.2020 rows. */
  vec3 linear2020 = vec3(
      dot(linear709, vec3(0.627403896, 0.329283038, 0.043313066)),
      dot(linear709, vec3(0.069097289, 0.919540395, 0.011362316)),
      dot(linear709, vec3(0.016391439, 0.088013308, 0.895595253)));
  vec3 nits = linear2020 * hdr.paperNits;

  /* Preserve shadows and midtones through a fixed K = 0.75 * paper knee.
   * The default key is BT.2020 Y, so saturated orange/red surfaces do not
   * expand merely because one channel is near 1. key=max retains the original
   * behavior. A common guarded scale preserves hue and channel peak. */
  float sourcePeak = max(nits.r, max(nits.g, nits.b));
  float sourceY = dot(nits, vec3(0.2627, 0.6780, 0.0593));
  float expansionKey = mix(sourcePeak, sourceY, hdr.useLuminance);
  if (hdr.expansion > 0.0 && expansionKey > hdr.kneeNits)
  {
    float t = clamp((expansionKey - hdr.kneeNits) /
                    (hdr.paperNits - hdr.kneeNits), 0.0, 1.0);
    float shoulder = t * t * (3.0 - 2.0 * t);
    float expandedKey = min(expansionKey + hdr.expansion *
                            (hdr.peakNits - hdr.paperNits) * shoulder,
                            hdr.peakNits);
    float scale = min(expandedKey / expansionKey,
                      hdr.peakNits / sourcePeak);
    nits *= scale;
  }

  /* Optional perceptual preference control in linear BT.2020. sat=1 is the
   * exact colorimetric conversion; clamp negative out-of-gamut excursions. */
  float luminance = dot(nits, vec3(0.2627, 0.6780, 0.0593));
  nits = max(vec3(luminance) + hdr.saturation * (nits - vec3(luminance)), vec3(0.0));

  vec3 encoded = vec3(pqInverseEotf(nits.r),
                      pqInverseEotf(nits.g),
                      pqInverseEotf(nits.b));
  FragColor = vec4(min(encoded, vec3(hdr.capPq)), color.a);
}
