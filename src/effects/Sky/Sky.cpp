#include "Sky.h"
#include "../../core/FrameWorker.h"	// UNOFFICIAL 2026-11-03 SkyOnWorker
#include "../../core/TestTools.h"	// NVR_TEST_TOOLS (SkyOnWorkerCheck)
#include <cstddef>


// UNOFFICIAL 2026-10-25: a setting that may be missing from an older defaults file (GetSettingF would read 0: a black sky)
static float SettingOr(const char* section, const char* key, float fallback) {
	SettingManager::Configuration::ConfigNode node;
	return TheSettingManager->Config.FillNode(&node, section, key) ? node.FloatValue : fallback;
}

void SkyShaders::RegisterConstants() {
	// Name -> memory, in the order the shaders' constants were always registered.
	struct Binding {
		const char* name;
		D3DXVECTOR4* value;
	};
	const Binding bindings[] = {
		{ "TESR_SkyData", &Constants.SkyData },
		{ "TESR_CloudData", &Constants.CloudData },
		{ "TESR_SunsetColor", &Constants.SunsetColor },
		{ "TESR_SkyIrradiance", &Constants.Irradiance[0] },
		{ "TESR_SkyIrradianceGame", &Constants.IrradianceGame[0] },			// UNOFFICIAL 2026-10-18 ClassicLook
		{ "TESR_SkyIrradianceObjects", &Constants.IrradianceObjects[0] },	// UNOFFICIAL 2026-10-25 LightsObjects
		{ "TESR_PhysSky", &Constants.PhysSky },
		{ "TESR_PhysSkySun", &Constants.PhysSkySun },
		{ "TESR_SkyTint", &Constants.SkyTint },								// UNOFFICIAL 2026-10-25
		{ "TESR_MoonData", &Constants.MoonData },
		{ "TESR_SkyTintHorizon", &Constants.SkyTintHorizon },				// UNOFFICIAL 2026-10-26
		{ "TESR_SkyLook", &Constants.SkyLook },
		{ "TESR_FogSkyLook", &Constants.FogSkyLook },
		{ "TESR_FogSkyColor", &Constants.FogSkyColor },						// UNOFFICIAL 2026-10-27 (UpdateFogSky)
		{ "TESR_FogSkyLowColor", &Constants.FogSkyLowColor },
		{ "TESR_FogHorizonColor", &Constants.FogHorizonColor },
		{ "TESR_FogWeatherColor", &Constants.FogWeatherColor },				// UNOFFICIAL 2026-10-30 (UpdateFogSky)
	};
	for (const Binding& binding : bindings) TheShaderManager->RegisterConstant(binding.name, binding.value);
	// UNOFFICIAL 2026-11-03 SkyOnWorker: what the worker's projection writes, Irradiance up to IrradianceObjects (PhysSky between them)
	static_assert(offsetof(SkyStruct, PhysSky) > offsetof(SkyStruct, Irradiance) && offsetof(SkyStruct, IrradianceObjects) > offsetof(SkyStruct, PhysSky) &&
		offsetof(SkyStruct, SkyTint) == offsetof(SkyStruct, IrradianceObjects) + sizeof(SkyStruct::IrradianceObjects), "SkyOnWorker: the results' range");
	SkyProjection::Begin = (const char*)&Constants.Irradiance[0];
	SkyProjection::End = (const char*)&Constants.SkyTint;
}

void SkyProjection::JoinNow() { if (TheShaderManager && TheShaderManager->Shaders.Sky) TheShaderManager->Shaders.Sky->JoinProjection(); }

// UNOFFICIAL 2026-10-14 physical sky: Effects/Includes/Atmosphere.hlsl AtmSky, line for line (see there). Kept in step by
// hand, like EvalSky above: change one and change the other.
namespace PhysAtmosphere {
	static const float Ground = 6360000.0f, Top = 6460000.0f;
	static const D3DXVECTOR3 Rayleigh(5.802e-6f, 13.558e-6f, 33.1e-6f);
	static const float RayleighH = 8000.0f, MieScatter = 3.996e-6f, MieExtinct = 4.44e-6f, MieH = 1200.0f, MieG = 0.8f;
	static const D3DXVECTOR3 Ozone(0.650e-6f, 1.881e-6f, 0.085e-6f);
	static const float OzoneColumn = 15000.0f, Pi = 3.14159265f;

	static float Chapman(float X, float h, float cosChi) {
		const float c = sqrtf(X + h);
		if (cosChi >= 0.0f) return c / (c * cosChi + 1.0f) * exp2f(-h);
		const float x0 = sqrtf(1.0f - cosChi * cosChi) * (X + h);
		return 2.0f * sqrtf(x0) * exp2f((std::min)(X - x0, 120.0f)) - c / (1.0f - c * cosChi) * exp2f(-h);
	}

	static D3DXVECTOR3 Exp(const D3DXVECTOR3& v) { return D3DXVECTOR3(expf(v.x), expf(v.y), expf(v.z)); }
	static D3DXVECTOR3 Mul(const D3DXVECTOR3& a, const D3DXVECTOR3& b) { return D3DXVECTOR3(a.x * b.x, a.y * b.y, a.z * b.z); }

	static D3DXVECTOR3 SunDepth(float r, float cosChi, float haze) {
		const float alt = r - Ground, hR = RayleighH * 0.693147f, hM = MieH * 0.693147f;
		const float chR = Chapman(Ground / hR, alt / hR, cosChi), chM = Chapman(Ground / hM, alt / hM, cosChi);
		const float airmass = chR / (std::max)(expf(-alt / RayleighH), 1e-6f);
		const float mie = MieExtinct * haze * MieH * chM;
		return Rayleigh * (RayleighH * chR) + D3DXVECTOR3(mie, mie, mie) + Ozone * (OzoneColumn * airmass);
	}

	static D3DXVECTOR3 Radiance(const D3DXVECTOR3& dir, const D3DXVECTOR3& sunDir, float camAlt, float haze) {
		const float r0 = Ground + camAlt, mu = dir.z;
		float tMax = -r0 * mu + sqrtf((std::max)(r0 * r0 * (mu * mu - 1.0f) + Top * Top, 0.0f));
		const float discGround = r0 * r0 * (mu * mu - 1.0f) + Ground * Ground;
		if (mu < 0.0f && discGround >= 0.0f) tMax = -r0 * mu - sqrtf(discGround);
		const float nu = D3DXVec3Dot(&dir, &sunDir);
		const float phaseR = 3.0f / (16.0f * Pi) * (1.0f + nu * nu);
		const float g2 = MieG * MieG;
		const float phaseM = 3.0f / (8.0f * Pi) * ((1.0f - g2) * (1.0f + nu * nu)) / ((2.0f + g2) * powf((std::max)(1.0f + g2 - 2.0f * MieG * nu, 1e-4f), 1.5f));
		D3DXVECTOR3 depth(0.0f, 0.0f, 0.0f), sumR(0.0f, 0.0f, 0.0f), sumM(0.0f, 0.0f, 0.0f);
		for (int i = 0; i < 12; i++) {
			const float s0 = i / 12.0f, s1 = (i + 1) / 12.0f, t0 = tMax * s0 * s0, t1 = tMax * s1 * s1, dt = t1 - t0;
			const D3DXVECTOR3 p = D3DXVECTOR3(0.0f, 0.0f, r0) + dir * (0.5f * (t0 + t1));
			const float r = D3DXVec3Length(&p), alt = r - Ground;
			const float dR = expf(-alt / RayleighH), dM = expf(-alt / MieH), dO = (std::max)(0.0f, 1.0f - fabsf(alt - 25000.0f) / 15000.0f);
			const float mie = MieExtinct * haze * dM;
			const D3DXVECTOR3 ext = Rayleigh * dR + D3DXVECTOR3(mie, mie, mie) + Ozone * dO;
			const D3DXVECTOR3 T = Exp(-(depth + ext * (0.5f * dt) + SunDepth(r, D3DXVec3Dot(&p, &sunDir) / r, haze)));
			sumR += T * (dR * dt);
			sumM += T * (dM * dt);
			depth += ext * dt;
		}
		return Mul(sumR, Rayleigh) * phaseR + sumM * (MieScatter * haze * phaseM);
	}

	static float Luma(const D3DXVECTOR3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

	// SKY.pso.hlsl's blend of the game's sky colour with the physical one (phys already scaled to the weather's brightness).
	static D3DXVECTOR3 Blend(const D3DXVECTOR3& game, const D3DXVECTOR3& phys, const D3DXVECTOR4& physSky) {
		const D3DXVECTOR3 tinted = phys + (game * (Luma(phys) / (std::max)(Luma(game), 1e-6f)) - phys) * physSky.z;
		return game + (tinted - game) * physSky.x;
	}
}

// Mirrors GetSunColor / GetSkyColor in Shaders/Includes/Sky.hlsl. Kept in step by hand: the
// two trees compile independently, exactly as Shadow.hlsl mirrors Effects/Includes/Shadows.hlsl.
// Change one and change the other.
//
// The atmosphere exponent is SKY.pso's own 8. The ambient path used to soften it to fake the
// width of a diffuse lobe; the integral below covers that properly, so the sky model is used
// unmodified and the ambient agrees with the dome on screen.
// The piecewise sRGB transfer function, matching linearize()/delinearize() in
// Shaders/Includes/Helpers.hlsl. A plain pow(c, 2.2) is NOT the same curve -- it diverges most
// in the darks, which is exactly where the sky colours sit at dawn and dusk.
static float SrgbToLinear(float c) {
	const float nonNegative = c > 0.0f ? c : 0.0f;	// the max(c, 0) of the windows.h macro
	return (nonNegative <= 0.04045f) ? (nonNegative / 12.92f) : powf((nonNegative + 0.055f) / 1.055f, 2.4f);
}


static D3DXVECTOR3 Linearize(const D3DXVECTOR4& c) {
	const D3DXVECTOR3 linear(SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z));
	return linear;
}


// UNOFFICIAL 2026-10-26 Shaders/Includes/Sky.hlsl SkyLook, line for line (saturation, then the main tint times the horizon tint
// fading out by look.y): change one and change the other.
static D3DXVECTOR3 SkyLook(D3DXVECTOR3 c, float up, const D3DXVECTOR4& tint, const D3DXVECTOR4& horizonTint, const D3DXVECTOR4& look) {
	if (look.x != 1.0f) {
		const float luminance = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
		c = D3DXVECTOR3((std::max)(luminance + (c.x - luminance) * look.x, 0.0f), (std::max)(luminance + (c.y - luminance) * look.x, 0.0f),
			(std::max)(luminance + (c.z - luminance) * look.x, 0.0f));
	}
	float overhead = look.y > 0.0f ? std::clamp(std::clamp(up, 0.0f, 1.0f) / look.y, 0.0f, 1.0f) : (up >= 0.0f ? 1.0f : 0.0f);
	overhead = overhead * overhead * (3.0f - 2.0f * overhead);	// smoothstep(0, look.y, saturate(up))
	for (int k = 0; k < 3; k++) c[k] *= tint[k] * (horizonTint[k] + (1.0f - horizonTint[k]) * overhead);
	return c;
}

// UNOFFICIAL 2026-10-27 (user: "tint horizon still seems to do nothing"): near the horizon VolumetricFog lies over the sky and
// coloured itself with the weather's untinted sky colours, so the horizon tint (and the main tint and saturation) were painted over
// there; only the SM6 build of the fog took them (a register more broke the ps_3_0 composite, X4507). Now the fog reads its own
// copies of the three colours with the sky look already in them, as the SM6 build applied it to the colour they make (SkyLook at the
// horizon: saturation, then SkyTint x SkyTintHorizon; both are linear in the colour, so applying them to each input of GetSkyColor's
// blend is the same): no register more, and the Vulkan fog passes take the same copies. Back in sRGB, as the shaders linearize.
static float LinearToSrgb(float c) {
	c = (std::max)(c, 0.0f);
	return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);
}

void SkyShaders::UpdateFogSky() {
	const D3DXVECTOR4* from[3] = { &TheShaderManager->ShaderConst.skyColor, &TheShaderManager->ShaderConst.skyLowColor, &TheShaderManager->ShaderConst.horizonColor };
	D3DXVECTOR4* to[3] = { &Constants.FogSkyColor, &Constants.FogSkyLowColor, &Constants.FogHorizonColor };
	const D3DXVECTOR4& f = Constants.FogSkyLook;
	const bool look = Enabled && (f.x != 1.0f || f.y != 1.0f || f.z != 1.0f || f.w != 1.0f);	// the game's sky (NVR's off): untinted, as it is drawn
	static const D3DXVECTOR4 white(1.0f, 1.0f, 1.0f, 1.0f);
	for (int i = 0; i < 3; i++) {
		if (!look) { *to[i] = *from[i]; continue; }
		const D3DXVECTOR3 c = SkyLook(Linearize(*from[i]), 0.0f, D3DXVECTOR4(f.x, f.y, f.z, 1.0f), white, D3DXVECTOR4(f.w, 1.0f, 0.0f, 0.0f));
		*to[i] = D3DXVECTOR4(LinearToSrgb(c.x), LinearToSrgb(c.y), LinearToSrgb(c.z), from[i]->w);
	}
	// UNOFFICIAL 2026-10-30 WeatherFogColor ([Shaders.VolumetricFog.Main]; tester: "a weather's fog colour set to 100% red changes
	// nothing"): the fog now takes the weather's own fog colour, so its copy of it gets the sky look on top too, outdoors, while the
	// fog in use takes it. Indoors (the cell's fog colour), with WeatherFogColor 0, LegacyFog, ClassicLook or the FogReference test
	// it is the raw colour, as those read it before. VolumetricFog.fx.hlsl reads it as TESR_FogColor.
	const D3DXVECTOR4& fog = TheShaderManager->ShaderConst.fogColor;
	const VolumetricFogEffect* volumetricFog = TheShaderManager->Effects.VolumetricFog;
	const bool fogTakesIt = volumetricFog && volumetricFog->Constants.Sun.y > 0.0f && !volumetricFog->IsLegacy() && !volumetricFog->IsClassic() && !volumetricFog->referenceTest;
	if (!look || !TheShaderManager->GameState.isExterior || !fogTakesIt) Constants.FogWeatherColor = fog;
	else {
		const D3DXVECTOR3 c = SkyLook(Linearize(fog), 0.0f, D3DXVECTOR4(f.x, f.y, f.z, 1.0f), white, D3DXVECTOR4(f.w, 1.0f, 0.0f, 0.0f));
		Constants.FogWeatherColor = D3DXVECTOR4(LinearToSrgb(c.x), LinearToSrgb(c.y), LinearToSrgb(c.z), fog.w);
	}
}

static D3DXVECTOR3 EvalSky(const D3DXVECTOR3& dir, const D3DXVECTOR4& sunPos, const D3DXVECTOR4& skyData,
	const D3DXVECTOR3& skyC, const D3DXVECTOR3& skyLowC, const D3DXVECTOR3& horizonC,
	const D3DXVECTOR3& sunColor, float sunHeight, float isDayTime) {
	// verticality: 0 at the horizon, 1 at the zenith; air: the athmosphere along the view; glow: the sun's share of the view
	const float verticality = powf(fabsf(dir.z * 0.5f + 0.5f), 3.0f);
	const float air = powf(fabsf(1.0f - verticality), 8.0f) * skyData.x;
	const float alignment = (dir.x * sunPos.x + dir.y * sunPos.y + dir.z * sunPos.z) * 0.5f + 0.5f;
	const float glow = powf(fabsf(alignment), 1.0f / skyData.y);

	D3DXVECTOR3 color = skyLowC + (skyC - skyLowC) * verticality;
	const float horizonBlend = std::clamp(air * (0.5f + glow * 0.5f), 0.0f, 1.0f);
	color = color + (horizonC - color) * horizonBlend;
	color += sunColor * (glow * (1.0f - sunHeight) * air * skyData.z * isDayTime);
	return color;
}

// Adds one radiance sample's share to the nine spherical-harmonic coefficients, for the direction d (its basis factors).
static void ProjectSkySample(D3DXVECTOR3* coefficients, const D3DXVECTOR3& radiance, const D3DXVECTOR3& d) {
	const float factors[9] = {
		0.282095f,
		0.488603f * d.y,
		0.488603f * d.z,
		0.488603f * d.x,
		1.092548f * d.x * d.y,
		1.092548f * d.y * d.z,
		0.315392f * (3.0f * d.z * d.z - 1.0f),
		1.092548f * d.x * d.z,
		0.546274f * (d.x * d.x - d.y * d.y),
	};
	for (int k = 0; k < 9; k++) coefficients[k] += radiance * factors[k];
}

void SkyShaders::UpdateConstants() {
	// Also: the effect sets its 'hide the game's cloud layers' flags in its own UpdateConstants, which does not run while it is
	// off, so switching it off in the menu left the flags at 1 and the sky with no clouds at all until a restart. Cleared here.
	if (VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds) {
		if (!clouds->Enabled) clouds->HideGameClouds = clouds->HideGameCloudsInReflection = 0.0f;
	}

Constants.SunsetColor.w = TheShaderManager->Shaders.Tonemapping->Enabled
		? TheShaderManager->GetTransitionValue(Settings.SkyMultiplierDay, Settings.SkyMultiplierNight, 1.0)
		: 1.0;

	// UNOFFICIAL 2026-10-25 TintDay / TintNight: the sky's colour multiplied by the tint for this hour (the same day/night curve
	// as SkyMultiplier), on screen (SKY.pso, and the sky the flat clouds blend with in SKYTEX.pso) and in the sky light on objects
	// below. MoonBrightness: the moon disc (SKYTEX.pso); its light is UpdateMoonlight's.
	// UNOFFICIAL 2026-10-26: TintHorizonDay / TintHorizonNight on top of it near the horizon, fading out by TintGradientHeight, and
	// SaturationDay / SaturationNight (Shaders/Includes/Sky.hlsl SkyLook). TintKeepsBrightness divides each tint by its own
	// brightness, so a paler or deeper colour on the wheel changes the sky's colour, not how bright it is.
	for (int c = 0; c < 3; c++) {
		Constants.SkyTint[c] = TheShaderManager->GetTransitionValue(Settings.TintDay[c], Settings.TintNight[c], 1.0f);
		Constants.SkyTintHorizon[c] = TheShaderManager->GetTransitionValue(Settings.TintHorizonDay[c], Settings.TintHorizonNight[c], 1.0f);
	}
	if (Settings.TintKeepsBrightness) {
		for (D3DXVECTOR4* tint : { &Constants.SkyTint, &Constants.SkyTintHorizon }) {
			const float luminance = 0.2126f * tint->x + 0.7152f * tint->y + 0.0722f * tint->z;
			if (luminance > 1e-4f) { tint->x /= luminance; tint->y /= luminance; tint->z /= luminance; }
		}
	}
	Constants.SkyLook.x = TheShaderManager->GetTransitionValue(Settings.SaturationDay, Settings.SaturationNight, 1.0f);
	Constants.SkyLook.y = Settings.TintGradientHeight;
	// UNOFFICIAL 2026-10-28 ColorLook (user: "maybe a hard toggle for all of the sky color settings"; macintroll: a pink horizon that the
	// colour wheels did not change): off, the tints and the saturation are left out everywhere they go -- the sky, the sky light on objects
	// below, the fog's sky colours (FogSkyLook) -- and the sunset colour boost too (UpdateSettings), so the sky is the weather's / the
	// physical sky's own. On: as before.
	if (!Settings.ColorLook) {
		Constants.SkyTint = D3DXVECTOR4(1.0f, 1.0f, 1.0f, Constants.SkyTint.w);
		Constants.SkyTintHorizon = D3DXVECTOR4(1.0f, 1.0f, 1.0f, Constants.SkyTintHorizon.w);
		Constants.SkyLook.x = 1.0f;
	}
	// the fog lies over the sky near the horizon and takes the sky's colour there: VolumetricFog.fx.hlsl, one register
	Constants.FogSkyLook = D3DXVECTOR4(Constants.SkyTint.x * Constants.SkyTintHorizon.x, Constants.SkyTint.y * Constants.SkyTintHorizon.y,
		Constants.SkyTint.z * Constants.SkyTintHorizon.z, Constants.SkyLook.x);
	Constants.MoonData.x = Settings.MoonBrightness;

	// Order-2 spherical harmonic projection of the sky, convolved with the clamped-cosine
	// kernel: E(N) = INTEGRAL L(w) max(N.w, 0) dw. The cosine kernel is a severe low-pass
	// filter, so 9 coefficients carry that integral to within about 1% (Ramamoorthi & Hanrahan
	// 2001), and the kernel has only three non-zero bands, A0 = pi, A1 = 2pi/3, A2 = pi/4.
	//
	// Integrated in LINEAR radiance, which is what the integral is defined on. The shader
	// encodes the reconstruction, because the object and terrain shaders carry no gamma
	// conversion and their ambient is gamma-encoded.
	//
	// Everything the ambient used to fake falls out of this: the wall/floor split, the sun-side
	// azimuthal bias, the cosine form factor, and the fact that a floor integrates the whole
	// dome while a wall integrates half. No directionality parameter is needed or meaningful.
	//
	// Radiance below the horizon is ZERO -- no ground bounce is modelled. That reproduces the
	// old form factor exactly: a floor sees the full dome, a wall half of it, a ceiling none.
	// UNOFFICIAL: skip the projection when nothing it reads has moved since the last run (Sky.h). Every input but the sun
	// direction must match exactly; the sun may have turned by up to 0.1 degrees (the sun disc itself is half a degree).
	// UNOFFICIAL 2026-10-18 [Shaders.Sky.Physical] ExactSunlight / MultipleScattering (SkyView.fx.hlsl techniques 3 and 4): the sky-view table takes
	// the sun's light from a transmittance table and, with MultipleScattering, adds the light scattered more than once. This function's sky model
	// (the brightness match and the sky light on objects) then runs on the CPU twin of the same tables (PhysSkyModel.h); with both off nothing
	// below changes (PhysAtmosphere, the closed form, as before).
	const bool physMs = PhysicalSettings.Enabled && PhysicalSettings.MultipleScattering;
	const bool physExact = PhysicalSettings.Enabled && (PhysicalSettings.ExactSunlight || PhysicalSettings.MultipleScattering);
	{
		PBRShaders* pbrForKey = TheShaderManager->Shaders.PBR;
		ProjectionInputs inputs = { TheShaderManager->ShaderConst.SunPosition, TheShaderManager->ShaderConst.skyColor,
			TheShaderManager->ShaderConst.skyLowColor, TheShaderManager->ShaderConst.horizonColor, Constants.SkyData,
			TheShaderManager->ShaderConst.sunDiskColor, Constants.SunsetColor,
			D3DXVECTOR4(PhysicalSettings.Enabled ? 1.0f : 0.0f, PhysicalSettings.Brightness, PhysicalSettings.WeatherTint, PhysicalSettings.Haze),
			TheShaderManager->ShaderConst.SunAmount.x,
			pbrForKey ? max(pbrForKey->SkylightSaturation, 0.0f) : 1.0f, TheShaderManager->GameState.isExterior ? 1.0f : 0.0f,
			D3DXVECTOR4(physExact ? 1.0f : 0.0f, physMs ? 1.0f : 0.0f, PhysicalSettings.GroundAlbedo, 0.0f), Constants.SkyTint,
			Constants.SkyTintHorizon, Constants.SkyLook };
		inputs.sunsetColor.w = 0.0f;	// only rgb is read below
		const bool sunStill = lastProjectionValid &&
			inputs.sunPos.x * lastProjection.sunPos.x + inputs.sunPos.y * lastProjection.sunPos.y + inputs.sunPos.z * lastProjection.sunPos.z >= 0.9999985f;
		if (sunStill) inputs.sunPos = lastProjection.sunPos;
		if (lastProjectionValid && memcmp(&inputs, &lastProjection, sizeof(inputs)) == 0) return;
		lastProjection = inputs;
		lastProjectionValid = true;
	}
	JoinProjection();	// UNOFFICIAL 2026-11-03 SkyOnWorker: a job still running reads the tables, and its job struct is reused below

	const D3DXVECTOR4& sunPos = TheShaderManager->ShaderConst.SunPosition;
	D3DXVECTOR3 skyC = Linearize(TheShaderManager->ShaderConst.skyColor);
	D3DXVECTOR3 skyLowC = Linearize(TheShaderManager->ShaderConst.skyLowColor);
	D3DXVECTOR3 horizonC = Linearize(TheShaderManager->ShaderConst.horizonColor);

	float sunHeight = max(sunPos.z, 0.0f);   // shade(sunPos, up)
	float dayTime = TheShaderManager->ShaderConst.SunAmount.x;
	float isDayTime = std::clamp(dayTime / 0.5f, 0.0f, 1.0f);
	isDayTime = isDayTime * isDayTime * (3.0f - 2.0f * isDayTime);   // smoothstep(0, 0.5, x)

	D3DXVECTOR3 sunDisk = Linearize(TheShaderManager->ShaderConst.sunDiskColor);
	D3DXVECTOR3 sunsetC = Linearize(Constants.SunsetColor);
	float sunSet = std::clamp(powf(1.0f - sunHeight, 8.0f), 0.0f, 1.0f) * dayTime;
	D3DXVECTOR3 sunColor = (1.0f + sunHeight) * sunDisk + sunsetC * (sunSet * Constants.SkyData.x);

	// UNOFFICIAL physical sky ([Shaders.Sky.Physical]): its share is full down to a sun 4 degrees below the horizon and fades out by
	// 10 degrees below (TEST 2026-11-04 belt of Venus; it was +-2.9 degrees round the horizon), so civil twilight is the air's own:
	// the earth's shadow rising opposite the sun with the pink band of the belt of Venus above it (with ExactSunlight or
	// MultipleScattering on; the closed form has no sharp shadow), and the game's own night after nautical twilight; outdoors only. Its brightness is matched to the weather's sky: the
	// average luminance over the dome, both measured on the same samples as the projection below, which then projects
	// the blend that SKY.pso shows, so the sky light on objects comes from the sky on screen.
	{
		D3DXVECTOR3 realSun(sunPos.x, sunPos.y, sunPos.z);
		if (!(TheShaderManager->GameState.isExterior && TheShaderManager->GameState.dayLight > 0.5f)) realSun.z = -realSun.z;	// ShaderManager turns it upward
		const float shareEnd = -0.173648f, shareFull = -0.069756f;	// sin(-10 deg), sin(-4 deg)
		float share = std::clamp((realSun.z - shareEnd) / (shareFull - shareEnd), 0.0f, 1.0f);
		share = share * share * (3.0f - 2.0f * share);
		if (!PhysicalSettings.Enabled || !TheShaderManager->GameState.isExterior) share = 0.0f;
		Constants.PhysSky = D3DXVECTOR4(share, 1.0f, PhysicalSettings.WeatherTint, PhysicalSettings.Haze);
		Constants.PhysSkySun = D3DXVECTOR4(realSun.x, realSun.y, realSun.z, physMs ? PhysicalSettings.GroundAlbedo : 0.0f);	// w: the ground's albedo for SkyView.fx.hlsl's SkyMS table
	}
	const bool physical = Constants.PhysSky.x > 0.0f;
	if (physical && physExact && !physTables.Matches(PhysicalSettings.Haze, PhysicalSettings.GroundAlbedo, physMs)) {
		// fixes-a #50: the rebuild takes 65-73 ms on the render thread and a menu drag changes Haze or GroundAlbedo every frame, so
		// when the old tables can stand in (built, and with the MS part if MS is wanted) it waits until the wanted values have held
		// still for 10 rendered frames. The projection is asked to run again meanwhile (its change check would otherwise skip it).
		static float wantHaze = -1.0f, wantAlbedo = -1.0f;
		static bool wantMs = false;
		static unsigned wantSince = 0;
		const unsigned now = TheFrameRateManager->FrameNumber;
		if (wantHaze != PhysicalSettings.Haze || wantAlbedo != PhysicalSettings.GroundAlbedo || wantMs != physMs) {
			wantHaze = PhysicalSettings.Haze; wantAlbedo = PhysicalSettings.GroundAlbedo; wantMs = physMs; wantSince = now;
		}
		const bool oldTablesUsable = physTables.built && (!physMs || physTables.hasMs);
		if (!oldTablesUsable || now - wantSince >= 10)
			physTables.Build(PhysicalSettings.Haze, PhysicalSettings.GroundAlbedo, physMs);	// about 60 ms, once, when a setting changed
		else
			lastProjectionValid = false;
	}
	// UNOFFICIAL 2026-11-03 [Main.Main.Performance] SkyOnWorker (cpu-workers, core/FrameWorker.h): the projection below is plain arithmetic
	// on values copied here, so it can run on the worker thread while the main thread goes on with the frame (shadow maps, the world's
	// draws); the results reach Constants at JoinProjection, before anything reads them (SkyProjection::Join: the shader and effect
	// constant uploads, the effect chains, the end of the frame). Off: the same function, here, at once (as before).
	ProjectionJob& job = projectionJob;
	job.sunPos = sunPos; job.skyC = skyC; job.skyLowC = skyLowC; job.horizonC = horizonC; job.sunColor = sunColor;
	job.sunHeight = sunHeight; job.isDayTime = isDayTime;
	job.physical = physical; job.physExact = physExact; job.physMs = physMs; job.tables = &physTables;
	job.settings = { PhysicalSettings.Haze, PhysicalSettings.Brightness, PhysicalSettings.LightsObjects };
	PBRShaders* pbr = TheShaderManager->Shaders.PBR;
	job.saturation = pbr ? (pbr->SkylightSaturation > 0.0f ? pbr->SkylightSaturation : 0.0f) : 1.0f;	// (the max of the Windows macro)
	ProjectionJob::Values& values = job.values;
	values.SkyData = Constants.SkyData; values.PhysSky = Constants.PhysSky; values.PhysSkySun = Constants.PhysSkySun;
	values.SkyTint = Constants.SkyTint; values.SkyTintHorizon = Constants.SkyTintHorizon; values.SkyLook = Constants.SkyLook;
	if (TheSettingManager->SettingsMain.Main.SkyOnWorker) {
		FrameWorker::Start(&SkyShaders::RunProjectionParallel, &job);
		projectionPending = SkyProjection::Pending = true;
	}
	else {
		RunProjection(&job);
		CopyProjection();
	}
}

// The convolution of the two projections into the job's results (both RunProjection and RunProjectionParallel end here).
static void ConvolveProjection(SkyShaders::ProjectionJob& j, const D3DXVECTOR3 (&sh)[9], const D3DXVECTOR3 (&shGame)[9]) {
	const bool physical = j.physical;
	const float saturation = j.saturation;
	const SkyShaders::ProjectionJob::Settings& PhysicalSettings = j.settings;
	SkyShaders::ProjectionJob::Values& Constants = j.values;
	// Convolve, and fold in each basis function's constant plus the 1/pi that converts
	// irradiance back to an average radiance -- the units the ambient path already works in, so
	// the existing scales keep their meaning. A0/pi = 1, A1/pi = 2/3, A2/pi = 1/4.
	const float A0 = 1.0f, A1 = 2.0f / 3.0f, A2 = 0.25f;
	const float bandWeight[9] = { A0, A1, A1, A1, A2, A2, A2, A2, A2 };
	const float basisNorm[9] = { 0.282095f, 0.488603f, 0.488603f, 0.488603f,
		1.092548f, 1.092548f, 0.315392f, 1.092548f, 0.546274f };

	// UNOFFICIAL [Shaders.PBR.*] SkylightingSaturation: saturation is linear in the colour (a blend with its own
	// luminance), so applying it to each coefficient equals applying it to the reconstructed irradiance, at no GPU
	// cost. 1 leaves the coefficients untouched.
	// (the sky tint, horizon tint and saturation are already in the samples: SkyLook above)
	for (int k = 0; k < 9; k++) {
		const float scale = bandWeight[k] * basisNorm[k];
		D3DXVECTOR3 coefficient = sh[k] * scale;
		// UNOFFICIAL 2026-10-18 ClassicLook: the original NVR's coefficients (the game's sky, unsaturated); without the physical sky
		// blending in, the same projection as below.
		const D3DXVECTOR3 gameCoefficient = physical ? shGame[k] * scale : coefficient;
		Constants.IrradianceGame[k] = D3DXVECTOR4(gameCoefficient.x, gameCoefficient.y, gameCoefficient.z, 0.0f);
		if (saturation != 1.0f) {
			const float luma = 0.2126f * coefficient.x + 0.7152f * coefficient.y + 0.0722f * coefficient.z;
			const D3DXVECTOR3 gray(luma, luma, luma);
			coefficient = gray + (coefficient - gray) * saturation;
		}
		Constants.Irradiance[k] = D3DXVECTOR4(coefficient.x, coefficient.y, coefficient.z, 0.0f);
		// UNOFFICIAL 2026-10-25 LightsObjects off (the physical sky's light on surfaces double-dips with the terrain / PBR sky
		// settings): the lighting shaders take the game's own sky, saturated as before; the physical sky stays in the sky, fog and clouds.
		D3DXVECTOR3 objectCoefficient = gameCoefficient;
		if (!PhysicalSettings.LightsObjects && physical && saturation != 1.0f) {
			const float luma = 0.2126f * objectCoefficient.x + 0.7152f * objectCoefficient.y + 0.0722f * objectCoefficient.z;
			const D3DXVECTOR3 gray(luma, luma, luma);
			objectCoefficient = gray + (objectCoefficient - gray) * saturation;
		}
		Constants.IrradianceObjects[k] = PhysicalSettings.LightsObjects || !physical ? Constants.Irradiance[k]
			: D3DXVECTOR4(objectCoefficient.x, objectCoefficient.y, objectCoefficient.z, 0.0f);
	}
}

// The projection itself (UpdateConstants above hands it its inputs in the job): on the worker thread with SkyOnWorker on, so it touches
// nothing but the job (the tables are read only; UpdateConstants joins before it rebuilds them).
void SkyShaders::RunProjection(void* arg) {
	ProjectionJob& j = *static_cast<ProjectionJob*>(arg);
	// (the names the code below was written with, on the main thread)
	const D3DXVECTOR4& sunPos = j.sunPos;
	const D3DXVECTOR3 &skyC = j.skyC, &skyLowC = j.skyLowC, &horizonC = j.horizonC, &sunColor = j.sunColor;
	const float sunHeight = j.sunHeight, isDayTime = j.isDayTime, saturation = j.saturation;
	const bool physical = j.physical, physExact = j.physExact, physMs = j.physMs;
	const PhysSky::Tables& physTables = *j.tables;
	const ProjectionJob::Settings& PhysicalSettings = j.settings;
	ProjectionJob::Values& Constants = j.values;
	D3DXVECTOR3* const physSamples = j.physSamples;
	// Upper hemisphere only; the lower half contributes nothing, so it is skipped rather than
	// evaluated. Order-2 SH is smooth enough that this stratification is ample.
	const int STEPS_T = 16, STEPS_P = 32;
	const float dTheta = (D3DX_PI * 0.5f) / STEPS_T;
	const float dPhi = (2.0f * D3DX_PI) / STEPS_P;

	D3DXVECTOR3 sh[9], shGame[9];	// (shGame: UNOFFICIAL ClassicLook, filled only while the physical sky blends in)
	for (int i = 0; i < 9; i++) sh[i] = shGame[i] = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	if (physical) {
		const D3DXVECTOR3 realSun(Constants.PhysSkySun.x, Constants.PhysSkySun.y, Constants.PhysSkySun.z);
		for (int i = 0; i < STEPS_T / 2; i++) {
			const float theta = (2 * i + 1) * dTheta, st = sinf(theta), ct = cosf(theta);
			for (int j = 0; j < STEPS_P / 2; j++) {
				const float phi = (2 * j + 1) * dPhi;
				D3DXVECTOR3 physDir(st * cosf(phi), st * sinf(phi), (std::max)(ct, 0.002f));
				D3DXVec3Normalize(&physDir, &physDir);
				if (physExact) {
					const PhysSky::V3 v = physTables.Radiance(PhysSky::V3(physDir.x, physDir.y, physDir.z), PhysSky::V3(realSun.x, realSun.y, realSun.z), 300.0f, PhysicalSettings.Haze, physMs);
					physSamples[i * (STEPS_P / 2) + j] = D3DXVECTOR3(v.x, v.y, v.z);
				}
				else physSamples[i * (STEPS_P / 2) + j] = PhysAtmosphere::Radiance(physDir, realSun, 300.0f, PhysicalSettings.Haze);
			}
		}
		float gameSum = 0.0f, physSum = 0.0f;
		for (int i = 0; i < STEPS_T; i++) {
			const float theta = (i + 0.5f) * dTheta, st = sinf(theta), ct = cosf(theta), dOmega = st * dTheta * dPhi;
			for (int j = 0; j < STEPS_P; j++) {
				const float phi = (j + 0.5f) * dPhi;
				const D3DXVECTOR3 d(st * cosf(phi), st * sinf(phi), ct);
				physSum += PhysAtmosphere::Luma(physSamples[(i / 2) * (STEPS_P / 2) + j / 2]) * dOmega;
				gameSum += PhysAtmosphere::Luma(EvalSky(d, sunPos, Constants.SkyData, skyC, skyLowC, horizonC, sunColor, sunHeight, isDayTime)) * dOmega;
			}
		}
		Constants.PhysSky.y = physSum > 1e-12f ? PhysicalSettings.Brightness * gameSum / physSum : 0.0f;
	}

	// The projection itself: every cell of the upper hemisphere, weighted by its solid angle.
	for (int row = 0; row < STEPS_T; row++) {
		const float theta = (row + 0.5f) * dTheta;
		const float sinTheta = sinf(theta), cosTheta = cosf(theta);
		const float cellSolidAngle = sinTheta * dTheta * dPhi;
		for (int col = 0; col < STEPS_P; col++) {
			const float phi = (col + 0.5f) * dPhi;
			const D3DXVECTOR3 d(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);

			// LINEAR radiance, as GetSkyColor returns and as the integral requires. The shader
			// encodes on reconstruction. Integrating encoded values instead was measurably
			// wrong -- it cost walls about a third of their brightness, since their lobe spans
			// the widest range of sky and a concave curve pulls an average of spread values
			// down hardest.
			D3DXVECTOR3 radiance = EvalSky(d, sunPos, Constants.SkyData, skyC, skyLowC, horizonC, sunColor, sunHeight, isDayTime);
			if (physical) {
				// UNOFFICIAL 2026-10-18 ClassicLook: the game's own sky projected as well (IrradianceGame), as the original NVR had it.
				const D3DXVECTOR3 gameSample = SkyLook(radiance, d.z, Constants.SkyTint, Constants.SkyTintHorizon, Constants.SkyLook) * cellSolidAngle;
				ProjectSkySample(shGame, gameSample, d);
				// UNOFFICIAL physical sky: the blend SKY.pso shows, from the same samples the brightness match used.
				const D3DXVECTOR3 physicalSample = physSamples[(row / 2) * (STEPS_P / 2) + col / 2] * Constants.PhysSky.y;
				radiance = PhysAtmosphere::Blend(radiance, physicalSample, Constants.PhysSky);
			}
			radiance = SkyLook(radiance, d.z, Constants.SkyTint, Constants.SkyTintHorizon, Constants.SkyLook) * cellSolidAngle;	// UNOFFICIAL 2026-10-25/26, as SKY.pso
			ProjectSkySample(sh, radiance, d);
		}
	}

	ConvolveProjection(j, sh, shGame);
}

// UNOFFICIAL 2026-11-03 SkyOnWorker, the job as it runs on the worker: the same samples and sums as RunProjection, but the samples that
// do not depend on each other -- the physical sky's 128 rays and the game sky's colour in each of the 512 cells -- are worked out on the
// worker and its helpers (FrameWorker::ParallelFor), each into its own slot; everything summed is then summed here, in RunProjection's
// order, from those slots. The expressions are RunProjection's own, so the results are its results bit for bit (SkyOnWorkerCheck).
static const int kSkySteps = 16, kSkyCells = 16 * 32, kSkyRays = (16 / 2) * (32 / 2);
static void ProjectionItem(void* arg, int item) {
	SkyShaders::ProjectionJob& j = *static_cast<SkyShaders::ProjectionJob*>(arg);
	const int STEPS_P = 32;
	const float dTheta = (D3DX_PI * 0.5f) / kSkySteps, dPhi = (2.0f * D3DX_PI) / STEPS_P;
	if (item < kSkyRays) {
		if (!j.physical) return;
		const int i = item / (STEPS_P / 2), k = item % (STEPS_P / 2);
		const D3DXVECTOR3 realSun(j.values.PhysSkySun.x, j.values.PhysSkySun.y, j.values.PhysSkySun.z);
		const float theta = (2 * i + 1) * dTheta, st = sinf(theta), ct = cosf(theta);
		const float phi = (2 * k + 1) * dPhi;
		D3DXVECTOR3 physDir(st * cosf(phi), st * sinf(phi), (std::max)(ct, 0.002f));
		D3DXVec3Normalize(&physDir, &physDir);
		if (j.physExact) {
			const PhysSky::V3 v = j.tables->Radiance(PhysSky::V3(physDir.x, physDir.y, physDir.z), PhysSky::V3(realSun.x, realSun.y, realSun.z), 300.0f, j.settings.Haze, j.physMs);
			j.physSamples[item] = D3DXVECTOR3(v.x, v.y, v.z);
		}
		else j.physSamples[item] = PhysAtmosphere::Radiance(physDir, realSun, 300.0f, j.settings.Haze);
		return;
	}
	const int cell = item - kSkyRays, row = cell / STEPS_P, col = cell % STEPS_P;
	const float theta = (row + 0.5f) * dTheta;
	const float sinTheta = sinf(theta), cosTheta = cosf(theta);
	const float phi = (col + 0.5f) * dPhi;
	const D3DXVECTOR3 d(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);
	j.cellDir[cell] = d;
	j.cellRadiance[cell] = EvalSky(d, j.sunPos, j.values.SkyData, j.skyC, j.skyLowC, j.horizonC, j.sunColor, j.sunHeight, j.isDayTime);
}

void SkyShaders::RunProjectionParallel(void* arg) {
	ProjectionJob& j = *static_cast<ProjectionJob*>(arg);
	FrameWorker::ParallelFor(kSkyRays + kSkyCells, &ProjectionItem, &j);
	const bool physical = j.physical;
	const ProjectionJob::Settings& PhysicalSettings = j.settings;
	ProjectionJob::Values& Constants = j.values;
	const D3DXVECTOR3* const physSamples = j.physSamples;
	const int STEPS_T = kSkySteps, STEPS_P = 32;
	const float dTheta = (D3DX_PI * 0.5f) / STEPS_T;
	const float dPhi = (2.0f * D3DX_PI) / STEPS_P;

	D3DXVECTOR3 sh[9], shGame[9];
	for (int i = 0; i < 9; i++) sh[i] = shGame[i] = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	if (physical) {	// the brightness match, summed in RunProjection's order
		float gameSum = 0.0f, physSum = 0.0f;
		for (int i = 0; i < STEPS_T; i++) {
			const float theta = (i + 0.5f) * dTheta, st = sinf(theta), dOmega = st * dTheta * dPhi;
			for (int k = 0; k < STEPS_P; k++) {
				physSum += PhysAtmosphere::Luma(physSamples[(i / 2) * (STEPS_P / 2) + k / 2]) * dOmega;
				gameSum += PhysAtmosphere::Luma(j.cellRadiance[i * STEPS_P + k]) * dOmega;
			}
		}
		Constants.PhysSky.y = physSum > 1e-12f ? PhysicalSettings.Brightness * gameSum / physSum : 0.0f;
	}
	for (int row = 0; row < STEPS_T; row++) {	// the projection, in RunProjection's order
		const float theta = (row + 0.5f) * dTheta;
		const float sinTheta = sinf(theta);
		const float cellSolidAngle = sinTheta * dTheta * dPhi;
		for (int col = 0; col < STEPS_P; col++) {
			const D3DXVECTOR3& d = j.cellDir[row * STEPS_P + col];
			D3DXVECTOR3 radiance = j.cellRadiance[row * STEPS_P + col];
			if (physical) {
				const D3DXVECTOR3 gameSample = SkyLook(radiance, d.z, Constants.SkyTint, Constants.SkyTintHorizon, Constants.SkyLook) * cellSolidAngle;
				ProjectSkySample(shGame, gameSample, d);
				const D3DXVECTOR3 physicalSample = physSamples[(row / 2) * (STEPS_P / 2) + col / 2] * Constants.PhysSky.y;
				radiance = PhysAtmosphere::Blend(radiance, physicalSample, Constants.PhysSky);
			}
			radiance = SkyLook(radiance, d.z, Constants.SkyTint, Constants.SkyTintHorizon, Constants.SkyLook) * cellSolidAngle;
			ProjectSkySample(sh, radiance, d);
		}
	}
	ConvolveProjection(j, sh, shGame);
}

// The job's results into Constants (main thread). PhysSky.y is the job's only when the physical sky blends in (else the 1 UpdateConstants set).
void SkyShaders::CopyProjection() {
	const ProjectionJob::Values& values = projectionJob.values;
	if (projectionJob.physical) Constants.PhysSky.y = values.PhysSky.y;
	memcpy(Constants.Irradiance, values.Irradiance, sizeof(Constants.Irradiance));
	memcpy(Constants.IrradianceGame, values.IrradianceGame, sizeof(Constants.IrradianceGame));
	memcpy(Constants.IrradianceObjects, values.IrradianceObjects, sizeof(Constants.IrradianceObjects));
}

void SkyShaders::JoinProjection() {
	if (!projectionPending) return;
	FrameWorker::Wait();
	projectionPending = SkyProjection::Pending = false;
	CopyProjection();
#if NVR_TEST_TOOLS
	// SkyOnWorkerCheck (test only): the job run again here, on the main thread, and its results compared bit for bit
	if (TheSettingManager->GetSettingI("Main.Main.Performance", "SkyOnWorkerCheck")) {
		static unsigned same = 0, differ = 0;
		static ProjectionJob again;	// (static: ~16 KB)
		again = projectionJob;
		RunProjection(&again);	// the old serial function, here
		if (!memcmp(&again.values, &projectionJob.values, sizeof(again.values))) same++; else differ++;
		if (same + differ >= 120) { Logger::Log("SKY ON WORKER CHECK: %u projections identical, %u differ", same, differ); same = differ = 0; }
	}
#endif
}

void SkyShaders::UpdateSettings() {
	const char* const skySection = "Shaders.Sky.Main";
	const char* const physicalSection = "Shaders.Sky.Physical";

	useSunDiskColor = TheSettingManager->GetSettingF(skySection, "UseSunDiskColor");

	Settings.SkyMultiplierDay = TheSettingManager->GetSettingF("Shaders.Tonemapping.Main", "SkyMultiplier");
	Settings.SkyMultiplierNight = TheSettingManager->GetSettingF("Shaders.Tonemapping.Night", "SkyMultiplier");

	Constants.SkyData.x = TheSettingManager->GetSettingF(skySection, "AthmosphereThickness");
	Constants.SkyData.y = TheSettingManager->GetSettingF(skySection, "SunInfluence");
	Constants.SkyData.z = TheSettingManager->GetSettingF(skySection, "SunStrength");
	Constants.SkyData.w = TheSettingManager->GetSettingF(skySection, "StarStrength");

	const char* const cloudsSection = "Shaders.Sky.Clouds";
	Constants.CloudData.x = TheSettingManager->GetSettingF(cloudsSection, "UseNormals");
	//Constants.CloudData.y = TheSettingManager->GetSettingF(cloudsSection, "SphericalNormals"); // not used much so it's disabled
	Constants.CloudData.z = TheSettingManager->GetSettingF(cloudsSection, "Transparency");
	Constants.CloudData.w = TheSettingManager->GetSettingF(cloudsSection, "Brightness");
	Constants.CloudData.y = TheSettingManager->GetSettingF(skySection, "StarTwinkle");

	// Settings that may be missing from an older defaults file read as their defaults (SettingOr), colours never below zero.
	// UNOFFICIAL 2026-10-25 sky tint and moon brightness (applied in UpdateConstants / UpdateMoonlight)
	const auto tintChannel = [skySection](const char* key, float fallback) { return (std::max)(SettingOr(skySection, key, fallback), 0.0f); };
	Settings.TintDay = D3DXVECTOR4(tintChannel("TintDayR", 1.0f), tintChannel("TintDayG", 1.0f), tintChannel("TintDayB", 1.0f), 1.0f);
	Settings.TintNight = D3DXVECTOR4(tintChannel("TintNightR", 1.0f), tintChannel("TintNightG", 1.0f), tintChannel("TintNightB", 1.0f), 1.0f);
	Settings.MoonBrightness = std::clamp(SettingOr(skySection, "MoonBrightness", 1.0f), 0.0f, 4.0f);
	// UNOFFICIAL 2026-10-26 horizon tint, gradient height, saturation, brightness-keeping tints (applied in UpdateConstants)
	Settings.TintHorizonDay = D3DXVECTOR4(tintChannel("TintHorizonDayR", 1.0f), tintChannel("TintHorizonDayG", 1.0f), tintChannel("TintHorizonDayB", 1.0f), 1.0f);
	Settings.TintHorizonNight = D3DXVECTOR4(tintChannel("TintHorizonNightR", 1.0f), tintChannel("TintHorizonNightG", 1.0f), tintChannel("TintHorizonNightB", 1.0f), 1.0f);
	Settings.TintGradientHeight = std::clamp(SettingOr(skySection, "TintGradientHeight", 0.3f), 0.01f, 1.0f);
	Settings.SaturationDay = std::clamp(SettingOr(skySection, "SaturationDay", 1.0f), 0.0f, 2.0f);
	Settings.SaturationNight = std::clamp(SettingOr(skySection, "SaturationNight", 1.0f), 0.0f, 2.0f);
	Settings.TintKeepsBrightness = SettingOr(skySection, "TintKeepsBrightness", 1.0f) != 0.0f;
	Settings.ColorLook = SettingOr(skySection, "ColorLook", 1.0f) != 0.0f;	// UNOFFICIAL 2026-10-28 (missing key = on)

	// UNOFFICIAL physical sky (Effects/Includes/Atmosphere.hlsl). Enabled is read at startup too (ShaderRecord compiles it in).
	auto& physical = PhysicalSettings;
	physical.Enabled = TheSettingManager->GetSettingI(physicalSection, "Enabled") != 0;
	physical.Brightness = std::clamp(TheSettingManager->GetSettingF(physicalSection, "Brightness"), 0.0f, 4.0f);
	physical.WeatherTint = std::clamp(TheSettingManager->GetSettingF(physicalSection, "WeatherTint"), 0.0f, 1.0f);
	physical.Haze = std::clamp(TheSettingManager->GetSettingF(physicalSection, "Haze"), 0.0f, 10.0f);
	physical.ExactSunlight = TheSettingManager->GetSettingI(physicalSection, "ExactSunlight") != 0;	// UNOFFICIAL 2026-10-18
	physical.MultipleScattering = TheSettingManager->GetSettingI(physicalSection, "MultipleScattering") != 0;
	physical.GroundAlbedo = std::clamp(TheSettingManager->GetSettingF(physicalSection, "GroundAlbedo"), 0.0f, 1.0f);
	physical.LightsObjects = TheSettingManager->GetSettingI(physicalSection, "LightsObjects") != 0;	// UNOFFICIAL 2026-10-25, live
	lastProjectionValid = false;

	// The sunset colour boost applies outdoors only, and only with ColorLook on (UNOFFICIAL 2026-10-28).
	if (TheShaderManager->GameState.isExterior && Settings.ColorLook) {
		Constants.SunsetColor.x = TheSettingManager->GetSettingF(skySection, "SunsetR");
		Constants.SunsetColor.y = TheSettingManager->GetSettingF(skySection, "SunsetG");
		Constants.SunsetColor.z = TheSettingManager->GetSettingF(skySection, "SunsetB");
	}
	else {
		Constants.SunsetColor.x = 0.0f;
		Constants.SunsetColor.y = 0.0f;
		Constants.SunsetColor.z = 0.0f;
	}
}

// UNOFFICIAL 2026-10-25 MoonBrightness: the light of the moon. At night the game's directional light (the sun's, dimmed and
// recoloured by the weather) is the moonlight; its colour is scaled here, before the world is drawn, so the game's own lighting
// shaders and NVR's effects (TESR_SunColor) light by the same moon. The game sets the light again whenever the weather or hour
// moves it: a colour that is not the one written last time is the game's new value (this runs twice a frame). Outdoors only, and
// by the day/night curve, so the sun and interiors are untouched; 1 writes the game's own colour back.
void SkyShaders::UpdateMoonlight() {
	static NiColor gameColor = {}, written = {};
	static bool haveWritten = false;
	NiDirectionalLight* light = Tes ? Tes->directionalLight : nullptr;
	if (!light) return;
	const float scale = TheShaderManager->GetTransitionValue(1.0f, Settings.MoonBrightness, 1.0f);
	if (!haveWritten || light->Diff.r != written.r || light->Diff.g != written.g || light->Diff.b != written.b) {
		if (scale == 1.0f) { haveWritten = false; return; }	// nothing of ours is in the light
		gameColor = light->Diff;
	}
	light->Diff.r = gameColor.r * scale;
	light->Diff.g = gameColor.g * scale;
	light->Diff.b = gameColor.b * scale;
	written = light->Diff;
	haveWritten = true;
	TheShaderManager->ShaderConst.sunColor.x *= scale;
	TheShaderManager->ShaderConst.sunColor.y *= scale;
	TheShaderManager->ShaderConst.sunColor.z *= scale;
}
