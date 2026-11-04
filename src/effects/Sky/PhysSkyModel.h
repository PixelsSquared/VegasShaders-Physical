#pragma once
// UNOFFICIAL 2026-10-18: the CPU twin of the physical sky's ExactSunlight / MultipleScattering path
// (Effects\SkyView.fx.hlsl techniques SkyTrans, SkyMS, SkyViewX, SkyViewXMS). Sky.cpp uses it for the brightness match and the
// sky light on objects, exactly as it uses PhysAtmosphere for the closed-form sky, so the sky on screen and the light it
// gives objects stay one model. Plain C++ (no D3DX), so tests\physsky_model.cpp builds the same code outside the game.
//
// Method: Bruneton and Neyret, "Precomputed Atmospheric Scattering", EGSR 2008 (transmittance table parameterisation) and
// Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020, CGF 39(4), DOI 10.1111/cgf.14050
// (multiple-scattering table Psi_ms = L2 / (1 - f_ms)); written from the papers, no code taken.
//
// The tables are coarser than the shader's (transmittance 128 x 32 with 2 x 24 points, multiple scattering 32 x 32 from 8 x 8
// directions and 8 steps instead of 256 x 64, 12 x 12, 12): about 60 ms to build once, within 1.1% of the shader's dome mean
// luminance (bench\deep-roadmap\evidence\skyms\cpucfg.py). Rebuilt only when Haze, GroundAlbedo or the switches change.
#include <cmath>
#include <vector>
#include <algorithm>

namespace PhysSky {

	struct V3 {
		float x, y, z;
		V3() : x(0), y(0), z(0) {}
		V3(float a, float b, float c) : x(a), y(b), z(c) {}
		V3 operator+(const V3& o) const { return V3(x + o.x, y + o.y, z + o.z); }
		V3 operator-(const V3& o) const { return V3(x - o.x, y - o.y, z - o.z); }
		V3 operator*(float s) const { return V3(x * s, y * s, z * s); }
		V3 operator*(const V3& o) const { return V3(x * o.x, y * o.y, z * o.z); }
		V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
	};
	inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline float Len(const V3& a) { return sqrtf(Dot(a, a)); }
	inline V3 Exp3(const V3& v) { return V3(expf(v.x), expf(v.y), expf(v.z)); }

	static const double Ground = 6360000.0, Top = 6460000.0, Hor = 1132254.388377453;
	static const V3 Rayleigh(5.802e-6f, 13.558e-6f, 33.1e-6f), Ozone(0.650e-6f, 1.881e-6f, 0.085e-6f);
	static const double RayleighH = 8000.0, MieH = 1200.0, MieScatter = 3.996e-6, MieExtinct = 4.44e-6, MieG = 0.8, Pi = 3.14159265358979;
	static const int TransW = 128, TransH = 32, MsN = 32, MsDirs = 8, MsSteps = 8, TransPts = 24;

	inline double Ozone01(double alt) { return (std::max)(0.0, 1.0 - fabs(alt - 25000.0) / 15000.0); }

	class Tables {
	public:
		bool built = false, hasMs = false;
		float haze = -1.0f, albedo = -1.0f;
		std::vector<float> trans;	// TransH x TransW x 3: optical depth (haze baked in)
		std::vector<float> ms;		// MsN x MsN x 3: Psi_ms (alt rows, sun-cosine columns)

		bool Matches(float h, float a, bool wantMs) const {
			return built && haze == h && (!wantMs || (hasMs && albedo == a));
		}

		void Build(float h, float a, bool wantMs) {
			haze = h; albedo = a;
			BuildTrans();
			hasMs = wantMs;
			if (wantMs) BuildMs();
			built = true;
		}

		// Optical depth (RGB) from radius r toward the sun, mu the sun's cosine from straight up there; the earth blocks -> huge.
		V3 SunOD(double r, double mu) const {
			r = (std::min)((std::max)(r, Ground), Top);
			const double g = Ground / r, muHorizon = -sqrt((std::max)(1.0 - g * g, 0.0));
			if (mu < muHorizon) return V3(1e4f, 1e4f, 1e4f);
			double u, v;
			ParamsToUV(r, mu, u, v);
			const double fx = u * TransW - 0.5, fy = v * TransH - 0.5;
			const int ix = (std::min)((std::max)((int)floor(fx), 0), TransW - 2), iy = (std::min)((std::max)((int)floor(fy), 0), TransH - 2);
			const float tx = (float)(std::min)((std::max)(fx - ix, 0.0), 1.0), ty = (float)(std::min)((std::max)(fy - iy, 0.0), 1.0);
			V3 o;
			float* c = &o.x;
			for (int k = 0; k < 3; k++) {
				const float a0 = T(iy, ix, k) * (1 - tx) + T(iy, ix + 1, k) * tx, a1 = T(iy + 1, ix, k) * (1 - tx) + T(iy + 1, ix + 1, k) * tx;
				c[k] = a0 * (1 - ty) + a1 * ty;
			}
			return o;
		}

		V3 Psi(double sunCos, double alt) const {
			const double fm = (std::min)((std::max)((sunCos + 1.0) * 0.5, 0.0), 1.0) * (MsN - 1), fa = (std::min)((std::max)(alt / (Top - Ground), 0.0), 1.0) * (MsN - 1);
			const int im = (std::min)((int)fm, MsN - 2), ia = (std::min)((int)fa, MsN - 2);
			const float tm = (float)(fm - im), ta = (float)(fa - ia);
			V3 o;
			float* c = &o.x;
			for (int k = 0; k < 3; k++) {
				const float a0 = M(ia, im, k) * (1 - tm) + M(ia, im + 1, k) * tm, a1 = M(ia + 1, im, k) * (1 - tm) + M(ia + 1, im + 1, k) * tm;
				c[k] = a0 * (1 - ta) + a1 * ta;
			}
			return o;
		}

		// SkyView.fx.hlsl AtmSkyX: 24 sqrt-spaced steps (TEST 2026-11-04 belt of Venus, was 12), the sun's depth from the table, optionally Psi_ms * sigma_s * T_view.
		V3 Radiance(const V3& dir, const V3& sunDir, float camAlt, float hz, bool withMs) const {
			const double r0 = Ground + camAlt, mu = dir.z;
			double tMax = -r0 * mu + sqrt((std::max)(r0 * r0 * (mu * mu - 1.0) + Top * Top, 0.0));
			const double disc = r0 * r0 * (mu * mu - 1.0) + Ground * Ground;
			if (mu < 0.0 && disc >= 0.0) tMax = -r0 * mu - sqrt(disc);
			const double nu = Dot(dir, sunDir), g2 = MieG * MieG;
			const float phaseR = (float)(3.0 / (16.0 * Pi) * (1.0 + nu * nu));
			const float phaseM = (float)(3.0 / (8.0 * Pi) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * pow((std::max)(1.0 + g2 - 2.0 * MieG * nu, 1e-4), 1.5)));
			V3 depth, sumR, sumM, sumMS;
			for (int i = 0; i < 24; i++) {
				const double s0 = i / 24.0, s1 = (i + 1) / 24.0, t0 = tMax * s0 * s0, t1 = tMax * s1 * s1, dt = t1 - t0;
				const double px = dir.x * 0.5 * (t0 + t1), py = dir.y * 0.5 * (t0 + t1), pz = r0 + dir.z * 0.5 * (t0 + t1);
				const double r = sqrt(px * px + py * py + pz * pz), alt = r - Ground;
				const float dR = (float)exp(-alt / RayleighH), dM = (float)exp(-alt / MieH), dO = (float)Ozone01(alt);
				const V3 ext = Rayleigh * dR + V3(1, 1, 1) * (float)(MieExtinct * hz * dM) + Ozone * dO;
				const double cosSun = (px * sunDir.x + py * sunDir.y + pz * sunDir.z) / r;
				const V3 view = depth + ext * (float)(0.5 * dt);
				const V3 t = Exp3((view + SunOD(r, cosSun)) * -1.0f);
				sumR += t * (float)(dR * dt);
				sumM += t * (float)(dM * dt);
				if (withMs) {
					const V3 sc = Rayleigh * dR + V3(1, 1, 1) * (float)(MieScatter * hz * dM);
					sumMS += Exp3(view * -1.0f) * Psi(cosSun, alt) * sc * (float)dt;
				}
				depth += ext * (float)dt;
			}
			return sumR * Rayleigh * phaseR + sumM * (float)(MieScatter * hz * phaseM) + sumMS;
		}

	private:
		float T(int y, int x, int k) const { return trans[((size_t)y * TransW + x) * 3 + k]; }
		float M(int y, int x, int k) const { return ms[((size_t)y * MsN + x) * 3 + k]; }

		static void ParamsToUV(double r, double mu, double& u, double& v) {
			const double rho = sqrt((std::max)(r * r - Ground * Ground, 0.0));
			const double d = -r * mu + sqrt((std::max)(r * r * (mu * mu - 1.0) + Top * Top, 0.0));
			const double dmin = Top - r, dmax = rho + Hor;
			const double xmu = (std::min)((std::max)((d - dmin) / (std::max)(dmax - dmin, 1.0), 0.0), 1.0), xr = (std::min)((std::max)(rho / Hor, 0.0), 1.0);
			u = 0.5 / TransW + xmu * (1.0 - 1.0 / TransW);
			v = 0.5 / TransH + xr * (1.0 - 1.0 / TransH);
		}

		void BuildTrans() {
			trans.assign((size_t)TransW * TransH * 3, 0.0f);
			for (int j = 0; j < TransH; j++) {
				const double xr = j / (TransH - 1.0), rho = xr * Hor, r = sqrt(rho * rho + Ground * Ground), dmin = Top - r, dmax = rho + Hor;
				for (int i = 0; i < TransW; i++) {
					const double xmu = i / (TransW - 1.0), d = dmin + xmu * (dmax - dmin);
					double mu = d < 1e-3 ? 1.0 : (Hor * Hor - rho * rho - d * d) / (2.0 * r * d);
					mu = (std::min)((std::max)(mu, -1.0), 1.0);
					const double dTop = -r * mu + sqrt((std::max)(r * r * (mu * mu - 1.0) + Top * Top, 0.0)), st = (std::max)(-r * mu, 0.0);
					double sR = 0, sM = 0, sO = 0;
					for (int seg = 0; seg < 2; seg++) {
						const double len = seg == 0 ? st : dTop - st, dir = seg == 0 ? -1.0 : 1.0;
						for (int k = 0; k < TransPts; k++) {
							const double v = (k + 0.5) / TransPts, s = st + dir * len * v * v * v, ds = 3.0 * v * v * (1.0 / TransPts) * len;
							const double rr = sqrt((std::max)(r * r + 2.0 * r * mu * s + s * s, 0.0)), a = (std::max)(rr - Ground, 0.0);
							sR += exp(-a / RayleighH) * ds; sM += exp(-a / MieH) * ds; sO += Ozone01(a) * ds;
						}
					}
					float* o = &trans[((size_t)j * TransW + i) * 3];
					o[0] = (float)(Rayleigh.x * sR + MieExtinct * haze * sM + Ozone.x * sO);
					o[1] = (float)(Rayleigh.y * sR + MieExtinct * haze * sM + Ozone.y * sO);
					o[2] = (float)(Rayleigh.z * sR + MieExtinct * haze * sM + Ozone.z * sO);
				}
			}
		}

		void BuildMs() {
			ms.assign((size_t)MsN * MsN * 3, 0.0f);
			const double omega = 4.0 * Pi / (MsDirs * MsDirs), iso = 1.0 / (4.0 * Pi), g2 = MieG * MieG;
			for (int j = 0; j < MsN; j++) {
				const double alt0 = j / (MsN - 1.0) * (Top - Ground), r0 = Ground + alt0;
				for (int i = 0; i < MsN; i++) {
					const double muS = -1.0 + 2.0 * i / (MsN - 1.0), sx = sqrt((std::max)(1.0 - muS * muS, 0.0)), sz = muS;
					double L2[3] = { 0, 0, 0 }, F[3] = { 0, 0, 0 };
					for (int a = 0; a < MsDirs; a++) {
						const double cosT = 1.0 - 2.0 * (a + 0.5) / MsDirs, sinT = sqrt((std::max)(1.0 - cosT * cosT, 0.0));
						for (int b = 0; b < MsDirs; b++) {
							const double phi = 2.0 * Pi * (b + 0.5) / MsDirs, dx = sinT * cos(phi), dy = sinT * sin(phi), dz = cosT;
							const double disc = r0 * r0 * (cosT * cosT - 1.0) + Ground * Ground;
							double tMax = -r0 * cosT + sqrt((std::max)(r0 * r0 * (cosT * cosT - 1.0) + Top * Top, 0.0));
							const bool ground = cosT < 0.0 && disc >= 0.0;
							if (ground) tMax = (std::max)(-r0 * cosT - sqrt((std::max)(disc, 0.0)), 0.0);
							const double nu = dx * sx + dz * sz;
							const double phaseR = 3.0 / (16.0 * Pi) * (1.0 + nu * nu);
							const double phaseM = 3.0 / (8.0 * Pi) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * pow((std::max)(1.0 + g2 - 2.0 * MieG * nu, 1e-4), 1.5));
							const double dt = tMax / MsSteps;
							double thr[3] = { 1, 1, 1 };
							for (int s = 0; s < MsSteps; s++) {
								const double tm = (s + 0.5) * dt, px = dx * tm, py = dy * tm, pz = r0 + dz * tm;
								const double r = sqrt(px * px + py * py + pz * pz), alt = r - Ground;
								const double dR = exp(-alt / RayleighH), dM = exp(-alt / MieH), dO = Ozone01(alt);
								const V3 od = SunOD(r, (px * sx + pz * sz) / r);
								const double rr[3] = { Rayleigh.x * dR, Rayleigh.y * dR, Rayleigh.z * dR }, odv[3] = { od.x, od.y, od.z }, oz[3] = { Ozone.x, Ozone.y, Ozone.z };
								const double scM = MieScatter * haze * dM;
								for (int k = 0; k < 3; k++) {
									const double ext = rr[k] + MieExtinct * haze * dM + oz[k] * dO, x = ext * dt;
									const double f = x < 1e-3 ? 1.0 - 0.5 * x : (1.0 - exp(-x)) / x;
									L2[k] += thr[k] * exp(-odv[k]) * (rr[k] * phaseR + scM * phaseM) * dt * f;
									F[k] += thr[k] * (rr[k] + scM) * dt * f;
									thr[k] *= exp(-x);
								}
							}
							if (ground) {
								const double gx = dx * tMax, gy = dy * tMax, gz = r0 + dz * tMax, rg = sqrt(gx * gx + gy * gy + gz * gz), cosG = (gx * sx + gz * sz) / rg;
								const V3 od = SunOD(rg, cosG);
								const double odv[3] = { od.x, od.y, od.z }, lam = (std::min)((std::max)(cosG, 0.0), 1.0) * albedo / Pi;
								for (int k = 0; k < 3; k++) L2[k] += thr[k] * exp(-odv[k]) * lam;
							}
						}
					}
					float* o = &ms[((size_t)j * MsN + i) * 3];
					for (int k = 0; k < 3; k++) o[k] = (float)((L2[k] * omega * iso) / (1.0 - F[k] * omega * iso));
				}
			}
		}
	};
}
