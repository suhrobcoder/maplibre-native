package org.maplibre.gltf

import android.content.Context
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.net.URL

/**
 * A glTF/GLB model parsed into GPU-ready buffers (interleaved vertices,
 * indices, decoded textures). Loading is pure CPU work — call the factory
 * functions off the main thread for anything non-trivial.
 *
 * Supports GLB and `.gltf` (external buffers resolved relative to the file
 * for [fromFile]). Release native memory with [close]; rendering (Phase 2)
 * uploads the buffers to the GPU and the model can be closed afterwards.
 */
class GltfModel private constructor(internal var nativePtr: Long) : Closeable {

    val drawableCount: Int get() = nativeDrawableCount(checkPtr())
    val materialCount: Int get() = nativeMaterialCount(checkPtr())
    val textureCount: Int get() = nativeTextureCount(checkPtr())
    val vertexCount: Long get() = nativeVertexCount(checkPtr())
    val indexCount: Long get() = nativeIndexCount(checkPtr())

    override fun close() {
        if (nativePtr != 0L) {
            nativeDestroy(nativePtr)
            nativePtr = 0L
        }
    }

    private fun checkPtr(): Long {
        check(nativePtr != 0L) { "GltfModel is closed" }
        return nativePtr
    }

    companion object {
        init {
            System.loadLibrary("maplibre-gltf-layer")
        }

        /** Load from a filesystem path (.glb, or .gltf with external buffers). */
        @Throws(IOException::class)
        fun fromFile(path: String): GltfModel = GltfModel(nativeLoadFromFile(path))

        /** Load from an in-memory blob (GLB, or fully embedded .gltf). */
        @Throws(IOException::class)
        fun fromBytes(bytes: ByteArray): GltfModel = GltfModel(nativeLoadFromBytes(bytes))

        /**
         * Load from an APK asset. The asset is a single self-contained file
         * (GLB recommended); .gltf with external sibling files should be
         * extracted to storage and loaded via [fromFile].
         */
        @Throws(IOException::class)
        fun fromAsset(context: Context, assetPath: String): GltfModel =
            context.assets.open(assetPath).use { fromBytes(it.readBytes()) }

        /**
         * Download and load from a URL. Blocking — never call on the main
         * thread. GLB recommended (self-contained).
         */
        @Throws(IOException::class)
        fun fromUrl(url: String): GltfModel =
            URL(url).openStream().use { fromBytes(it.readBytes()) }

        @JvmStatic private external fun nativeLoadFromFile(path: String): Long
        @JvmStatic private external fun nativeLoadFromBytes(bytes: ByteArray): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
        @JvmStatic private external fun nativeDrawableCount(handle: Long): Int
        @JvmStatic private external fun nativeMaterialCount(handle: Long): Int
        @JvmStatic private external fun nativeTextureCount(handle: Long): Int
        @JvmStatic private external fun nativeVertexCount(handle: Long): Long
        @JvmStatic private external fun nativeIndexCount(handle: Long): Long
    }
}
