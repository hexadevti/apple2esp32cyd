#include "../../emu.h"

#include "driver/dac.h"

// The Apple II speaker is a 1-bit toggle at $C030. We output it through the ESP32
// DAC on GPIO26 (DAC channel 2) instead of a plain digitalWrite, so the square-wave
// amplitude scales with `volume` (0x00..0xF0) and `sound` can mute it. This is what
// makes the Settings volume slider / mute actually do something.
// NOTE: SPEAKER_PIN must be GPIO26 (DAC_CHANNEL_2) for this path.
void speakerSetup() {
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_voltage(DAC_CHANNEL_2, 0);
}

void speakerToggle() {
  speaker_state = !speaker_state;
  // High half of the wave outputs `volume`; low half outputs 0. Muted -> always 0.
  dac_output_voltage(DAC_CHANNEL_2, (sound && speaker_state) ? volume : 0);
}
