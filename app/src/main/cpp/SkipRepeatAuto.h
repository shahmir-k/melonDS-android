#ifndef SKIPREPEATAUTO_H
#define SKIPREPEATAUTO_H

// "Skip repeated frames: Auto" at 1x speed: turns skip-repeat on while the game can't hold
// 60 fps, and probes turning it off again with a backoff.
// While skipping is on, no frame's work time tells what the cost without skipping would be:
// a drawn frame that follows a skipped one is cheap too (it doesn't wait on the previous
// frame's render), so on-device it read ~8 ms vs ~17 ms with skipping off and a
// "drawn frames fit, turn off" test flapped every 3 s. So the off decision is a probe: after
// `hold` frames on, turn off; if the game falls behind again within PROBE frames, the probe
// failed and the next hold doubles (3 s, 6 s, ... 60 s).
// ponytail: each failed probe costs ~0.3 s below 60 fps; a scene that turns light while on
// keeps skipping until the next probe (<= 60 s). Upgrade: a cost model per frame pair.
struct SkipRepeatAuto
{
    static constexpr int HOLD_MIN = 180, HOLD_MAX = 3600, PROBE = 300;

    bool on = false;
    double emaMs = 0.0;
    int frames = PROBE + 1;   // frames since the last switch
    int hold = HOLD_MIN;   // frames to stay on before the next off-probe

    void reset() { on = false; emaMs = 0.0; frames = PROBE + 1; hold = HOLD_MIN; }

    // workMs: this frame's work time (no limiter sleep); budgetMs: the frame period at the
    // target speed. Returns the new state.
    bool update(double workMs, double budgetMs)
    {
        // clamp so one hitch (savestate load, GC) can't switch it on by itself
        double w = workMs < budgetMs * 2 ? workMs : budgetMs * 2;
        emaMs = emaMs <= 0.0 ? w : emaMs * 0.9 + w * 0.1;
        frames++;
        if (!on)
        {
            if (frames > PROBE)
                hold = HOLD_MIN;                      // the last probe held: start over
            if (emaMs > budgetMs * 1.035)             // below ~58 fps
            {
                if (frames <= PROBE)                  // probe failed: stay on longer next time
                    hold = hold * 2 < HOLD_MAX ? hold * 2 : HOLD_MAX;
                on = true;
                frames = 0;
            }
        }
        else if (frames >= hold)
        {
            on = false;
            frames = 0;
        }
        return on;
    }
};

#endif
