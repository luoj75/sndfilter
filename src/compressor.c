// (c) Copyright 2016, Sean Connelly (@voidqk), http://syntheti.cc
// MIT License
// Project Home: https://github.com/voidqk/sndfilter

#include "compressor.h"
#include <math.h>
#include <string.h>

// core algorithm extracted from Chromium source, DynamicsCompressorKernel.cpp, here:
//   https://git.io/v1uSK

// sane defaults
const sf_compressor_params_st sf_compressor_defaults = (sf_compressor_params_st){
	.threshold    = -24.000f,  // [-100, 0] dB
	.knee         =  30.000f,  // [0, 40]   dB
	.ratio        =  12.000f,  // [1, 20]   unit less
	.attack       =   0.003f,  // [0, 1]    seconds
	.release      =   0.250f,  // [0, 1]    seconds
	.predelay     =   0.006f,  // seconds
	.releasezone1 =   0.090f,  // release zones range from 0 to 1, increasing
	.releasezone2 =   0.160f,  //
	.releasezone3 =   0.420f,  //
	.releasezone4 =   0.980f,  //
	.postgain     =   0.000f,  // dB
	.wet          =   1.000f   // [0, 1]
};

static inline float db2lin(float db){ // dB to linear
	return powf(10.0f, 0.05f * db);
}

static inline float lin2db(float lin){ // linear to dB
	return 20.0f * log10f(lin);
}

// for more information on the knee curve, check out the compressor-curve.html demo + source code
// included in this repo
static inline float kneecurve(float x, float k, float linearthreshold){
	return linearthreshold + (1.0f - expf(-k * (x - linearthreshold))) / k;
}

static inline float kneeslope(float x, float k, float linearthreshold){
	return k * x / ((k * linearthreshold + 1.0f) * expf(k * (x - linearthreshold)) - 1);
}

static inline float compcurve(float x, float k, float slope, float linearthreshold,
	float linearthresholdknee, float threshold, float knee, float kneedboffset){
	if (x < linearthreshold)
		return x;
	if (knee <= 0.0f) // no knee in curve
		return db2lin(threshold + slope * (lin2db(x) - threshold));
	if (x < linearthresholdknee)
		return kneecurve(x, k, linearthreshold);
	return db2lin(kneedboffset + slope * (lin2db(x) - threshold - knee));
}

// for more information on the adaptive release curve, check out adaptive-release-curve.html demo +
// source code included in this repo
static inline float adaptivereleasecurve(float x, float a, float b, float c, float d){
	// a*x^3 + b*x^2 + c*x + d
	float x2 = x * x;
	return a * x2 * x + b * x2 + c * x + d;
}

static inline float clampf(float v, float min, float max){
	return v < min ? min : (v > max ? max : v);
}

static inline float absf(float v){
	return v < 0.0f ? -v : v;
}

static inline void meterout(float gaindb){
	// this function doesn't do anything, but if you want to have a meter that is displayed showing
	// the full effect of the compressor, you would plug in here
	// it is currently updated after every chunk, but could be updated after every sample if you
	// realy wanted it... but that seems like overkill to me
}

sf_snd sf_compressor(sf_snd snd, sf_compressor_params_st params){
	// setup the predelay buffer and output sound
	int delaybufsize = snd->rate * params.predelay;
	sf_sample_st *delaybuf = malloc(sizeof(sf_sample_st) * delaybufsize);
	if (delaybuf == NULL)
		return NULL;
	memset(delaybuf, 0, sizeof(sf_sample_st) * delaybufsize);
	sf_snd out = sf_snd_new();
	if (out == NULL){
		free(delaybuf);
		return NULL;
	}
	int samplesperchunk = 32; // process everything in chunks of 32 samples at a time
	int chunks = snd->size / samplesperchunk;
	out->samples = malloc(sizeof(sf_sample_st) * chunks * samplesperchunk);
	if (out->samples == NULL){
		sf_snd_free(out);
		free(delaybuf);
		return NULL;
	}

	// useful values
	float linearthreshold = db2lin(params.threshold);
	float slope = 1.0f / params.ratio;
	float attacksamples = (float)snd->rate * params.attack;
	float attacksamplesinv = 1.0f / attacksamples;
	float releasesamples = (float)snd->rate * params.release;
	float satrelease = 0.0025f; // seconds
	float satreleasesamplesinv = 1.0f / ((float)snd->rate * satrelease);
	float dry = 1.0f - params.wet;

	// metering values (not used in core algorithm, but used to output a meter if desired)
	float metergain = 1.0f;
	float meterfalloff = 0.325f; // seconds
	float meterrelease = 1.0f - expf(-1.0f / ((float)snd->rate * meterfalloff));

	// calculate knee curve parameters
	float k = 5;
	float kneedboffset;
	float linearthresholdknee;
	if (params.knee > 0.0f){ // if a knee exists, search for a good k value
		float xknee = db2lin(params.threshold + params.knee);
		float mink = 0.1f;
		float maxk = 10000.0f;
		// search by comparing the knee slope at the current k guess, to the ideal slope
		for (int i = 0; i < 15; i++){
			if (kneeslope(xknee, k, linearthreshold) < slope)
				maxk = k;
			else
				mink = k;
			k = sqrtf(mink * maxk);
		}
		kneedboffset = lin2db(kneecurve(xknee, k, linearthreshold));
		linearthresholdknee = db2lin(params.threshold + params.knee);
	}

	// calculate a master gain based on what sounds good
	float fulllevel = compcurve(1.0f, k, slope, linearthreshold, linearthresholdknee,
		params.threshold, params.knee, kneedboffset);
	float mastergain = db2lin(params.postgain) * powf(1.0f / fulllevel, 0.6f);

	// calculate the adaptive release curve parameters
	// solve a,b,c,d in `y = a*x^3 + b*x^2 + c*x + d`
	// using known points (x, y) to (0, y1), (1, y2), (2, y3), (3, y4)
	float y1 = releasesamples * params.releasezone1;
	float y2 = releasesamples * params.releasezone2;
	float y3 = releasesamples * params.releasezone3;
	float y4 = releasesamples * params.releasezone4;
	float a = (-y1 + 3.0f * y2 - 3.0f * y3 + y4) / 6.0f;
	float b = y1 - 2.5f * y2 + 2.0f * y3 - 0.5f * y4;
	float c = (-11.0f * y1 + 18.0f * y2 - 9.0f * y3 + 2.0f * y4) / 6.0f;
	float d = y1;

	// process chunks
	int samplepos = 0;
	int delaywritepos = 0;
	int delayreadpos = 1;
	float detectoravg = 0.0f;
	float compgain = 1.0f;
	float ang90 = (float)M_PI * 0.5f;
	float ang90inv = 2.0f / (float)M_PI;
	float maxcompdiffdb = -1;
	float spacingdb = 5.0f;
	for (int ch = 0; ch < chunks; ch++){
		float desiredgain = detectoravg;
		float scaleddesiredgain = asinf(desiredgain) * ang90inv;
		float compdiffdb = lin2db(compgain / scaleddesiredgain);

		// calculate envelope rate based on whether we're attacking or releasing
		float enveloperate;
		if (compdiffdb < 0.0f){ // compgain < scaleddesiredgain, so we're releasing
			maxcompdiffdb = -1; // reset for a future attack mode
			// apply the adaptive release curve
			// scale compdiffdb between 0-3
			float x = (clampf(compdiffdb, -12.0f, 0.0f) + 12.0f) * 0.25f;
			float releasesamples = adaptivereleasecurve(x, a, b, c, d);
			enveloperate = db2lin(spacingdb / releasesamples);
		}
		else{ // compresorgain > scaleddesiredgain, so we're attacking
			if (maxcompdiffdb == -1 || maxcompdiffdb < compdiffdb)
				maxcompdiffdb = compdiffdb;
			float attenuate = maxcompdiffdb;
			if (attenuate < 0.5f)
				attenuate = 0.5f;
			enveloperate = 1.0f - powf(0.25f / attenuate, attacksamplesinv);
		}

		// process the chunk
		for (int chi = 0; chi < samplesperchunk; chi++, samplepos++,
			delayreadpos = (delayreadpos + 1) % delaybufsize,
			delaywritepos = (delaywritepos + 1) % delaybufsize){

			delaybuf[delaywritepos] = snd->samples[samplepos];
			float inputL = absf(snd->samples[samplepos].L);
			float inputR = absf(snd->samples[samplepos].R);
			float inputmax = inputL > inputR ? inputL : inputR;

			float attenuation;
			if (inputmax < 0.0001f)
				attenuation = 1.0f;
			else{
				float inputcomp = compcurve(inputmax, k, slope, linearthreshold,
					linearthresholdknee, params.threshold, params.knee, kneedboffset);
				attenuation = inputcomp / inputmax;
			}

			float rate;
			if (attenuation > detectoravg){ // if releasing
				float attenuationdb = -lin2db(attenuation);
				if (attenuationdb < 2.0f)
					attenuationdb = 2.0f;
				float dbpersample = attenuationdb * satreleasesamplesinv;
				rate = db2lin(dbpersample) - 1.0f;
			}
			else
				rate = 1.0f;

			detectoravg += (attenuation - detectoravg) * rate;
			if (detectoravg > 1.0f)
				detectoravg = 1.0f;

			if (enveloperate < 1) // attack, reduce gain
				compgain += (scaleddesiredgain - compgain) * enveloperate;
			else{ // release, increase gain
				compgain *= enveloperate;
				if (compgain > 1.0f)
					compgain = 1.0f;
			}

			// the final gain value!
			float premixgain = sinf(ang90 * compgain);
			float gain = dry + params.wet * mastergain * premixgain;

			// calculate metering (not used in core algo, but used to output a meter if desired)
			float premixgaindb = lin2db(premixgain);
			if (premixgaindb < metergain)
				metergain = premixgaindb; // spike immediately
			else
				metergain += (premixgaindb - metergain) * meterrelease; // fall slowly
			//meterout(metergain); // output a meter per sample

			// apply the gain
			out->samples[samplepos] = (sf_sample_st){
				.L = delaybuf[delayreadpos].L * gain,
				.R = delaybuf[delayreadpos].R * gain
			};
		}
		meterout(metergain); // output a meter per chunk
	}
	free(delaybuf);
	return out;
}
