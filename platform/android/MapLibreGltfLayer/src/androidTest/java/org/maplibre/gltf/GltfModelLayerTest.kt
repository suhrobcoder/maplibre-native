package org.maplibre.gltf

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GltfModelLayerTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun descriptorPlacementsSupportSharedInstancesAndUpdates() {
        val layer = GltfModelLayer("descriptor-test")
        val source = GltfModelDescriptor(
            id = "source",
            name = "Source",
            file = "Box.glb",
            latitude = 41.55,
            longitude = 60.63,
            bearing = Math.PI / 2.0,
            scale = 2.0,
            offsetEast = 3.0,
            offsetNorth = -4.0,
            offsetUp = 5.0,
            modelUrl = "https://example.com/Box.glb",
        )
        val instance = source.copy(id = "instance", latitude = 41.56)

        try {
            assertFalse(layer.darkModeLightingEnabled)
            layer.darkModeLightingEnabled = true
            assertTrue(layer.darkModeLightingEnabled)
            layer.darkModeLightingEnabled = false

            layer.addModel(source, GltfModel.fromAsset(context, "Box.glb"))
            layer.addInstance(instance, sourceModelId = source.id)

            assertEquals(setOf("source", "instance"), layer.modelIds)
            assertTrue(layer.updateModel(instance))
            assertTrue(layer.removeModel(instance.id))
            assertEquals(setOf("source"), layer.modelIds)
        } finally {
            layer.close()
        }
    }
}
