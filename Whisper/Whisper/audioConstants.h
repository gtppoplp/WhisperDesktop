#pragma once
#include <stdint.h>

namespace Whisper
{
	// WHISPER_SAMPLE_RATE, 16 kHz
	constexpr uint32_t SAMPLE_RATE = 16000;
	// WHISPER_N_FFT, 25 milliseconds
	constexpr uint32_t FFT_SIZE = 400;
	// WHISPER_HOP_LENGTH, 10 milliseconds
	constexpr uint32_t FFT_STEP = 160;
    // WHISPER_N_MEL: model-dependent, 80 for legacy/v2 and 128 for v3.
	// Keep this value as an upper bound for fixed-size stack buffers.
	constexpr uint32_t N_MEL = 128;
}