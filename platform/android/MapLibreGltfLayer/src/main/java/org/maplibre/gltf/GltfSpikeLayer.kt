package org.maplibre.gltf

import androidx.annotation.Keep
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.style.layers.CustomLayer

/**
 * Phase 0 spike: a [CustomLayer] that renders a single standing triangle
 * anchored to [anchor], proving the projection pipeline end to end.
 *
 * Add [layer] to a style via `style.addLayer(...)`. The native host is owned
 * by the core [CustomLayer]; a new instance is required after removal.
 */
@Keep
class GltfSpikeLayer(id: String, anchor: LatLng) {

    /** The layer to add to a MapLibre style. */
    val layer: CustomLayer = CustomLayer(id, nativeCreateHost(anchor.latitude, anchor.longitude))

    private external fun nativeCreateHost(lat: Double, lng: Double): Long

    companion object {
        init {
            System.loadLibrary("maplibre-gltf-layer")
        }
    }
}
