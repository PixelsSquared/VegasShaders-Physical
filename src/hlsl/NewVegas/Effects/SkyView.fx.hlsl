// UNOFFICIAL 2026-10-14: the physical sky's lookup table ([Shaders.Sky.Physical], src/effects/SkyView.cpp). Once a frame, the
// light the air sends toward the eye from every direction of the upper half of the sky (Includes/Atmosphere.hlsl), into a
// 128 x 64 table that the sky shader (SKY.pso.hlsl) reads instead of working the air out for every pixel of the sky
// (Hillaire 2020's sky-view table).
//   u: the angle round from the sun's bearing, 0 (toward the sun) to 1 (away from it); the sky is the same on both sides.
//   v: the height above the horizon, as the square root of the angle (0 horizon, 1 straight up), so most rows sit near the
//      horizon, where the sky changes fastest.
// Stored x 1000 (TableScale): half floats lose the faint light of a low sun otherwise.

float4 TESR_PhysSky;        // x: share of the physical sky, y: brightness, z: weather tint, w: haze (Sky.cpp)
float4 TESR_PhysSkySun;     // xyz: the sun's real direction

#include "Includes/Atmosphere.hlsl"

static const float TableScale = 1000.0f;

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN IN) {
    VSOUT OUT = (VSOUT)0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return OUT;
}

float4 SkyViewPS(VSOUT IN) : COLOR0 {
    float azimuth = IN.UVCoord.x * AtmPi;
    float elevation = IN.UVCoord.y * IN.UVCoord.y * (0.5f * AtmPi);
    // In the sun's own frame: the sun along +x at its height, the view at this bearing from it.
    float3 dir = float3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), max(sin(elevation), 0.002f));
    float3 sun = float3(sqrt(saturate(1.0f - TESR_PhysSkySun.z * TESR_PhysSkySun.z)), 0.0f, TESR_PhysSkySun.z);
    return float4(AtmSky(normalize(dir), sun, 300.0f, TESR_PhysSky.w) * TableScale, 1.0f);
}

technique SkyView {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SkyViewPS();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-18 ExactSunlight + MultipleScattering ([Shaders.Sky.Physical], src/effects/SkyView.cpp). Everything below is new;
// technique 0 above (the shipping sky-view table) is untouched and is what runs with both switches off.
//
//  SkyTrans (technique 1): the sun's optical depth through the air as a table over (altitude, the sun's cosine there), the
//      parameterisation of Bruneton and Neyret (EGSR 2008) / Hillaire (2020): x_r from the horizon distance rho, x_mu from the
//      distance to the top of the air. It replaces the Chapman closed form (AtmSunDepth), which is 5-17% short in Rayleigh/Mie depth
//      at a low sun, models the ozone layer with the Rayleigh airmass of the point, and has no true earth shadow. Rays that the
//      earth blocks are cut (T = 0) at lookup, as in Bruneton's reference. RGB optical depth with the haze baked in.
//  SkyMS (technique 2): Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020 (CGF 39(4),
//      DOI 10.1111/cgf.14050), multiple scattering table Psi_ms(sun cosine at the point, altitude), 32 x 32: the light that has
//      scattered twice or more, taken as isotropic: Psi = L2 / (1 - f_ms), L2 the second order (here with the REAL phase for the
//      first scatter, which tracks a Monte Carlo reference better than the paper's isotropic one), f_ms the share of light scattered
//      on again, ground bounce at Lambert albedo TESR_PhysSkySun.w. Depends on haze and albedo only, never on the sun: rendered
//      when those change (and refreshed now and then), not every frame. ps_3_0 loops, 12 x 12 directions x 12 steps per texel.
//  SkyViewX / SkyViewXMS (techniques 3, 4): the sky-view table as technique 0 but with the sun depth from SkyTrans, and (4) with
//      Psi_ms * sigma_s * T_view added along the view ray.
// Stored x TableScale like technique 0 (SkyTrans stores optical depth, which half floats hold unscaled).
sampler2D TESR_SkyTransLUT : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SkyMSLUT : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

static const float TransW = 256.0f, TransH = 64.0f;
static const float MSN = 32.0f;
static const float AtmHor = 1132254.4f;     // sqrt(top^2 - ground^2): the distance from the ground's tangent point to the top of the air
static const int MSDirs = 12, MSSteps = 12;

float3 AtmSunOD(float r, float mu) {
    float rho = sqrt(max(r * r - AtmGround * AtmGround, 0.0f));
    float d = -r * mu + sqrt(max(r * r * (mu * mu - 1.0f) + AtmTop * AtmTop, 0.0f));
    float dmin = AtmTop - r, dmax = rho + AtmHor;
    float xmu = saturate((d - dmin) / max(dmax - dmin, 1.0f));
    float xr = saturate(rho / AtmHor);
    float2 uv = float2(0.5f / TransW + xmu * (1.0f - 1.0f / TransW), 0.5f / TransH + xr * (1.0f - 1.0f / TransH));
    float3 od = tex2Dlod(TESR_SkyTransLUT, float4(uv, 0.0f, 0.0f)).rgb;
    float g = AtmGround / r;
    float muHorizon = -sqrt(max(1.0f - g * g, 0.0f));
    return (mu < muHorizon) ? float3(1.0e4f, 1.0e4f, 1.0e4f) : od;      // the earth is in the way
}

float4 SkyTransPS(VSOUT IN) : COLOR0 {
    float2 ij = floor(IN.UVCoord * float2(TransW, TransH) + 0.25f);       // the texel, whichever half-pixel convention the quad has
    float xmu = ij.x / (TransW - 1.0f), xr = ij.y / (TransH - 1.0f);
    float rho = xr * AtmHor;
    float r = sqrt(rho * rho + AtmGround * AtmGround);
    float dmin = AtmTop - r, dmax = rho + AtmHor;
    float d = dmin + xmu * (dmax - dmin);
    float mu = (d < 1.0e-3f) ? 1.0f : clamp((AtmHor * AtmHor - rho * rho - d * d) / (2.0f * r * d), -1.0f, 1.0f);
    float dTop = -r * mu + sqrt(max(r * r * (mu * mu - 1.0f) + AtmTop * AtmTop, 0.0f));
    float st = max(-r * mu, 0.0f);                                          // the tangent point (the ray's lowest point when it heads down first)
    float3 sp = 0.0f;                                                       // Rayleigh, Mie, ozone density integrals (m)
    [loop] for (int seg = 0; seg < 2; seg++) {
        float len = (seg == 0) ? st : dTop - st;
        float dir = (seg == 0) ? -1.0f : 1.0f;
        [loop] for (int i = 0; i < 40; i++) {                               // cubic spacing: dense where the air is thick
            float v = (i + 0.5f) / 40.0f;
            float s = st + dir * len * v * v * v;
            float ds = 3.0f * v * v * (1.0f / 40.0f) * len;
            float rr = sqrt(max(r * r + 2.0f * r * mu * s + s * s, 0.0f));
            float a = max(rr - AtmGround, 0.0f);
            sp += float3(exp(-a / AtmRayleighH), exp(-a / AtmMieH), max(0.0f, 1.0f - abs(a - 25000.0f) / 15000.0f)) * ds;
        }
    }
    return float4(AtmRayleigh * sp.x + AtmMieExtinct * TESR_PhysSky.w * sp.y + AtmOzone * sp.z, 1.0f);
}

float4 SkyMSPS(VSOUT IN) : COLOR0 {
    float2 ij = floor(IN.UVCoord * MSN + 0.25f);
    float muS = -1.0f + 2.0f * ij.x / (MSN - 1.0f);
    float alt0 = ij.y / (MSN - 1.0f) * (AtmTop - AtmGround);
    float r0 = AtmGround + alt0;
    float haze = TESR_PhysSky.w, albedo = TESR_PhysSkySun.w;
    float3 sun = float3(sqrt(saturate(1.0f - muS * muS)), 0.0f, muS);
    float3 L2 = 0.0f, F = 0.0f;
    [loop] for (int a = 0; a < MSDirs; a++) {
        float cosT = 1.0f - 2.0f * (a + 0.5f) / MSDirs;
        float sinT = sqrt(saturate(1.0f - cosT * cosT));
        [loop] for (int b = 0; b < MSDirs; b++) {
            float phi = 2.0f * AtmPi * (b + 0.5f) / MSDirs;
            float3 dir = float3(sinT * cos(phi), sinT * sin(phi), cosT);
            float disc = r0 * r0 * (cosT * cosT - 1.0f) + AtmGround * AtmGround;
            float tMax = -r0 * cosT + sqrt(max(r0 * r0 * (cosT * cosT - 1.0f) + AtmTop * AtmTop, 0.0f));
            bool ground = (cosT < 0.0f) && (disc >= 0.0f);
            if (ground) tMax = max(-r0 * cosT - sqrt(max(disc, 0.0f)), 0.0f);
            float nu = dot(dir, sun);
            float phaseR = 3.0f / (16.0f * AtmPi) * (1.0f + nu * nu);
            float g2 = AtmMieG * AtmMieG;
            float phaseM = 3.0f / (8.0f * AtmPi) * ((1.0f - g2) * (1.0f + nu * nu)) / ((2.0f + g2) * pow(max(1.0f + g2 - 2.0f * AtmMieG * nu, 1e-4f), 1.5f));
            float dt = tMax / MSSteps;
            float3 thr = 1.0f;
            [loop] for (int s = 0; s < MSSteps; s++) {
                float3 p = float3(0.0f, 0.0f, r0) + dir * ((s + 0.5f) * dt);
                float r = length(p);
                float alt = r - AtmGround;
                float dR = exp(-alt / AtmRayleighH), dM = exp(-alt / AtmMieH), dO = max(0.0f, 1.0f - abs(alt - 25000.0f) / 15000.0f);
                float3 scR = AtmRayleigh * dR;
                float3 scM = AtmMieScatter * haze * dM;
                float3 ext = scR + AtmMieExtinct * haze * dM + AtmOzone * dO;
                float3 x = ext * dt;
                float3 f = lerp(1.0f - 0.5f * x, (1.0f - exp(-x)) / max(x, 1e-30f), step(1e-3f, x));      // (1 - e^-x) / x without the cancellation
                float3 tSun = exp(-AtmSunOD(r, dot(p, sun) / r));
                L2 += thr * tSun * (scR * phaseR + scM * phaseM) * (dt * f);
                F += thr * (scR + scM) * (dt * f);
                thr *= exp(-x);
            }
            if (ground) {                                                   // light the ground sends back (Lambert)
                float3 pg = float3(0.0f, 0.0f, r0) + dir * tMax;
                float rg = length(pg);
                float cosG = dot(pg, sun) / rg;
                L2 += thr * exp(-AtmSunOD(rg, cosG)) * (saturate(cosG) * albedo / AtmPi);
            }
        }
    }
    const float omega = 4.0f * AtmPi / (MSDirs * MSDirs), iso = 1.0f / (4.0f * AtmPi);
    float3 psi = (L2 * (omega * iso)) / (1.0f - F * (omega * iso));
    return float4(psi * TableScale, 1.0f);
}

float3 AtmSkyX(float3 dir, float3 sunDir, float camAlt, float haze, bool ms) {
    float r0 = AtmGround + camAlt;
    float mu = dir.z;
    float tMax = -r0 * mu + sqrt(max(r0 * r0 * (mu * mu - 1.0f) + AtmTop * AtmTop, 0.0f));
    float discGround = r0 * r0 * (mu * mu - 1.0f) + AtmGround * AtmGround;
    if (mu < 0.0f && discGround >= 0.0f) tMax = -r0 * mu - sqrt(discGround);

    float nu = dot(dir, sunDir);
    float phaseR = 3.0f / (16.0f * AtmPi) * (1.0f + nu * nu);
    float g2 = AtmMieG * AtmMieG;
    float phaseM = 3.0f / (8.0f * AtmPi) * ((1.0f - g2) * (1.0f + nu * nu)) / ((2.0f + g2) * pow(max(1.0f + g2 - 2.0f * AtmMieG * nu, 1e-4f), 1.5f));

    float3 depth = 0.0f, sumR = 0.0f, sumM = 0.0f, sumMS = 0.0f;
    // TEST 2026-11-04 belt of Venus: 24 steps (was 12). Across the earth's shadow edge in twilight 12 left 2-4% ripples in the
    // sky's brightness by height; 24 is within 1% of a 96-step reference. Sky/PhysSkyModel.h and Compute/SkyTables.glsl match.
    [loop] for (int i = 0; i < 24; i++) {
        float s0 = i / 24.0f, s1 = (i + 1) / 24.0f;
        float t0 = tMax * s0 * s0, t1 = tMax * s1 * s1;
        float dt = t1 - t0;
        float3 p = float3(0.0f, 0.0f, r0) + dir * (0.5f * (t0 + t1));
        float r = length(p);
        float alt = r - AtmGround;
        float dR = exp(-alt / AtmRayleighH), dM = exp(-alt / AtmMieH), dO = max(0.0f, 1.0f - abs(alt - 25000.0f) / 15000.0f);
        float3 ext = AtmRayleigh * dR + AtmMieExtinct * haze * dM + AtmOzone * dO;
        float cosSun = dot(p, sunDir) / r;
        float3 view = depth + ext * (0.5f * dt);
        float3 T = exp(-(view + AtmSunOD(r, cosSun)));
        sumR += T * (dR * dt);
        sumM += T * (dM * dt);
        if (ms) {
            float2 uv = float2(((cosSun * 0.5f + 0.5f) * (MSN - 1.0f) + 0.5f) / MSN, (saturate(alt / (AtmTop - AtmGround)) * (MSN - 1.0f) + 0.5f) / MSN);
            float3 psi = tex2Dlod(TESR_SkyMSLUT, float4(saturate(uv), 0.0f, 0.0f)).rgb * 0.001f;
            sumMS += exp(-view) * psi * ((AtmRayleigh * dR + AtmMieScatter * haze * dM) * dt);
        }
        depth += ext * dt;
    }
    return sumR * AtmRayleigh * phaseR + sumM * (AtmMieScatter * haze * phaseM) + sumMS;
}

float4 SkyViewXPS(VSOUT IN) : COLOR0 {
    float azimuth = IN.UVCoord.x * AtmPi;
    float elevation = IN.UVCoord.y * IN.UVCoord.y * (0.5f * AtmPi);
    float3 dir = float3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), max(sin(elevation), 0.002f));
    float3 sun = float3(sqrt(saturate(1.0f - TESR_PhysSkySun.z * TESR_PhysSkySun.z)), 0.0f, TESR_PhysSkySun.z);
    return float4(AtmSkyX(normalize(dir), sun, 300.0f, TESR_PhysSky.w, false) * TableScale, 1.0f);
}

float4 SkyViewXMSPS(VSOUT IN) : COLOR0 {
    float azimuth = IN.UVCoord.x * AtmPi;
    float elevation = IN.UVCoord.y * IN.UVCoord.y * (0.5f * AtmPi);
    float3 dir = float3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), max(sin(elevation), 0.002f));
    float3 sun = float3(sqrt(saturate(1.0f - TESR_PhysSkySun.z * TESR_PhysSkySun.z)), 0.0f, TESR_PhysSkySun.z);
    return float4(AtmSkyX(normalize(dir), sun, 300.0f, TESR_PhysSky.w, true) * TableScale, 1.0f);
}

technique SkyTrans {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SkyTransPS();
    }
}

technique SkyMS {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SkyMSPS();
    }
}

technique SkyViewX {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SkyViewXPS();
    }
}

technique SkyViewXMS {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SkyViewXMSPS();
    }
}
