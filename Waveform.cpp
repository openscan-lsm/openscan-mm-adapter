#include "Waveform.h"

#include <math.h>
#include <stdlib.h>


inline int
VoltsToDACUnits(double p, double zoom, uint16_t *result)
{
	double scaled = round(p / zoom * 3276.8 + 32768.0);
	if (scaled < 0 || scaled > UINT16_MAX)
		return -1;
	*result = (uint16_t)scaled;
	return 0;
}


int
GenerateScaledWaveforms(uint32_t resolution, double zoom, uint16_t *xScaled, uint16_t *yScaled,
	double galvoOffsetX, double galvoOffsetY)
{
	size_t xLength = X_UNDERSHOOT + resolution + X_RETRACE_LEN;
	size_t yLength = resolution;

	/*
	Right now the galvoOffset is in fractions of the FoV.  Eventually we will want it to be in
	angle, so we can scale it properly.  These two variables will replace galvoOffset in the
	GenerateGalvoWaveform call once we figure out the proper scaling.  Otherwise we can modify
	the input galvoOffset to be in fractions

	scaledOffsetX;
	scaledOffsetY;
	*/

	double *xWaveform = (double *)malloc(sizeof(double) * xLength);
	double *yWaveform = (double *)malloc(sizeof(double) * yLength);
	GenerateGalvoWaveform(resolution, X_RETRACE_LEN, X_UNDERSHOOT,
		-0.5 + galvoOffsetX, 0.5 + galvoOffsetX, xWaveform);
	GenerateGalvoWaveform(resolution, 0, 0, 
		-0.5 + galvoOffsetY, 0.5 + galvoOffsetY, yWaveform);

	for (int i = 0; i < xLength; ++i) {
		if (VoltsToDACUnits(xWaveform[i], zoom, &(xScaled[i])) != 0)
			return -1;
	}
	for (int j = 0; j < yLength; ++j) {
		if (VoltsToDACUnits(yWaveform[j], zoom, &(yScaled[j])) != 0)
			return -1;
	}

	free(xWaveform);
	free(yWaveform);

	return 0;
}


void
GenerateGalvoWaveform(int32_t effectiveScanLen, int32_t retraceLen,
	int32_t undershootLen, double scanStart, double scanEnd, double *waveform)
{
	double scanAmplitude = scanEnd - scanStart;
	double step = scanAmplitude / (effectiveScanLen - 1);
	int32_t linearLen = undershootLen + effectiveScanLen;

	// Generate the linear scan curve
	double undershootStart = scanStart - undershootLen * step;
	for (int i = 0; i < linearLen; ++i)
	{
		waveform[i] = undershootStart + scanAmplitude * ((double)i / (effectiveScanLen - 1));
	}

	// Generate the rescan curve
	// Slope at start end end are both equal to the linear scan
	if (retraceLen > 0)
	{
		SplineInterpolate(retraceLen, scanEnd, undershootStart, step, step, waveform + linearLen);
	}
}


void SplineInterpolate(int32_t n, double yFirst, double yLast,
	double slopeFirst, double slopeLast, double* result)
{
	double c[4];

	c[0] = slopeFirst / (n*n) + 2.0 / (n*n*n)*yFirst + slopeLast / (n*n) - 2.0 / (n*n*n)*yLast;
	c[1] = 3.0 / (n*n)*yLast - slopeLast / n - 2.0 / n*slopeFirst - 3.0 / (n*n)*yFirst;
	c[2] = slopeFirst;
	c[3] = yFirst;

	for (int32_t x = 0; x < n; x++)
	{
		result[x] = c[0] * x*x*x + c[1] * x*x + c[2] * x + c[3];
	}
}