
package org.plasma.app.ui

import android.app.Activity
import android.content.Intent
import android.view.KeyEvent
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.TrayPopup

class ShortcutHandler(private val session: DesktopSession, private val activity: Activity) {
    private var metaAlone = false
    private val swallowed = HashSet<Int>()

    fun handle(e: KeyEvent): Boolean {
        val code = e.keyCode
        val isMeta = code == KeyEvent.KEYCODE_META_LEFT || code == KeyEvent.KEYCODE_META_RIGHT
        if (e.action == KeyEvent.ACTION_UP) {
            if (isMeta) {
                if (metaAlone && !session.locked) { val o = !session.startMenuOpen; session.closePopups(); session.startMenuOpen = o }
                metaAlone = false
                return true
            }
            return swallowed.remove(code)
        }
        if (e.action != KeyEvent.ACTION_DOWN) return false
        if (session.locked) { session.locked = false; swallowed += code; return true }
        if (isMeta) { if (e.repeatCount == 0) metaAlone = true; return true }
        val alt = e.isAltPressed; val ctrl = e.isCtrlPressed; val meta = e.isMetaPressed; val shift = e.isShiftPressed
        if (meta) metaAlone = false
        val wm = session.wm
        val s = wm.state.value
        val focused = s.focusedId
        val handled = when {
            alt && code == KeyEvent.KEYCODE_TAB -> { wm.cycle(!shift); true }
            alt && code == KeyEvent.KEYCODE_F4 -> { focused?.let { session.closeWindow(it) }; true }
            meta && code == KeyEvent.KEYCODE_D -> { session.closePopups(); wm.toggleShowDesktop(); true }
            meta && code == KeyEvent.KEYCODE_L -> { session.closePopups(); session.locked = true; true }
            ctrl && alt && code == KeyEvent.KEYCODE_T -> { openTerminal(); true }
            meta && code == KeyEvent.KEYCODE_DPAD_LEFT -> { focused?.let { wm.snap(it, true) }; true }
            meta && code == KeyEvent.KEYCODE_DPAD_RIGHT -> { focused?.let { wm.snap(it, false) }; true }
            meta && code == KeyEvent.KEYCODE_DPAD_UP -> { focused?.let { wm.maximize(it) }; true }
            meta && code == KeyEvent.KEYCODE_DPAD_DOWN -> { focused?.let { wm.restore(it) }; true }
            ctrl && alt && code == KeyEvent.KEYCODE_DPAD_LEFT -> { wm.switchDesktop(s.desktop - 1); true }
            ctrl && alt && code == KeyEvent.KEYCODE_DPAD_RIGHT -> { wm.switchDesktop(s.desktop + 1); true }
            code == KeyEvent.KEYCODE_ESCAPE && (session.startMenuOpen || session.tray != TrayPopup.NONE || session.menu != null) -> { session.closePopups(); true }
            else -> false
        }
        if (handled) swallowed += code
        return handled
    }

    private fun openTerminal() { session.openTool("terminal") }
}
