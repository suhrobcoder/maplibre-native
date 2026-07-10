package org.maplibre.gltf.test

import android.app.Activity
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.view.WindowManager
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import org.maplibre.android.MapLibre
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.log.Logger
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.style.layers.FillExtrusionLayer
import org.maplibre.android.style.layers.PropertyFactory
import org.maplibre.gltf.GltfModel
import org.maplibre.gltf.GltfModelLayer
import org.maplibre.gltf.ModelOptions
import kotlin.coroutines.resume

class GltfBenchmarkActivity : AppCompatActivity() {
    private val TAG = "GltfBenchmark"
    private val anchor = LatLng(52.51870, 13.40600)
    private val benchmarkDurationMs = 20000

    private lateinit var mapView: MapView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MapLibre.getInstance(this)
        if (!isDebugBuild()) {
            Toast.makeText(this, "Test app is debug-only", Toast.LENGTH_LONG).show()
            finish()
            return
        }
        setContentView(R.layout.activity_gltf_benchmark)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        mapView = findViewById(R.id.mapView)
        mapView.onCreate(savedInstanceState)
        mapView.getMapAsync { map ->
            map.setStyle(
                "https://tiles.openfreemap.org/styles/liberty"
            ) {
                map.cameraPosition = org.maplibre.android.camera.CameraPosition.Builder()
                    .target(anchor)
                    .zoom(16.0)
                    .tilt(70.0)
                    .build()
                lifecycleScope.launch { runBenchmark(map) }
            }
        }
    }

    private fun isDebugBuild(): Boolean {
        return (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
    }

    private suspend fun runBenchmark(map: MapLibreMap) {
        val style = map.style ?: return
        Logger.i(TAG, "=== glTF Benchmark starting ===")

        val layer = GltfModelLayer("gltf-benchmark")
        val topExtrusion = style.layers.lastOrNull { it is FillExtrusionLayer }
        if (topExtrusion != null) {
            (topExtrusion as FillExtrusionLayer).setProperties(
                PropertyFactory.fillExtrusionOpacity(1.0f)
            )
            style.addLayerBelow(layer.layer, topExtrusion.id)
        } else {
            style.addLayer(layer.layer)
        }

        val model = GltfModel.fromAsset(this, "Duck.glb")
        layer.addModel("duck", model, anchor, ModelOptions(scale = 100.0))
        layer.addInstance(
            "duck-east", "duck",
            LatLng(anchor.latitude, anchor.longitude + 0.0025),
            ModelOptions(scale = 60.0, rotationDegrees = 90.0),
        )
        layer.addInstance(
            "duck-spinner", "duck",
            LatLng(anchor.latitude + 0.0012, anchor.longitude - 0.0015),
            ModelOptions(scale = 60.0),
        )

        var total = 3
        for (row in 0 until 7) {
            for (col in 0 until 7) {
                layer.addInstance(
                    "grid-$row-$col", "duck",
                    LatLng(anchor.latitude - 0.0010 - row * 0.0007, anchor.longitude - 0.0021 + col * 0.0007),
                    ModelOptions(scale = 20.0, rotationDegrees = (row * 7 + col) * 15.0),
                )
                total++
            }
        }
        map.triggerRepaint()
        Logger.i(TAG, "Added $total duck instances")

        mapView.post { Thread.yield() }

        var frameCount = 0
        var totalEncodingSec = 0.0
        var totalRenderingSec = 0.0
        var totalDrawCalls = 0L
        var minFps = Double.MAX_VALUE

        val statListener = object : MapView.OnDidFinishRenderingFrameWithStatsListener {
            override fun onDidFinishRenderingFrame(dropped: Boolean, stats: org.maplibre.android.maps.RenderingStats) {
                frameCount++
                totalEncodingSec += stats.encodingTime
                totalRenderingSec += stats.renderingTime
                totalDrawCalls += stats.totalDrawCalls
                if (frameCount > 5) {
                    val currentFps = 1.0 / (stats.encodingTime + stats.renderingTime + 1e-9)
                    if (currentFps < minFps) minFps = currentFps
                }
            }
        }
        mapView.addOnDidFinishRenderingFrameListener(statListener)

        val places = listOf(
            anchor,
            LatLng(anchor.latitude, anchor.longitude + 0.005),
            LatLng(anchor.latitude + 0.003, anchor.longitude + 0.003),
            LatLng(anchor.latitude + 0.003, anchor.longitude - 0.003),
            LatLng(anchor.latitude - 0.002, anchor.longitude - 0.002),
        )

        val perLegMs = benchmarkDurationMs / places.size
        val startTime = System.nanoTime()
        for (place in places) {
            suspendCancellableCoroutine<Unit> { cont ->
                map.animateCamera(
                    CameraUpdateFactory.newLatLngZoom(place, 16.0),
                    perLegMs,
                    object : MapLibreMap.CancelableCallback {
                        override fun onCancel() { cont.resume(Unit) }
                        override fun onFinish() { cont.resume(Unit) }
                    }
                )
            }
        }
        val elapsedNs = System.nanoTime() - startTime

        mapView.removeOnDidFinishRenderingFrameListener(statListener)

        val fps = if (elapsedNs > 0) (frameCount * 1E9) / elapsedNs else 0.0
        val avgEncodingMs = if (frameCount > 0) totalEncodingSec * 1000.0 / frameCount else 0.0
        val avgRenderingMs = if (frameCount > 0) totalRenderingSec * 1000.0 / frameCount else 0.0

        Logger.i(TAG, "=== glTF Benchmark Results ===")
        Logger.i(TAG, "Total frames: $frameCount")
        Logger.i(TAG, "Average FPS: %.1f".format(fps))
        Logger.i(TAG, "Min FPS: %.1f".format(if (minFps == Double.MAX_VALUE) 0.0 else minFps))
        Logger.i(TAG, "Avg encoding time: %.2f ms".format(avgEncodingMs))
        Logger.i(TAG, "Avg rendering time: %.2f ms".format(avgRenderingMs))
        Logger.i(TAG, "Total draw calls: $totalDrawCalls")
        Logger.i(TAG, "Avg draw calls/frame: %.1f".format(if (frameCount > 0) totalDrawCalls.toDouble() / frameCount else 0.0))
        Logger.i(TAG, "=== glTF Benchmark complete ===")

        layer.close()
        setResult(Activity.RESULT_OK)
        finish()
    }

    override fun onStart() { super.onStart(); mapView.onStart() }
    override fun onResume() { super.onResume(); mapView.onResume() }
    override fun onPause() { super.onPause(); mapView.onPause() }
    override fun onStop() { super.onStop(); mapView.onStop() }
    override fun onDestroy() { super.onDestroy(); mapView.onDestroy() }
    override fun onLowMemory() { super.onLowMemory(); mapView.onLowMemory() }
    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }
}
