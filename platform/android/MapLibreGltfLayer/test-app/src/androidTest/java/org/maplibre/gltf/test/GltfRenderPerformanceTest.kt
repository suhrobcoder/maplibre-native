package org.maplibre.gltf.test

import android.app.Activity
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ActivityScenario
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4ClassRunner::class)
class GltfRenderPerformanceTest {

    @Test
    fun rendersMultiInstanceScene() {
        val scenario = ActivityScenario.launchActivityForResult(GltfBenchmarkActivity::class.java)
        try {
            while (scenario.state !== Lifecycle.State.DESTROYED) {
                Thread.sleep(500)
            }

            assertEquals(Activity.RESULT_OK, scenario.result.resultCode)
            val data = scenario.result.resultData
            assertTrue("benchmark produced no frames", data?.getIntExtra("frame_count", 0) ?: 0 > 0)
            assertTrue("benchmark produced no FPS", data?.getDoubleExtra("fps", 0.0) ?: 0.0 > 0.0)
            assertTrue(
                "benchmark produced no draw calls",
                data?.getDoubleExtra("avg_draw_calls", 0.0) ?: 0.0 > 0.0,
            )
        } finally {
            scenario.close()
        }
    }
}
