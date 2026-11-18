
package org.plasma.wm

import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class WindowManagerCoreTest {
    private lateinit var wm: WindowManagerCore
    private val wa = Frame(0f, 0f, 1000f, 600f)

    @Before fun setUp() { wm = WindowManagerCore(wa, 3) }
    private val s get() = wm.state.value

    @Test fun openFocusesAndStacksOnTop() {
        val a = wm.open("A", "app:a", "android"); val b = wm.open("B", "app:b", "android")
        assertEquals(b, s.focusedId); assertEquals(listOf(a, b), s.windows.map { it.id })
        wm.focus(a)
        assertEquals(listOf(b, a), s.windows.map { it.id }); assertEquals(a, s.focusedId)
    }

    @Test fun minimizeMovesFocusAndTaskToggleRestores() {
        val a = wm.open("A", "x", "k"); val b = wm.open("B", "y", "k")
        wm.minimize(b)
        assertEquals(a, s.focusedId); assertTrue(s.window(b)!!.minimized)
        wm.toggleTask(b)
        assertEquals(b, s.focusedId); assertFalse(s.window(b)!!.minimized)
        wm.toggleTask(b)
        assertTrue(s.window(b)!!.minimized)
    }

    @Test fun maximizeAndRestoreKeepFrame() {
        val a = wm.open("A", "x", "k"); val before = s.window(a)!!.frame
        wm.maximize(a)
        assertEquals(wa, s.window(a)!!.frame); assertEquals(WinState.MAXIMIZED, s.window(a)!!.state)
        wm.restore(a)
        assertEquals(before, s.window(a)!!.frame)
    }

    @Test fun snapLeftAndRight() {
        val a = wm.open("A", "x", "k")
        wm.snap(a, true); assertEquals(Frame(0f, 0f, 500f, 600f), s.window(a)!!.frame)
        wm.snap(a, false); assertEquals(Frame(500f, 0f, 500f, 600f), s.window(a)!!.frame)
    }

    @Test fun dragEndNearEdgesSnaps() {
        val a = wm.open("A", "x", "k"); wm.dragEnd(a, 5f, 300f)
        assertEquals(WinState.SNAP_LEFT, s.window(a)!!.state)
        val b = wm.open("B", "y", "k"); wm.dragEnd(b, 500f, 2f)
        assertEquals(WinState.MAXIMIZED, s.window(b)!!.state)
    }

    @Test fun resizeRespectsMinimumSize() {
        val a = wm.open("A", "x", "k", Frame(100f, 100f, 400f, 300f))
        wm.resize(a, Edge.RIGHT or Edge.BOTTOM, -1000f, -1000f)
        val f = s.window(a)!!.frame
        assertEquals(WindowManagerCore.MIN_W, f.w); assertEquals(WindowManagerCore.MIN_H, f.h)
    }

    @Test fun resizeLeftKeepsRightEdge() {
        val a = wm.open("A", "x", "k", Frame(100f, 100f, 400f, 300f))
        wm.resize(a, Edge.LEFT, -50f, 0f)
        val f = s.window(a)!!.frame
        assertEquals(50f, f.x, 0.01f); assertEquals(500f, f.right, 0.01f)
    }

    @Test fun moveFromMaximizedRestoresSize() {
        val a = wm.open("A", "x", "k", Frame(100f, 100f, 400f, 300f)); wm.maximize(a)
        wm.move(a, 0f, 0f, pointerX = 500f)
        val w = s.window(a)!!
        assertEquals(WinState.NORMAL, w.state); assertEquals(400f, w.frame.w, 0.01f); assertEquals(300f, w.frame.x, 0.01f)
    }

    @Test fun altTabCyclesMostRecent() {
        val a = wm.open("A", "x", "k"); val b = wm.open("B", "y", "k"); val c = wm.open("C", "z", "k")
        wm.cycle(true); assertEquals(b, s.focusedId)
        wm.cycle(true); assertEquals(c, s.focusedId)
        wm.cycle(false); assertEquals(a, s.focusedId)
    }

    @Test fun showDesktopHidesAndFocusRestores() {
        val a = wm.open("A", "x", "k")
        wm.toggleShowDesktop(); assertTrue(s.visible.isEmpty()); assertNull(s.focusedId)
        wm.toggleShowDesktop(); assertEquals(a, s.focusedId)
        wm.toggleShowDesktop(); wm.focus(a); assertFalse(s.showingDesktop)
    }

    @Test fun virtualDesktopsIsolateWindows() {
        val a = wm.open("A", "x", "k"); wm.switchDesktop(1)
        assertTrue(s.visible.isEmpty()); val b = wm.open("B", "y", "k")
        assertEquals(listOf(b), s.visible.map { it.id })
        wm.moveToDesktop(b, 0); assertTrue(s.visible.isEmpty())
        wm.switchDesktop(0); assertEquals(setOf(a, b), s.visible.map { it.id }.toSet())
        wm.switchDesktop(9); assertEquals(0, s.desktop)
    }

    @Test fun reducingDesktopCountMovesWindows() {
        wm.switchDesktop(2); val a = wm.open("A", "x", "k")
        wm.setDesktopCount(2)
        assertEquals(1, s.window(a)!!.desktop); assertEquals(1, s.desktop)
    }

    @Test fun workAreaChangeKeepsMaximizedFilled() {
        val a = wm.open("A", "x", "k"); wm.maximize(a)
        val n = Frame(0f, 40f, 800f, 400f); wm.setWorkArea(n)
        assertEquals(n, s.window(a)!!.frame)
    }

    @Test fun closeRefocusesTop() {
        val a = wm.open("A", "x", "k"); val b = wm.open("B", "y", "k")
        wm.close(b); assertEquals(a, s.focusedId); wm.close(a); assertNull(s.focusedId)
    }
}
