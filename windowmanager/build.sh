#!/usr/bin/env bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -euo pipefail
cd "$(dirname "$0")"
command -v gradle >/dev/null || { echo "gradle nao encontrado (instale gradle 8.9+ ou gere o wrapper: gradle wrapper --gradle-version 8.9)"; exit 1; }
[ -f local.properties ] || [ -n "${ANDROID_HOME:-}${ANDROID_SDK_ROOT:-}" ] || { echo "Defina ANDROID_HOME ou crie local.properties"; exit 1; }
GR=./gradlew; [ -x "$GR" ] || GR=gradle
echo "==> testes unitarios do gerenciador de janelas (JVM)"
$GR :windowmanager:test --console=plain
echo "==> APK debug"
$GR :app:assembleDebug --console=plain
mkdir -p dist && cp app/build/outputs/apk/debug/app-debug.apk dist/PlasmaDroid-debug.apk
echo "APK: dist/PlasmaDroid-debug.apk"
