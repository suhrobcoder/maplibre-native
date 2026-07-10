package org.maplibre.gltf

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.IOException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GltfModelTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().context

    @Test
    fun loadsBoxGlb() {
        GltfModel.fromAsset(context, "Box.glb").use { model ->
            assertEquals(1, model.drawableCount)
            assertEquals(1, model.materialCount)
            assertEquals(0, model.textureCount) // Box is untextured
            assertEquals(24, model.vertexCount) // 6 faces * 4 vertices
            assertEquals(36, model.indexCount) // 12 triangles
        }
    }

    @Test
    fun loadsDuckGlbWithTexture() {
        GltfModel.fromAsset(context, "Duck.glb").use { model ->
            assertEquals(1, model.drawableCount)
            assertEquals(1, model.materialCount)
            assertEquals(1, model.textureCount)
            assertTrue(model.vertexCount > 1000)
            assertTrue(model.indexCount >= model.vertexCount)
        }
    }

    @Test
    fun loadFromBytesMatchesAssetLoad() {
        val bytes = context.assets.open("Box.glb").use { it.readBytes() }
        GltfModel.fromBytes(bytes).use { model ->
            assertEquals(1, model.drawableCount)
            assertEquals(24, model.vertexCount)
        }
    }

    @Test
    fun malformedDataThrowsIoException() {
        assertThrows(IOException::class.java) {
            GltfModel.fromBytes(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8))
        }
    }

    @Test
    fun truncatedGlbThrowsIoException() {
        val bytes = context.assets.open("Box.glb").use { it.readBytes() }
        assertThrows(IOException::class.java) {
            GltfModel.fromBytes(bytes.copyOf(bytes.size / 2))
        }
    }

    @Test
    fun loadsGltfWithExternalBufferAndTexture() {
        // External resources need real files: copy the .gltf trio out of assets.
        val dir = java.io.File(context.cacheDir, "gltf-test").apply { mkdirs() }
        for (name in listOf("Duck.gltf", "Duck0.bin", "DuckCM.png")) {
            context.assets.open(name).use { input ->
                java.io.File(dir, name).outputStream().use { input.copyTo(it) }
            }
        }
        GltfModel.fromFile(java.io.File(dir, "Duck.gltf").absolutePath).use { model ->
            assertEquals(1, model.drawableCount)
            assertEquals(1, model.textureCount) // DuckCM.png decoded from disk
            assertTrue(model.vertexCount > 1000)
        }
    }

    @Test
    fun closedModelRejectsAccess() {
        val model = GltfModel.fromAsset(context, "Box.glb")
        model.close()
        assertThrows(IllegalStateException::class.java) { model.drawableCount }
    }
}
