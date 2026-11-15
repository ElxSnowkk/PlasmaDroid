/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

package org.plasma.app.ui

import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberUpdatedState
import android.os.SystemClock
import android.view.TextureView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.key
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.LayoutCoordinates
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.runtime.withFrameNanos
import kotlinx.coroutines.channels.Channel
import org.plasma.render.gl.GlCompositor
import org.plasma.render.gl.Scene
import org.plasma.render.RenderBackend
import org.plasma.render.VulkanSurfaceView

class GlassRegistry {
    class Entry(
        var coords: LayoutCoordinates? = null, var radius: Float = 0f, var shadow: Float = 0f, var shadowAlpha: Float = 0.4f,
        var mix: Float = 1f, var z: Float = 0f, var alpha: () -> Float = { 1f },
    )

    private val map = LinkedHashMap<Any, Entry>()
    private val trigger = Channel<Unit>(Channel.CONFLATED)
    private var orderCache: List<Entry> = emptyList()
    private var orderDirty = true
    @Volatile private var animUntil = 0L

    fun update(key: Any, c: LayoutCoordinates, radius: Float, shadow: Float, shadowAlpha: Float, mix: Float, z: Float, alpha: () -> Float) {
        val old = map[key]
        val e = old ?: Entry().also { map[key] = it; orderDirty = true }
        if (old != null && old.z != z) orderDirty = true
        e.coords = c; e.radius = radius; e.shadow = shadow; e.shadowAlpha = shadowAlpha; e.mix = mix; e.z = z; e.alpha = alpha
        trigger.trySend(Unit)
    }

    fun remove(key: Any) {
        if (map.remove(key) != null) {
            orderDirty = true
            trigger.trySend(Unit)
        }
    }

    fun kick(ms: Int) {
        animUntil = maxOf(animUntil, SystemClock.uptimeMillis() + ms)
        trigger.trySend(Unit)
    }

    fun snapshot(sat: Float, contrast: Float, brightness: Float): FloatArray {
        if (orderDirty) {
            orderCache = map.values.sortedBy { it.z }
            orderDirty = false
        }
        val out = FloatArray(orderCache.size * Scene.STRIDE)
        var i = 0
        for (e in orderCache) {
            val coords = e.coords ?: continue
            if (!coords.isAttached) continue
            val r = coords.boundsInRoot()
            if (r.width < 1f || r.height < 1f) continue
            val a = e.alpha().coerceIn(0f, 1f)
            if (a <= 0.004f) continue
            out[i] = r.left; out[i + 1] = r.top; out[i + 2] = r.width; out[i + 3] = r.height
            out[i + 4] = e.radius; out[i + 5] = e.shadow; out[i + 6] = e.shadowAlpha; out[i + 7] = e.mix
            out[i + 8] = sat; out[i + 9] = contrast; out[i + 10] = brightness; out[i + 11] = a
            i += Scene.STRIDE
        }
        return if (i == out.size) out else out.copyOf(i)
    }

    suspend fun run(sat: () -> Float, push: (FloatArray) -> Unit) {
        while (true) {
            trigger.receive()
            push(snapshot(sat(), 1f, 1f))
            while (SystemClock.uptimeMillis() < animUntil) {
                withFrameNanos { }
                push(snapshot(sat(), 1f, 1f))
            }
        }
    }
}

val LocalGlass = compositionLocalOf { GlassRegistry() }

fun Modifier.glass(
    tint: Color, radius: Dp = 0.dp, shadow: Dp = 0.dp, shadowAlpha: Float = 0.38f, kind: Float = 1f,
    z: Float = 0f, alpha: () -> Float = { 1f }, fixedAlpha: Float? = null,
): Modifier = composed {
    val reg = LocalGlass.current
    val fx = LocalFx.current
    val d = LocalDensity.current
    val key = remember { Any() }
    DisposableEffect(key) { onDispose { reg.remove(key) } }
    val rPx = with(d) { radius.toPx() }
    val sPx = if (fx.shadows) with(d) { shadow.toPx() } else 0f
    val shape = RoundedCornerShape(radius)
    val c = tint.copy(alpha = fixedAlpha ?: (fx.alpha(kind) * tint.alpha))
    Modifier
        .onGloballyPositioned { reg.update(key, it, rPx, sPx, shadowAlpha, if (fx.realBlur) 1f else 0f, z, alpha) }
        .clip(shape)
        .background(c, shape)
}

@Composable
fun GlSurface(compositor: GlCompositor, modifier: Modifier = Modifier.fillMaxSize()) {
    val selection by compositor.backendSelection.collectAsState()
    val customWallpaper by compositor.hasCustomWallpaper.collectAsState()
    val useVulkan = selection?.active == RenderBackend.VULKAN && !customWallpaper
    key(useVulkan) {
        if (useVulkan) {
            AndroidView(
                modifier = modifier,
                factory = { ctx -> VulkanSurfaceView(ctx, compositor.vulkanSurfaceCompositor) },
            )
        } else {
            AndroidView(modifier = modifier, factory = { ctx -> TextureView(ctx).also { compositor.attach(it) } })
        }
    }
}

@Composable
fun GlassPump(reg: GlassRegistry, compositor: GlCompositor, saturation: Float) {
    val sat = androidx.compose.runtime.rememberUpdatedState(saturation)
    LaunchedEffect(reg, compositor) { reg.run({ sat.value }) { compositor.setScene(it) } }
}
