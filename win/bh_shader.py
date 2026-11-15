#!/usr/bin/env python3
"""bh_shader.py: writes win/bh_shader.h, the black hole's GPU shader for the Windows panel (win/gpu.c), from
web/blackhole-mock.html, so the panel and the mock run the same shader: blackhole-simulation's (MIT, credited in
LICENSE) with Vigil's patches (OURS_PATCHES in the mock), plus the panel's own (PANEL_PATCHES below: desktop GLSL, the
shadow in alpha, and the glow of the actions on its gas). Run it again after changing the mock's shader:

    python3 win/bh_shader.py
"""
import json, os, re

here = os.path.dirname(os.path.abspath(__file__))
mock = open(os.path.join(here, '..', 'web', 'blackhole-mock.html'), encoding='utf-8').read()

i = mock.index('const THEIR_FRAG = ') + len('const THEIR_FRAG = ')
frag, end = json.JSONDecoder().raw_decode(mock[i:])

def js_strings(text):
    """the single-quoted JS strings in text, unescaped"""
    out = []
    for m in re.finditer(r"'((?:[^'\\]|\\.)*)'", text):
        out.append(re.sub(r"\\(.)", lambda e: {'n': '\n', 't': '\t'}.get(e.group(1), e.group(1)), m.group(1)))
    return out

j = mock.index('const OURS_PATCHES = [')
k = mock.index('\n];', j)
strs = js_strings(mock[mock.index('\n', j):k])
assert len(strs) % 2 == 0, 'OURS_PATCHES: pairs of strings'
patches = list(zip(strs[0::2], strs[1::2]))

PANEL_PATCHES = [
    ('#version 300 es', '#version 330 core'),
    ('uniform vec4 u_camQuat;', 'uniform vec4 u_camQuat;\n  uniform float u_heat, u_spot, u_spot_a, u_spot_t, u_flare, u_flare_a, u_flare_t;\n  uniform vec4 u_gp[64]; uniform vec2 u_gq[64]; uniform int u_gn; uniform vec4 u_gb;'),
    # the panel's gas: the actions' streams (bh_gas_points), traced as glowing tubes, so they bend round it and hide behind it
    ('void main() {', '''float vigil_edge = 1.0; /* (1, fading to 0 at the picture's edge: gas out there is not cut off at a stroke) */
void vigil_gas(vec3 a0, vec3 a1, inout vec3 accC, inout float accA) {
  if (u_gn < 2) return;
  vec3 mid = 0.5 * (a0 + a1), rd = a1 - a0;
  float hs = 0.5 * length(rd), rl2 = max(dot(rd, rd), 1e-8), sl = sqrt(rl2);
  if (length(mid - u_gb.xyz) > u_gb.w + hs) return;
  for (int i = 0; i < 63; i++) {
    if (i + 1 >= u_gn) break;
    if (u_gq[i + 1].y < 0.0) continue; /* (a new stream starts there) */
    vec3 b0 = u_gp[i].xyz, b1 = u_gp[i + 1].xyz, sd = b1 - b0, sm = 0.5 * (b0 + b1);
    float r0 = abs(u_gq[i].y), r1 = abs(u_gq[i + 1].y), sl2 = max(dot(sd, sd), 1e-8);
    if (length(mid - sm) > hs + 0.5 * sqrt(sl2) + 3.0 * max(r0, r1)) continue;
    float t = clamp(dot(sm - a0, rd) / rl2, 0.0, 1.0); vec3 pr = a0 + rd * t;
    float h = clamp(dot(pr - b0, sd) / sl2, 0.0, 1.0); vec3 q = b0 + sd * h;
    t = clamp(dot(q - a0, rd) / rl2, 0.0, 1.0); pr = a0 + rd * t;
    h = clamp(dot(pr - b0, sd) / sl2, 0.0, 1.0); q = b0 + sd * h;
    float rr = mix(r0, r1, h), d2 = dot(pr - q, pr - q) / (rr * rr);
    if (d2 > 9.0) continue;
    float br = mix(u_gp[i].w, u_gp[i + 1].w, h), tp = mix(u_gq[i].x, u_gq[i + 1].x, h);
    float dens = br * exp(-d2) * min(sl, 1.77 * rr) * 0.7 * vigil_edge;
    vec3 col = mix(vec3(0.72, 0.84, 1.0), vec3(1.0, 0.5, 0.16), tp) * (1.6 - 0.6 * tp);
    accC += col * dens * (1.0 - accA);
    accA += dens * 0.12;
  }
}
/* the hot spot (as the flares of Sgr A*, GRAVITY 2018): a compact clump of hot gas riding the inner disc with it, its light
 * boosted as it comes toward you; it smears out behind itself into an arc as it goes round, and its lensed image, bent over
 * the top of the shadow, races round the photon ring with it */
void vigil_spot(vec3 a0, vec3 a1, vec3 v, inout vec3 accC, inout float accA) {
  if (u_spot <= 0.0) return;
  const float R = 7.0;
  if (min(abs(a0.y), abs(a1.y)) > 2.0 && a0.y * a1.y > 0.0) return;
  float lo = min(length(a0), length(a1)), hi = max(length(a0), length(a1));
  vec3 sd = a1 - a0; float rl2 = max(dot(sd, sd), 1e-8), sl = sqrt(rl2);
  if (lo > R + 2.5 || hi + sl < R - 2.5) return;
  float om = sign(u_spin + 1e-8) / (pow(R, 1.5) + u_spin), aw = vigil_spot_at(); /* (om: its true turn, for its beaming) */
  float smear = 0.3 + 0.2 * u_spot_t;
  for (int i = 0; i < 9; i++) {
    float u = float(i) / 8.0, ang = aw - sign(om) * smear * u * u; /* (its tail behind it) */
    vec3 c = vec3(R * cos(ang), 0.0, R * sin(ang));
    float tt = clamp(dot(c - a0, sd) / rl2, 0.0, 1.0); vec3 pr = a0 + sd * tt;
    float rr = (0.42 + 0.25 * u) * (1.0 + 0.03 * u_spot_t), d2 = dot(pr - c, pr - c) / (rr * rr);
    if (d2 > 9.0) continue;
    float L = c.z * v.x - c.x * v.z, Om = om, del = 1.0 / max(0.05, 1.25 * (1.0 - Om * L)), beam = pow(del, 3.0);
    float br = u_spot * (i == 0 ? 7.0 : 3.2 * (1.0 - 0.8 * u)) / (1.0 + 0.12 * u_spot_t), dens = br * exp(-d2) * min(sl, 1.77 * rr);
    vec3 col = mix(vec3(1.0, 0.93, 0.82), vec3(1.0, 0.62, 0.3), u);
    accC += col * dens * beam * (1.0 - accA);
    accA += dens * 0.02;
  }
}
void main() {
  { vec2 uv = gl_FragCoord.xy / u_resolution; vigil_edge = smoothstep(0.0, 0.12, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y))); }'''),
    ('sample_accretion_disk(p, p_prev, ro, v, r_new, isco, M, a, currentDt, rs, accumulatedColor, accumulatedAlpha);', 'sample_accretion_disk(p, p_prev, ro, v, r_new, isco, M, a, currentDt, rs, accumulatedColor, accumulatedAlpha);\n        vigil_gas(p_prev, p, accumulatedColor, accumulatedAlpha);\n        vigil_spot(p_prev, p, v, accumulatedColor, accumulatedAlpha);'),
    ('  void sample_accretion_disk(', '''  /* the panel's actions on the gas: a torn star's heat, a hot spot riding the inner edge, a flare's flash (each 0: off) */
  /* where the hot spot is (its angle round the disc, the view's frame): once round in 5 s, faster than the gas's slow turn, so
   * it is seen to go round, in front of the shadow and over it */
  float vigil_spot_at() { return u_spot_a + sign(u_spin + 1e-8) * u_spot_t * 6.2831853 / 5.0; } /* (the way the gas goes: its angle growing) */
  float vigil_boost(vec3 q, float r) {
      float b = 1.0 + u_heat * 2.0 * exp(-(r - 7.0) * (r - 7.0) / 30.0);
      float az = atan(q.z, q.x);
      if (u_spot > 0.0) { float da = az - vigil_spot_at() + sign(u_spin + 1e-8) / (pow(r, 1.5) + u_spin) * u_disk_time * 1.2; /* (az: the gas's own frame) */ da -= 6.2831853 * floor((da + 3.14159265) / 6.2831853); if (da * sign(u_spin + 1e-8) < 0.0) da *= 0.4; /* (smeared out behind it) */
          float sa = 0.3 + 0.12 * u_spot_t, sr = 0.75 + 0.04 * u_spot_t; b += 10.0 * u_spot * exp(-(r - 7.0) * (r - 7.0) / (sr * sr) - da * da / (sa * sa)) / (1.0 + 0.15 * u_spot_t); }
      if (u_flare > 0.0) { /* the flare: the inner disc warms as its loop rises, then flashes as it snaps (BH_FLARE_SNAP), the
                              * flash running outward and fading */
          float t = u_flare_t - 1.8;
          if (t > 0.0) { float fr = 5.0 + 6.0 * t, fw = 0.8 + 0.6 * t; b += 3.0 * u_flare * exp(-t * 1.2) * (exp(-(r - fr) * (r - fr) / (fw * fw)) + exp(-t * 3.0) * exp(-(r - 5.0) * (r - 5.0) / 6.0)); }
          else b += 0.6 * u_flare * smoothstep(-1.8, 0.0, t) * exp(-(r - 6.5) * (r - 6.5) / 4.0);
      }
      return b;
  }
  void sample_accretion_disk('''),
    ('vec3 diskColor = blackbody(temperature) * beaming;', 'vec3 diskColor = blackbody(temperature) * beaming * vigil_boost(noiseP, sampleR);'),
    ('fragColor = vec4(finalColor, 1.0);\n}', 'fragColor = vec4(finalColor, hitHorizon ? 1.0 : clamp(accumulatedAlpha, 0.0, 1.0)); /* (alpha: the shadow, and the gas in front) */\n}'),
]
src = frag
for a, b in patches + PANEL_PATCHES:
    assert src.count(a) >= 1, 'patch point missing: ' + a[:60]
    src = src.replace(a, b, 1)
lines = src.split('\n')
at = next(n for n, l in enumerate(lines) if l.strip().startswith('precision')) + 1
lines[at:at] = ['#define ENABLE_LENSING 1', '#define ENABLE_DISK 1', '#define ENABLE_DOPPLER 1', '#define ENABLE_PHOTON_GLOW 1', '#define ENABLE_STARS 1', '#define RAY_QUALITY_HIGH 1']
src = '\n'.join(lines)

with open(os.path.join(here, 'bh_shader.h'), 'w', newline='\n') as f:
    f.write('/* bh_shader.h: written by bh_shader.py from web/blackhole-mock.html, do not edit. blackhole-simulation\'s fragment\n'
            ' * shader (https://github.com/steeltroops-ai/blackhole-simulation, MIT License, Copyright (c) 2026 Mayank; see LICENSE)\n'
            ' * with Vigil\'s patches. */\n')
    f.write('static const char BH_FRAG[] =\n')
    for line in src.split('\n'):
        f.write('    "' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n')
    f.write('    ;\n')
print('wrote win/bh_shader.h (%d bytes of shader)' % len(src))
