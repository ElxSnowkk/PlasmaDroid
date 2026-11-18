
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories { google(); mavenCentral() }
}
rootProject.name = "KDEPlasmaAndroidDesktop"
include(":app", ":proot", ":core", ":render", ":windowmanager", ":bridge", ":display", ":sharing", ":wayland", ":x11")
