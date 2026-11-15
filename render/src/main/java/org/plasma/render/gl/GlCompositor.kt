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

package org.plasma.render.gl

import android.graphics.Bitmap
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES30
import android.opengl.GLUtils
import android.os.SystemClock
import android.view.TextureView
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.plasma.core.PlasmaLog
import org.plasma.render.RenderBackend
import org.plasma.render.RenderBackendManager
import org.plasma.render.RenderSelection
import org.plasma.render.VulkanCompositor
import kotlin.math.max

data class GpuInfo(
    val ready: Boolean = false,
    val backend: String = "OpenGL ES",
    val renderer: String = "",
    val vendor: String = "",
    val version: String = "",
    val glsl: String = "",
    val maxTexture: Int = 0,
    val fps: Float = 0f,
    val error: String = "",
)

data class FxConfig(
    val blur: Boolean = true,
    val blurStrength: Float = 0.6f,
    val saturation: Float = 1.25f,
    val contrast: Float = 1.0f,
    val brightness: Float = 1.0f,
    val wallpaper: WallpaperDef = Wallpapers.ALL[0],
    val animated: Boolean = true,
    val fpsCap: Int = 60,
    val vignette: Float = 0.35f,
    val dim: Float = 0f,
)

object Scene { const val STRIDE = 12 }

class GlCompositor(private val renderBackend: RenderBackendManager) {
    private val _info = MutableStateFlow(GpuInfo())
    val info: StateFlow<GpuInfo> = _info.asStateFlow()
    val backendSelection: StateFlow<RenderSelection?> get() = renderBackend.selection
    private val _hasCustomWallpaper = MutableStateFlow(false)
    val hasCustomWallpaper: StateFlow<Boolean> = _hasCustomWallpaper.asStateFlow()
    val vulkanSurfaceCompositor = VulkanCompositor(
        onReady = { name ->
            alive = true
            _info.value = GpuInfo(ready = true, backend = "Vulkan", renderer = name,
                vendor = "Vulkan", version = "Vulkan 1.1 · swapchain", glsl = "SPIR-V", fps = 0f)
        },
        onFailure = { reason ->
            alive = false
            _info.value = _info.value.copy(ready = false, backend = "Vulkan (falhou)", error = reason)
            renderBackend.runtimeFallback(reason)
        },
    )

    @Volatile private var fx = FxConfig()
    @Volatile private var scene = FloatArray(0)
    @Volatile private var sceneGeneration = 0
    private val sceneLock = Any()
    @Volatile private var pendingBitmap: Bitmap? = null
    @Volatile private var bitmapGeneration = 0
    @Volatile private var thread: RenderThread? = null
    @Volatile var alive = false
        private set

    fun setFx(c: FxConfig) {
        if (c != fx) {
            fx = c
            vulkanSurfaceCompositor.setFx(c)
            wake()
        }
    }

    fun setScene(s: FloatArray) {
        val snapshot = synchronized(sceneLock) {
            if (s.contentEquals(scene)) null else s.copyOf().also {
                scene = it
                sceneGeneration++
            }
        }
        if (snapshot != null) {
            vulkanSurfaceCompositor.setScene(snapshot)
            wake()
        }
    }

    fun setWallpaperBitmap(b: Bitmap?) {
        pendingBitmap = b
        bitmapGeneration++
        _hasCustomWallpaper.value = b != null && !b.isRecycled
        if (_hasCustomWallpaper.value) {
            _info.value = _info.value.copy(backend = "OpenGL ES · wallpaper personalizado")
        }
        wake()
    }

    private fun wake() { thread?.poke() }

    fun attach(view: TextureView) {
        view.isOpaque = true
        view.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                thread?.shutdown()
                thread = RenderThread(st, w, h).also { it.start() }
            }
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) { thread?.resize(w, h) }
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                _info.value = _info.value.copy(ready = false)
                thread?.shutdown(); thread = null; alive = false
                return true
            }
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
    }

    private class Target(val tex: Int, val fbo: Int, val w: Int, val h: Int)

    private class Prog(vs: String, fs: String) {
        val id: Int
        private val locs = HashMap<String, Int>()
        init {
            val v = compile(GLES30.GL_VERTEX_SHADER, vs)
            val f = compile(GLES30.GL_FRAGMENT_SHADER, fs)
            id = GLES30.glCreateProgram()
            GLES30.glAttachShader(id, v); GLES30.glAttachShader(id, f)
            GLES30.glLinkProgram(id)
            val ok = IntArray(1); GLES30.glGetProgramiv(id, GLES30.GL_LINK_STATUS, ok, 0)
            if (ok[0] == 0) error("link: " + GLES30.glGetProgramInfoLog(id))
            GLES30.glDeleteShader(v); GLES30.glDeleteShader(f)
        }
        fun loc(n: String): Int = locs.getOrPut(n) { GLES30.glGetUniformLocation(id, n) }
        private fun compile(type: Int, src: String): Int {
            val s = GLES30.glCreateShader(type)
            GLES30.glShaderSource(s, src); GLES30.glCompileShader(s)
            val ok = IntArray(1); GLES30.glGetShaderiv(s, GLES30.GL_COMPILE_STATUS, ok, 0)
            if (ok[0] == 0) error("shader: " + GLES30.glGetShaderInfoLog(s))
            return s
        }
    }

    private inner class RenderThread(private val st: SurfaceTexture, @Volatile private var w: Int, @Volatile private var h: Int) : Thread("plasma-gl") {
        @Volatile private var running = true
        private val lock = Object()
        private var dirty = true

        private var dpy: EGLDisplay = EGL14.EGL_NO_DISPLAY
        private var ctx: EGLContext = EGL14.EGL_NO_CONTEXT
        private var surf: EGLSurface = EGL14.EGL_NO_SURFACE

        private lateinit var pWall: Prog
        private lateinit var pDown: Prog
        private lateinit var pUp: Prog
        private lateinit var pPresent: Prog
        private lateinit var pGlass: Prog
        private var vao = 0
        private var instanceVbo = 0
        private var instanceCapacityBytes = 0
        private var instanceData: FloatBuffer = ByteBuffer.allocateDirect(13 * 64 * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
        private var instanceCount = 0
        private var uploadedSceneGeneration = -1
        private var base: Target? = null
        private var chain = ArrayList<Target>()
        private var chainKey = ""
        private var imgTex = 0
        private var imgAspect = 1f
        private var imgGen = -1
        private var dummyTex = 0
        private var wallKey = ""
        private var blurKey = ""
        private var fpsFrames = 0
        private var fpsT0 = SystemClock.elapsedRealtime()
        private var lastBlurUpdateMs = 0L
        private val t0 = SystemClock.elapsedRealtime()

        fun poke() { synchronized(lock) { dirty = true; lock.notifyAll() } }
        fun resize(nw: Int, nh: Int) {
            if (nw <= 0 || nh <= 0 || (w == nw && h == nh)) return
            w = nw; h = nh
            uploadedSceneGeneration = -1
            poke()
        }
        fun shutdown() {
            running = false
            poke()
            if (Thread.currentThread() !== this) runCatching { join(96) }
        }

        override fun run() {
            try {
                runCatching { android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DISPLAY) }
                initEgl(); initGl()
                alive = true
                loop()
            } catch (t: Throwable) {
                PlasmaLog.e("gl", "compositor falhou", t)
                if (thread === this) {
                    _info.value = _info.value.copy(ready = false, error = "${t.javaClass.simpleName}: ${t.message}")
                }
            } finally {
                if (thread === this) {
                    alive = false
                    _info.value = _info.value.copy(ready = false)
                }
                runCatching { releaseGl() }
            }
        }

        private fun initEgl() {
            dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val ver = IntArray(2)
            check(EGL14.eglInitialize(dpy, ver, 0, ver, 1)) { "eglInitialize" }
            val cfgAttr = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_NONE)
            val cfgs = arrayOfNulls<EGLConfig>(1); val n = IntArray(1)
            check(EGL14.eglChooseConfig(dpy, cfgAttr, 0, cfgs, 0, 1, n, 0) && n[0] > 0) { "sem EGLConfig ES3" }
            ctx = EGL14.eglCreateContext(dpy, cfgs[0], EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE), 0)
            check(ctx != EGL14.EGL_NO_CONTEXT) { "eglCreateContext ES3" }
            surf = EGL14.eglCreateWindowSurface(dpy, cfgs[0], st, intArrayOf(EGL14.EGL_NONE), 0)
            check(surf != EGL14.EGL_NO_SURFACE) { "eglCreateWindowSurface" }
            check(EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) { "eglMakeCurrent" }
            EGL14.eglSwapInterval(dpy, 1)
        }

        private fun initGl() {
            pWall = Prog(Shaders.FULLSCREEN_VS, Shaders.WALLPAPER_FS)
            pDown = Prog(Shaders.FULLSCREEN_VS, Shaders.KAWASE_DOWN_FS)
            pUp = Prog(Shaders.FULLSCREEN_VS, Shaders.KAWASE_UP_FS)
            pPresent = Prog(Shaders.FULLSCREEN_VS, Shaders.PRESENT_FS)
            pGlass = Prog(Shaders.GLASS_VS, Shaders.GLASS_FS)
            val a = IntArray(1)
            GLES30.glGenVertexArrays(1, a, 0); vao = a[0]
            GLES30.glBindVertexArray(vao)
            val b = IntArray(1)
            GLES30.glGenBuffers(1, b, 0); instanceVbo = b[0]
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, instanceVbo)
            instanceCapacityBytes = instanceData.capacity() * 4
            GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, instanceCapacityBytes, null, GLES30.GL_DYNAMIC_DRAW)
            val stride = 13 * 4
            val attrs = intArrayOf(0, 1, 2, 3)
            val sizes = intArrayOf(4, 4, 4, 1)
            val offsets = intArrayOf(0, 16, 32, 48)
            for (i in attrs.indices) {
                GLES30.glEnableVertexAttribArray(attrs[i])
                GLES30.glVertexAttribPointer(attrs[i], sizes[i], GLES30.GL_FLOAT, false, stride, offsets[i])
                GLES30.glVertexAttribDivisor(attrs[i], 1)
            }
            dummyTex = newTexture(1, 1)
            val mx = IntArray(1); GLES30.glGetIntegerv(GLES30.GL_MAX_TEXTURE_SIZE, mx, 0)
            _info.value = GpuInfo(
                ready = true,
                backend = if (_hasCustomWallpaper.value) "OpenGL ES · wallpaper personalizado" else "OpenGL ES",
                renderer = GLES30.glGetString(GLES30.GL_RENDERER) ?: "",
                vendor = GLES30.glGetString(GLES30.GL_VENDOR) ?: "",
                version = GLES30.glGetString(GLES30.GL_VERSION) ?: "",
                glsl = GLES30.glGetString(GLES30.GL_SHADING_LANGUAGE_VERSION) ?: "",
                maxTexture = mx[0],
            )
            PlasmaLog.i("gl", "compositor OpenGL ES pronto: ${_info.value.renderer} · ${_info.value.version}")
        }

        private fun newTexture(tw: Int, th: Int): Int {
            val a = IntArray(1); GLES30.glGenTextures(1, a, 0)
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, a[0])
            GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGBA8, tw, th, 0, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, null)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
            return a[0]
        }

        private fun newTarget(tw: Int, th: Int): Target {
            val tex = newTexture(tw, th)
            val f = IntArray(1); GLES30.glGenFramebuffers(1, f, 0)
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, f[0])
            GLES30.glFramebufferTexture2D(GLES30.GL_FRAMEBUFFER, GLES30.GL_COLOR_ATTACHMENT0, GLES30.GL_TEXTURE_2D, tex, 0)
            check(GLES30.glCheckFramebufferStatus(GLES30.GL_FRAMEBUFFER) == GLES30.GL_FRAMEBUFFER_COMPLETE) { "FBO incompleto ${tw}x$th" }
            return Target(tex, f[0], tw, th)
        }

        private fun freeTarget(t: Target) {
            GLES30.glDeleteFramebuffers(1, intArrayOf(t.fbo), 0)
            GLES30.glDeleteTextures(1, intArrayOf(t.tex), 0)
        }

        private fun ensureTargets(levels: Int, scale: Float) {
            val bw = max(1, (w * scale).toInt()); val bh = max(1, (h * scale).toInt())
            val key = "${bw}x$bh/$levels"
            if (key == chainKey) return
            base?.let(::freeTarget); chain.forEach(::freeTarget); chain.clear()
            base = newTarget(bw, bh)
            var cw = bw; var ch = bh
            for (i in 0 until levels) {
                cw = max(1, cw / 2); ch = max(1, ch / 2)
                chain += newTarget(cw, ch)
            }
            chainKey = key; wallKey = ""; blurKey = ""
        }

        private fun uploadImage(b: Bitmap?) {
            if (imgTex != 0) { GLES30.glDeleteTextures(1, intArrayOf(imgTex), 0); imgTex = 0 }
            if (b == null || b.isRecycled) return
            val a = IntArray(1); GLES30.glGenTextures(1, a, 0); imgTex = a[0]
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, imgTex)
            GLUtils.texImage2D(GLES30.GL_TEXTURE_2D, 0, b, 0)
            GLES30.glGenerateMipmap(GLES30.GL_TEXTURE_2D)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR_MIPMAP_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
            imgAspect = b.width.toFloat() / b.height.coerceAtLeast(1)
        }

        private fun updateInstances() {
            val (s, generation) = synchronized(sceneLock) { scene to sceneGeneration }
            if (uploadedSceneGeneration == generation) return
            val required = ((s.size / Scene.STRIDE).coerceAtLeast(1) * 13 * 4)
            if (required > instanceCapacityBytes) {
                val maximum = 64 * 1024 * 1024
                var cap = instanceCapacityBytes.coerceAtLeast(13 * 64 * 4)
                while (cap < required && cap < maximum) cap = (cap * 2).coerceAtMost(maximum)
                check(cap >= required) { "camada de vidro excede 64 MiB" }
                instanceCapacityBytes = cap
                instanceData = ByteBuffer.allocateDirect(cap).order(ByteOrder.nativeOrder()).asFloatBuffer()
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, instanceVbo)
                GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, cap, null, GLES30.GL_DYNAMIC_DRAW)
            }
            instanceData.clear()
            instanceCount = 0
            var i = 0
            while (i + Scene.STRIDE <= s.size) {
                val x = s[i]; val y = s[i + 1]; val rw = s[i + 2]; val rh = s[i + 3]
                val right = x + rw; val bottom = y + rh
                val valid = x.isFinite() && y.isFinite() && rw.isFinite() && rh.isFinite() && rw > 0f && rh > 0f
                if (valid && right > 0f && bottom > 0f && x < w.toFloat() && y < h.toFloat()) {
                    instanceData.put(s, i, 12)
                    instanceData.put(s[i + 11])
                    instanceCount++
                }
                i += Scene.STRIDE
            }
            instanceData.flip()
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, instanceVbo)
            val bytes = instanceData.remaining() * 4
            if (bytes > 0) GLES30.glBufferSubData(GLES30.GL_ARRAY_BUFFER, 0, bytes, instanceData)
            uploadedSceneGeneration = generation
        }

        private fun loop() {
            while (running) {
                val cfg = fx
                val animated = cfg.animated && imgTex == 0 && pendingBitmap == null
                synchronized(lock) {
                    while (!dirty && !animated && running) runCatching { lock.wait() }
                    dirty = false
                }
                if (!running) break
                if (w <= 0 || h <= 0) continue
                val frameStart = SystemClock.elapsedRealtime()
                renderFrame(fx)
                check(EGL14.eglSwapBuffers(dpy, surf)) { "eglSwapBuffers" }
                fpsFrames++
                val now = SystemClock.elapsedRealtime()
                if (now - fpsT0 >= 1000) {
                    _info.value = _info.value.copy(fps = fpsFrames * 1000f / (now - fpsT0))
                    fpsFrames = 0; fpsT0 = now
                }
                if (animated) {
                    val budget = 1000L / fx.fpsCap.coerceIn(10, 120)
                    val spent = SystemClock.elapsedRealtime() - frameStart
                    if (spent < budget) runCatching { Thread.sleep(budget - spent) }
                }
            }
        }

        private fun fullscreen() { GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3) }

        private fun bindTex(unit: Int, tex: Int) {
            GLES30.glActiveTexture(GLES30.GL_TEXTURE0 + unit)
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, tex)
        }

        private var wkId: String? = null; private var wkTex = -1; private var wkW = -1; private var wkH = -1; private var wkStr = ""
        private fun wallKeyOf(id: String, tex: Int, tw: Int, th: Int): String {
            if (id != wkId || tex != wkTex || tw != wkW || th != wkH) {
                wkId = id; wkTex = tex; wkW = tw; wkH = th; wkStr = "$id/$tex/${tw}x$th"
            }
            return wkStr
        }
        private var bkWk: String? = null; private var bkLevels = -1; private var bkOffset = Float.NaN; private var bkStr = ""
        private fun blurKeyOf(wk: String, levels: Int, offset: Float): String {
            if (wk != bkWk || levels != bkLevels || offset != bkOffset) {
                bkWk = wk; bkLevels = levels; bkOffset = offset; bkStr = "$wk/$levels/$offset"
            }
            return bkStr
        }
        private val wallColorNames = Array(4) { "uC$it" }

        private fun renderFrame(c: FxConfig) {
            if (bitmapGeneration != imgGen) {
                imgGen = bitmapGeneration
                uploadImage(pendingBitmap)
                wallKey = ""
            }
            val strength = c.blurStrength.coerceIn(0f, 1f)

            val scale = if (imgTex == 0) 0.5f else 1f
            val baseLevels = if (c.blur) (2 + (strength * 3f).toInt()).coerceIn(2, 5) else 1
            val levels = if (c.blur && scale < 1f) (baseLevels - 1).coerceAtLeast(2) else baseLevels
            val offset = 1.4f + strength * 1.8f
            ensureTargets(levels, scale)
            val b = base ?: return
            val animated = c.animated && imgTex == 0
            val time = (SystemClock.elapsedRealtime() - t0) / 1000f
            var wallpaperUpdated = false

            GLES30.glDisable(GLES30.GL_BLEND)
            val wk = wallKeyOf(c.wallpaper.id, imgTex, b.w, b.h)
            if (animated || wk != wallKey) {
                GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, b.fbo)
                GLES30.glViewport(0, 0, b.w, b.h)
                GLES30.glUseProgram(pWall.id)
                GLES30.glUniform2f(pWall.loc("uRes"), b.w.toFloat(), b.h.toFloat())
                GLES30.glUniform1f(pWall.loc("uTime"), time + 12f)
                GLES30.glUniform1i(pWall.loc("uStyle"), c.wallpaper.style)
                GLES30.glUniform1f(pWall.loc("uMotion"), if (animated) c.wallpaper.motion else 0f)
                val pal = c.wallpaper.palette
                for (i in 0 until 4) {
                    val col = pal[i]
                    GLES30.glUniform3f(pWall.loc(wallColorNames[i]), ((col shr 16) and 255) / 255f, ((col shr 8) and 255) / 255f, (col and 255) / 255f)
                }
                val useImg = imgTex != 0
                GLES30.glUniform3f(pWall.loc("uImageInfo"), if (useImg) 1f else 0f, imgAspect, 1.0f + 0.03f * kotlin.math.sin(time * 0.05f))
                GLES30.glUniform1i(pWall.loc("uImage"), 1)
                bindTex(1, if (useImg) imgTex else dummyTex)
                fullscreen()
                wallKey = wk
                wallpaperUpdated = true
            }
            val bk = blurKeyOf(wk, levels, offset)
            val nowMs = SystemClock.elapsedRealtime()
            val blurIntervalMs = if (animated) 84L else 0L
            val refreshBlur = c.blur && (bk != blurKey ||
                (wallpaperUpdated && nowMs - lastBlurUpdateMs >= blurIntervalMs))
            if (refreshBlur) {
                GLES30.glUseProgram(pDown.id)
                GLES30.glUniform1i(pDown.loc("uTex"), 0)
                var src: Target = b
                for (t in chain) {
                    GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, t.fbo)
                    GLES30.glViewport(0, 0, t.w, t.h)
                    bindTex(0, src.tex)
                    GLES30.glUniform2f(pDown.loc("uHalfPixel"), 0.5f / src.w, 0.5f / src.h)
                    GLES30.glUniform1f(pDown.loc("uOffset"), offset)
                    fullscreen()
                    src = t
                }
                GLES30.glUseProgram(pUp.id)
                GLES30.glUniform1i(pUp.loc("uTex"), 0)
                for (i in chain.size - 1 downTo 1) {
                    val s = chain[i]; val d = chain[i - 1]
                    GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, d.fbo)
                    GLES30.glViewport(0, 0, d.w, d.h)
                    bindTex(0, s.tex)
                    GLES30.glUniform2f(pUp.loc("uHalfPixel"), 0.5f / s.w, 0.5f / s.h)
                    GLES30.glUniform1f(pUp.loc("uOffset"), offset)
                    fullscreen()
                }
                blurKey = bk
                lastBlurUpdateMs = nowMs
            }
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0)
            GLES30.glViewport(0, 0, w, h)
            GLES30.glUseProgram(pPresent.id)
            GLES30.glUniform1i(pPresent.loc("uTex"), 0)
            GLES30.glUniform2f(pPresent.loc("uRes"), w.toFloat(), h.toFloat())
            GLES30.glUniform1f(pPresent.loc("uVignette"), c.vignette)
            GLES30.glUniform1f(pPresent.loc("uDim"), c.dim)
            bindTex(0, b.tex)
            fullscreen()
            val s = scene
            if (s.size >= Scene.STRIDE) {
                GLES30.glEnable(GLES30.GL_BLEND)
                GLES30.glBlendFunc(GLES30.GL_ONE, GLES30.GL_ONE_MINUS_SRC_ALPHA)
                updateInstances()
                if (instanceCount > 0) {
                    GLES30.glUseProgram(pGlass.id)
                    GLES30.glUniform2f(pGlass.loc("uRes"), w.toFloat(), h.toFloat())
                    GLES30.glUniform1i(pGlass.loc("uBlur"), 0)
                    GLES30.glUniform1i(pGlass.loc("uSharp"), 1)
                    bindTex(1, b.tex)
                    bindTex(0, if (c.blur) chain.first().tex else b.tex)
                    GLES30.glBindVertexArray(vao)
                    GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, instanceVbo)
                    GLES30.glDrawArraysInstanced(GLES30.GL_TRIANGLE_STRIP, 0, 4, instanceCount)
                }
                GLES30.glDisable(GLES30.GL_BLEND)
            }
        }

        private fun releaseGl() {
            if (instanceVbo != 0) GLES30.glDeleteBuffers(1, intArrayOf(instanceVbo), 0)
            if (vao != 0) GLES30.glDeleteVertexArrays(1, intArrayOf(vao), 0)
            instanceVbo = 0; vao = 0
            if (dpy != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (surf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, surf)
                if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx)
                EGL14.eglTerminate(dpy)
            }
            dpy = EGL14.EGL_NO_DISPLAY; ctx = EGL14.EGL_NO_CONTEXT; surf = EGL14.EGL_NO_SURFACE
        }
    }
}
