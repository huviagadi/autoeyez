#include "midi_fft_frame.h"
#include <cassert>

int main() {
    autoeyez::MidiFftFrame frame;
    assert(!frame.accept(110, 12));
    assert(!frame.accept(111, 34));
    assert(!frame.accept(113, 1)); // Missing high band: discard.
    assert(!frame.valid && frame.frames == 0);
    assert(!frame.accept(110, 1));
    assert(!frame.accept(111, 2));
    assert(!frame.accept(112, 3));
    assert(frame.accept(113, 2));
    assert(frame.valid && frame.frames == 1);
    assert(frame.values[0] == 1 && frame.values[1] == 2 && frame.values[2] == 3);
    assert(!frame.accept(114, 99));
    assert(!frame.accept(110, 7));
    assert(!frame.accept(113, 3)); // No old values leak into a partial frame.
    assert(frame.frames == 1 && frame.values[0] == 1);
}
