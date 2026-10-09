/* dsp::minBlepImpulse, answered from VCV Rack's own numbers.
 *
 * The table every saw and square edge is built from is computed at start-up
 * through a cepstrum: a logarithm of a spectrum whose stop band is rounding
 * noise, an FFT back, an exponential. What comes out depends on that noise,
 * so on the compiler and on the FFT's instruction set. Measured against VCV
 * Rack Free 2.6.4 for Linux (rack_ui_smoke --dump, vcv_dump.cpp): this
 * source built for a PC gave a table 0.3% away from the desktop's, and built
 * for arm64 one up to 19% away -- 0.227 where the desktop has 0.336 a tenth
 * of the way up the step, an overshoot of 0.954 for 1.074. Every oscillator
 * edge on a phone had another shape than in VCV Rack.
 *
 * So the sizes the shipped modules ask for (16 zero crossings, 16 and 32
 * times oversampled) are answered with the desktop's table, bit for bit, and
 * any other size is computed as before: native/CMakeLists.txt compiles
 * Rack's minblep.cpp with its function renamed to minBlepImpulseComputed. */
#include <cstring>

#include <dsp/minblep.hpp>

#include "minblep_vcv.inc"


namespace rack {
namespace dsp {


void minBlepImpulseComputed(int z, int o, float* output);


void minBlepImpulse(int z, int o, float* output) {
	if (z == 16 && o == 16)
		std::memcpy(output, minBlepVcv_16_16, sizeof(minBlepVcv_16_16));
	else if (z == 16 && o == 32)
		std::memcpy(output, minBlepVcv_16_32, sizeof(minBlepVcv_16_32));
	else
		minBlepImpulseComputed(z, o, output);
}


} // namespace dsp
} // namespace rack
