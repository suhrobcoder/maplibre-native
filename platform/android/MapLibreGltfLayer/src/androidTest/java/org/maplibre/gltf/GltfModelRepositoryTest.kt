package org.maplibre.gltf

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.SocketPolicy
import okio.Buffer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GltfModelRepositoryTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var server: MockWebServer
    private lateinit var repository: GltfModelRepository
    private val modelBytes by lazy { context.assets.open("Box.glb").use { it.readBytes() } }

    @Before
    fun setUp() {
        server = MockWebServer().apply { start() }
        repository = GltfModelRepository(context, GltfModelRepository.Config(requireHttps = false))
        repository.clearCache()
    }

    @After
    fun tearDown() {
        repository.close()
        server.shutdown()
    }

    @Test
    fun downloadsOnceThenUsesDiskCache() = runBlocking {
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        val url = server.url("/box.glb").toString()

        repository.getModel(url).getOrThrow().close()
        repository.getModel(url).getOrThrow().close()

        assertEquals(1, server.requestCount)
    }

    @Test
    fun concurrentRequestsShareOneDownload() = runBlocking {
        server.enqueue(
            MockResponse()
                .setBody(modelBytes.toBuffer())
                .throttleBody(modelBytes.size.toLong(), 100, TimeUnit.MILLISECONDS),
        )
        val url = server.url("/box.glb").toString()

        coroutineScope {
            (0 until 4).map {
                async { repository.getModel(url).getOrThrow() }
            }.awaitAll().forEach { it.close() }
        }

        assertEquals(1, server.requestCount)
    }

    @Test
    fun expiredEntryUsesConditionalRequest() = runBlocking {
        var now = 1_000L
        repository.close()
        repository = GltfModelRepository(
            context,
            GltfModelRepository.Config(nowMillis = { now }, requireHttps = false),
        )
        repository.clearCache()
        server.enqueue(
            MockResponse()
                .setHeader("ETag", "box-v1")
                .setHeader("Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT")
                .setBody(modelBytes.toBuffer()),
        )
        server.enqueue(MockResponse().setResponseCode(304))
        val url = server.url("/box.glb").toString()

        repository.getModel(url).getOrThrow().close()
        now += TimeUnit.DAYS.toMillis(8)
        repository.getModel(url).getOrThrow().close()

        assertEquals(2, server.requestCount)
        server.takeRequest()
        val request = server.takeRequest()
        assertEquals("box-v1", request.getHeader("If-None-Match"))
    }

    @Test
    fun staleEntryIsUsedWhenRefreshFails() = runBlocking {
        var now = 1_000L
        repository.close()
        repository = GltfModelRepository(
            context,
            GltfModelRepository.Config(nowMillis = { now }, requireHttps = false),
        )
        repository.clearCache()
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        server.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START))
        val url = server.url("/box.glb").toString()

        repository.getModel(url).getOrThrow().close()
        now += TimeUnit.DAYS.toMillis(8)
        val result = repository.getModel(url)

        assertTrue(result.isSuccess)
        result.getOrThrow().close()
    }

    @Test
    fun invalidationForcesDownload() = runBlocking {
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        val url = server.url("/box.glb").toString()

        repository.getModel(url).getOrThrow().close()
        repository.invalidate(url)
        repository.getModel(url).getOrThrow().close()

        assertEquals(2, server.requestCount)
    }

    @Test
    fun lruEvictionRemovesLeastRecentlyUsedEntry() = runBlocking {
        var now = 0L
        repository.close()
        repository = GltfModelRepository(
            context,
            GltfModelRepository.Config(
                maxCacheBytes = modelBytes.size.toLong(),
                nowMillis = { now },
                requireHttps = false,
            ),
        )
        repository.clearCache()
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        val firstUrl = server.url("/first.glb").toString()
        val secondUrl = server.url("/second.glb").toString()

        repository.getModel(firstUrl).getOrThrow().close()
        now = 1L
        repository.getModel(secondUrl).getOrThrow().close()
        now = 2L
        repository.getModel(firstUrl).getOrThrow().close()

        assertEquals(3, server.requestCount)
    }

    @Test
    fun oversizedEntryIsReturnedButNotCached() = runBlocking {
        repository.close()
        repository = GltfModelRepository(
            context,
            GltfModelRepository.Config(
                maxCacheBytes = (modelBytes.size - 1).toLong(),
                requireHttps = false,
            ),
        )
        repository.clearCache()
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))
        val url = server.url("/oversized.glb").toString()

        repository.getModel(url).getOrThrow().close()
        repository.getModel(url).getOrThrow().close()

        assertEquals(2, server.requestCount)
    }

    @Test
    fun httpFailureIsTyped() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(404))
        val result = repository.getModel(server.url("/missing.glb").toString())

        assertTrue(result.isFailure)
        assertTrue(result.exceptionOrNull() is GltfModelRepository.ModelRepositoryException.HttpStatus)
    }

    @Test
    fun retriesTransientServerFailure() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(503))
        server.enqueue(MockResponse().setBody(modelBytes.toBuffer()))

        val result = repository.getModel(server.url("/retry.glb").toString())

        assertTrue(result.isSuccess)
        result.getOrThrow().close()
        assertEquals(2, server.requestCount)
    }

    @Test
    fun parseFailureDoesNotEvictCachedBytes() = runBlocking {
        server.enqueue(MockResponse().setBody("not-a-model"))
        val url = server.url("/broken.glb").toString()

        val first = repository.getModel(url)
        val second = repository.getModel(url)

        assertTrue(first.isFailure)
        assertTrue(second.isFailure)
        assertTrue(first.exceptionOrNull() is GltfModelRepository.ModelRepositoryException.Parse)
        assertEquals(1, server.requestCount)
    }
}

private fun ByteArray.toBuffer(): Buffer = Buffer().write(this)
