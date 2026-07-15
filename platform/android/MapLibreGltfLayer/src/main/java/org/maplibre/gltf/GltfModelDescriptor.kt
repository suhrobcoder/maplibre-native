package org.maplibre.gltf

/**
 * Metadata and placement information for one remotely hosted glTF model.
 *
 * [bearing] is a compass bearing in radians: zero points north and positive
 * values rotate clockwise. Offsets are local east/north/up translations in
 * meters and are not affected by [scale].
 */
data class GltfModelDescriptor(
    val id: String,
    val name: String,
    val file: String,
    val latitude: Double,
    val longitude: Double,
    val altitude: Double = 0.0,
    val bearing: Double = 0.0,
    val scale: Double = 1.0,
    val offsetEast: Double = 0.0,
    val offsetNorth: Double = 0.0,
    val offsetUp: Double = 0.0,
    val modelUrl: String,
)
