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

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import org.plasma.app.session.DesktopSession
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@Composable
fun LockScreen(session: DesktopSession) {
    val now by rememberNow(1000)
    val z = Instant.ofEpochMilli(now).atZone(ZoneId.systemDefault())
    Box(Modifier.fillMaxSize().zIndex(5000f).background(Color(0xE6000000)).clickable { session.locked = false }, contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            T(DateTimeFormatter.ofPattern("HH:mm").format(z), 72.sp, Color.White, FontWeight.Light)
            T(DateTimeFormatter.ofPattern("EEEE, dd 'de' MMMM").format(z), 16.sp, Color(0xFFBBBBBB))
            T("Toque ou pressione Enter para desbloquear", 13.sp, Color(0xFF888888), modifier = Modifier.padding(top = 32.dp))
        }
    }
}
