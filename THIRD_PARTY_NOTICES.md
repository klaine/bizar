# Third-party notices

## Plaits oscillators

Parts of `src/engines.c` are adapted from Emilie Gillet's Plaits:

- V.Analog: `VirtualAnalogEngine` VA_VARIANT 2,
  `VariableSawOscillator`, `VariableShapeOscillator`
- KarplusStr: `StringEngine`, `StringVoice`, `String`
- Two Op. FM: `FMEngine` and `resources.cc`'s
  `lut_fm_frequency_quantizer`
- Formant: `GrainEngine`, `GrainletOscillator`
- Waveshaper: `WaveshapingEngine`, `Oscillator`'s slope shape, and
  `resources.cc`'s `lut_ws_*` / `lut_fold*` tables (generated into
  `src/plaits_waveshaping_luts.h`)
- Chords: `ChordEngine`, `ChordBank`, and `StringSynthOscillator`'s
  divide-down registration
- Modal: `ModalEngine`, `ModalVoice`, `Resonator`, and `resources.cc`'s
  `lut_stiffness` table (generated into `src/plaits_modal_luts.h`)
- Speech: `SpeechEngine` and `NaiveSpeechSynth`'s five-vowel formant bank
  (Plaits' SAM/LPC word banks are not included)

Sources:

- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/virtual_analog_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/oscillator/variable_saw_oscillator.h
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/string_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/physical_modelling/string_voice.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/physical_modelling/string.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/fm_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/grain_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/oscillator/grainlet_oscillator.h
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/waveshaping_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/oscillator/oscillator.h
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/chord_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/chords/chord_bank.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/oscillator/string_synth_oscillator.h
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/modal_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/physical_modelling/modal_voice.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/physical_modelling/resonator.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/speech_engine.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/speech/naive_speech_synth.cc
- https://github.com/pichenettes/eurorack/blob/master/plaits/resources.cc

Copyright 2016 Emilie Gillet. Used and adapted under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.
