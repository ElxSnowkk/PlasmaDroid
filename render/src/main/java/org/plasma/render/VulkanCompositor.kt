
package org.plasma.render

import android.view.Surface
import org.plasma.core.PlasmaLog
import org.plasma.render.gl.FxConfig
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

class VulkanCompositor(
    private val onReady: (String) -> Unit,
    private val onFailure: (String) -> Unit,
) {
    private val executor: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "plasma-vulkan-control").apply { isDaemon = true }
    }
    private val generation = AtomicLong(0L)
    @Volatile private var handle: Long = 0L
    @Volatile private var watchdog: ScheduledFuture<*>? = null
    @Volatile private var fx = FxConfig()
    @Volatile private var scene = FloatArray(0)

    fun start(surface: Surface) {
        val token = generation.incrementAndGet()
        executor.execute {
            if (generation.get() != token) return@execute
            cancelWatchdog()
            destroyHandle()
            try {
                val created = NativeVulkan.create(surface)
                if (created == 0L) {
                    onFailure("não foi possível criar VkSurfaceKHR/swapchain ou pipelines SPIR-V")
                    return@execute
                }
                if (generation.get() != token) {
                    NativeVulkan.destroy(created)
                    return@execute
                }
                handle = created
                applyCurrentState(created)
                val name = NativeVulkan.deviceName(created).ifBlank { "Dispositivo Vulkan" }
                PlasmaLog.i("vulkan", "compositor Vulkan real iniciado: $name · swapchain + SPIR-V")
                onReady(name)
                watchdog = executor.scheduleAtFixedRate({
                    val active = handle
                    if (generation.get() == token && active != 0L && !runCatching { NativeVulkan.isRunning(active) }.getOrDefault(false)) {
                        cancelWatchdog()
                        handle = 0L
                        runCatching { NativeVulkan.destroy(active) }
                        generation.compareAndSet(token, token + 1L)
                        val reason = "thread de composição Vulkan encerrou após a inicialização"
                        PlasmaLog.w("vulkan", reason)
                        onFailure(reason)
                    }
                }, 600, 600, TimeUnit.MILLISECONDS)
            } catch (t: Throwable) {
                PlasmaLog.e("vulkan", "falha ao iniciar compositor Vulkan", t)
                onFailure("${t.javaClass.simpleName}: ${t.message ?: "falha nativa"}")
            }
        }
    }

    fun setFx(value: FxConfig) {
        if (value == fx) return
        fx = value
        enqueue { h -> applyFx(h) }
    }

    fun setScene(value: FloatArray) {
        scene = value
        enqueue { h -> NativeVulkan.setScene(h, scene) }
    }

    fun resize(width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        val token = generation.get()
        executor.execute {
            val h = handle
            if (h != 0L && generation.get() == token) runCatching { NativeVulkan.resize(h, width, height) }
        }
    }

    fun stop() {
        generation.incrementAndGet()
        executor.execute {
            cancelWatchdog()
            destroyHandle()
        }
    }

    private fun enqueue(block: (Long) -> Unit) {
        val token = generation.get()
        executor.execute {
            val h = handle
            if (h != 0L && generation.get() == token) block(h)
        }
    }

    private fun applyFx(h: Long) {
        val currentFx = fx
        NativeVulkan.configure(h, currentFx.wallpaper.style, palette(currentFx), currentFx.animated,
            currentFx.fpsCap, currentFx.blur, currentFx.blurStrength, currentFx.vignette, currentFx.dim)
    }

    private fun applyCurrentState(h: Long) {
        applyFx(h)
        NativeVulkan.setScene(h, scene)
    }

    private fun cancelWatchdog() {
        watchdog?.cancel(false)
        watchdog = null
    }

    private fun destroyHandle() {
        val old = handle
        handle = 0L
        if (old != 0L) runCatching { NativeVulkan.destroy(old) }
            .onFailure { PlasmaLog.w("vulkan", "falha ao encerrar compositor", it) }
    }

    private fun palette(c: FxConfig): FloatArray = FloatArray(12).also { out ->
        for (i in 0 until 4) {
            val rgb = c.wallpaper.palette.getOrElse(i) { 0 }
            out[i * 3] = ((rgb shr 16) and 255) / 255f
            out[i * 3 + 1] = ((rgb shr 8) and 255) / 255f
            out[i * 3 + 2] = (rgb and 255) / 255f
        }
    }
}
