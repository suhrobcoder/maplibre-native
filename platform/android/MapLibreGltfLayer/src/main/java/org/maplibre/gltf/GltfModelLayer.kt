package org.maplibre.gltf

import java.io.Closeable
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.style.layers.CustomLayer

/**
 * A style layer rendering any number of glTF model instances anchored to map
 * coordinates.
 *
 * Usage:
 * ```
 * val gltfLayer = GltfModelLayer("gltf-layer")
 * style.addLayerBelow(gltfLayer.layer, topFillExtrusionLayerId)
 * gltfLayer.addModel("duck", GltfModel.fromAsset(ctx, "Duck.glb"), latLng, ModelOptions(scale = 100.0))
 * gltfLayer.addInstance("duck-2", sourceModelId = "duck", position = otherLatLng) // shares GPU copy
 * gltfLayer.updateModel("duck", position = newLatLng) // then map.triggerRepaint()
 * ```
 *
 * Placement rules: add [layer] to the style BELOW the topmost fill-extrusion
 * layer for correct depth interaction with 3D buildings.
 *
 * Threading contract: all methods may be called from any thread (the main
 * thread is fine) — they only mutate shared state under a native mutex. GPU
 * upload/draw/free happens on the map render thread. Changes take effect on
 * the next rendered frame; when the map is idle (e.g. animating a model),
 * call `MapLibreMap.triggerRepaint()` after updates.
 *
 * [addModel] takes ownership of the passed [GltfModel] — its native memory is
 * consumed and the handle becomes closed. To place the same model several
 * times without re-loading, use [addInstance]: all instances of one model
 * share a single GPU copy.
 */
class GltfModelLayer(id: String) : Closeable {

    private var statePtr: Long = nativeCreateState()

    /** The [CustomLayer] to add to the style. */
    val layer: CustomLayer = CustomLayer(id, nativeCreateHost(statePtr))

    // Kotlin-side mirror of placements so partial updates can fill in the
    // unchanged half. Guarded by synchronized(this).
    private data class Entry(
        var position: LatLng,
        var options: ModelOptions,
        var bearingRadians: Double,
        var offsetEast: Double = 0.0,
        var offsetNorth: Double = 0.0,
        var offsetUp: Double = 0.0,
    )
    private val entries = HashMap<String, Entry>()
    private var darkModeLighting = false

    /** Enables the dim lighting intended for dark-mode map styles. */
    var darkModeLightingEnabled: Boolean
        @Synchronized get() = darkModeLighting
        @Synchronized set(value) {
            checkOpen()
            darkModeLighting = value
            nativeSetDarkModeLighting(statePtr, value)
        }

    /**
     * Adds a model instance. Consumes [model] (its handle becomes closed).
     *
     * @throws IllegalArgumentException if [modelId] is already used
     * @throws IllegalStateException if [model] is closed or the layer is closed
     */
    @Synchronized
    fun addModel(
        modelId: String,
        model: GltfModel,
        position: LatLng,
        options: ModelOptions = ModelOptions(),
    ) {
        checkOpen()
        val modelPtr = model.nativePtr
        check(modelPtr != 0L) { "GltfModel is closed" }
        require(!entries.containsKey(modelId)) { "model id already exists: $modelId" }
        model.nativePtr = 0L // ownership transferred to native
        nativeAddModel(
            statePtr, modelId, modelPtr,
            position.latitude, position.longitude,
            options.scale, Math.toRadians(options.rotationDegrees), options.altitudeMeters,
            0.0, 0.0, 0.0,
            options.opacity, options.heightScale,
        )
        entries[modelId] = Entry(position, options, Math.toRadians(options.rotationDegrees))
    }

    /** Adds a model using the full remote-model descriptor. Consumes [model]. */
    @Synchronized
    fun addModel(descriptor: GltfModelDescriptor, model: GltfModel) {
        checkOpen()
        val modelPtr = model.nativePtr
        check(modelPtr != 0L) { "GltfModel is closed" }
        require(!entries.containsKey(descriptor.id)) { "model id already exists: ${descriptor.id}" }
        model.nativePtr = 0L
        nativeAddModel(
            statePtr, descriptor.id, modelPtr,
            descriptor.latitude, descriptor.longitude,
            descriptor.scale, descriptor.bearing, descriptor.altitude,
            descriptor.offsetEast, descriptor.offsetNorth, descriptor.offsetUp,
            1.0f, 1.0,
        )
        entries[descriptor.id] = descriptor.entry()
    }

    /**
     * Adds another instance of an already-added model. Both instances share
     * one GPU copy of the geometry and textures.
     *
     * @throws IllegalArgumentException if [instanceId] exists or [sourceModelId] doesn't
     */
    @Synchronized
    fun addInstance(
        instanceId: String,
        sourceModelId: String,
        position: LatLng,
        options: ModelOptions = ModelOptions(),
    ) {
        checkOpen()
        require(!entries.containsKey(instanceId)) { "model id already exists: $instanceId" }
        require(entries.containsKey(sourceModelId)) { "no such model: $sourceModelId" }
        nativeAddInstance(
            statePtr, instanceId, sourceModelId,
            position.latitude, position.longitude,
            options.scale, Math.toRadians(options.rotationDegrees), options.altitudeMeters,
            0.0, 0.0, 0.0,
            options.opacity, options.heightScale,
        )
        entries[instanceId] = Entry(position, options, Math.toRadians(options.rotationDegrees))
    }

    /** Adds a descriptor placement that shares the GPU model of [sourceModelId]. */
    @Synchronized
    fun addInstance(descriptor: GltfModelDescriptor, sourceModelId: String) {
        checkOpen()
        require(!entries.containsKey(descriptor.id)) { "model id already exists: ${descriptor.id}" }
        require(entries.containsKey(sourceModelId)) { "no such model: $sourceModelId" }
        nativeAddInstance(
            statePtr, descriptor.id, sourceModelId,
            descriptor.latitude, descriptor.longitude,
            descriptor.scale, descriptor.bearing, descriptor.altitude,
            descriptor.offsetEast, descriptor.offsetNorth, descriptor.offsetUp,
            1.0f, 1.0,
        )
        entries[descriptor.id] = descriptor.entry()
    }

    /**
     * Updates an instance's position and/or options; omitted arguments keep
     * their current value. Takes effect on the next rendered frame — call
     * `MapLibreMap.triggerRepaint()` if the map is idle.
     *
     * @return false if [modelId] is unknown
     */
    @Synchronized
    fun updateModel(
        modelId: String,
        position: LatLng? = null,
        options: ModelOptions? = null,
    ): Boolean {
        checkOpen()
        val entry = entries[modelId] ?: return false
        val newPosition = position ?: entry.position
        val newOptions = options ?: entry.options
        val newBearing = if (options == null) entry.bearingRadians else Math.toRadians(newOptions.rotationDegrees)
        val newOffsetEast = if (options == null) entry.offsetEast else 0.0
        val newOffsetNorth = if (options == null) entry.offsetNorth else 0.0
        val newOffsetUp = if (options == null) entry.offsetUp else 0.0
        nativeUpdateModel(
            statePtr, modelId,
            newPosition.latitude, newPosition.longitude,
            newOptions.scale, newBearing, newOptions.altitudeMeters,
            newOffsetEast, newOffsetNorth, newOffsetUp,
            newOptions.opacity, newOptions.heightScale,
        )
        entry.position = newPosition
        entry.options = newOptions
        entry.bearingRadians = newBearing
        entry.offsetEast = newOffsetEast
        entry.offsetNorth = newOffsetNorth
        entry.offsetUp = newOffsetUp
        return true
    }

    /** Updates an existing descriptor placement without changing its model. */
    @Synchronized
    fun updateModel(descriptor: GltfModelDescriptor): Boolean {
        checkOpen()
        val entry = entries[descriptor.id] ?: return false
        nativeUpdateModel(
            statePtr, descriptor.id,
            descriptor.latitude, descriptor.longitude,
            descriptor.scale, descriptor.bearing, descriptor.altitude,
            descriptor.offsetEast, descriptor.offsetNorth, descriptor.offsetUp,
            1.0f, 1.0,
        )
        entry.position = LatLng(descriptor.latitude, descriptor.longitude)
        entry.options = descriptor.options()
        entry.bearingRadians = descriptor.bearing
        entry.offsetEast = descriptor.offsetEast
        entry.offsetNorth = descriptor.offsetNorth
        entry.offsetUp = descriptor.offsetUp
        return true
    }

    /** Removes an instance. GPU memory is freed once a model's last instance is gone. */
    @Synchronized
    fun removeModel(modelId: String): Boolean {
        checkOpen()
        entries.remove(modelId) ?: return false
        return nativeRemoveModel(statePtr, modelId)
    }

    /** Ids of all current instances. */
    @get:Synchronized
    val modelIds: Set<String>
        get() = entries.keys.toSet()

    /**
     * Releases the Kotlin-side native handle. Safe to call before or after
     * the layer is removed from the style (the render host keeps its own
     * reference to the shared state).
     */
    @Synchronized
    override fun close() {
        if (statePtr != 0L) {
            nativeDestroyState(statePtr)
            statePtr = 0L
            entries.clear()
        }
    }

    private fun checkOpen() = check(statePtr != 0L) { "GltfModelLayer is closed" }

    private fun GltfModelDescriptor.entry() = Entry(
        position = LatLng(latitude, longitude),
        options = options(),
        bearingRadians = bearing,
        offsetEast = offsetEast,
        offsetNorth = offsetNorth,
        offsetUp = offsetUp,
    )

    private fun GltfModelDescriptor.options() = ModelOptions(
        scale = scale,
        rotationDegrees = Math.toDegrees(bearing),
        altitudeMeters = altitude,
    )

    private companion object {
        init {
            System.loadLibrary("maplibre-gltf-layer")
        }

        @JvmStatic private external fun nativeCreateState(): Long
        @JvmStatic private external fun nativeDestroyState(stateHandle: Long)
        @JvmStatic private external fun nativeCreateHost(stateHandle: Long): Long
        @JvmStatic private external fun nativeSetDarkModeLighting(stateHandle: Long, enabled: Boolean)

        @JvmStatic private external fun nativeAddModel(
            stateHandle: Long, id: String, modelHandle: Long,
            lat: Double, lng: Double, scale: Double, bearingRad: Double, altitudeM: Double,
            offsetEastM: Double, offsetNorthM: Double, offsetUpM: Double,
            opacity: Float, heightScale: Double,
        ): Boolean

        @JvmStatic private external fun nativeAddInstance(
            stateHandle: Long, id: String, sourceId: String,
            lat: Double, lng: Double, scale: Double, bearingRad: Double, altitudeM: Double,
            offsetEastM: Double, offsetNorthM: Double, offsetUpM: Double,
            opacity: Float, heightScale: Double,
        ): Boolean

        @JvmStatic private external fun nativeUpdateModel(
            stateHandle: Long, id: String,
            lat: Double, lng: Double, scale: Double, bearingRad: Double, altitudeM: Double,
            offsetEastM: Double, offsetNorthM: Double, offsetUpM: Double,
            opacity: Float, heightScale: Double,
        ): Boolean

        @JvmStatic private external fun nativeRemoveModel(stateHandle: Long, id: String): Boolean
    }
}
