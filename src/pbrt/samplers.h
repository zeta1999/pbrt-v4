// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#ifndef PBRT_SAMPLERS_H
#define PBRT_SAMPLERS_H

#include <pbrt/pbrt.h>

#include <pbrt/base/sampler.h>
#include <pbrt/filters.h>
#include <pbrt/options.h>
#include <pbrt/util/bluenoise.h>
#include <pbrt/util/check.h>
#include <pbrt/util/error.h>
#include <pbrt/util/lowdiscrepancy.h>
#include <pbrt/util/math.h>
#include <pbrt/util/pmj02tables.h>
#include <pbrt/util/primes.h>
#include <pbrt/util/pstd.h>
#include <pbrt/util/rng.h>
#include <pbrt/util/vecmath.h>

#include <limits>
#include <memory>
#include <string>

namespace pbrt {

// HaltonSampler Definition
class HaltonSampler {
  public:
    // HaltonSampler Public Methods
    HaltonSampler(int samplesPerPixel, const Point2i &fullResolution, int seed = 0,
                  Allocator alloc = {});

    PBRT_CPU_GPU
    static constexpr const char *Name() { return "HaltonSampler"; }
    static HaltonSampler *Create(const ParameterDictionary &parameters,
                                 const Point2i &fullResolution, const FileLoc *loc,
                                 Allocator alloc);

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return samplesPerPixel; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int sampleIndex, int dim) {
        haltonIndex = 0;
        int sampleStride = baseScales[0] * baseScales[1];
        // Compute Halton sample offset for first sample in pixel _p_
        if (sampleStride > 1) {
            Point2i pm(Mod(p[0], MaxHaltonResolution), Mod(p[1], MaxHaltonResolution));
            for (int i = 0; i < 2; ++i) {
                uint64_t dimOffset =
                    (i == 0) ? InverseRadicalInverse(pm[i], 2, baseExponents[i])
                             : InverseRadicalInverse(pm[i], 3, baseExponents[i]);
                haltonIndex +=
                    dimOffset * (sampleStride / baseScales[i]) * multInverse[i];
            }
            haltonIndex %= sampleStride;
        }

        haltonIndex += sampleIndex * sampleStride;
        dimension = dim;
    }

    PBRT_CPU_GPU
    Float Get1D() {
        if (dimension >= PrimeTableSize)
            dimension = 2;
        int dim = dimension++;
        return ScrambledRadicalInverse(dim, haltonIndex, (*digitPermutations)[dim]);
    }

    PBRT_CPU_GPU
    Point2f Get2D() {
        if (dimension == 0) {
            // Return Halton pixel sample
            dimension += 2;
            return {RadicalInverse(0, haltonIndex >> baseExponents[0]),
                    RadicalInverse(1, haltonIndex / baseScales[1])};

        } else {
            // Return non-pixel 2D Halton sample
            if (dimension + 1 >= PrimeTableSize)
                dimension = 2;
            int dim = dimension;
            dimension += 2;
            return {ScrambledRadicalInverse(dim, haltonIndex, (*digitPermutations)[dim]),
                    ScrambledRadicalInverse(dim + 1, haltonIndex,
                                            (*digitPermutations)[dim + 1])};
        }
    }

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);
    std::string ToString() const;

  private:
    // HaltonSampler Private Methods
    static uint64_t multiplicativeInverse(int64_t a, int64_t n) {
        int64_t x, y;
        extendedGCD(a, n, &x, &y);
        return Mod(x, n);
    }

    static void extendedGCD(uint64_t a, uint64_t b, int64_t *x, int64_t *y) {
        if (b == 0) {
            *x = 1;
            *y = 0;
            return;
        }
        int64_t d = a / b, xp, yp;
        extendedGCD(b, a % b, &xp, &yp);
        *x = yp;
        *y = xp - (d * yp);
    }

    // HaltonSampler Private Members
    int samplesPerPixel;
    pstd::vector<DigitPermutation> *digitPermutations;
    static constexpr int MaxHaltonResolution = 128;
    Point2i baseScales, baseExponents;
    int multInverse[2];
    int64_t haltonIndex = 0;
    int dimension = 0;
};

// PaddedSobolSampler Definition
class PaddedSobolSampler {
  public:
    // PaddedSobolSampler Public Methods
    PBRT_CPU_GPU
    static constexpr const char *Name() { return "PaddedSobolSampler"; }
    static PaddedSobolSampler *Create(const ParameterDictionary &parameters,
                                      const FileLoc *loc, Allocator alloc);

    PaddedSobolSampler(int samplesPerPixel, RandomizeStrategy randomizer)
        : samplesPerPixel(samplesPerPixel), randomizeStrategy(randomizer) {
        if (!IsPowerOf2(samplesPerPixel))
            Warning("Sobol samplers with non power-of-two sample counts (%d) are "
                    "sub-optimal.",
                    samplesPerPixel);
    }

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return samplesPerPixel; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int index, int dim) {
        pixel = p;
        sampleIndex = index;
        dimension = dim;
    }

    PBRT_CPU_GPU
    Float Get1D() {
        // Get permuted index for current pixel sample
        uint64_t hash = MixBits(((uint64_t)pixel.x << 48) ^ ((uint64_t)pixel.y << 32) ^
                                ((uint64_t)dimension << 16) ^ GetOptions().seed);
        int index = PermutationElement(sampleIndex, samplesPerPixel, hash);

        int dim = dimension++;
        // Return randomized 1D van der Corput sample for dimension _dim_
        if (randomizeStrategy == RandomizeStrategy::CranleyPatterson)
            // Return 1D sample randomized with Cranley-Patterson rotation
            return SobolSample(index, 0, CranleyPattersonRotator(BlueNoise(dim, pixel)));

        else
            return SampleDimension(0, index, hash >> 32);
    }

    PBRT_CPU_GPU
    Point2f Get2D() {
        // Get permuted index for current pixel sample
        uint64_t hash = MixBits(((uint64_t)pixel.x << 48) ^ ((uint64_t)pixel.y << 32) ^
                                ((uint64_t)dimension << 16) ^ GetOptions().seed);
        int index = PermutationElement(sampleIndex, samplesPerPixel, hash);

        int dim = dimension;
        dimension += 2;
        // Return randomized 2D Sobol' sample
        if (randomizeStrategy == RandomizeStrategy::CranleyPatterson) {
            // Return 2D sample randomized with Cranley-Patterson rotation
            return {SobolSample(index, 0, CranleyPattersonRotator(BlueNoise(dim, pixel))),
                    SobolSample(index, 1,
                                CranleyPattersonRotator(BlueNoise(dim + 1, pixel)))};

        } else
            return {SampleDimension(0, index, hash >> 8),
                    SampleDimension(1, index, hash >> 32)};
    }

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);
    std::string ToString() const;

  private:
    // PaddedSobolSampler Private Methods
    PBRT_CPU_GPU
    Float SampleDimension(int dimension, uint32_t a, uint32_t hash) const {
        switch (randomizeStrategy) {
        case RandomizeStrategy::None:
            return SobolSample(a, dimension, NoRandomizer());
        case RandomizeStrategy::XOR:
            return SobolSample(a, dimension, XORScrambler(hash));
        case RandomizeStrategy::Owen:
            return SobolSample(a, dimension, OwenScrambler(hash));
        default:
            LOG_FATAL("Unhandled randomization strategy");
            return {};
        }
    }

    // PaddedSobolSampler Private Members
    int samplesPerPixel;
    RandomizeStrategy randomizeStrategy;
    Point2i pixel;
    int sampleIndex, dimension;
};

// PMJ02BNSampler Definition
class PMJ02BNSampler {
  public:
    // PMJ02BNSampler Public Methods
    PMJ02BNSampler(int samplesPerPixel, int seed = 0, Allocator alloc = {});

    PBRT_CPU_GPU
    static constexpr const char *Name() { return "PMJ02BNSampler"; }
    static PMJ02BNSampler *Create(const ParameterDictionary &parameters,
                                  const FileLoc *loc, Allocator alloc);

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return samplesPerPixel; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int index, int dim) {
        pixel = p;
        sampleIndex = index;
        dimension = dim;
    }

    PBRT_CPU_GPU
    Float Get1D() {
        // Find permuted sample index for 1D PMJ02BNSampler sample
        uint64_t hash = MixBits(((uint64_t)pixel.x << 48) ^ ((uint64_t)pixel.y << 32) ^
                                ((uint64_t)dimension << 16) ^ seed);
        int index = PermutationElement(sampleIndex, samplesPerPixel, hash);

        Float delta = BlueNoise(dimension, pixel);
        ++dimension;
        return std::min((index + delta) / samplesPerPixel, OneMinusEpsilon);
    }

    PBRT_CPU_GPU
    Point2f Get2D() {
        if (dimension == 0) {
            // Return pmj02bn pixel sample
            int px = pixel.x % pixelTileSize, py = pixel.y % pixelTileSize;
            int offset = (px + py * pixelTileSize) * samplesPerPixel;
            dimension += 2;
            return (*pixelSamples)[offset + sampleIndex];

        } else {
            // Compute index for 2D pmj02bn sample
            int index = sampleIndex;
            int pmjInstance = dimension / 2;
            if (pmjInstance >= nPMJ02bnSets) {
                // Permute index to be used for pmj02bn sample array
                uint64_t hash =
                    MixBits(((uint64_t)pixel.x << 48) ^ ((uint64_t)pixel.y << 32) ^
                            ((uint64_t)dimension << 16) ^ seed);
                index = PermutationElement(sampleIndex, samplesPerPixel, hash);
            }

            // Return randomized pmj02bn sample for current dimension
            Point2f u = GetPMJ02BNSample(pmjInstance, index);
            // Apply Cranley-Patterson rotation to pmj02bn sample _u_
            u += Vector2f(BlueNoise(dimension, pixel), BlueNoise(dimension + 1, pixel));
            if (u.x >= 1)
                u.x -= 1;
            if (u.y >= 1)
                u.y -= 1;

            dimension += 2;
            return {std::min(u.x, OneMinusEpsilon), std::min(u.y, OneMinusEpsilon)};
        }
    }

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);
    std::string ToString() const;

  private:
    // PMJ02BNSampler Private Members
    int samplesPerPixel, seed;
    int pixelTileSize;
    pstd::vector<Point2f> *pixelSamples;
    Point2i pixel;
    int sampleIndex, dimension;
};

// RandomSampler Definition
class RandomSampler {
  public:
    // RandomSampler Public Methods
    RandomSampler(int samplesPerPixel, int seed = 0)
        : samplesPerPixel(samplesPerPixel), seed(seed) {}

    static RandomSampler *Create(const ParameterDictionary &parameters,
                                 const FileLoc *loc, Allocator alloc);
    PBRT_CPU_GPU
    static constexpr const char *Name() { return "RandomSampler"; }

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return samplesPerPixel; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int sampleIndex, int dimension) {
        rng.SetSequence((p.x + p.y * 65536) | (uint64_t(seed) << 32));
        rng.Advance(sampleIndex * 65536 + dimension);
    }

    PBRT_CPU_GPU
    Float Get1D() { return rng.Uniform<Float>(); }
    PBRT_CPU_GPU
    Point2f Get2D() { return {rng.Uniform<Float>(), rng.Uniform<Float>()}; }

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);
    std::string ToString() const;

  private:
    // RandomSampler Private Members
    int samplesPerPixel, seed;
    RNG rng;
};

// SobolSampler Definition
class SobolSampler {
  public:
    // SobolSampler Public Methods
    SobolSampler(int samplesPerPixel, const Point2i &fullResolution,
                 RandomizeStrategy randomizeStrategy)
        : samplesPerPixel(samplesPerPixel), randomizeStrategy(randomizeStrategy) {
        if (!IsPowerOf2(samplesPerPixel))
            Warning("Non power-of-two sample count %d will perform sub-optimally with "
                    "the SobolSampler.",
                    samplesPerPixel);
        scale = RoundUpPow2(std::max(fullResolution.x, fullResolution.y));
    }

    PBRT_CPU_GPU
    static constexpr const char *Name() { return "SobolSampler"; }
    static SobolSampler *Create(const ParameterDictionary &parameters,
                                const Point2i &fullResolution, const FileLoc *loc,
                                Allocator alloc);

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return samplesPerPixel; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int sampleIndex, int dim) {
        pixel = p;
        dimension = dim;
        sobolIndex = SobolIntervalToIndex(Log2Int(scale), sampleIndex, pixel);
    }

    PBRT_CPU_GPU
    Float Get1D() {
        if (dimension >= NSobolDimensions)
            dimension = 2;
        return SampleDimension(dimension++);
    }

    PBRT_CPU_GPU
    Point2f Get2D() {
        if (dimension + 1 >= NSobolDimensions)
            dimension = 2;
        Point2f u(SampleDimension(dimension), SampleDimension(dimension + 1));
        if (dimension == 0) {
            // Remap Sobol$'$ dimensions used for pixel samples
            for (int dim = 0; dim < 2; ++dim) {
                CHECK_RARE(1e-7, u[dim] * scale - pixel[dim] < 0);
                CHECK_RARE(1e-7, u[dim] * scale - pixel[dim] > 1);
                u[dim] = Clamp(u[dim] * scale - pixel[dim], 0, OneMinusEpsilon);
            }
        }
        dimension += 2;
        return u;
    }

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);
    std::string ToString() const;

  private:
    // SobolSampler Private Methods
    PBRT_CPU_GPU
    Float SampleDimension(int dimension) const {
        // Return un-randomized Sobol sample if appropriate
        if (dimension < 2 || randomizeStrategy == RandomizeStrategy::None)
            return SobolSample(sobolIndex, dimension, NoRandomizer());

        // Return randomized Sobol sample using _randomizeStrategy_
        uint32_t hash = MixBits((uint64_t(dimension) << 32) ^ GetOptions().seed);
        if (randomizeStrategy == RandomizeStrategy::CranleyPatterson)
            return SobolSample(sobolIndex, dimension, CranleyPattersonRotator(hash));
        else if (randomizeStrategy == RandomizeStrategy::XOR)
            return SobolSample(sobolIndex, dimension, XORScrambler(hash));
        else
            return SobolSample(sobolIndex, dimension, OwenScrambler(hash));
    }

    // SobolSampler Private Members
    int samplesPerPixel, scale;
    RandomizeStrategy randomizeStrategy;
    Point2i pixel;
    int dimension;
    int64_t sobolIndex;
};

// StratifiedSampler Definition
class StratifiedSampler {
  public:
    // StratifiedSampler Public Methods
    StratifiedSampler(int xPixelSamples, int yPixelSamples, bool jitter, int seed = 0)
        : xPixelSamples(xPixelSamples),
          yPixelSamples(yPixelSamples),
          seed(seed),
          jitter(jitter) {}

    static StratifiedSampler *Create(const ParameterDictionary &parameters,
                                     const FileLoc *loc, Allocator alloc);
    PBRT_CPU_GPU
    static constexpr const char *Name() { return "StratifiedSampler"; }

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return xPixelSamples * yPixelSamples; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int index, int dim) {
        pixel = p;
        sampleIndex = index;
        dimension = dim;
        rng.SetSequence((p.x + p.y * 65536) | (uint64_t(seed) << 32));
        rng.Advance(sampleIndex * 65536 + dimension);
    }

    PBRT_CPU_GPU
    Float Get1D() {
        // Compute _stratum_ index for current pixel and dimension
        uint64_t hash = MixBits(((uint64_t)pixel.x << 48) ^ ((uint64_t)pixel.y << 32) ^
                                ((uint64_t)dimension << 16) ^ seed);
        int stratum = PermutationElement(sampleIndex, SamplesPerPixel(), hash);

        ++dimension;
        Float delta = jitter ? rng.Uniform<Float>() : 0.5f;
        return (stratum + delta) / SamplesPerPixel();
    }

    PBRT_CPU_GPU
    Point2f Get2D() {
        // Compute _stratum_ index for current pixel and dimension
        uint64_t hash = MixBits(((uint64_t)pixel.x << 48) ^ ((uint64_t)pixel.y << 32) ^
                                ((uint64_t)dimension << 16) ^ seed);
        int stratum = PermutationElement(sampleIndex, SamplesPerPixel(), hash);

        dimension += 2;
        int x = stratum % xPixelSamples, y = stratum / xPixelSamples;
        Float dx = jitter ? rng.Uniform<Float>() : 0.5f;
        Float dy = jitter ? rng.Uniform<Float>() : 0.5f;
        return {(x + dx) / xPixelSamples, (y + dy) / yPixelSamples};
    }

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);
    std::string ToString() const;

  private:
    // StratifiedSampler Private Members
    int xPixelSamples, yPixelSamples, seed;
    bool jitter;
    RNG rng;
    Point2i pixel;
    int sampleIndex = 0, dimension = 0;
};

// MLTSampler Definition
class MLTSampler {
  public:
    // MLTSampler Public Methods
    MLTSampler(int mutationsPerPixel, int rngSequenceIndex, Float sigma,
               Float largeStepProbability, int streamCount)
        : mutationsPerPixel(mutationsPerPixel),
          rng(rngSequenceIndex),
          sigma(sigma),
          largeStepProbability(largeStepProbability),
          streamCount(streamCount) {}

    PBRT_CPU_GPU
    void StartIteration();

    PBRT_CPU_GPU
    void Reject();

    PBRT_CPU_GPU
    void StartStream(int index);

    PBRT_CPU_GPU
    int GetNextIndex() { return streamIndex + streamCount * sampleIndex++; }

    PBRT_CPU_GPU
    int SamplesPerPixel() const { return mutationsPerPixel; }

    PBRT_CPU_GPU
    void StartPixelSample(const Point2i &p, int sampleIndex, int dim) {
        rng.SetSequence(p.x + p.y * 65536);
        rng.Advance(sampleIndex * 65536 + dim * 8192);
    }

    PBRT_CPU_GPU
    Float Get1D();

    PBRT_CPU_GPU
    Point2f Get2D();

    std::vector<SamplerHandle> Clone(int n, Allocator alloc);

    PBRT_CPU_GPU
    void Accept();

    std::string DumpState() const;

    std::string ToString() const {
        return StringPrintf(
            "[ MLTSampler rng: %s sigma: %f largeStepProbability: %f "
            "streamCount: %d X: %s currentIteration: %d largeStep: %s "
            "lastLargeStepIteration: %d streamIndex: %d sampleIndex: %d ] ",
            rng, sigma, largeStepProbability, streamCount, X, currentIteration, largeStep,
            lastLargeStepIteration, streamIndex, sampleIndex);
    }

  protected:
    // MLTSampler Private Declarations
    struct PrimarySample {
        Float value = 0;
        // PrimarySample Public Methods
        PBRT_CPU_GPU
        void Backup() {
            valueBackup = value;
            modifyBackup = lastModificationIteration;
        }
        PBRT_CPU_GPU
        void Restore() {
            value = valueBackup;
            lastModificationIteration = modifyBackup;
        }

        std::string ToString() const {
            return StringPrintf("[ PrimarySample lastModificationIteration: %d "
                                "valueBackup: %f modifyBackup: %d ]",
                                lastModificationIteration, valueBackup, modifyBackup);
        }

        // PrimarySample Public Members
        int64_t lastModificationIteration = 0;
        Float valueBackup = 0;
        int64_t modifyBackup = 0;
    };

    // MLTSampler Private Methods
    PBRT_CPU_GPU
    void EnsureReady(int index);

    // MLTSampler Private Members
    int mutationsPerPixel;
    RNG rng;
    const Float sigma, largeStepProbability;
    const int streamCount;
    pstd::vector<PrimarySample> X;
    int64_t currentIteration = 0;
    bool largeStep = true;
    int64_t lastLargeStepIteration = 0;
    int streamIndex, sampleIndex;
};

class DebugMLTSampler : public MLTSampler {
  public:
    static DebugMLTSampler Create(pstd::span<const std::string> state,
                                  int nSampleStreams);

    PBRT_CPU_GPU
    Float Get1D() {
        int index = GetNextIndex();
        CHECK_LT(index, u.size());
#ifdef PBRT_IS_GPU_CODE
        return 0;
#else
        return u[index];
#endif
    }

    PBRT_CPU_GPU
    Point2f Get2D() { return {Get1D(), Get1D()}; }

    std::string ToString() const {
        return StringPrintf("[ DebugMLTSampler %s u: %s ]",
                            ((const MLTSampler *)this)->ToString(), u);
    }

  private:
    DebugMLTSampler(int nSampleStreams) : MLTSampler(1, 0, 0.5, 0.5, nSampleStreams) {}

    std::vector<Float> u;
};

inline void SamplerHandle::StartPixelSample(const Point2i &p, int sampleIndex,
                                            int dimension) {
    auto start = [&](auto ptr) {
        return ptr->StartPixelSample(p, sampleIndex, dimension);
    };
    return Dispatch(start);
}

inline int SamplerHandle::SamplesPerPixel() const {
    auto spp = [&](auto ptr) { return ptr->SamplesPerPixel(); };
    return Dispatch(spp);
}

inline Float SamplerHandle::Get1D() {
    auto get = [&](auto ptr) { return ptr->Get1D(); };
    return Dispatch(get);
}

inline Point2f SamplerHandle::Get2D() {
    auto get = [&](auto ptr) { return ptr->Get2D(); };
    return Dispatch(get);
}

// Sampler Inline Functions
template <typename Sampler>
inline PBRT_CPU_GPU CameraSample GetCameraSample(Sampler sampler, const Point2i &pPixel,
                                                 FilterHandle filter) {
    FilterSample fs = filter.Sample(sampler.Get2D());
    if (GetOptions().disablePixelJitter) {
        fs.p = Point2f(0, 0);
        fs.weight = 1;
    }

    CameraSample cs;
    cs.pFilm = pPixel + fs.p + Vector2f(0.5, 0.5);
    cs.time = sampler.Get1D();
    cs.pLens = sampler.Get2D();
    cs.weight = fs.weight;
    return cs;
}

}  // namespace pbrt

#endif  // PBRT_SAMPLERS_H
