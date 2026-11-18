
package org.plasma.wm

import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class WindowAspectTest {
    private val wa = Frame(0f, 0f, 1000f, 600f)
    private lateinit var wm: WindowManagerCore

    @Before fun setUp() { wm = WindowManagerCore(wa, 2) }
    private val s get() = wm.state.value

    @Test fun orientationFollowsScreenShape() {
        assertEquals(Orientation.LANDSCAPE, WindowAspect.orientationOf(Frame(0f, 0f, 2280f, 1080f)))
        assertEquals(Orientation.PORTRAIT, WindowAspect.orientationOf(Frame(0f, 0f, 1080f, 2280f)))
        assertEquals(Orientation.LANDSCAPE, WindowAspect.orientationOf(Frame(0f, 0f, 800f, 800f)))
        assertEquals(Orientation.PORTRAIT, Orientation.LANDSCAPE.flipped)
    }

    @Test fun parsePresets() {
        assertEquals(16f / 9f, WindowAspect.parse("16:9")!!, 1e-4f)
        assertEquals(16f / 9f, WindowAspect.parse("9:16")!!, 1e-4f)
        assertNull(WindowAspect.parse(WindowAspect.SCREEN))
        assertNull(WindowAspect.parse("0:9")); assertNull(WindowAspect.parse("abc")); assertNull(WindowAspect.parse("16:9:1"))
        WindowAspect.PRESETS.filter { it != WindowAspect.SCREEN }.forEach { assertNotNull(it, WindowAspect.parse(it)) }
    }

    @Test fun targetRatioPerOrientation() {
        val screen = Frame(0f, 0f, 2280f, 1080f)
        assertEquals(16f / 9f, WindowAspect.targetRatio("16:9", screen, Orientation.LANDSCAPE), 1e-4f)
        assertEquals(9f / 16f, WindowAspect.targetRatio("16:9", screen, Orientation.PORTRAIT), 1e-4f)
        assertEquals(2280f / 1080f, WindowAspect.targetRatio(WindowAspect.SCREEN, screen, Orientation.LANDSCAPE), 1e-4f)
        assertEquals(1080f / 2280f, WindowAspect.targetRatio(WindowAspect.SCREEN, screen, Orientation.PORTRAIT), 1e-4f)

        assertEquals(2280f / 1080f, WindowAspect.targetRatio("lixo", screen, Orientation.LANDSCAPE), 1e-4f)
    }

    @Test fun fitLetterboxesWideAreaIntoNarrowRatio() {

        val f = WindowAspect.fit(Frame(0f, 0f, 2280f, 1080f), 16f / 9f)
        assertEquals(1080f, f.h, 0f); assertEquals(1920f, f.w, 0f)
        assertEquals(180f, f.x, 0f); assertEquals(0f, f.y, 0f)
    }

    @Test fun fitPillarboxesIntoPortrait() {

        val f = WindowAspect.fit(Frame(0f, 0f, 2280f, 1080f), 9f / 16f)
        assertEquals(1080f, f.h, 0f); assertEquals(607f, f.w, 0f)
        assertEquals((2280f - 607f) / 2f, f.x, 0.01f)
    }

    @Test fun fitKeepsAreaWhenRatioMatchesAndHandlesDegenerateInput() {
        val a = Frame(10f, 20f, 1600f, 900f)
        assertEquals(a, WindowAspect.fit(a, 16f / 9f))
        assertEquals(Frame(0f, 0f, 0f, 0f), WindowAspect.fit(Frame(0f, 0f, 0f, 0f), 1.7f))
        assertEquals(a, WindowAspect.fit(a, 0f))
    }

    @Test fun fitNeverExceedsArea() {
        listOf(16f / 9f, 4f / 3f, 21f / 9f, 9f / 16f, 0.5f, 3f).forEach { r ->
            val area = Frame(0f, 0f, 1234f, 777f)
            val f = WindowAspect.fit(area, r)
            assertTrue(f.w <= area.w && f.h <= area.h && f.x >= 0f && f.y >= 0f)
            assertEquals(r, f.w / f.h, 0.02f)
        }
    }

    @Test fun rotateSwapsSizeAroundCenter() {
        val f = WindowAspect.rotate(Frame(100f, 100f, 400f, 200f), wa)
        assertEquals(200f, f.w, 0.01f); assertEquals(400f, f.h, 0.01f)
        assertEquals(300f, f.x + f.w / 2f, 0.01f); assertEquals(200f, f.y + f.h / 2f, 0.01f)
    }

    @Test fun rotateShrinksToFitWorkArea() {
        val f = WindowAspect.rotate(Frame(0f, 0f, 900f, 500f), wa)
        assertTrue(f.h <= wa.h + 0.01f && f.w <= wa.w)
        assertEquals(500f / 900f, f.w / f.h, 1e-3f)
    }

    @Test fun wmRotateTurnsNormalWindowAndStaysInsideWorkArea() {
        val a = wm.open("A", "x", "k", Frame(100f, 50f, 500f, 300f))
        wm.rotate(a)
        val f = s.window(a)!!.frame
        assertEquals(300f, f.w, 0.01f); assertEquals(500f, f.h, 0.01f)
        assertTrue(f.x >= wa.x && f.y >= wa.y && f.right <= wa.right && f.bottom <= wa.bottom)
        wm.rotate(a)
        val g = s.window(a)!!.frame
        assertEquals(500f, g.w, 0.01f); assertEquals(300f, g.h, 0.01f)
    }

    @Test fun wmRotateOnMaximizedRestoresFirstThenRotates() {
        val a = wm.open("A", "x", "k", Frame(100f, 50f, 500f, 300f))
        wm.maximize(a); wm.rotate(a)
        val w = s.window(a)!!
        assertEquals(WinState.NORMAL, w.state); assertNull(w.restore)
        assertEquals(300f, w.frame.w, 0.01f); assertEquals(500f, w.frame.h, 0.01f)
    }

    @Test fun wmRotateIgnoresFullscreenAndUnknownIds() {
        val a = wm.open("A", "x", "k", Frame(100f, 50f, 500f, 300f))
        wm.fullscreen(a); val before = s.window(a)!!
        wm.rotate(a); assertEquals(before, s.window(a))
        wm.rotate(9999)
    }
}
