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

package org.plasma.core

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object PlasmaLog {
    data class Entry(val time: Long, val level: Char, val tag: String, val msg: String)

    private const val MAX = 1500
    private val lock = Any()
    private val fmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)
    private val buffer = ArrayDeque<Entry>()
    private val _entries = MutableStateFlow<List<Entry>>(emptyList())
    val entries: StateFlow<List<Entry>> = _entries.asStateFlow()
    @Volatile private var file: File? = null

    fun init(dir: File) {
        dir.mkdirs()
        val f = File(dir, "plasma.log")
        if (f.length() > 1_000_000) f.delete()
        file = f
        i("log", "arquivo de log: ${f.absolutePath}")
    }

    fun d(tag: String, msg: String) = add('D', tag, msg, null)
    fun i(tag: String, msg: String) = add('I', tag, msg, null)
    fun w(tag: String, msg: String, t: Throwable? = null) = add('W', tag, msg, t)
    fun e(tag: String, msg: String, t: Throwable? = null) = add('E', tag, msg, t)

    fun format(e: Entry): String = synchronized(lock) { "${fmt.format(Date(e.time))} ${e.level}/${e.tag}: ${e.msg}" }

    fun dump(): String = entries.value.joinToString("\n") { format(it) }

    private fun add(level: Char, tag: String, msg: String, t: Throwable?) {
        val text = if (t != null) "$msg: ${t.javaClass.simpleName}: ${t.message}" else msg
        when (level) {
            'D' -> Log.d(tag, text)
            'I' -> Log.i(tag, text)
            'W' -> Log.w(tag, text)
            else -> Log.e(tag, text)
        }
        val e = Entry(System.currentTimeMillis(), level, tag, text)
        synchronized(lock) {
            buffer.addLast(e)
            while (buffer.size > MAX) buffer.removeFirst()
            _entries.value = buffer.toList()
            file?.let { f -> runCatching { f.appendText(format(e) + "\n") } }
        }
    }
}
