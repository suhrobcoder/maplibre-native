package org.maplibre.gltf

/**
 * Per-instance placement options for a model in a [GltfModelLayer].
 *
 * @param scale uniform scale applied to the model (model units → meters)
 * @param heightScale vertical stretch factor (1 = full height, 0 = flat on ground)
 * @param rotationDegrees heading rotation around the up axis
 * @param altitudeMeters lift above the map surface
 * @param opacity alpha multiplier (0=invisible, 1=opaque)
 */
data class ModelOptions(
    val scale: Double = 1.0,
    val heightScale: Double = 1.0,
    val rotationDegrees: Double = 0.0,
    val altitudeMeters: Double = 0.0,
    val opacity: Float = 1.0f,
)
