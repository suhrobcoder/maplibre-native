package org.maplibre.gltf

import android.content.Context
import android.util.Log
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.net.URI
import java.security.MessageDigest
import java.util.Properties
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request

/** Downloads and caches self-contained glTF/GLB model assets. */
class GltfModelRepository(
    context: Context,
    private val config: Config = Config(),
) : Closeable {

    data class Config(
        val maxCacheBytes: Long = 256L * 1024L * 1024L,
        val cacheTtlMillis: Long = 7L * 24L * 60L * 60L * 1000L,
        val maxConcurrentDownloads: Int = 4,
        val connectTimeoutMillis: Long = 15_000L,
        val readTimeoutMillis: Long = 60_000L,
        val callTimeoutMillis: Long = 60_000L,
        val nowMillis: () -> Long = System::currentTimeMillis,
        internal val requireHttps: Boolean = true,
    ) {
        init {
            require(maxCacheBytes > 0) { "maxCacheBytes must be positive" }
            require(cacheTtlMillis >= 0) { "cacheTtlMillis must not be negative" }
            require(maxConcurrentDownloads > 0) { "maxConcurrentDownloads must be positive" }
        }
    }

    sealed class ModelRepositoryException(message: String, cause: Throwable? = null) : IOException(message, cause) {
        class InvalidUrl(message: String) : ModelRepositoryException(message)

        class HttpStatus(val statusCode: Int, message: String) : ModelRepositoryException(message)

        class Network(message: String, cause: Throwable) : ModelRepositoryException(message, cause)

        class Parse(message: String, cause: Throwable) : ModelRepositoryException(message, cause)

        class Closed : ModelRepositoryException("GltfModelRepository is closed")
    }

    private data class CacheEntry(
        val bytesFile: File,
        val metadataFile: File,
        val fetchedAt: Long,
        val lastAccessAt: Long,
        val etag: String?,
        val lastModified: String?,
    )

    private data class DownloadedBytes(
        val bytes: ByteArray,
        val fetchedAt: Long,
        val etag: String?,
        val lastModified: String?,
    )

    private val cacheDirectory = File(context.applicationContext.cacheDir, "maplibre-gltf-models").apply { mkdirs() }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val semaphore = Semaphore(config.maxConcurrentDownloads)
    private val inFlight = ConcurrentHashMap<String, kotlinx.coroutines.Deferred<ByteArray>>()
    private val client = OkHttpClient.Builder()
        .connectTimeout(config.connectTimeoutMillis, TimeUnit.MILLISECONDS)
        .readTimeout(config.readTimeoutMillis, TimeUnit.MILLISECONDS)
        .callTimeout(config.callTimeoutMillis, TimeUnit.MILLISECONDS)
        .followRedirects(true)
        .build()
    private val lock = Any()
    @Volatile private var closed = false

    /** Returns a freshly parsed model. Ownership is transferred to the caller. */
    suspend fun getModel(modelUrl: String): Result<GltfModel> {
        return try {
            checkOpen()
            val bytes = loadBytes(modelUrl)
            val model = try {
                withContext(Dispatchers.Default) { GltfModel.fromBytes(bytes) }
            } catch (error: IOException) {
                throw ModelRepositoryException.Parse("Failed to parse $modelUrl", error)
            }
            Result.success(model)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: ModelRepositoryException) {
            Result.failure(error)
        } catch (error: IOException) {
            Result.failure(ModelRepositoryException.Network("Failed to load $modelUrl", error))
        } catch (error: Exception) {
            Result.failure(ModelRepositoryException.Parse("Failed to parse $modelUrl", error))
        }
    }

    /** Removes one URL's cached bytes and metadata. */
    fun invalidate(modelUrl: String) {
        val key = cacheKey(modelUrl)
        synchronized(lock) {
            File(cacheDirectory, "$key.bin").delete()
            File(cacheDirectory, "$key.meta").delete()
        }
    }

    /** Removes every cached model asset. */
    fun clearCache() {
        synchronized(lock) {
            cacheDirectory.listFiles()?.forEach { it.delete() }
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        scope.coroutineContext[Job]?.cancel()
        client.dispatcher.cancelAll()
        client.dispatcher.executorService.shutdown()
        client.connectionPool.evictAll()
    }

    private suspend fun loadBytes(modelUrl: String): ByteArray {
        validateUrl(modelUrl)
        val existing = inFlight[modelUrl]
        if (existing != null) return existing.await()

        lateinit var deferred: Deferred<ByteArray>
        deferred = scope.async(start = CoroutineStart.LAZY) {
            try {
                loadBytesOnce(modelUrl)
            } finally {
                inFlight.remove(modelUrl, deferred)
            }
        }
        val selected = inFlight.putIfAbsent(modelUrl, deferred)
        if (selected != null) {
            deferred.cancel()
            return selected.await()
        }
        deferred.start()
        return deferred.await()
    }

    private suspend fun loadBytesOnce(modelUrl: String): ByteArray {
        val cached = readCache(modelUrl)
        if (cached != null && config.nowMillis() - cached.fetchedAt <= config.cacheTtlMillis) {
            touch(cached)
            return cached.bytesFile.readBytes()
        }

        return try {
            val downloaded = download(modelUrl, cached)
            writeCache(modelUrl, downloaded)
            downloaded.bytes
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            if (cached != null && cached.bytesFile.exists()) {
                Log.w(TAG, "Using stale cached model after refresh failure: $modelUrl", error)
                touch(cached)
                cached.bytesFile.readBytes()
            } else {
                throw when (error) {
                    is ModelRepositoryException -> error
                    is IOException -> ModelRepositoryException.Network("Failed to download $modelUrl", error)
                    else -> ModelRepositoryException.Network("Failed to download $modelUrl", error)
                }
            }
        }
    }

    private suspend fun download(modelUrl: String, cached: CacheEntry?): DownloadedBytes = semaphore.withPermit {
        var attempt = 0
        var lastError: Exception? = null
        while (attempt < 3) {
            try {
                return@withPermit withContext(Dispatchers.IO) {
                    val request = Request.Builder()
                        .url(modelUrl)
                        .apply {
                            cached?.etag?.let { header("If-None-Match", it) }
                            cached?.lastModified?.let { header("If-Modified-Since", it) }
                        }
                        .build()
                    client.newCall(request).execute().use { response ->
                        if (response.code == 304 && cached != null) {
                            return@withContext DownloadedBytes(
                                cached.bytesFile.readBytes(),
                                config.nowMillis(),
                                cached.etag,
                                cached.lastModified,
                            )
                        }
                        if (!response.isSuccessful) {
                            val error = ModelRepositoryException.HttpStatus(
                                response.code,
                                "Model request returned HTTP ${response.code} for $modelUrl",
                            )
                            if (response.code !in 500..599) throw error
                            throw error
                        }
                        val body = response.body ?: throw IOException("Empty response body for $modelUrl")
                        DownloadedBytes(
                            body.bytes(),
                            config.nowMillis(),
                            response.header("ETag"),
                            response.header("Last-Modified"),
                        )
                    }
                }
            } catch (error: IOException) {
                lastError = error
                if (error is ModelRepositoryException.HttpStatus && error.statusCode !in 500..599) throw error
                attempt++
                if (attempt < 3) delay(250L * (1L shl (attempt - 1)))
            }
        }
        throw ModelRepositoryException.Network("Failed after retries: $modelUrl", lastError ?: IOException("Unknown error"))
    }

    private fun readCache(modelUrl: String): CacheEntry? {
        val key = cacheKey(modelUrl)
        val bytesFile = File(cacheDirectory, "$key.bin")
        val metadataFile = File(cacheDirectory, "$key.meta")
        if (!bytesFile.isFile || !metadataFile.isFile) return null
        return runCatching {
            val properties = Properties().apply { metadataFile.inputStream().use { load(it) } }
            require(properties.getProperty("url") == modelUrl)
            CacheEntry(
                bytesFile,
                metadataFile,
                properties.getProperty("fetchedAt").toLong(),
                properties.getProperty("lastAccessAt").toLong(),
                properties.getProperty("etag").takeUnless { it.isNullOrEmpty() },
                properties.getProperty("lastModified").takeUnless { it.isNullOrEmpty() },
            )
        }.getOrNull()
    }

    private fun touch(entry: CacheEntry) {
        synchronized(lock) {
            val properties = Properties().apply { entry.metadataFile.inputStream().use { load(it) } }
            properties.setProperty("lastAccessAt", config.nowMillis().toString())
            writePropertiesAtomically(entry.metadataFile, properties)
        }
    }

    private fun writeCache(modelUrl: String, downloaded: DownloadedBytes) {
        synchronized(lock) {
            val key = cacheKey(modelUrl)
            val bytesFile = File(cacheDirectory, "$key.bin")
            val metadataFile = File(cacheDirectory, "$key.meta")
            val tempBytes = File(cacheDirectory, "$key.bin.tmp")
            tempBytes.outputStream().use { it.write(downloaded.bytes) }
            if (!tempBytes.renameTo(bytesFile)) {
                tempBytes.delete()
                throw IOException("Unable to commit model cache entry")
            }
            val properties = Properties().apply {
                setProperty("url", modelUrl)
                setProperty("fetchedAt", downloaded.fetchedAt.toString())
                setProperty("lastAccessAt", config.nowMillis().toString())
                downloaded.etag?.let { setProperty("etag", it) }
                downloaded.lastModified?.let { setProperty("lastModified", it) }
            }
            writePropertiesAtomically(metadataFile, properties)
            evictIfNeeded()
        }
    }

    private fun writePropertiesAtomically(file: File, properties: Properties) {
        val temp = File(file.parentFile, "${file.name}.tmp")
        temp.outputStream().use { properties.store(it, null) }
        if (!temp.renameTo(file)) {
            temp.delete()
            throw IOException("Unable to commit model cache metadata")
        }
    }

    private fun evictIfNeeded() {
        val entries = cacheDirectory.listFiles()
            ?.filter { it.extension == "bin" }
            ?.mapNotNull { bytesFile ->
                readCacheByBytesFile(bytesFile)?.let { bytesFile to it }
            }
            ?.toMutableList()
            ?: return
        var total = entries.sumOf { it.first.length() }
        while (total > config.maxCacheBytes && entries.isNotEmpty()) {
            val oldest = entries.minBy { it.second.lastAccessAt }
            total -= oldest.first.length()
            oldest.first.delete()
            oldest.second.metadataFile.delete()
            entries.remove(oldest)
        }
    }

    private fun readCacheByBytesFile(bytesFile: File): CacheEntry? {
        val metadataFile = File(cacheDirectory, bytesFile.nameWithoutExtension + ".meta")
        if (!metadataFile.isFile) return null
        return runCatching {
            val properties = Properties().apply { metadataFile.inputStream().use { load(it) } }
            CacheEntry(
                bytesFile,
                metadataFile,
                properties.getProperty("fetchedAt").toLong(),
                properties.getProperty("lastAccessAt").toLong(),
                properties.getProperty("etag").takeUnless { it.isNullOrEmpty() },
                properties.getProperty("lastModified").takeUnless { it.isNullOrEmpty() },
            )
        }.getOrNull()
    }

    private fun validateUrl(modelUrl: String) {
        val uri = runCatching { URI(modelUrl) }.getOrNull()
        val scheme = uri?.scheme?.lowercase()
        if (uri?.host.isNullOrBlank() || (config.requireHttps && scheme != "https") ||
            (!config.requireHttps && scheme !in setOf("http", "https"))
        ) {
            throw ModelRepositoryException.InvalidUrl("Only HTTPS model URLs are supported: $modelUrl")
        }
    }

    private fun cacheKey(modelUrl: String): String = MessageDigest.getInstance("SHA-256")
        .digest(modelUrl.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }

    private fun checkOpen() {
        if (closed) throw ModelRepositoryException.Closed()
    }

    private companion object {
        const val TAG = "GltfModelRepository"
    }
}
