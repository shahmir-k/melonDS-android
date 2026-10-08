// Self-check for SkipRepeatAuto (not part of the app build):
//   c++ -std=c++17 app/src/main/cpp/SkipRepeatAuto_check.cpp -o /tmp/sra && /tmp/sra
#include <cassert>
#include <cstdio>
#include "SkipRepeatAuto.h"

const double B = 1000.0 / 60;

// a scene that costs `off` ms per frame with skipping off and `on` ms with it on;
// returns how many of `n` frames ran with skipping on
static int run(SkipRepeatAuto& c, double off, double on, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) k += c.update(c.on ? on : off, B);
    return k;
}

int main()
{
    SkipRepeatAuto c;
    assert(run(c, 12.0, 12.0, 600) == 0);       // fits at 60: stays off
    assert(!c.update(80.0, B));                 // one hitch: stays off

    // heavy scene (17.5 ms off, 9 ms on, like Shrek at 1.1 GHz): on almost all the time,
    // and the off-probes back off to 60 s instead of flapping every 3 s
    int k = run(c, 17.5, 9.0, 36000);
    assert(k > 36000 * 0.95);
    assert(c.hold == SkipRepeatAuto::HOLD_MAX);

    // scene turns light: off at the next probe, and stays off
    run(c, 11.0, 6.0, SkipRepeatAuto::HOLD_MAX + 1);
    assert(!c.on && run(c, 11.0, 6.0, 1200) == 0);
    assert(c.hold == SkipRepeatAuto::HOLD_MIN); // backoff reset after a probe held

    // borderline (16 ms, 96% of budget): never switches on
    c.reset();
    assert(run(c, 16.0, 8.0, 600) == 0);
    std::puts("SkipRepeatAuto ok");
}
