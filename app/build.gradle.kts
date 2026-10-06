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
        applicationId = "com.sereneds.app"
        minSdk = AppConfig.minSdkVersion
        targetSdk = AppConfig.targetSdkVersion
        versionCode = AppConfig.versionCode
        versionName = AppConfig.versionName
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            // The LITEV optimisations (A64 JIT dispatcher/linking, fastmem tiers,
            // NEON paths) are AArch64-only and the target device is arm64, so
            // build arm64-v8a only.
            abiFilters.addAll(listOf("arm64-v8a"))
        }
        externalNativeBuild {
            cmake {
                // -fno-emulated-tls: the core is linked into a SHARED .so, where the
                // NDK's default emulated TLS turns every thread_local access (notably
                // NDS::Current in the ARM9 JIT slow-path helpers) into an
                // __emutls_get_address call. Native ELF TLS uses the cheaper
                // TLS-descriptor sequence instead. Requires Android >= 10 (native
                // TLS); the target runs Android 14.
                cppFlags("-std=c++17 -Wno-write-strings -fno-emulated-tls")
                // The debug (.dev) variant defaults to CMAKE_BUILD_TYPE=Debug (-O0),
                // which leaves the emulator core several times too slow. Build the
                // Debug-config native code at release optimisation so the APK stays
                // debuggable/installable but the core runs at speed.
                // The release variant builds CMAKE_BUILD_TYPE=RelWithDebInfo, whose
                // default is -O2; pin it (and Release) to the same -O3 so the shipped
                // APK runs the code that was measured on .dev.
                arguments(
                    "-DCMAKE_C_FLAGS_DEBUG=-O3 -DNDEBUG",
                    "-DCMAKE_CXX_FLAGS_DEBUG=-O3 -DNDEBUG",
                    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -DNDEBUG",
                    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -DNDEBUG",
                    "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG",
                    "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG"
                )
                // LITEV performance flags. Every option is declared in
                // melonDS-android-lib/CMakeLists.txt and defaults OFF there (LINK_*
                // default ON but are inert without JIT_DISPATCH); this list is the
                // shipping set. Exactness classes (see the lib's
                // docs/LITEV-OPTIMIZATIONS.md): A = guest byte- and cycle-identical,
                // B = deterministic timing relaxation, R = emulation identical,
                // only local video/audio output may differ.
                arguments(
                    // --- JIT (A64 backend; all A) ---
                    // Emitted A64 block dispatcher; static block chaining (LINK_*) needs it.
                    "-DLITEV_JIT_DISPATCH=ON",
                    // Chain unconditional / conditional / fall-through block exits directly.
                    "-DLITEV_LINK_UNCOND=ON",
                    "-DLITEV_LINK_COND=ON",
                    "-DLITEV_LINK_FALLTHROUGH=ON",
                    // DTCM LDM/STM block and MainRAM u32 load emitted inline.
                    "-DLITEV_MEM_DTCM_BLOCK=ON",
                    "-DLITEV_MEM_MAINRAM_LOAD=ON",
                    // Evaluate guest conditions on host NZCV (MSR NZCV + branch).
                    "-DLITEV_JIT_CONDFOLD=ON",
                    // Keep guest flags in host NZCV across the ALU hot path.
                    "-DLITEV_JIT_FIXEDREG=ON",
                    // Pin the hottest never-banked guest registers to host registers
                    // across block boundaries.
                    "-DLITEV_JIT_GLOBALREG=ON",
                    // Full lazy flags: NZCV in host PSTATE/memory, RCPSR freed for an
                    // extra register pin. Needs DISPATCH+CONDFOLD+FIXEDREG+GLOBALREG.
                    "-DLITEV_JIT_LAZYFLAGS=ON",
                    // Per-site 2-way guest-PC -> host-code cache in the dispatcher.
                    "-DLITEV_JIT_ICACHE=ON",
                    // Monomorphic exit sites become direct guarded branches. Needs ICACHE.
                    "-DLITEV_JIT_DIRECTPATCH=ON",
                    // simpleperf perf-<pid>.map for JIT blocks. Diagnostic; writes nothing
                    // unless debug.litev.perfmap names a directory.
                    "-DLITEV_JIT_PERFMAP=ON",
                    // LDM/STM via ldp/stp pairs + MainRAM inline block-load tier.
                    "-DLITEV_JIT_LDMSTM=ON",
                    "-DLITEV_JIT_LDM_FASTMEM=ON",
                    // Batch per-instruction cycle adds until the next cycle read (exact).
                    "-DLITEV_JIT_CYCLE_BATCH=ON",
                    // One MRS NZCV merge instead of per-flag CSET+BFI for S-ops.
                    "-DLITEV_JIT_FLAGMERGE=ON",
                    // Hoist SlowBlockTransfer9's region dispatch for whole-in-TCM blocks.
                    "-DLITEV_JIT_BLOCKXFER_FAST=ON",
                    // Map DTCM into the fastmem window (runtime prop debug.litev.dtcmfastmem).
                    "-DLITEV_MEM_DTCM_FASTMEM=ON",
                    // Offset-indexed jump table for ARM9 32-bit I/O register access.
                    "-DLITEV_IO_DISPATCH_TABLE=ON",
                    // -fno-plt (core) + -Bsymbolic-functions (this app's .so, see
                    // app/CMakeLists.txt). Host linkage only (A).
                    "-DLITEV_LINKOPT=ON",

                    // --- Scheduler / timing / audio ---
                    // Run each CPU to the true next event instead of 64-cycle slices (B).
                    "-DLITEV_EVENT_SLICES=ON",
                    // Drain DMA9/GXFIFO-stall steps in one scheduler iteration while ARM7 is halted (A).
                    "-DLITEV_SCHED_DRAIN=ON",
                    // Dispatcher looks up the last 4 code regions instead of returning to C++ (A).
                    "-DLITEV_JIT_REGION_CACHE=ON",
                    // Slice budget in a register across JIT hops; Timestamp written only where C++ reads it (A).
                    "-DLITEV_JIT_BUDGET_REG=ON", "-DLITEV_JIT_IRQMASK_CONT=ON", "-DLITEV_LAZY_SQRT=ON", "-DLITEV_GEOM_CLIP_PLANESKIP=ON", "-DLITEV_HYB_TEXSTAGE=ON", "-DLITEV_GXFIFO_READ_INLINE=ON", "-DLITEV_DMA_ARMED_MASK=ON",
                    // Linked hops: no PC store, stop check folded into the budget compare, conditions
                    // on host NZCV when it equals the slot (A). Needs BUDGET_REG and the LINK_* set.
                    "-DLITEV_JIT_EXIT_PROTO=ON",
                    // Hot C++ functions laid out together at link time (app CMakeLists.txt) (A).
                    "-DLITEV_HOT_ORDER=ON",
                    // Profile-guided optimisation from a recorded PW/Shrek profile (app CMakeLists.txt) (A).
                    "-DLITEV_PGO_USE=ON",
                    // Divider/sqrt results computed at register write; no completion event (B).
                    "-DLITEV_INSTANT_DIVSQRT=ON",
                    // Generate 8 SPU samples per scheduler event (B).
                    "-DLITEV_SPU_BATCH=ON",
                    // Skip inert 32 kHz RTC ticks while no RTC IRQ is armed (B).
                    "-DLITEV_COARSE_RTC=ON",
                    // Cache the next timer-overflow deadline (A).
                    "-DLITEV_TIMER_FAST=ON",
                    // Fast-forward register-recurrent poll loops as idle (B).
                    "-DLITEV_IDLE_AGGRESSIVE=ON",
                    // Defer DMA unit-timing lookups to the branch that uses them (A).
                    "-DLITEV_DMA_TIMING_LAZY=ON",
                    // Linear SPU sample interpolation (R: approximate audio).
                    "-DLITEV_SPU_FAST_INTERP=ON",
                    // Integer-NEON 16-channel SPU mix (A).
                    "-DLITEV_SPU_MIX_NEON=ON",

                    // --- Geometry engine (emu thread; all A) ---
                    // Batched threaded-code GXFIFO command interpreter.
                    "-DLITEV_GXFIFO_THREADED=ON",
                    // Geometry DMA writes straight into the GXFIFO.
                    "-DLITEV_DMA_GXFIFO_FAST=ON",
                    // ...with the FIFO producer inlined into the DMA loop
                    // (runtime prop debug.litev.gxinline).
                    "-DLITEV_GXFIFO_DMA_INLINE=ON",
                    // PIPE and FIFO in one ring (FIFO->PIPE moves are counter updates; exact).
                    "-DLITEV_GXFIFO_UNIFIED=ON",
                    // Integer-NEON vertex/matrix math (waves 1-3).
                    "-DLITEV_NEON_GEOMETRY=ON",
                    "-DLITEV_GEOM_NEON2=ON",
                    "-DLITEV_GEOM_NEON3=ON",
                    // Allocation-free stable radix sort for the polygon Y-sort.
                    "-DLITEV_POLY_RADIX=ON",

                    // --- Software renderer (all R unless noted) ---
                    // NEON 2D output conversion (A).
                    "-DLITEV_NEON_RENDERER=ON",
                    // Whole-frame deferred 2D raster on worker threads.
                    "-DLITEV_SOFT2D_THREADED=ON",
                    // Double-buffered 2D snapshots: emu runs a frame ahead of 2D.
                    "-DLITEV_SOFT2D_DEPTH2=ON",
                    // NEON 2D compositor / sprite reject-scan / 3D-layer compositor (A).
                    "-DLITEV_SOFT2D_NEON=ON",
                    "-DLITEV_SOFT2D_OBJNEON=ON",
                    "-DLITEV_SOFT2D_BG3DNEON=ON",
                    // 3D raster may run into the next frame (no VBlank barrier).
                    "-DLITEV_SOFT3D_ASYNC=ON",
                    // Tile-based 3D renderer + coordinator thread with double-buffered
                    // geometry (the emu no longer waits on the raster).
                    "-DLITEV_SOFT3D_DRASTIC=ON",
                    "-DLITEV_TILE_COORD=ON",
                    // Pin the render worker threads to cores {0,1,2}, off the emu core.
                    "-DLITEV_PIN_RENDER=ON",
                    // Dirty-incremental VRAM shadow snapshots instead of full memcpy.
                    "-DLITEV_SNAP_DIRTY=ON",
                    // Staged deep prefetch in BuildFrameGeom (A; runtime prop
                    // debug.litev.geoprefetch).
                    "-DLITEV_GEOM_PREFETCH2=ON",
                    // Frameskip lever: compiled in, inert at the default target 0;
                    // driven at runtime by debug.litev.frameskip (MelonInstance).
                    "-DLITEV_AGGRESSIVE_SKIP=ON",
                    // Adaptive frameskip behind the "Auto frameskip" setting (default off):
                    // holds real-time pace instead of slow motion when a scene can't hit
                    // 60 fps. UX only; keep the setting OFF when measuring performance.
                    // Ignored by lib commits that predate the option.
                    "-DLITEV_AUTO_FRAMESKIP=ON",
                    // Diagnostic render-phase profiler: names the render threads and logs a
                    // LITEV_SOFTPROF phase line every 60 frames (debug.litev.softprof=1
                    // adds a costly colour-effect census; keep it 0 when measuring).
                    "-DLITEV_SOFTPROF=ON"
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
            // ponytail: R8 full-mode minification throws a ClassCastException in the
            // Hilt/Compose startup graph at launch, so the QA release ships unminified
            // (the proven-working .dev build is also unminified). Upgrade path: re-enable
            // minify and add the missing keep rules (deobfuscate the crash with the R8
            // mapping) before any Play Store / size-sensitive release.
            isMinifyEnabled = false
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