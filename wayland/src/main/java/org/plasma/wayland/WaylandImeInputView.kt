
package org.plasma.wayland

import android.text.InputType

import android.content.Context
import android.graphics.Rect
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

class WaylandImeInputView(
    context: Context,
    private val commit: (String) -> Unit,
    private val preedit: (String) -> Unit,
    private val key: (KeyEvent) -> Unit,
) : View(context) {
    init {
        isFocusable = true
        isFocusableInTouchMode = true
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
        alpha = 0f
        isClickable = false
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                if (!text.isNullOrEmpty()) commit(text.toString())
                preedit("")
                return true
            }

            override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
                preedit(text?.toString().orEmpty())
                return true
            }

            override fun finishComposingText(): Boolean {
                preedit("")
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                key(event)
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                repeat(beforeLength.coerceIn(0, 64)) {
                    key(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL))
                    key(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL))
                }
                repeat(afterLength.coerceIn(0, 64)) {
                    key(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_FORWARD_DEL))
                    key(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_FORWARD_DEL))
                }
                return true
            }
        }
    }

    fun requestSoftKeyboard(show: Boolean) {
        post {
            val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
                ?: return@post
            if (show) {
                requestFocus()
                imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
            } else {
                imm.hideSoftInputFromWindow(windowToken, 0)
            }
        }
    }

    override fun getFocusedRect(r: Rect) {
        r.set(0, 0, width.coerceAtLeast(1), height.coerceAtLeast(1))
    }
}
