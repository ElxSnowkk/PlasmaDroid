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

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.plasma.app.session.DesktopSession
import org.plasma.core.Keys
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun ClockWidget() {
    val now by rememberNow(1000)
    val z = Instant.ofEpochMilli(now).atZone(ZoneId.systemDefault())
    val b = LocalBreeze.current
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Canvas(Modifier.size(120.dp)) {
            val r = size.width / 2f
            drawCircle(b.line, r, style = androidx.compose.ui.graphics.drawscope.Stroke(2.dp.toPx()))
            for (i in 0 until 12) {
                val a = i * Math.PI / 6
                drawLine(b.dim, Offset(r + (r * .85f * cos(a)).toFloat(), r + (r * .85f * sin(a)).toFloat()),
                    Offset(r + (r * .95f * cos(a)).toFloat(), r + (r * .95f * sin(a)).toFloat()), 2.dp.toPx())
            }
            fun hand(angle: Double, len: Float, w: Float, c: androidx.compose.ui.graphics.Color) =
                drawLine(c, Offset(r, r), Offset(r + (r * len * sin(angle)).toFloat(), r - (r * len * cos(angle)).toFloat()), w)
            hand((z.hour % 12 + z.minute / 60.0) * Math.PI / 6, .5f, 4.dp.toPx(), b.text)
            hand((z.minute + z.second / 60.0) * Math.PI / 30, .75f, 3.dp.toPx(), b.text)
            hand(z.second * Math.PI / 30, .85f, 1.5.dp.toPx(), b.accent)
        }
        T(DateTimeFormatter.ofPattern("HH:mm:ss").format(z), 22.sp, weight = FontWeight.Light, modifier = Modifier.padding(top = 6.dp))
        T(DateTimeFormatter.ofPattern("dd/MM/yyyy").format(z), 12.sp, b.dim)
    }
}

@Composable
fun NotesWidget(session: DesktopSession) {
    val st = session.app.settings
    val b = LocalBreeze.current
    var text by remember { mutableStateOf(st.string(Keys.NOTES)) }
    BasicTextField(text, { text = it; st.put(Keys.NOTES, it) }, Modifier.fillMaxWidth().heightIn(min = 120.dp),
        textStyle = TextStyle(color = b.text, fontSize = 13.sp), cursorBrush = SolidColor(b.accent),
        decorationBox = { inner -> if (text.isEmpty()) T("Escreva suas notas…", 13.sp, b.dim); inner() })
}
