import com.android.build.gradle.internal.cxx.configure.gradleLocalProperties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.hilt.android)
    alias(libs.plugins.kotlin.parcelize)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
}

android {
    signingConfigs {
        create("release") {
            val props = gradleLocalProperties(rootDir, providers)
            (props["MELONDS_KEYSTORE"] as String?)?.let { storeFile = file(it) }
            storePassword = props["MELONDS_KEYSTORE_PASSWORD"] as String? ?: ""
            keyAlias = props["MELONDS_KEY_ALIAS"] as String? ?: ""
            keyPassword = props["MELONDS_KEY_PASSWORD"] as String? ?: ""
        }
    }

    namespace = "me.magnum.melonds"
    compileSdk = AppConfig.compileSdkVersion
    ndkVersion = AppConfig.ndkVersion
    defaultConfig {
        applicationId = "me.magnum.melonds"
        minSdk = AppConfig.minSdkVersion
        targetSdk = AppConfig.targetSdkVersion
        versionCode = AppConfig.versionCode
        versionName = AppConfig.versionName
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            // liteDS-v2-android: the LITEV performance flags (JIT dispatcher,
            // block linking, tiered memory fast paths, NEON renderer) are
            // AArch64-only, so this playable build targets arm64-v8a exclusively.
            abiFilters.addAll(listOf("arm64-v8a"))
        }
        externalNativeBuild {
            cmake {
                cppFlags("-std=c++17 -Wno-write-strings")
                // The debug variant defaults CMAKE_BUILD_TYPE=Debug (-O0), which
                // makes the emulator core ~6x too slow. Force the Debug-config
                // native flags to release-grade optimisation (-O3 -DNDEBUG) so
                // the APK stays debuggable/installable but the core runs fast.
                arguments(
                    "-DCMAKE_C_FLAGS_DEBUG=-O3 -DNDEBUG",
                    "-DCMAKE_CXX_FLAGS_DEBUG=-O3 -DNDEBUG"
                )
                // liteDS-v2 LITEV flags. Conservative first-playable set:
                // ARM7 idle detection and aggressive frameskip stay OFF.
                arguments(
                    // LITEV_JIT_DISPATCH (emitted A64 block dispatcher + block
                    // linking) triggers an on-device memory-corruption crash
                    // during NDS construction on this A55/arm64 target
                    // (SIGSEGV/SEGV_ACCERR in NDS::NDS -> FIFO::FIFO). Disabled
                    // for the first playable build pending a fix; the LINK_*
                    // flags depend on it so they are off too. All other LITEV
                    // optimisations (event slices, tiered memory fast paths,
                    // NEON 2D) remain active.
                    "-DLITEV_JIT_DISPATCH=ON",
                    "-DLITEV_LINK_UNCOND=ON",
                    "-DLITEV_LINK_COND=ON",
                    "-DLITEV_LINK_FALLTHROUGH=ON",
                    "-DLITEV_EVENT_SLICES=ON",
                    "-DLITEV_MEM_DTCM_BLOCK=ON",
                    "-DLITEV_MEM_MAINRAM_LOAD=ON",
                    // DraStic branchless software page-table fastmem (loads) +
                    // register pin. Headless A/B: ARM9 u32-read helper calls
                    // 21366->3540/frame, ARM9 exec -0.9ms, bit-exact. GLOBALREG
                    // frees the MemBase reg via the sw-table.
                    "-DLITEV_JIT_FIXEDREG=ON",
                    "-DLITEV_JIT_GLOBALREG=ON",
                    "-DLITEV_MEM_SWTABLE=ON",
                    // DraStic #3: batched GXFIFO threaded-code interpreter. Headless
                    // A/B: gpu3d dispatch -6% (281->264 ns/cmd), bit-exact.
                    "-DLITEV_GXFIFO_THREADED=ON",
                    // DraStic-model software 2D renderer: whole-frame deferred raster
                    // banded across idle A55 cores (LITEV_SOFT2D_THREADED). Active only
                    // when the Software renderer is selected (debug.litev.software=1);
                    // headless bit-exact vs inline. Removes the GL driver + VRAM->GL +
                    // geometry->GL render-prep from the emu thread when in software mode.
                    "-DLITEV_SOFT2D_THREADED=ON",
                    "-DLITEV_NEON_RENDERER=ON",
                    "-DLITEV_ARM7_IDLE=OFF",
                    // Aggressive frameskip: compiled in and runtime-controllable
                    // (default target 0 = no skip). The glue reads the frameskip
                    // target from the emulator config / a debug property and calls
                    // GPU::SetFrameskipTarget. On this A55 the frame is ARM-CPU
                    // bound (~20ms emulation of a ~30ms frame), so skipping only
                    // rasterisation caps near ~45 FPS; exposed as a user lever.
                    "-DLITEV_AGGRESSIVE_SKIP=ON",
                    // LITEV_PROFILE OFF for shipping: the core per-event ScopeTimer
                    // instrumentation costs ~7.5% of the emu thread (clock_gettime,
                    // ~1.8ms/frame, simpleperf-measured). The app's own LITEV_PROF
                    // fps log is gated on the debug.litev.prof runtime prop, not this
                    // compile flag, so it still works. Re-enable only for headless-
                    // style per-event core decomposition.
                    "-DLITEV_PROFILE=OFF",
                    // R3 LOCAL-ONLY (do not commit): enable the GL redundant
                    // bind/param shadow cache (the diet under test). Kept ON
                    // together with LITEV_PROFILE so the before/after LITEV_GL
                    // call counts are provable on-device.
                    "-DLITEV_GL_STATE_CACHE=ON",
                    // R4 LOCAL-ONLY (do not commit): render-thread offload
                    // capture/submit seam. Compiles in the deferred 2D
                    // final-composite phase; runtime toggle debug.litev.renderthread
                    // (default ON when compiled). Set OFF here (or drop the line)
                    // for the flag-OFF parity baseline APK.
                    "-DLITEV_RENDER_THREAD=ON"
                    // G) geometry-transform offload — temporarily OFF: same-thread
                    // G2 gives ~0 FPS and garbled the device render under the R4
                    // render thread (VCount-215 deferred-raster vs VBlank-flush
                    // ReplayGeometry ordering). Re-enable only after the render-
                    // private-state G3 refactor + FBHASH gate.
                    // , "-DLITEV_GEOM_OFFLOAD=ON"
                )
            }
        }
        vectorDrawables.useSupportLibrary = true
    }
    buildFeatures {
        viewBinding = true
        compose = true
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("release")
        }
        getByName("debug") {
            applicationIdSuffix = ".dev"
        }
    }

    flavorDimensions.add("version")
    flavorDimensions.add("build")
    productFlavors {
        create("playStore") {
            dimension = "version"
            versionNameSuffix = " PS"
        }
        create("gitHub") {
            dimension = "version"
            isDefault = true
            versionNameSuffix = " GH"
        }

        create("prod") {
            dimension = "build"
            isDefault = true
        }
        create("nightly") {
            dimension = "build"
            applicationIdSuffix = ".nightly"
            versionNameSuffix = " (NIGHTLY)"
        }
    }
    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }
    sourceSets {
        // Adds exported schema location as test app assets.
        getByName("androidTest").assets.directories += "$projectDir/schemas"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_21
        freeCompilerArgs.add("-opt-in=kotlin.ExperimentalUnsignedTypes")
    }

    ksp {
        arg("room.schemaLocation", "$projectDir/schemas")
    }
}

dependencies {
    val gitHubImplementation by configurations

    implementation(projects.masterswitch)
    implementation(projects.rcheevosApi)
    implementation(projects.common)

    implementation(libs.androidx.activity)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.camera2)
    implementation(libs.androidx.camera.lifecycle)
    implementation(libs.androidx.cardview)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core)
    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.fragment)
    implementation(libs.androidx.hilt.work)
    implementation(libs.androidx.lifecycle.viewmodel)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.preference)
    implementation(libs.androidx.recyclerview)
    implementation(libs.androidx.room)
    implementation(libs.androidx.room.ktx)
    implementation(libs.androidx.room.rxjava)
    implementation(libs.androidx.splashscreen)
    implementation(libs.androidx.startup)
    implementation(libs.androidx.swiperefreshlayout)
    implementation(libs.androidx.window)
    implementation(libs.androidx.work)
    implementation(libs.android.material)

    implementation(platform(libs.compose.bom))
    implementation(libs.compose.foundation)
    implementation(libs.compose.material)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons)
    implementation(libs.compose.navigation)
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.tooling.preview)

    debugImplementation(libs.compose.ui.tooling)

    implementation(libs.coil)
    implementation(libs.gson)
    implementation(libs.hilt)
    implementation(libs.kotlin.serialization)
    implementation(libs.kotlinx.coroutines.rx)
    implementation(libs.picasso)
    implementation(libs.markwon)
    implementation(libs.markwon.imagepicasso)
    implementation(libs.markwon.linkify)
    implementation(libs.commons.compress)
    implementation(libs.xz)

    gitHubImplementation(libs.retrofit)
    gitHubImplementation(libs.retrofit.converter.kotlinx)

    ksp(libs.hilt.compiler)
    ksp(libs.hilt.compiler.android)
    ksp(libs.room.compiler)

    testImplementation(libs.junit)

    androidTestImplementation(libs.androidx.room.testing)
    androidTestImplementation(libs.androidx.test.core)
    androidTestImplementation(libs.androidx.test.junit)
    androidTestImplementation(libs.androidx.test.runner)
}