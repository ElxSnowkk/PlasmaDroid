
package org.plasma.app.android

import android.app.ActivityOptions
import android.graphics.SurfaceTexture
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Surface
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.plasma.app.PlasmaApp
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.WindowContent.AndroidWin
import org.plasma.core.Keys
import org.plasma.core.PlasmaLog

class WindowLauncher(private val app: PlasmaApp, private val session: DesktopSession) {
    private val main = Handler(Looper.getMainLooper())
    private val pendingResize = HashMap<AndroidWin, Runnable>()

    fun onSurface(win: AndroidWin, st: SurfaceTexture, w: Int, h: Int) {
        release(win)
        win.texture = st; win.w = w; win.h = h
        st.setDefaultBufferSize(w, h)
        val surface = Surface(st)
        win.surface = surface
        val dpi = app.settings.int(Keys.ANDROID_DPI, 240)
        app.appScope.launch(Dispatchers.IO) {
            try { start(win, surface, w, h, dpi) } catch (t: Throwable) {
                PlasmaLog.e("launcher", "falha inesperada em ${win.app.pkg}", t); win.status = "Erro: ${t.message}"
            }
        }
    }

    private suspend fun start(win: AndroidWin, surface: Surface, w: Int, h: Int, dpi: Int) {
        val pkg = win.app.pkg
        val s = app.settings

        val remembered = s.string(Keys.compat(pkg)).takeIf { it == "B" || it == "A" }
        val order: List<String> = when (s.string(Keys.METHOD, "auto")) {
            "shizuku" -> listOf("B", "E")
            "app" -> listOf("A", "E")
            "conventional" -> listOf("E")
            else -> listOf("B", "A", "E").let { base -> if (remembered != null && remembered in base) listOf(remembered) + base.filter { it != remembered } else base }
        }
        for (m in order) {
            win.status = "Tentando método $m…"
            val ok = when (m) { "B" -> tryB(win, surface, w, h, dpi); "A" -> tryA(win, surface, w, h, dpi); else -> tryE(win) }
            if (ok) {
                if (m != "E") s.put(Keys.compat(pkg), m)
                PlasmaLog.i("launcher", "$pkg aberto pelo método $m"); return
            }
        }
        win.status = "Nenhum método funcionou"
        PlasmaLog.e("launcher", "$pkg: nenhum método funcionou")
    }

    private fun tryB(win: AndroidWin, surface: Surface, w: Int, h: Int, dpi: Int): Boolean {
        val b = app.backends.current()
        if (b == null || !b.trustedDisplays) { PlasmaLog.i("launcher", "B indisponível (sem Shizuku conectado)"); return false }
        val id = b.createDisplay("plasma-${win.app.pkg}", w, h, dpi, surface)
        if (id <= 0) { PlasmaLog.w("launcher", "B: createDisplay falhou: ${b.lastError()}"); return false }
        val err = b.launch(id, win.app.component)
        if (err != null) { PlasmaLog.w("launcher", "B: launch falhou: $err"); b.releaseDisplay(id); return false }
        win.displayId = id; win.backend = b; win.interactive = true
        win.method = "B · Shizuku (display confiável)"; win.status = "Em execução no display $id"
        return true
    }

    private suspend fun tryA(win: AndroidWin, surface: Surface, w: Int, h: Int, dpi: Int): Boolean {
        val vd = app.vdHost.create("plasma-${win.app.pkg}", surface, w, h, dpi) ?: return false
        val id = vd.display.displayId
        val t0 = SystemClock.uptimeMillis()
        try {
            val opts = ActivityOptions.makeBasic().setLaunchDisplayId(id)
            app.startActivity(app.catalog.launchIntent(win.app), opts.toBundle())
        } catch (t: Throwable) {
            PlasmaLog.w("launcher", "A: startActivity recusado", t); vd.release(); return false
        }
        delay(1800)
        if (app.lastStopAt >= t0) {
            PlasmaLog.w("launcher", "A: o Android abriu ${win.app.pkg} no display principal (display do app não é confiável)")
            vd.release(); return false
        }
        win.vd = vd; win.displayId = id
        val b = app.backends.current(); win.backend = b; win.interactive = b != null
        win.method = "A · Virtual Display do app"
        win.status = if (b != null) "Em execução (display $id)" else "Em execução (somente visualização: sem Shizuku/root para enviar entrada)"
        return true
    }

    private fun tryE(win: AndroidWin): Boolean = try {
        app.startActivity(app.catalog.launchIntent(win.app))
        win.method = "E · Convencional"; win.fellBack = true
        win.status = "Aberto em tela cheia, fora do desktop. Volte pelo app/recentes."
        true
    } catch (t: Throwable) { PlasmaLog.e("launcher", "E falhou", t); false }

    fun onResize(win: AndroidWin, w: Int, h: Int) {
        if (w <= 0 || h <= 0 || (w == win.w && h == win.h)) return
        pendingResize.remove(win)?.let { main.removeCallbacks(it) }
        val r = Runnable {
            win.w = w; win.h = h
            win.texture?.setDefaultBufferSize(w, h)
            val dpi = app.settings.int(Keys.ANDROID_DPI, 240)
            val id = win.displayId
            app.appScope.launch(Dispatchers.IO) {
                win.vd?.resize(w, h, dpi)
                if (win.vd == null && id > 0) win.backend?.resizeDisplay(id, w, h, dpi)
            }
        }
        pendingResize[win] = r
        main.postDelayed(r, 120)
    }

    fun release(win: AndroidWin) {
        pendingResize.remove(win)?.let { main.removeCallbacks(it) }
        val id = win.displayId
        runCatching { win.vd?.release() }
        if (win.vd == null && id > 0) { val b = win.backend; app.appScope.launch(Dispatchers.IO) { b?.releaseDisplay(id) } }
        runCatching { win.surface?.release() }
        win.vd = null; win.surface = null; win.displayId = -1; win.interactive = false
    }
}
