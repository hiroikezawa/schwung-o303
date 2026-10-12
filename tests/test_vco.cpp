/* o303 - the second VCO (dsp/open303_sync.h).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 *   1. At its defaults the sync engine IS Open303: bit-identical output to
 *      the unmodified rosic::Open303 over notes, accents, slides and rests.
 *   2. VCO2's level is calibrated against VCO1's.
 *   3. Hard sync: VCO2 tuned up keeps VCO1's period and gets brighter.
 *   4. Sync Env sweeps: bright at the attack, darker as the envelope falls.
 *   5. Nothing it can be set to produces a non-finite or runaway sample.
 */
#include "open303_sync.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static const double SR = 44100.0;

template <class E> static void setup(E &e) {
    e.setSampleRate(SR);
    e.setCutoff(1000.0); e.setResonance(50.0); e.setEnvMod(50.0);
    e.setDecay(600.0); e.setAccent(50.0); e.setVolume(-12.0);
}

/* A bar of TB-3PO-style MIDI: plain, accent, a slide pair, a rest. */
template <class E> static std::vector<double> play(E &e, double seconds) {
    std::vector<double> out;
    const int step = (int)(SR * 60.0 / 120.0 / 4.0);
    const int notes[] = {33, 45, 40, 52, 36, 48, 43, 31};
    for (int s = 0, held = -1; s * step < seconds * SR; ++s) {
        const int n = notes[s % 8], kind = s % 4;     /* 0 plain 1 accent 2 slide 3 rest */
        if (kind == 3) { if (held >= 0) e.noteOn(held, 0, 0.0); held = -1; }
        else {
            e.noteOn(n, kind == 1 ? 118 : 72, 0.0);
            if (held >= 0 && held != n) e.noteOn(held, 0, 0.0);   /* legato: on before off */
            held = n;
        }
        for (int i = 0; i < step; ++i) {
            if (i == step / 2 && kind == 0 && held >= 0) { e.noteOn(held, 0, 0.0); held = -1; }
            out.push_back(e.getSample());
        }
    }
    return out;
}

static double rms(const std::vector<double> &x, size_t a = 0, size_t b = 0) {
    if (!b) b = x.size();
    double s = 0; for (size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(s / (double)(b - a));
}
static double bright(const std::vector<double> &x, size_t a, size_t b) {   /* difference energy share */
    double d = 0, s = 0;
    for (size_t i = a + 1; i < b; ++i) { d += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); s += x[i] * x[i]; }
    return std::sqrt(d / (s + 1e-30));
}
/* One held note, filter wide open, for looking at the oscillators. */
static std::vector<double> held(rosic::Open303Sync &e, int note, double seconds) {
    e.setCutoff(2394.0); e.setResonance(0.0); e.setEnvMod(0.0); e.setDecay(2000.0);
    e.noteOn(note, 72, 0.0);
    std::vector<double> out;
    for (int i = 0; i < (int)(seconds * SR); ++i) out.push_back(e.getSample());
    return out;
}
/* The lag (in samples) of the strongest autocorrelation peak past the first trough. */
static int period(const std::vector<double> &x, size_t a, int maxLag) {
    int best = 0; double bestV = -1e30; bool dipped = false;
    for (int lag = 1; lag < maxLag; ++lag) {
        double s = 0; for (size_t i = a; i < a + 4096; ++i) s += x[i] * x[i + lag];
        if (s < 0) dipped = true;
        if (dipped && s > bestV) { bestV = s; best = lag; }
    }
    return best;
}

int main() {
    /* 1. bit-identical at the defaults */
    static rosic::Open303 ref;
    static rosic::Open303Sync syn;
    setup(ref); setup(syn);
    const std::vector<double> a = play(ref, 6.0), b = play(syn, 6.0);
    CHECK(a.size() == b.size() && rms(a) > 0.01);
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { std::fprintf(stderr, "sample %zu: %.17g vs %.17g\n", i, a[i], b[i]); CHECK(0); }

    /* 2. VCO2's saw at VCO1's level, VCO1 off: the same loudness */
    static rosic::Open303Sync v1, v2;
    setup(v1); setup(v2);
    v2.setVco1Level(0.0); v2.setVco2Level(1.0); v2.setVco2Sync(0);
    const std::vector<double> h1 = held(v1, 45, 1.0), h2 = held(v2, 45, 1.0);
    const double ratio = rms(h2, 22050, 44100) / rms(h1, 22050, 44100);
    std::printf("VCO2/VCO1 level: %.3f\n", ratio);
    CHECK(ratio > 0.8 && ratio < 1.25);

    /* 3. hard sync. +5 semitones is a ratio of 1.335, nowhere near a
     *    harmonic: free-running, VCO2 has its own period (300 samples at
     *    A2); synced, the waveform repeats at VCO1's 110 Hz (401 samples).
     *    (+19 would be a bad probe: 2.997x is almost VCO1's 3rd harmonic.) */
    static rosic::Open303Sync free5, sync5, sync0, sync17;
    rosic::Open303Sync *all[] = {&free5, &sync5, &sync0, &sync17};
    for (rosic::Open303Sync *e : all) { setup(*e); e->setVco1Level(0.0); e->setVco2Level(1.0); e->setVco2Sync(1); }
    free5.setVco2Sync(0);
    free5.setVco2Tune(5.0); sync5.setVco2Tune(5.0); sync17.setVco2Tune(17.0);
    const std::vector<double> yf = held(free5, 45, 1.0), y5 = held(sync5, 45, 1.0);
    const std::vector<double> y0 = held(sync0, 45, 1.0), y17 = held(sync17, 45, 1.0);
    const int pf = period(yf, 22050, 1000), p5 = period(y5, 22050, 1000);
    std::printf("period at +5 st: free %d, synced %d samples (VCO1 110 Hz = 401)\n", pf, p5);
    CHECK(std::abs(pf - 300) <= 2 && std::abs(p5 - 401) <= 2);
    const double b0 = bright(y0, 22050, 44100), b17 = bright(y17, 22050, 44100);
    std::printf("synced brightness: +0 st %.3f, +17 st %.3f\n", b0, b17);
    CHECK(b17 > 1.5 * b0);
    CHECK(std::abs(period(y17, 22050, 1000) - 401) <= 2);

    /* 4. Sync Env: bright early, darker late (it follows the decaying main envelope) */
    static rosic::Open303Sync env;
    setup(env); env.setVco1Level(0.0); env.setVco2Level(1.0); env.setSyncEnv(1.0);
    env.setCutoff(2394.0); env.setResonance(0.0); env.setEnvMod(0.0); env.setDecay(300.0);
    env.noteOn(45, 72, 0.0);
    std::vector<double> e;
    for (int i = 0; i < (int)SR; ++i) e.push_back(env.getSample());
    const double early = bright(e, 400, 2400), late = bright(e, 30000, 32000);
    std::printf("sync env brightness: attack %.3f, tail %.3f\n", early, late);
    CHECK(early > 1.5 * late);

    /* 5. every setting stays finite and bounded, through notes and slides */
    unsigned r = 7;
    auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (double)(r >> 8) / 16777216.0; };
    static rosic::Open303Sync z;
    setup(z);
    double peak = 0;
    for (int round = 0; round < 200; ++round) {
        z.setVco1Level(rnd()); z.setVco2Level(rnd()); z.setVco2Wave(rnd() < 0.5);
        z.setVco2Tune(rnd() * 48.0 - 24.0); z.setVco2Sync(rnd() < 0.8); z.setSyncEnv(rnd());
        z.setResonance(rnd() * 100.0); z.setCutoff(314.0 + rnd() * 2080.0);
        z.noteOn(24 + (int)(rnd() * 72), rnd() < 0.4 ? 118 : 72, 0.0);
        for (int i = 0; i < 2000; ++i) {
            const double x = z.getSample();
            CHECK(std::isfinite(x));
            if (std::fabs(x) > peak) peak = std::fabs(x);
        }
    }
    std::printf("random settings: peak %.3f\n", peak);
    CHECK(peak < 4.0);
    std::printf("PASS: vco (bit-identical to Open303 at the defaults over %zu samples)\n", a.size());
    return 0;
}
