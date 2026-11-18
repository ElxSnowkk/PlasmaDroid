
package org.plasma.render.gl

data class WallpaperDef(val id: String, val label: String, val style: Int, val palette: List<Int>, val motion: Float = 1f)

object Wallpapers {
    val ALL = listOf(
        WallpaperDef("next", "Next", 0, listOf(0x040A24, 0x0D2B6B, 0x1A85F7, 0x8C61FA)),
        WallpaperDef("breeze", "Breeze Dark", 1, listOf(0x171C21, 0x364859, 0x3DAEE9, 0x8099B3), 0.6f),
        WallpaperDef("aurora", "Aurora", 2, listOf(0x050D17, 0x08202B, 0x26D98C, 0x7366F2)),
        WallpaperDef("sunset", "Pôr do sol", 3, listOf(0x1F1438, 0x8C3366, 0xFA994D, 0xFFBF59), 0.5f),
        WallpaperDef("nebula", "Nebulosa", 4, listOf(0x050514, 0x291459, 0x8C2E8C, 0x338CF2), 0.7f),
        WallpaperDef("nord", "Nord", 2, listOf(0x2E3440, 0x3B4252, 0x88C0D0, 0xB48EAD)),
        WallpaperDef("mocha", "Catppuccin Mocha", 4, listOf(0x11111B, 0x1E1E2E, 0xCBA6F7, 0x89B4FA)),
        WallpaperDef("dracula", "Dracula", 4, listOf(0x14151F, 0x282A36, 0xBD93F9, 0xFF79C6)),
        WallpaperDef("gruvbox", "Gruvbox", 3, listOf(0x1D2021, 0x7C4A2D, 0xFE8019, 0xFABD2F), 0.5f),
        WallpaperDef("light", "Breeze Claro", 0, listOf(0xB9D9F0, 0x7FB7E6, 0xFFFFFF, 0x9CCBF0), 0.6f),
    )
    fun byId(id: String): WallpaperDef = ALL.firstOrNull { it.id == id } ?: ALL[0]
}
