package org.maplibre.gltf.test

import android.animation.ValueAnimator
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.view.animation.DecelerateInterpolator
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.floatingactionbutton.FloatingActionButton
import org.maplibre.android.MapLibre
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.style.layers.FillExtrusionLayer
import org.maplibre.android.style.layers.PropertyFactory
import org.maplibre.gltf.GltfModel
import org.maplibre.gltf.GltfModelLayer
import org.maplibre.gltf.ModelOptions

private const val ZOOM_THRESHOLD = 14.0
private const val FULL_SCALE = 24.0
private const val FERRIS_SCALE = 1.5
private const val REVEAL_DURATION_MS = 600L

class GltfSpikeActivity : AppCompatActivity() {
    private lateinit var maplibreMap: MapLibreMap
    private lateinit var mapView: MapView
    private lateinit var fab: FloatingActionButton
    private var gltfLayer: GltfModelLayer? = null
    private var revealAnimator: ValueAnimator? = null
    private var cameraListener: MapLibreMap.OnCameraMoveListener? = null
    private var wasVisible = false

    private val nestAnchor = LatLng(41.55379195503115, 60.62949260146542)
    private val ferrisAnchor = LatLng(41.551861350301806, 60.61369321962073)

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
            map.setStyle(
                "https://tiles.openfreemap.org/styles/liberty"
            ) {
                map.cameraPosition = CameraPosition.Builder()
                    .target(nestAnchor)
                    .zoom(17.0)
                    .tilt(60.0)
                    .build()
                initFab()
            }
        }
    }

    private fun initFab() {
        fab = findViewById(R.id.fab)
        fab.setOnClickListener { toggleLayer() }
    }

    private fun isDebugBuild(): Boolean {
        return (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
    }

    private fun toggleLayer() {
        val style = maplibreMap.style ?: return
        if (gltfLayer != null) {
            removeLayer()
            return
        }
        val layer = GltfModelLayer("gltf-nest")
        addLayerBelowBuildings(style, layer)
        val nestModel = GltfModel.fromAsset(this, "nest_one.glb")
        layer.addModel("nest", nestModel, nestAnchor, ModelOptions(scale = FULL_SCALE, heightScale = 0.0))
        val ferrisModel = GltfModel.fromAsset(this, "ferris_wheel.glb")
        layer.addModel("ferris", ferrisModel, ferrisAnchor, ModelOptions(scale = FERRIS_SCALE))
        gltfLayer = layer
        wasVisible = false

        cameraListener = MapLibreMap.OnCameraMoveListener {
            checkZoom(maplibreMap.cameraPosition.zoom)
        }
        maplibreMap.addOnCameraMoveListener(cameraListener!!)
        checkZoom(maplibreMap.cameraPosition.zoom)

        Toast.makeText(this, "Nest One — zoom past 14 to reveal", Toast.LENGTH_LONG).show()
    }

    private fun removeLayer() {
        revealAnimator?.cancel()
        revealAnimator = null
        cameraListener?.let { maplibreMap.removeOnCameraMoveListener(it) }
        cameraListener = null
        val style = maplibreMap.style ?: return
        val existing = gltfLayer ?: return
        style.removeLayer(existing.layer)
        existing.close()
        gltfLayer = null
    }

    private fun checkZoom(zoom: Double) {
        val visible = zoom >= ZOOM_THRESHOLD
        if (visible == wasVisible) return
        wasVisible = visible
        animateReveal(visible)
    }

    private fun animateReveal(show: Boolean) {
        revealAnimator?.cancel()
        val layer = gltfLayer ?: return
        val from = if (show) 0f else 1f
        val to = if (show) 1f else 0f
        revealAnimator = ValueAnimator.ofFloat(from, to).apply {
            duration = REVEAL_DURATION_MS
            interpolator = DecelerateInterpolator()
            addUpdateListener { anim ->
                val t = anim.animatedValue as Float
                layer.updateModel("nest", options = ModelOptions(scale = FULL_SCALE, heightScale = t.toDouble()))
                maplibreMap.triggerRepaint()
            }
            start()
        }
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

    override fun onStart() { super.onStart(); mapView.onStart() }
    override fun onResume() { super.onResume(); mapView.onResume() }
    override fun onPause() { super.onPause(); mapView.onPause() }
    override fun onStop() { super.onStop(); mapView.onStop() }
    override fun onLowMemory() { super.onLowMemory(); mapView.onLowMemory() }
    override fun onDestroy() {
        revealAnimator?.cancel()
        cameraListener?.let { maplibreMap.removeOnCameraMoveListener(it) }
        gltfLayer?.close()
        super.onDestroy()
        mapView.onDestroy()
    }
    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }
}
