// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#ifndef PBRT_FILM_H
#define PBRT_FILM_H

// PhysLight code contributed by Anders Langlands and Luca Fascione
// Copyright (c) 2020, Weta Digital, Ltd.
// SPDX-License-Identifier: Apache-2.0

#include <pbrt/pbrt.h>

#include <pbrt/base/bxdf.h>
#include <pbrt/base/camera.h>
#include <pbrt/base/film.h>
#include <pbrt/bsdf.h>
#include <pbrt/util/color.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/parallel.h>
#include <pbrt/util/pstd.h>
#include <pbrt/util/sampling.h>
#include <pbrt/util/spectrum.h>
#include <pbrt/util/transform.h>
#include <pbrt/util/vecmath.h>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace pbrt {

// PixelSensor Definition
class PixelSensor {
  public:
    // PixelSensor Public Methods
    static PixelSensor *Create(const std::string &name, const RGBColorSpace *colorSpace,
                               Float exposureTime, Float fNumber, Float ISO, Float C,
                               Float whiteBalanceTemp, const FileLoc *loc,
                               Allocator alloc);

    PixelSensor(const RGBColorSpace *outputColorSpace, Float whiteBalanceTemp,
                Float exposureTime, Float fNumber, Float ISO, Float C, Allocator alloc)
        : r_bar(&Spectra::X(), alloc),
          g_bar(&Spectra::Y(), alloc),
          b_bar(&Spectra::Z(), alloc),
          exposureTime(exposureTime),
          fNumber(fNumber),
          ISO(ISO),
          C(C) {
        // Compute white balancing matrix for XYZ _PixelSensor_
        if (whiteBalanceTemp != 0) {
            auto whiteIlluminant = Spectra::D(whiteBalanceTemp, alloc);
            Point2f sourceWhite = SpectrumToXYZ(&whiteIlluminant).xy();
            Point2f targetWhite = outputColorSpace->w;
            XYZFromCameraRGB = WhiteBalance(sourceWhite, targetWhite);
        }
    }

    static PixelSensor *CreateDefault(Allocator alloc = {});

    PixelSensor(SpectrumHandle r_bar, SpectrumHandle g_bar, SpectrumHandle b_bar,
                const RGBColorSpace *outputColorSpace, Float whiteBalanceTemp,
                Float exposureTime, Float fNumber, Float ISO, Float C, Allocator alloc)
        : r_bar(r_bar, alloc),
          g_bar(g_bar, alloc),
          b_bar(b_bar, alloc),
          exposureTime(exposureTime),
          fNumber(fNumber),
          ISO(ISO),
          C(C) {
        // Compute XYZ from camera RGB matrix
        DenselySampledSpectrum sensorWhiteIlluminant =
            Spectra::D(whiteBalanceTemp, alloc);
        pstd::optional<SquareMatrix<3>> m =
            SolveCameraMatrix(sensorWhiteIlluminant, outputColorSpace->illuminant);
        if (!m)
            ErrorExit("Camera matrix could not be solved");
        XYZFromCameraRGB = *m;

        // Compute sensor's RGB normalization factor
        // Compute white normalization factor for sensor
        RGB white =
            IlluminantToCameraRGB(outputColorSpace->illuminant, r_bar, g_bar, b_bar);

        cameraRGBWhiteNorm = RGB(1, 1, 1) / white;
    }

    PBRT_CPU_GPU
    RGB ToCameraRGB(const SampledSpectrum &L, const SampledWavelengths &lambda) const {
        RGB rgb((r_bar.Sample(lambda) * L).Average(),
                (g_bar.Sample(lambda) * L).Average(),
                (b_bar.Sample(lambda) * L).Average());
        return rgb * cameraRGBWhiteNorm;
    }

    PBRT_CPU_GPU
    Float ImagingRatio() const {
        return Pi * exposureTime * ISO * K_m / (C * fNumber * fNumber);
    }

    // PixelSensor Public Members
    SquareMatrix<3> XYZFromCameraRGB;

  private:
    // PixelSensor Private Methods
    pstd::optional<SquareMatrix<3>> SolveCameraMatrix(
        const DenselySampledSpectrum &srcw, const DenselySampledSpectrum &dstw) const;

    static RGB IlluminantToCameraRGB(const DenselySampledSpectrum &illum,
                                     const DenselySampledSpectrum &r,
                                     const DenselySampledSpectrum &g,
                                     const DenselySampledSpectrum &b);

    static Float IlluminantToY(const DenselySampledSpectrum &illum,
                               const DenselySampledSpectrum &g);

    static RGB SpectrumToCameraRGB(SpectrumHandle s, const DenselySampledSpectrum &illum,
                                   const DenselySampledSpectrum &r,
                                   const DenselySampledSpectrum &g,
                                   const DenselySampledSpectrum &b);

    // PixelSensor Private Members
    DenselySampledSpectrum r_bar, g_bar, b_bar;
    Float exposureTime, fNumber, ISO, C;
    static std::vector<SpectrumHandle> trainingSwatches;
    RGB cameraRGBWhiteNorm = RGB(1, 1, 1);
};

// VisibleSurface Definition
class VisibleSurface {
  public:
    // VisibleSurface Public Methods
    VisibleSurface() = default;
    PBRT_CPU_GPU
    VisibleSurface(const SurfaceInteraction &si, const CameraTransform &cameraTransform,
                   const SampledSpectrum &albedo, const SampledWavelengths &lambda);

    std::string ToString() const;

    PBRT_CPU_GPU
    operator bool() const { return set; }

    // VisibleSurface Public Members
    bool set = false;
    Point3f p;
    Normal3f n, ns;
    Float time = 0;
    Float dzdx = 0, dzdy = 0;  // x/y: raster space, z: camera space
    SampledSpectrum albedo;
};

// FilmBase Definition
class FilmBase {
  public:
    // FilmBase Public Methods
    FilmBase(const Point2i &resolution, const Bounds2i &pixelBounds, FilterHandle filter,
             Float diagonal, const PixelSensor *sensor, const std::string &filename)
        : fullResolution(resolution),
          diagonal(diagonal * .001),
          filter(filter),
          filename(filename),
          pixelBounds(pixelBounds),
          sensor(sensor) {
        CHECK(!pixelBounds.IsEmpty());
        CHECK_GE(pixelBounds.pMin.x, 0);
        CHECK_LE(pixelBounds.pMax.x, resolution.x);
        CHECK_GE(pixelBounds.pMin.y, 0);
        CHECK_LE(pixelBounds.pMax.y, resolution.y);
        LOG_VERBOSE("Created film with full resolution %s, pixelBounds %s", resolution,
                    pixelBounds);
    }

    PBRT_CPU_GPU
    FilterHandle GetFilter() const { return filter; }
    PBRT_CPU_GPU
    Point2i FullResolution() const { return fullResolution; }
    PBRT_CPU_GPU
    Float Diagonal() const { return diagonal; }
    PBRT_CPU_GPU
    Bounds2i PixelBounds() const { return pixelBounds; }
    PBRT_CPU_GPU
    const PixelSensor *GetPixelSensor() const { return sensor; }
    std::string GetFilename() const { return filename; }

    std::string BaseToString() const;

    PBRT_CPU_GPU
    Bounds2f SampleBounds() const;

  protected:
    // FilmBase Protected Members
    Point2i fullResolution;
    Float diagonal;
    FilterHandle filter;
    std::string filename;
    Bounds2i pixelBounds;
    const PixelSensor *sensor;
};

// RGBFilm Definition
class RGBFilm : public FilmBase {
  public:
    // RGBFilm Public Methods
    PBRT_CPU_GPU
    void AddSample(const Point2i &pFilm, SampledSpectrum L,
                   const SampledWavelengths &lambda, const VisibleSurface *visibleSurface,
                   Float weight) {
        // First convert to sensor exposure, H, then to camera RGB
        SampledSpectrum H = L * sensor->ImagingRatio();
        RGB rgb = sensor->ToCameraRGB(H, lambda);
        // Optionally clamp sensor RGB value
        Float m = std::max({rgb.r, rgb.g, rgb.b});
        if (m > maxComponentValue) {
            H *= maxComponentValue / m;
            rgb *= maxComponentValue / m;
        }

        DCHECK(InsideExclusive(pFilm, pixelBounds));
        // Update pixel variance estimate
        // pixels[pFilm].varianceEstimator.Add(H.Average());
        pixels[pFilm].varianceEstimator.Add(L.Average());

        // Update pixel values with filtered sample contribution
        Pixel &pixel = pixels[pFilm];
        for (int c = 0; c < 3; ++c)
            pixel.rgbSum[c] += weight * rgb[c];
        pixel.weightSum += weight;
    }

    PBRT_CPU_GPU
    bool UsesVisibleSurface() const { return false; }

    PBRT_CPU_GPU
    RGB GetPixelRGB(const Point2i &p, Float splatScale = 1) const {
        const Pixel &pixel = pixels[p];
        RGB rgb(pixel.rgbSum[0], pixel.rgbSum[1], pixel.rgbSum[2]);
        // Normalize _rgb_ with weight sum
        Float weightSum = pixel.weightSum;
        if (weightSum != 0)
            rgb /= weightSum;

        // Add splat value at pixel
        for (int c = 0; c < 3; ++c)
            rgb[c] += splatScale * pixel.splatRGB[c] / filterIntegral;

        // Scale pixel value by _scale_
        rgb *= scale;

        // Convert _rgb_ to output RGB color space
        rgb = Mul<RGB>(outputRGBFromCameraRGB, rgb);

        return rgb;
    }

    RGBFilm() = default;
    RGBFilm(const PixelSensor *sensor, const Point2i &resolution,
            const Bounds2i &pixelBounds, FilterHandle filter, Float diagonal,
            const std::string &filename, Float scale, const RGBColorSpace *colorSpace,
            Float maxComponentValue = Infinity, bool writeFP16 = true,
            Allocator allocator = {});

    static RGBFilm *Create(const ParameterDictionary &parameters, Float exposureTime,
                           FilterHandle filter, const RGBColorSpace *colorSpace,
                           const FileLoc *loc, Allocator alloc);

    PBRT_CPU_GPU
    SampledWavelengths SampleWavelengths(Float u) const;

    PBRT_CPU_GPU
    void AddSplat(const Point2f &p, SampledSpectrum v, const SampledWavelengths &lambda);

    PBRT_CPU_GPU
    RGB ToOutputRGB(const SampledSpectrum &L, const SampledWavelengths &lambda) const {
        RGB cameraRGB = sensor->ToCameraRGB(L * sensor->ImagingRatio(), lambda);
        return Mul<RGB>(outputRGBFromCameraRGB, cameraRGB);
    }

    void WriteImage(ImageMetadata metadata, Float splatScale = 1);
    Image GetImage(ImageMetadata *metadata, Float splatScale = 1);

    std::string ToString() const;

  private:
    // RGBFilm::Pixel Definition
    struct Pixel {
        Pixel() = default;
        double rgbSum[3] = {0., 0., 0.};
        double weightSum = 0.;
        AtomicDouble splatRGB[3];
        VarianceEstimator<Float> varianceEstimator;
    };

    // RGBFilm Private Members
    Array2D<Pixel> pixels;
    Float scale;
    const RGBColorSpace *colorSpace;
    Float maxComponentValue;
    bool writeFP16;
    Float filterIntegral;
    SquareMatrix<3> outputRGBFromCameraRGB;
};

// GBufferFilm Definition
class GBufferFilm : public FilmBase {
  public:
    // GBufferFilm Public Methods
    GBufferFilm(const PixelSensor *sensor, const Point2i &resolution,
                const Bounds2i &pixelBounds, FilterHandle filter, Float diagonal,
                const std::string &filename, Float scale, const RGBColorSpace *colorSpace,
                Float maxComponentValue = Infinity, bool writeFP16 = true,
                Allocator alloc = {});

    static GBufferFilm *Create(const ParameterDictionary &parameters, Float exposureTime,
                               FilterHandle filter, const RGBColorSpace *colorSpace,
                               const FileLoc *loc, Allocator alloc);

    PBRT_CPU_GPU
    SampledWavelengths SampleWavelengths(Float u) const;

    PBRT_CPU_GPU
    void AddSample(const Point2i &pFilm, SampledSpectrum L,
                   const SampledWavelengths &lambda, const VisibleSurface *visibleSurface,
                   Float weight);

    PBRT_CPU_GPU
    void AddSplat(const Point2f &p, SampledSpectrum v, const SampledWavelengths &lambda);

    PBRT_CPU_GPU
    RGB ToOutputRGB(const SampledSpectrum &L, const SampledWavelengths &lambda) const {
        RGB cameraRGB = sensor->ToCameraRGB(L * sensor->ImagingRatio(), lambda);
        return Mul<RGB>(outputRGBFromCameraRGB, cameraRGB);
    }

    PBRT_CPU_GPU
    bool UsesVisibleSurface() const { return true; }

    PBRT_CPU_GPU
    RGB GetPixelRGB(const Point2i &p, Float splatScale = 1) const {
        const Pixel &pixel = pixels[p];
        RGB rgb(pixel.rgbSum[0], pixel.rgbSum[1], pixel.rgbSum[2]);

        // Normalize pixel with weight sum
        Float weightSum = pixel.weightSum;
        if (weightSum != 0)
            rgb /= weightSum;

        // Add splat value at pixel
        for (int c = 0; c < 3; ++c)
            rgb[c] += splatScale * pixel.splatRGB[c] / filterIntegral;

        // Scale pixel value by _scale_
        rgb *= scale;

        rgb = Mul<RGB>(outputRGBFromCameraRGB, rgb);

        return rgb;
    }

    void WriteImage(ImageMetadata metadata, Float splatScale = 1);
    Image GetImage(ImageMetadata *metadata, Float splatScale = 1);

    std::string ToString() const;

  private:
    // GBufferFilm::Pixel Definition
    struct Pixel {
        Pixel() = default;
        double rgbSum[3] = {0., 0., 0.};
        double weightSum = 0.;
        AtomicDouble splatRGB[3];
        Point3f pSum;
        Float dzdxSum = 0, dzdySum = 0;
        Normal3f nSum, nsSum;
        double albedoSum[3] = {0., 0., 0.};
        VarianceEstimator<Float> rgbVarianceEstimator;
    };

    // GBufferFilm Private Members
    Array2D<Pixel> pixels;
    Float scale;
    const RGBColorSpace *colorSpace;
    Float maxComponentValue;
    bool writeFP16;
    Float filterIntegral;
    SquareMatrix<3> outputRGBFromCameraRGB;
};

PBRT_CPU_GPU
inline SampledWavelengths FilmHandle::SampleWavelengths(Float u) const {
    auto sample = [&](auto ptr) { return ptr->SampleWavelengths(u); };
    return Dispatch(sample);
}

PBRT_CPU_GPU
inline Bounds2f FilmHandle::SampleBounds() const {
    auto sb = [&](auto ptr) { return ptr->SampleBounds(); };
    return Dispatch(sb);
}

PBRT_CPU_GPU
inline Bounds2i FilmHandle::PixelBounds() const {
    auto pb = [&](auto ptr) { return ptr->PixelBounds(); };
    return Dispatch(pb);
}

PBRT_CPU_GPU
inline Point2i FilmHandle::FullResolution() const {
    auto fr = [&](auto ptr) { return ptr->FullResolution(); };
    return Dispatch(fr);
}

PBRT_CPU_GPU
inline Float FilmHandle::Diagonal() const {
    auto diag = [&](auto ptr) { return ptr->Diagonal(); };
    return Dispatch(diag);
}

PBRT_CPU_GPU
inline FilterHandle FilmHandle::GetFilter() const {
    auto filter = [&](auto ptr) { return ptr->GetFilter(); };
    return Dispatch(filter);
}

PBRT_CPU_GPU
inline bool FilmHandle::UsesVisibleSurface() const {
    auto uses = [&](auto ptr) { return ptr->UsesVisibleSurface(); };
    return Dispatch(uses);
}

PBRT_CPU_GPU
inline RGB FilmHandle::GetPixelRGB(const Point2i &p, Float splatScale) const {
    auto get = [&](auto ptr) { return ptr->GetPixelRGB(p, splatScale); };
    return Dispatch(get);
}

PBRT_CPU_GPU
inline RGB FilmHandle::ToOutputRGB(const SampledSpectrum &L,
                                   const SampledWavelengths &lambda) const {
    auto get = [&](auto ptr) { return ptr->ToOutputRGB(L, lambda); };
    return Dispatch(get);
}

PBRT_CPU_GPU
inline void FilmHandle::AddSample(const Point2i &pFilm, SampledSpectrum L,
                                  const SampledWavelengths &lambda,
                                  const VisibleSurface *visibleSurface, Float weight) {
    auto add = [&](auto ptr) {
        return ptr->AddSample(pFilm, L, lambda, visibleSurface, weight);
    };
    return Dispatch(add);
}

PBRT_CPU_GPU
inline const PixelSensor *FilmHandle::GetPixelSensor() const {
    auto filter = [&](auto ptr) { return ptr->GetPixelSensor(); };
    return Dispatch(filter);
}

}  // namespace pbrt

#endif  // PBRT_FILM_H
