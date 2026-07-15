package org.maplibre.gltf.test

import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import okhttp3.OkHttpClient
import okhttp3.Request
import kotlin.math.max
import kotlin.math.min
import org.maplibre.android.MapLibre
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.geometry.LatLngBounds
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.style.layers.FillExtrusionLayer
import org.maplibre.android.style.layers.PropertyFactory
import org.maplibre.gltf.GltfModelDescriptor
import org.maplibre.gltf.GltfModelLayer
import org.maplibre.gltf.GltfModelRepository

private const val MODELS_URL = "https://maps.megago.uz/poi-api/models-3d"
private const val VIEWPORT_PADDING = 0.25

@Serializable
private data class ModelsResponse(val data: List<ModelRecord> = emptyList())

@Serializable
private data class ModelRecord(
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
) {
    fun toDescriptorOrNull(): GltfModelDescriptor? {
        val values = listOf(
            latitude,
            longitude,
            altitude,
            bearing,
            scale,
            offsetEast,
            offsetNorth,
            offsetUp,
        )
        if (id.isBlank() || modelUrl.isBlank() || !modelUrl.startsWith("https://") ||
            !values.all(Double::isFinite) || scale <= 0.0 || latitude !in -90.0..90.0 ||
            longitude !in -180.0..180.0
        ) {
            return null
        }
        return GltfModelDescriptor(
            id,
            name,
            file,
            latitude,
            longitude,
            altitude,
            bearing,
            scale,
            offsetEast,
            offsetNorth,
            offsetUp,
            modelUrl,
        )
    }
}

class GltfSpikeActivity : AppCompatActivity() {
    private lateinit var maplibreMap: MapLibreMap
    private lateinit var mapView: MapView
    private var gltfLayer: GltfModelLayer? = null
    private var repository: GltfModelRepository? = null
    private var cameraIdleListener: MapLibreMap.OnCameraIdleListener? = null
    private var catalog: Map<String, GltfModelDescriptor> = emptyMap()
    private val desiredIds = mutableSetOf<String>()
    private val sourceByUrl = mutableMapOf<String, String>()
    private val metadataClient = OkHttpClient()
    private val json = Json { ignoreUnknownKeys = true }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MapLibre.getInstance(this)
        if (!isDebugBuild()) {
            Toast.makeText(this, "Test app is debug-only", Toast.LENGTH_LONG).show()
            finish()
            return
        }
        setContentView(R.layout.activity_gltf_spike)
        mapView = findViewById(R.id.mapView)
        mapView.onCreate(savedInstanceState)
        mapView.getMapAsync { map ->
            maplibreMap = map
            map.setStyle("https://tiles.openfreemap.org/styles/liberty") {
                map.cameraPosition = CameraPosition.Builder()
                    .target(LatLng(41.55379195503115, 60.62949260146542))
                    .zoom(17.0)
                    .tilt(60.0)
                    .build()
                setupRemoteLayer()
            }
        }
    }

    private fun setupRemoteLayer() {
        val style = maplibreMap.style ?: return
        val layer = GltfModelLayer("gltf-remote-models")
        layer.darkModeLightingEnabled = false
        addLayerBelowBuildings(style, layer)
        gltfLayer = layer
        repository = GltfModelRepository(this)
        cameraIdleListener = MapLibreMap.OnCameraIdleListener { refreshVisibleModels() }
        cameraIdleListener?.let(maplibreMap::addOnCameraIdleListener)

        lifecycleScope.launch {
            catalog = fetchCatalog()
                .mapNotNull { it.toDescriptorOrNull() }
                .distinctBy { it.id }
                .associateBy { it.id }
            refreshVisibleModels()
        }
        Toast.makeText(this, "Loading nearby 3D models", Toast.LENGTH_SHORT).show()
    }

    private suspend fun fetchCatalog(): List<ModelRecord> = withContext(Dispatchers.IO) {
        val request = Request.Builder().url(MODELS_URL).build()
        metadataClient.newCall(request).execute().use { response ->
            if (!response.isSuccessful) throw IOException("Models endpoint returned HTTP ${response.code}")
            val body = response.body?.string() ?: throw IOException("Models endpoint returned an empty body")
            json.decodeFromString<ModelsResponse>(body).data
        }
    }

    private fun refreshVisibleModels() {
        val layer = gltfLayer ?: return
        if (catalog.isEmpty()) return

        val bounds = paddedBounds(maplibreMap.projection.visibleRegion.latLngBounds)
        val visible = catalog.values.filter { bounds.contains(LatLng(it.latitude, it.longitude)) }
        desiredIds.clear()
        desiredIds.addAll(visible.map { it.id })

        layer.modelIds.filterNot(desiredIds::contains).forEach { id -> layer.removeModel(id) }
        sourceByUrl.entries.removeIf { (_, sourceId) -> sourceId !in layer.modelIds }

        visible.groupBy { it.modelUrl }.values.forEach { group ->
            lifecycleScope.launch { ensureGroupLoaded(group) }
        }
        maplibreMap.triggerRepaint()
    }

    private suspend fun ensureGroupLoaded(group: List<GltfModelDescriptor>) {
        val layer = gltfLayer ?: return
        val repo = repository ?: return
        val url = group.firstOrNull()?.modelUrl ?: return
        val currentGroup = group.filter { desiredIds.contains(it.id) }
        if (currentGroup.isEmpty()) return

        val existingSource = sourceByUrl[url]?.takeIf { it in layer.modelIds }
        if (existingSource != null) {
            applyGroup(layer, currentGroup, existingSource)
            return
        }

        repo.getModel(url).onSuccess { model ->
            val stillNeeded = currentGroup.filter { desiredIds.contains(it.id) }
            if (stillNeeded.isEmpty()) {
                model.close()
                return@onSuccess
            }
            val sourceId = sourceByUrl[url]?.takeIf { it in layer.modelIds }
            if (sourceId != null) {
                model.close()
                applyGroup(layer, stillNeeded, sourceId)
                return@onSuccess
            }
            val source = stillNeeded.first()
            layer.addModel(source, model)
            sourceByUrl[url] = source.id
            applyGroup(layer, stillNeeded, source.id)
            maplibreMap.triggerRepaint()
        }
    }

    private fun applyGroup(
        layer: GltfModelLayer,
        descriptors: List<GltfModelDescriptor>,
        sourceId: String,
    ) {
        descriptors.forEach { descriptor ->
            when {
                descriptor.id == sourceId -> layer.updateModel(descriptor)
                descriptor.id in layer.modelIds -> layer.updateModel(descriptor)
                else -> layer.addInstance(descriptor, sourceId)
            }
        }
    }

    private fun paddedBounds(bounds: LatLngBounds): LatLngBounds {
        val latitudePadding = (bounds.latitudeNorth - bounds.latitudeSouth) * VIEWPORT_PADDING
        val longitudePadding = (bounds.longitudeEast - bounds.longitudeWest) * VIEWPORT_PADDING
        return LatLngBounds.Builder()
            .include(
                LatLng(
                    min(90.0, bounds.latitudeNorth + latitudePadding),
                    min(180.0, bounds.longitudeEast + longitudePadding),
                ),
            )
            .include(
                LatLng(
                    max(-90.0, bounds.latitudeSouth - latitudePadding),
                    max(-180.0, bounds.longitudeWest - longitudePadding),
                ),
            )
            .build()
    }

    private fun addLayerBelowBuildings(style: Style, layer: GltfModelLayer) {
        val topExtrusion = style.layers.lastOrNull { it is FillExtrusionLayer }
        if (topExtrusion != null) {
            (topExtrusion as FillExtrusionLayer).setProperties(
                PropertyFactory.fillExtrusionOpacity(1.0f)
            )
            style.addLayerBelow(layer.layer, topExtrusion.id)
        } else {
            style.addLayer(layer.layer)
        }
    }

    private fun isDebugBuild(): Boolean =
        applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0

    override fun onStart() { super.onStart(); mapView.onStart() }
    override fun onResume() { super.onResume(); mapView.onResume() }
    override fun onPause() { super.onPause(); mapView.onPause() }
    override fun onStop() { super.onStop(); mapView.onStop() }
    override fun onLowMemory() { super.onLowMemory(); mapView.onLowMemory() }

    override fun onDestroy() {
        if (::maplibreMap.isInitialized) {
            cameraIdleListener?.let(maplibreMap::removeOnCameraIdleListener)
            maplibreMap.style?.let { style -> gltfLayer?.let { style.removeLayer(it.layer) } }
        }
        gltfLayer?.close()
        repository?.close()
        metadataClient.dispatcher.cancelAll()
        metadataClient.dispatcher.executorService.shutdown()
        metadataClient.connectionPool.evictAll()
        super.onDestroy()
        mapView.onDestroy()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }
}
