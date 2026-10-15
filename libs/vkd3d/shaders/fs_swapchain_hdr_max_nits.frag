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

float pqEotf(float code)
{
  float p = pow(clamp(code, 0.0, 1.0), 1.0 / PQ_M2);
  return 10000.0 * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

float pqInverseEotf(float nits)
{
  float p = pow(clamp(nits / 10000.0, 0.0, 1.0), PQ_M1);
  return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

void main()
{
  vec4 color = textureLod(Tex, vUV, 0.0);
  float peakCode = max(color.r, max(color.g, color.b));

  /* PS5: Preserve every sampled component exactly below K = 0.75 * MAX.
   * Above K, use a C1-continuous Reinhard-style rational shoulder:
   *   f(L) = K + (MAX - K) * (L - K) / ((L - K) + (MAX - K)).
   * It has unit slope at K and approaches, but never exceeds, MAX. Apply
   * f(max(R,G,B)) / max(R,G,B) in linear-nits space to preserve hue. */
  if (peakCode <= hdr.kneePq)
  {
    FragColor = color;
    return;
  }

  vec3 linearNits = vec3(pqEotf(color.r), pqEotf(color.g), pqEotf(color.b));
  float peakNits = max(linearNits.r, max(linearNits.g, linearNits.b));
  float shoulder = hdr.peakNits - hdr.kneeNits;
  float excess = peakNits - hdr.kneeNits;
  float mappedPeak = hdr.kneeNits + shoulder * excess / (excess + shoulder);
  float scale = mappedPeak / peakNits;

  vec3 encoded = vec3(pqInverseEotf(linearNits.r * scale),
                      pqInverseEotf(linearNits.g * scale),
                      pqInverseEotf(linearNits.b * scale));

  /* capPq is floored to a 10-bit code on the CPU. This keeps UNORM
   * quantization from rounding a channel above the requested luminance. */
  FragColor = vec4(min(encoded, vec3(hdr.capPq)), color.a);
}
