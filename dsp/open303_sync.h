/* o303 - Open303 with a second VCO, hard-synced to the first.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * Open303 is compiled unmodified (vendor/). This is a subclass that replaces
 * one function, getSample(), with a copy of Open303's own in which the line
 *
 *     tmp = -oscillator.getSample();
 *
 * becomes "VCO1 * level1 + VCO2 * level2". Everything around that line --
 * the pitch slew that makes a slide, the main envelope and its accent path,
 * the TeeBee filter, the 4x oversampling and its anti-alias filter, the amp
 * envelope -- is Open303's, reached through members it already exposes, so
 * VCO2 goes through the 303's filter and is shaped by Cutoff / Env Mod /
 * Accent / Decay exactly as VCO1 is, and follows a slide note for note.
 *
 *   VCO1  Open303's own BlendOscillator (the 303 saw / square), untouched.
 *   VCO2  a PolyBLEP saw / square, running at Open303's oversampled rate,
 *         its phase reset each time VCO1 completes a cycle (hard sync). A
 *         reset is a step in the waveform, so it is band-limited like the
 *         waveform's own edges: a PolyBLEP correction straddling the two
 *         samples around it, with VCO2 one oversampled sample late (5.7 us)
 *         to have the second sample to put it in. This is the same scheme
 *         as Mutable Instruments' Plaits oscillators.
 *
 * SYNC ENV raises VCO2's pitch by up to SYNC_ENV_SEMITONES times Open303's
 * main envelope (the one Env Mod sends to the filter), more on an accented
 * note: the classic sync sweep, falling with the decay.
 *
 * At the defaults (VCO1 level 1, VCO2 level 0) the arithmetic is Open303's,
 * expression for expression, so the output is bit-identical to the
 * unmodified engine -- tests/test_vco.cpp holds it to that.
 */
#ifndef OPEN303_SYNC_H
#define OPEN303_SYNC_H

#include "open303/rosic_Open303.h"
#include <cmath>

namespace rosic {

/* VCO1's phase, read without touching BlendOscillator: a pointer to its
 * protected member, formed inside a class derived from it (the one place C++
 * allows that), then applied to Open303's oscillator. */
struct Open303SyncPeek : BlendOscillator {
    static constexpr double BlendOscillator::*phase() { return &Open303SyncPeek::phaseIndex; }
    static constexpr double BlendOscillator::*length() { return &Open303SyncPeek::tableLengthDbl; }
};

class Open303Sync : public Open303 {
public:
    static constexpr double SYNC_ENV_SEMITONES = 36.0;
    /* VCO2's saw against VCO1's 303 saw, RMS for RMS (tests/test_vco.cpp). */
    static constexpr double VCO2_GAIN = 1.0;

    void setVco1Level(double v) { vco1Target = clip01(v); }
    void setVco2Level(double v) { vco2Target = clip01(v); }
    void setVco2Wave(int square) { vco2Square = square != 0; }
    void setVco2Tune(double semitones) { vco2Semis = semitones; }       /* coarse + fine */
    void setVco2Sync(int on) { syncOn = on != 0; }
    void setSyncEnv(double amount) { syncEnv = clip01(amount); }

    /** Open303::getSample(), with VCO2 mixed in before the filter. */
    INLINE double getSample() {
        if (idle)
            return 0.0;

        if (sequencer.getSequencerMode() != AcidSequencer::OFF) {
            noteOffCountDown--;
            if (noteOffCountDown == 0 || sequencer.isRunning() == false)
                releaseNote(currentNote);
            AcidNote *note = sequencer.getNote();
            if (note != NULL) {
                if (note->gate == true && currentNote != -1) {
                    int key = note->key + 12 * note->octave + currentNote;
                    key = clip(key, 0, 127);
                    if (!slideToNextNote)
                        triggerNote(key, note->accent);
                    else
                        slideToNote(key, note->accent);
                    AcidNote *nextNote = sequencer.getNextScheduledNote();
                    if (note->slide && nextNote->gate == true) {
                        noteOffCountDown = INT_MAX;
                        slideToNextNote = true;
                    } else {
                        noteOffCountDown = sequencer.getStepLengthInSamples();
                        slideToNextNote = false;
                    }
                }
            }
        }

        double instFreq = pitchSlewLimiter.getSample(oscFreq);
        oscillator.setFrequency(instFreq * pitchWheelFactor);
        oscillator.calculateIncrement();

        double mainEnvOut = mainEnv.getSample();
        double tmp1 = n1 * rc1.getSample(mainEnvOut);
        double tmp2 = 0.0;
        if (accentGain > 0.0)
            tmp2 = mainEnvOut;
        tmp2 = n2 * rc2.getSample(tmp2);
        tmp1 = envScaler * (tmp1 - envOffset);
        tmp2 = accentGain * tmp2;
        double instCutoff = cutoff * pow(2.0, tmp1 + tmp2);
        filter.setCutoff(instCutoff);

        double ampEnvOut = ampEnv.getSample();
        if (ampEnv.isNoteOn())
            ampEnvOut += 0.45 * mainEnvOut + accentGain * 4.0 * mainEnvOut;
        ampEnvOut = ampDeClicker.getSample(ampEnvOut);

        /* The one change: what feeds highpass1. */
        double tmp;
        const bool plain = vco1 == 1.0 && vco1Target == 1.0 && vco2 == 0.0 && vco2Target == 0.0;
        if (plain) {
            for (int i = 1; i <= oversampling; i++) {
                tmp = -oscillator.getSample();
                tmp = highpass1.getSample(tmp);
                tmp = filter.getSample(tmp);
                tmp = antiAliasFilter.getSample(tmp);
            }
        } else {
            vco1 += (vco1Target - vco1) * LEVEL_COEF;
            vco2 += (vco2Target - vco2) * LEVEL_COEF;
            if (std::fabs(vco1Target - vco1) < 1e-6) vco1 = vco1Target;
            if (std::fabs(vco2Target - vco2) < 1e-6) vco2 = vco2Target;
            /* VCO2's pitch: VCO1's (so a slide glides both), transposed,
             * plus the sync sweep. Once per sample, not per oversample. */
            const double sweep = syncEnv * SYNC_ENV_SEMITONES * mainEnvOut * (1.0 + accentGain);
            const double L = oscillator.*Open303SyncPeek::length();
            const double inc = oscillator.getIncrement();
            /* VCO1's increment is in table points per oversampled sample, so
             * inc / L is its frequency in cycles per sample; VCO2's is that
             * times its ratio, capped near 20 kHz (0.11 of 176.4 kHz) and
             * well clear of 0.5, where an edge could be skipped. */
            double dt = (L > 0.0 ? inc / L : 0.0) * std::pow(2.0, (vco2Semis + sweep) / 12.0);
            if (!(dt > 0.0)) dt = 0.0;
            if (dt > 0.11) dt = 0.11;
            for (int i = 1; i <= oversampling; i++) {
                const double v1 = oscillator.getSample();
                /* Did VCO1 just finish a cycle, and how long ago? Its phase is
                 * advanced after it is read, so a phase at or past the table
                 * end means the wrap happened inside this step; VCO1's own
                 * output shows it from the next step, and so -- one sample
                 * late by construction -- does VCO2's reset. */
                const double p = oscillator.*Open303SyncPeek::phase();
                double since = -1.0;
                if (syncOn && inc > 0.0 && p >= L) since = (p - L) / inc;
                const double v2 = vco2Step(dt, since);
                tmp = -(vco1 * v1 + vco2 * VCO2_GAIN * v2);
                tmp = highpass1.getSample(tmp);
                tmp = filter.getSample(tmp);
                tmp = antiAliasFilter.getSample(tmp);
            }
        }

        tmp = allpass.getSample(tmp);
        tmp = highpass2.getSample(tmp);
        tmp = notch.getSample(tmp);
        tmp *= ampEnvOut;
        tmp *= ampScaler;

        idle = false;
        return tmp;
    }

private:
    static constexpr double LEVEL_COEF = 0.002;       /* ~11 ms at 44.1 kHz */
    static double clip01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    /* The naive waveform on a 0..1 scale. */
    double naive(double ph) const { return vco2Square ? (ph < 0.5 ? 1.0 : 0.0) : ph; }

    /* A step of size `jump`, `t` samples before the end of this step. */
    void blep(double jump, double t) {
        thisSample += jump * 0.5 * t * t;
        const double u = 1.0 - t;
        nextSample -= jump * 0.5 * u * u;
    }

    /* Run the phase for `d` samples, ending `tail` samples before the end of
     * the step, band-limiting every edge the waveform has on the way. */
    void advance(double dt, double d, double tail) {
        double ph = phase2, left = d;
        for (int guard = 0; guard < 4 && left > 0.0; ++guard) {
            const double edge = (vco2Square && ph < 0.5) ? 0.5 : 1.0;
            const double reach = (edge - ph) / dt;           /* samples until it */
            if (reach > left) { ph += left * dt; left = 0.0; break; }
            left -= reach;
            const double t = left + tail;
            if (edge == 1.0) { blep(vco2Square ? 1.0 : -1.0, t); ph = 0.0; }
            else { blep(-1.0, t); ph = 0.5; }
        }
        phase2 = ph >= 1.0 ? ph - 1.0 : ph;
    }

    /* One oversampled VCO2 sample, -1..1. `since` >= 0: VCO1 restarted that
     * many samples before the end of this step. */
    double vco2Step(double dt, double since) {
        thisSample = nextSample;
        nextSample = 0.0;
        if (since >= 0.0 && since < 1.0) {
            advance(dt, 1.0 - since, since);
            const double jump = naive(0.0) - naive(phase2);  /* back to the start of the cycle */
            if (jump != 0.0) blep(jump, since);
            phase2 = 0.0;
            advance(dt, since, 0.0);
        } else {
            advance(dt, 1.0, 0.0);
        }
        nextSample += naive(phase2);
        return 2.0 * thisSample - 1.0;
    }

    double vco1 = 1.0, vco1Target = 1.0;
    double vco2 = 0.0, vco2Target = 0.0;
    bool vco2Square = false;
    double vco2Semis = 0.0;
    bool syncOn = true;
    double syncEnv = 0.0;
    double phase2 = 0.0, thisSample = 0.0, nextSample = 0.0;
};

} // namespace rosic

#endif
