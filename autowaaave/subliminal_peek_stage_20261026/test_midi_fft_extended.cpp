#include "midi_fft_frame.h"
#include <cassert>

int main() {
    autoeyez::MidiFftFrame frame;
    for(unsigned i=0; i<3; ++i) assert(!frame.accept(110+i, i+1));
    const unsigned raw[] = {0x3f, 0x7f, 0x01, 0x02, 0x00, 0x40};
    for(unsigned i=0; i<6; ++i) assert(!frame.accept(114+i, raw[i]));
    assert(frame.accept(113, 7));
    assert(frame.valid && frame.hasExtended && frame.frames==1);
    assert(frame.extendedValues[0]==8191);
    assert(frame.extendedValues[1]==130);
    assert(frame.extendedValues[2]==64);

    // A complete legacy frame must fall back to 7-bit values; stale extended
    // bytes from the previous frame must never be reused.
    for(unsigned i=0; i<3; ++i) frame.accept(110+i, 20+i);
    frame.accept(114, 0x7f);
    assert(frame.accept(113, 8));
    assert(frame.frames==2 && !frame.hasExtended);
    assert(frame.values[0]==20 && frame.values[2]==22);

    // A partial legacy frame must not commit, even with six extended bytes.
    for(unsigned i=0; i<6; ++i) frame.accept(114+i, raw[i]);
    frame.accept(110, 5);
    assert(!frame.accept(113, 9));
    assert(frame.frames==2 && frame.values[0]==20);
}
