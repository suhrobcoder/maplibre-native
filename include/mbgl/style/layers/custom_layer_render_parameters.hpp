#pragma once

#include <memory>
#include <array>

namespace mbgl {

class PaintParameters;

namespace style {

/**
 * Parameters that define the current camera position for a
 * `CustomLayerHost::render()` function.
 */
struct CustomLayerRenderParameters {
    double width;
    double height;
    double latitude;
    double longitude;
    double zoom;
    double bearing;
    double pitch;
    double fieldOfView;

    /// Standard projection matrix (nearZ = 1 tile unit).
    /// Use this for 2D/flat custom geometry.
    std::array<double, 16> projectionMatrix;

    /// A 4×4 matrix representing the map view’s current near clip projection state.
    std::array<double, 16> nearClippedProjectionMatrix;

    /// Upper bound of the depth range used by the map's 3D content (e.g.
    /// fill-extrusion). A custom layer using OpenGL must render with
    /// `glDepthRangef(0, depthRangeSize)` to depth-test against that content.
    double depthRangeSize;

    CustomLayerRenderParameters(const PaintParameters&);
};

} // namespace style
} // namespace mbgl
