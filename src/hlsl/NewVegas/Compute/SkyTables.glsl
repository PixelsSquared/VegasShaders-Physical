// The sampled sky tables (needs SkyAtmosphere.glsl first): AtmSunOD (the SkyTrans table) and AtmSkyX (the sky-view table of
// techniques 3 and 4) of Effects/SkyView.fx.hlsl, line for line. The including shader declares t_trans (and t_ms where it reads
// the MS table) and defines SAMPLE_TRANS(coord) and SAMPLE_MS(coord) as textureLod calls (or vec4(0.0) where the table is not read).
#ifndef SKY_TABLES_INCLUDED
#define SKY_TABLES_INCLUDED

// The sun's optical depth from radius r toward the sun, from the SkyTrans table (rays the earth blocks are set to 1e4).
vec3 AtmSunOD(float r, float mu) {
    float rho = sqrt(max(r * r - AtmGround * AtmGround, 0.0));
    float d = -r * mu + sqrt(max(r * r * (mu * mu - 1.0) + AtmTop * AtmTop, 0.0));
    float dmin = AtmTop - r, dmax = rho + AtmHor;
    float xmu = clamp((d - dmin) / max(dmax - dmin, 1.0), 0.0, 1.0);
    float xr = clamp(rho / AtmHor, 0.0, 1.0);
    vec2 tuv = vec2(0.5 / TransW + xmu * (1.0 - 1.0 / TransW), 0.5 / TransH + xr * (1.0 - 1.0 / TransH));
    vec3 od = SAMPLE_TRANS(tuv).rgb;
    float g = AtmGround / r;
    float muHorizon = -sqrt(max(1.0 - g * g, 0.0));
    return (mu < muHorizon) ? vec3(1.0e4, 1.0e4, 1.0e4) : od;      // the earth is in the way
}

// The sky-view table's radiance (technique 3, ms false; technique 4, ms true: the MS table added along the view ray).
vec3 AtmSkyX(vec3 dir, vec3 sunDir, float camAlt, float haze, bool ms) {
    float r0 = AtmGround + camAlt;
    float mu = dir.z;
    float tMax = -r0 * mu + sqrt(max(r0 * r0 * (mu * mu - 1.0) + AtmTop * AtmTop, 0.0));
    float discGround = r0 * r0 * (mu * mu - 1.0) + AtmGround * AtmGround;
    if (mu < 0.0 && discGround >= 0.0) tMax = -r0 * mu - sqrt(discGround);

    float nu = dot(dir, sunDir);
    float phaseR = 3.0 / (16.0 * AtmPi) * (1.0 + nu * nu);
    float g2 = AtmMieG * AtmMieG;
    float phaseM = 3.0 / (8.0 * AtmPi) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * AtmMieG * nu, 1e-4), 1.5));

    vec3 depth = vec3(0.0), sumR = vec3(0.0), sumM = vec3(0.0), sumMS = vec3(0.0);
    for (int i = 0; i < 24; i++) {   // TEST 2026-11-04 belt of Venus: 24 steps, as SkyView.fx.hlsl AtmSkyX
        float s0 = float(i) / 24.0, s1 = float(i + 1) / 24.0;
        float t0 = tMax * s0 * s0, t1 = tMax * s1 * s1;
        float dt = t1 - t0;
        vec3 p = vec3(0.0, 0.0, r0) + dir * (0.5 * (t0 + t1));
        float r = length(p);
        float alt = r - AtmGround;
        float dR = exp(-alt / AtmRayleighH), dM = exp(-alt / AtmMieH), dO = max(0.0, 1.0 - abs(alt - 25000.0) / 15000.0);
        vec3 ext = AtmRayleigh * dR + AtmMieExtinct * haze * dM + AtmOzone * dO;
        float cosSun = dot(p, sunDir) / r;
        vec3 view = depth + ext * (0.5 * dt);
        vec3 T = exp(-(view + AtmSunOD(r, cosSun)));
        sumR += T * (dR * dt);
        sumM += T * (dM * dt);
        if (ms) {
            vec2 muv = vec2(((cosSun * 0.5 + 0.5) * (MSN - 1.0) + 0.5) / MSN, (clamp(alt / (AtmTop - AtmGround), 0.0, 1.0) * (MSN - 1.0) + 0.5) / MSN);
            vec3 psi = SAMPLE_MS(clamp(muv, 0.0, 1.0)).rgb * 0.001;
            sumMS += exp(-view) * psi * ((AtmRayleigh * dR + AtmMieScatter * haze * dM) * dt);
        }
        depth += ext * dt;
    }
    return sumR * AtmRayleigh * phaseR + sumM * (AtmMieScatter * haze * phaseM) + sumMS;
}

#endif
