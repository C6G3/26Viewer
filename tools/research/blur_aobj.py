"""Reference reader for Blur's audio entity files (audio/dat/*.dat, magic "JBOA").

Layout recovered from Blur.exe (header check at 0x562810, component factory
switch at 0x5643f8, per-type load functions = vtable slot 1):

  0x00 "JBOA"
  0x04 u16 version (11), u16 2
  0x08 u32 string pool offset (from the file start)
  0x0c u32 entity name (offset into the pool)
  0x10 u32 component count
  0x14 u32 update rate (int)
  0x18 components, back to back: u32 type, u32 name (pool offset), payload

Every load function calls the shared reader (name + FNV-1a hash) and returns
the next record; the payload size per type is below. Usage:

  python blur_aobj.py <file-or-folder>
"""
import os
import struct
import sys

# type id: (class name, payload size after the type field, or a function of the record)
FIXED = {}
VARIABLE = {}

NAMES = {
    0x00: 'ParameterIn', 0x01: 'ParameterOut', 0x02: 'ActionIn', 0x03: 'ActionOut', 0x04: 'VectorIn',
    0x06: 'ControlInput', 0x07: 'ControlOutput', 0x08: 'EventInput', 0x09: 'EventOutput',
    0x0a: 'CoordinateInput', 0x0b: 'CoordinateOutput', 0x0c: 'MonoAudioInput', 0x0d: 'MonoAudioOutput',
    0x0e: 'StereoAudioInput', 0x0f: 'StereoAudioOutput', 0x10: 'SurroundAudioInput',
    0x11: 'SurroundAudioOutput', 0x12: 'SurroundLFEAudioInput', 0x13: 'SurroundLFEAudioOutput',
    0x14: 'Variable', 0x15: 'PiecewiseLinearFunction', 0x17: 'Source', 0x18: 'Rolloff', 0x19: 'Cone',
    0x1a: 'LowPass', 0x1c: 'Timer', 0x1d: 'Metronome', 0x20: 'Random', 0x21: 'Selector',
    0x22: 'StateMachine', 0x23: 'Quantize', 0x24: 'Sum', 0x25: 'Product', 0x28: 'Exponent', 0x29: 'Max',
    0x2a: 'Min', 0x2b: 'Invert', 0x2c: 'Gradient', 0x2d: 'Spring', 0x2f: 'Sine', 0x33: 'SampleAndHold',
    0x34: 'Envelope', 0x36: 'GateSwitch', 0x37: 'Array', 0x38: 'Fraction', 0x3b: 'MonoOneShotSound',
    0x3c: 'StereoOneShotSound', 0x3d: 'MonoOneShotSoundTable', 0x3e: 'StereoOneShotSoundTable',
    0x3f: 'MonoLoopingSound', 0x40: 'StereoLoopingSound', 0x44: 'MonoToMonoMixer',
    0x45: 'MonoToStereoMixer', 0x46: 'MonoToSurroundMixer', 0x47: 'MonoToSurroundLFEMixer',
    0x48: 'StereoToMonoMixer', 0x49: 'StereoToStereoMixer', 0x4a: 'StereoToQuadMixer',
    0x4b: 'StereoToSurroundMixer', 0x4c: 'QuadToSurroundMixer', 0x4f: 'SurroundToSurroundMixer',
    0x50: 'SurroundToSurroundLFEMixer', 0x51: 'SurroundLFEToSurroundLFEMixer', 0x70: 'MonoDelay',
    0x76: 'MonoRMSLevel', 0x77: 'StereoRMSLevel', 0x78: 'SurroundRMSLevel', 0x79: 'MonoSaturator',
    0x7a: 'StereoSaturator', 0x7b: 'SurroundSaturator', 0x82: 'MonoConvolution', 0x8e: 'MonoLowPass',
    0x8f: 'StereoLowPass', 0x90: 'SurroundLowPass', 0x91: 'MonoHighPass', 0x92: 'StereoHighPass',
    0x93: 'SurroundHighPass', 0x94: 'MonoResonantLowPass', 0x95: 'StereoResonantLowPass',
    0x96: 'SurroundResonantLowPass', 0x9a: 'Mono24dBResonantLowPass', 0x9c: 'Surround24dBResonantLowPass',
    0xa3: 'MonoPeakEQ', 0xa4: 'StereoPeakEQ', 0xa5: 'SurroundPeakEQ', 0xa6: 'MonoCompressor',
    0xa7: 'StereoCompressor', 0xa8: 'SurroundCompressor', 0xaa: 'StereoPitchShift', 0xbd: 'Script',
    0xbe: 'Vector', 0xbf: 'VectorNearestPointOnLine', 0xc0: 'DopplerShift', 0xc1: 'DebugPrint',
    0xc2: 'Sequence', 0xc3: 'MonoSpectralReverb', 0xc4: 'MonoToAmbisonicsMixer',
    0xc5: 'AmbisonicsToSurroundMixer', 0xc6: 'ImpactFilter', 0xc7: 'MessageDelay', 0xca: 'MonoFlanger',
    0xcc: 'SurroundFlanger', 0xcd: 'MonoBitCrusher', 0xcf: 'SurroundBitCrusher',
    0xd0: 'MonoRingModulator', 0xd2: 'SurroundRingModulator', 0xd6: 'MonoSend', 0xd7: 'StereoSend',
    0xd9: 'MonoReturn', 0xda: 'StereoReturn',
}


def u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


# variable layouts: (header bytes, [(count offset, bytes per item), ...])
COUNTED = {
    0x15: (0x14, [(0x10, 12)]),            # PiecewiseLinearFunction: points
    0x21: (0x10, [(0x0c, 32)]),            # Selector
    0x22: (0x10, [(0x0c, 8)]),             # StateMachine
    0x24: (0x08, [(0x04, 8)]),             # Sum: inputs
    0x25: (0x08, [(0x04, 8)]),             # Product
    0x29: (0x08, [(0x04, 8)]),             # Max
    0x2a: (0x08, [(0x04, 8)]),             # Min
    0x37: (0x14, [(0x10, 8)]),             # Array: entries
    0x3b: (0x34, [(0x30, 4)]),             # MonoOneShotSound
    0x3c: (0x34, [(0x30, 4)]),             # StereoOneShotSound
    0x3d: (0x3c, [(0x34, 4), (0x38, 4)]),  # MonoOneShotSoundTable
    0x3e: (0x3c, [(0x34, 4), (0x38, 4)]),  # StereoOneShotSoundTable
    0x3f: (0x3c, [(0x38, 2)]),             # MonoLoopingSound
    0x40: (0x3c, [(0x38, 4)]),             # StereoLoopingSound
    0xbd: (0x08, [(0x04, 4)]),             # Script
    0xc2: (0x0c, [(0x08, 16)]),            # Sequence
    0xd9: (0x18, [(0x14, 4)]),             # MonoReturn
    0xda: (0x18, [(0x14, 4)]),             # StereoReturn
}
# mixers: header, inputs (u16 at header-4, 4 bytes each), outputs (u16 at header-2, bytes each)
MIXERS = {
    0x44: (0x18, 20), 0x45: (0x18, 36), 0x46: (0x3c, 20), 0x47: (0x4c, 20), 0x48: (0x18, 36),
    0x49: (0x18, 68), 0x4a: (0x18, 132), 0x4b: (0x5c, 36), 0x4c: (0x9c, 68), 0x4f: (0x18, 20),
    0x50: (0x28, 20), 0x51: (0x18, 20), 0xc4: (0x28, 20), 0xc5: (0x20, 20),
}


def record_size(t, d, rec):
    """Bytes from `rec` (just after the type field) to the next record."""
    if t in COUNTED:
        head, lists = COUNTED[t]
        return head + sum(u16(d, rec + o) * n for o, n in lists)
    if t in MIXERS:
        head, per = MIXERS[t]
        return head + u16(d, rec + head - 4) * 4 + u16(d, rec + head - 2) * per
    if t in FIXED:
        return FIXED[t]
    raise KeyError(f'type {t:#x} has no known size')


def parse(d):
    assert d[:4] == b'JBOA', 'not a JBOA file'
    version, two, pool, name, count, rate = struct.unpack_from('<HHIIII', d, 4)
    assert version == 11 and two == 2, (version, two)

    def s(off):
        o = pool + off
        return d[o:d.index(b'\0', o)].decode('latin1')

    comps = []
    pos = 0x18
    for _ in range(count):
        t, nm = struct.unpack_from('<II', d, pos)
        size = record_size(t, d, pos + 4)
        comps.append((pos, t, NAMES.get(t, f'type{t:#x}'), s(nm), d[pos + 8:pos + 4 + size]))
        pos += 4 + size
    return {'name': s(name), 'rate': rate, 'components': comps, 'end': pos, 'pool': pool}


# Fixed payload sizes (load function = shared reader; add eax, N), N counted from the name field.
FIXED.update({
    0x00: 0x8,  # ParameterIn
    0x01: 0xc,  # ParameterOut
    0x02: 0x8,  # ActionIn
    0x03: 0x4,  # ActionOut
    0x04: 0x10,  # VectorIn
    0x06: 0x4,  # ControlInput
    0x07: 0xc,  # ControlOutput
    0x08: 0x8,  # EventInput
    0x09: 0x4,  # EventOutput
    0x0a: 0x4,  # CoordinateInput
    0x0b: 0x8,  # CoordinateOutput
    0x0c: 0x4,  # MonoAudioInput
    0x0d: 0x8,  # MonoAudioOutput
    0x0e: 0x4,  # StereoAudioInput
    0x0f: 0x8,  # StereoAudioOutput
    0x10: 0x4,  # SurroundAudioInput
    0x11: 0x8,  # SurroundAudioOutput
    0x12: 0x4,  # SurroundLFEAudioInput
    0x13: 0x8,  # SurroundLFEAudioOutput
    0x14: 0x8,  # Variable
    0x17: 0xa8,  # Source
    0x18: 0x30,  # Rolloff
    0x19: 0x40,  # Cone
    0x1a: 0x10,  # LowPass
    0x1c: 0x20,  # Timer
    0x1d: 0x28,  # Metronome
    0x20: 0x14,  # Random
    0x23: 0x18,  # Quantize
    0x28: 0x10,  # Exponent
    0x2b: 0x8,  # Invert
    0x2c: 0x8,  # Gradient
    0x2d: 0x18,  # Spring
    0x2f: 0x8,  # Sine
    0x33: 0x8,  # SampleAndHold
    0x34: 0x38,  # Envelope
    0x36: 0x30,  # GateSwitch
    0x38: 0x8,  # Fraction
    0x70: 0x24,  # MonoDelay
    0x76: 0xc,  # MonoRMSLevel
    0x77: 0xc,  # StereoRMSLevel
    0x78: 0xc,  # SurroundRMSLevel
    0x79: 0x24,  # MonoSaturator
    0x7a: 0x24,  # StereoSaturator
    0x7b: 0x24,  # SurroundSaturator
    0x82: 0x24,  # MonoConvolution
    0x8e: 0x2c,  # MonoLowPass
    0x8f: 0x2c,  # StereoLowPass
    0x90: 0x2c,  # SurroundLowPass
    0x91: 0x2c,  # MonoHighPass
    0x92: 0x2c,  # StereoHighPass
    0x93: 0x2c,  # SurroundHighPass
    0x94: 0x3c,  # MonoResonantLowPass
    0x95: 0x3c,  # StereoResonantLowPass
    0x96: 0x3c,  # SurroundResonantLowPass
    0x9a: 0x3c,  # Mono24dBResonantLowPass
    0x9c: 0x3c,  # Surround24dBResonantLowPass
    0xa3: 0x3c,  # MonoPeakEQ
    0xa4: 0x3c,  # StereoPeakEQ
    0xa5: 0x3c,  # SurroundPeakEQ
    0xa6: 0x40,  # MonoCompressor
    0xa7: 0x40,  # StereoCompressor
    0xa8: 0x40,  # SurroundCompressor
    0xaa: 0x24,  # StereoPitchShift
    0xbe: 0x20,  # Vector
    0xc0: 0x14,  # DopplerShift
    0xc1: 0xc,  # DebugPrint
    0xc3: 0x84,  # MonoSpectralReverb
    0xca: 0x38,  # MonoFlanger
    0xcc: 0x38,  # SurroundFlanger
    0xcd: 0x1c,  # MonoBitCrusher
    0xcf: 0x1c,  # SurroundBitCrusher
    0xd0: 0x1c,  # MonoRingModulator
    0xd2: 0x1c,  # SurroundRingModulator
    0xd6: 0x18,  # MonoSend
    0xd7: 0x18,  # StereoSend
    0xbf: 0xc,  # VectorNearestPointOnLine
    0xc6: 0x1c,  # ImpactFilter
    0xc7: 0x10,  # MessageDelay
})

if __name__ == '__main__':
    target = sys.argv[1]
    files = [os.path.join(target, f) for f in sorted(os.listdir(target))] if os.path.isdir(target) else [target]
    ok = 0
    for f in files:
        d = open(f, 'rb').read()
        try:
            e = parse(d)
        except Exception as ex:  # noqa: BLE001
            print('FAIL', os.path.basename(f), ex)
            continue
        if e['end'] != e['pool']:
            print('GAP ', os.path.basename(f), hex(e['end']), hex(e['pool']))
            continue
        ok += 1
    print(f'{ok}/{len(files)} files parse exactly up to their string pool')
