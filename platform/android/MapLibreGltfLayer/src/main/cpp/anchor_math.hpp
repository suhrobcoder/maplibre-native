// Shared anchor/projection math for the glTF layer hosts. Header-only, no
// mbgl symbols: the projection convention is documented in gltf_layer_host.cpp
// (pixel space = web-mercator[0,1] * worldSize, Z in meters).
#pragma once

#include <array>
#include <cmath>

namespace maplibre_gltf {

constexpr double kTileSize = 512.0;
constexpr double kEarthRadiusM = 6378137.0;
constexpr double kPi = 3.14159265358979323846;

// Web-mercator projection of a lat/lng (degrees) to normalized [0,1] space.
inline void mercator01(double lat, double lng, double& outX, double& outY) {
    outX = (lng + 180.0) / 360.0;
    const double latRad = lat * kPi / 180.0;
    outY = 0.5 - std::log(std::tan(kPi / 4.0 + latRad / 2.0)) / (2.0 * kPi);
}

// Column-major 4x4 multiply: out = a * b. out must not alias a or b.
inline void mat4Multiply(std::array<double, 16>& out,
                         const std::array<double, 16>& a,
                         const std::array<double, 16>& b) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

// projection * translate(anchor px) * scale(ppm, ppm, 1): maps meter offsets
// from the anchor (Z up, meters) to clip space. Composed in double precision
// to avoid float32 jitter at high zoom.
inline std::array<double, 16> anchorMatrix(const std::array<double, 16>& projectionMatrix,
                                           double anchorLat,
                                           double anchorLng,
                                           double zoom) {
    double mx, my;
    mercator01(anchorLat, anchorLng, mx, my);
    const double worldSize = kTileSize * std::pow(2.0, zoom);
    const double latRad = anchorLat * kPi / 180.0;
    const double pixelsPerMeter = worldSize / (std::cos(latRad) * 2.0 * kPi * kEarthRadiusM);

    const std::array<double, 16> localToWorld = {
        pixelsPerMeter, 0.0,            0.0, 0.0,
        0.0,            pixelsPerMeter, 0.0, 0.0,
        0.0,            0.0,            1.0, 0.0,
        mx * worldSize, my * worldSize, 0.0, 1.0,
    };
    std::array<double, 16> out;
    mat4Multiply(out, projectionMatrix, localToWorld);
    return out;
}

} // namespace maplibre_gltf
