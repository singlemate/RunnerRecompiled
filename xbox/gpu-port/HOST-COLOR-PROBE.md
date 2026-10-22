# Host MSAA color до EDRAM dump — 22 октября 2026

## Уточнение архитектуры

TryResolveCopyDirectly в текущей версии SDK выполняет preflight и вызывает DumpRenderTargets; отдельные source-image direct shaders ещё не задействованы. Поэтому прежнее объяснение, что direct path полностью обходит EDRAM, было неточным. Первый EDRAM hook стоял в ветке!direct_resolved и не наблюдал вызов Dump внутри TryResolveCopyDirectly. Это не найденная ошибка resolve. Исправление описания внесено в EDRAM-SOURCE-PROBE.md.

## Владелец тайлов

prepare_owner_trace.py / ME_DIAGNOSTIC_OWNER_TRACE=1, толькоMSAA owner metadata, до96 записей. Первый общий trace исчерпал лимит на загрузочных1Xкадрах; после фильтрации получен host-owner-msaa-game.log,timeout35/exit124.

Для tiles1953/1384 владелец key00C88400:base1024,pitch16,guest2samples,host2samples,format3,VkFormat97(R16G16B16A16_SFLOAT),1280×1024. Это нативный2X: helperGetD3D10SampleIndexForGuest2xMSAA выдаёт host IDs1/0 для guest0/1. SDK умеет guest2→host4 с IDs0/3, но в этом запуске эта эмуляция не активна.

## Чтение исходных RGBA

host_msaa_probe.comp компилируется NDKglslc дляVulkan1.0,SPIR-V2020байт. prepare_host_probe.py устанавливает binary header вprivate SDK, добавляет ME_DIAGNOSTIC_HOST_COLOR=1. В DumpRenderTargets после перехода нужных исходных изображений в shader-read layout, ДО dump compute, отдельный compute читает texelFetch(texture2DMS) двух точек128,468 и640,180 по двум host IDs в64байтаSSBO. Mapping выполняется в IssueCopy послеfence и invalidate. Pipeline/layout живут до завершения probe-процесса, временные buffer/memory освобождаются. По умолчанию режим выключен.

Условия: точный ownerbase1024,pitch16,format3,guest2X,scale1,оба нужных tile покрываются rectangle. До8 снимков. Произвольные владельцы/MSAA/координаты не поддержаны. Режим не включает ME_DIAGNOSTIC_EDRAM_SOURCE и не переключает direct setting. GPU sampling и синхронизация меняют тайминги, не использовать дляFPS.

host-color-game.log:40с,timeout124. Получены32 RGBA records(8×2точки×2samples). Первый снимок нулевой в обеих точках — это не объявлено ошибкой. В кадрах182/183:

| point | guest lanes / host IDs | RGBA |
| --- | --- | --- |
| target128,468 |0/1,1/0|0,0,0,0 в обоих|
| control640,180 |0/1,1/0|7.21875,8.3125,9.5078125,4012 в кадре182;alpha4004 в183|

RGBcontrol согласуется с packed7e3(7.21875,8.3125,9.5) с округлением. Hostalpha доdump может превышать1; её нельзя напрямую сравнивать с2-bit alpha packed7e3 либо утверждать, что это ошибка. CPU float32 values и metadata:work/startup/host-color-analysis.json.

Вывод только для проверенной области/кадров: RGBtarget чёрный уже в owner host image доdump, а контроль ненулевой. Упаковка EDRAM/resolve conversion не является первым местом появления нуля в этом опыте. Нельзя на этом основании объявить весь render backend корректным или исключить другие артефакты.

Следующее: bounded observation host color после отдельных draw/clear, с привязкойVS/PS/depth/RT, чтобы установить первый проход, который оставляет/формирует ноль. Не удалять объекты или подменять RGB как исправление.

Финальная сборка без env:641self-test,exit0(host-color-self-test.log). Все фоновые пробы завершены; просмотр закрыт. Артефакт не исправлен.


## 2026-10-22: Per-draw instrumentation prepared

Added opt-in `ME_DIAGNOSTIC_HOST_AFTER_DRAW=1` capture after individual draws, bounded to 48 snapshots, with VS/PS hashes and depth-control context. Added a read-only base-cache accessor for the currently used render targets. Both affected Vulkan objects and the native executable built successfully. Device deployment was not executed: automatic approval review returned a service HTTP 403 failure. The new probe is therefore untested on device; no new graphics result or fix is established. The previous device executable remains installed.


### Device retry succeeded

Deployment succeeded on test device. `host-after-draw-game.log`: 45-second bounded run ended with expected timeout 124; 48 draw contexts and 192 RGBA records across frames 184–185. Target (128,468) was zero in both native MSAA samples in every captured draw. Control (640,180) changed among five RGBA values, including sky RGB (7.21875,8.3125,9.5078125). Contexts include city PS 89670FB6FAB9E7C6, sky PS 5EBE814012217F48 and water PS DABFFF86E69AD4B5. This does not identify a bad shader: coverage/depth rejection and zero shader output remain distinct explanations. The diagnostic adds GPU synchronization and is not a performance measurement. No production graphics fix established. Next: discriminate depth/coverage from material output at the target coordinate.


### Selective sky depth/stencil bypass

Added reproducible opt-in `prepare_depth_bypass.py`, flag `ME_DIAGNOSTIC_DEPTH_BYPASS_PS=<hex hash>`. It disables depth testing/writes and stencil only in normalized host state for the selected PS, before shader modification/pipeline selection. No guest registers are changed. Device run `sky-depth-bypass-game.log`, PS 5EBE814012217F48, ended by expected 40-second timeout 124; four activation markers, 48 contexts, 192 color records. Target point had 56 zero lane records and 40 nonzero records RGB (11.9921875,12.59375,13.171875), versus all 96 zero target records in baseline. Thus depth/stencil rejection affects this point; this does not prove a depth-conversion bug or that the silhouette is erroneous geometry. Stencil was also bypassed, so isolate depth versus stencil next, then identify the preceding depth writer. Bypass is diagnostic only, disabled by default.


### Depth writer isolated (2026-10-22)

RB_DEPTHCONTROL 00700766 has stencil_enable bit 0 clear, z_enable and z_write_enable set, zfunc=6. GetNormalizedDepthControl preserves this state. Therefore stencil was already disabled in the sky experiment.

`depth-writer-bf8-omit-game.log`: omitted VS BF8AD3FD7A465985 using existing opt-in skip flag, with all other depth tests intact. 40-second run ended 124; four omission markers, 48 contexts, 192 color records. Target: 50 zero records, 30 records RGB (11.9921875,12.59375,13.171875), 16 records RGB (1.875,3.078125,4.7460938). This identifies an influential depth-only draw, not a proven faulty draw.

Added reproducible prepare_depth_writer_dump.py. Normal-render dump run ended 124, marker present; captured private work/startup/depth-writer.txt. Draw has six indices, no PS, vf0 FMT_32_32_32_FLOAT stride3, affine transforms c5–c8 then c0–c3 into oPos. c5–c7 identity basis, c8 includes translation -960 on Y. No texture/color calculation in this shader. Next: capture indexed vertices, viewport and clip control, evaluate transformed positions and compare depth-only geometry coverage with color passes. Do not delete this draw as a fix.


### Depth geometry capture

Expanded prepare_depth_writer_dump.py with bounded vertex memory reads, DMA registers and clip/viewport state. Device depth-geometry-game.log ended 124, dump marker present. Captured fetch95 at 146CFF00: four float3 vertices, endian2, all source Z=0; X approximately 3417.775/-3748.642, Y -2528.238/4574.94. Clip control 00080000, VTE 0000043F; viewport Xscale/offset640, Yscale-360/offset360, Zscale-1/offset1. Thus depth range is reversed by the viewport, and this must be retained in comparisons.

Added analyze_depth_geometry.py to evaluate the captured VS instructions including explicit swizzles, using Python double arithmetic (not bit-exact GPU float32). Result in private work/startup/depth-geometry-analysis.json. Some clip W values are negative: raw perspective-divided coordinates cannot establish raster coverage without homogeneous clipping. DMA register address 1478FFCC did not pass the physical range validator, so actual indices were not captured; do not assume triangle ordering. Next capture via the runtime-supported DMA/index path and compare homogeneous clipping/viewport depth handling. No graphics fix applied.


### Indexed clipping baseline

Index dump fixed: previous 64-byte read extended beyond the allocation tail; reading the actual 12 bytes succeeds. depth-indices-game.log ended 124; processed type1 guestDMA, base1478FFCC, format0 int16,count6. Endian8-in16 gives indices 0,1,2,2,1,3.

Added clip_depth_geometry.py CPU homogeneous D3D clip-space reference (x/y ±w,z0..w), using captured VS ALU evaluation. For the first captured geometry, surviving screen polygons are around x581..1280,y574..604 and neither target center128.5,468.5 nor control640.5,180.5 is covered. This is a discrepancy to investigate, not proof of a driver/translator bug: geometry and color captures came from different launches/frames, CPU uses doubles and omits host shader fixups/culling/scissor/MSAA. Next collect synchronized per-draw geometry and output/host modification state before attributing coverage mismatch. Existing reverse-Z path inspected: GetHostViewportInfo called with allow_reverse_z=false; when swapping z bounds it reverses ndc scale/offset as compensation. No missing reversal proven.


### Correction: shared VS hash identifies multiple draws

Synchronized depth-sync-game.log (expected timeout124) records at least eight BF8AD3FD7A465985 calls in frame188. Original dump stored only the first call, while ME_DIAGNOSTIC_SKIP_VS removes all calls sharing the hash. Thus the earlier clipping reference does NOT demonstrate a CPU/GPU coverage discrepancy. It compares only the first geometry against an experiment affecting the whole shader group. Actual viewport diagnostic: ndc_scale(1,-1,1), offset(.00078125,.0013888889,0), z_bounds(.5,0). Reverse Vulkan depth bounds are explicitly present, requiring host shader float24 adjustments in any full reference. Expanded the dump to bounded48 separate files with snapshot/frame/index-count tags before further attribution.


### Individual snapshots confirm a scene depth prepass group

Depth-snapshots-game.log completed with expected timeout124. First calls within frame184 have index counts 6,2187,54522,48099,1482,48336,618,2313,1197,657,50556,1329. First four private snapshots pulled successfully. The shader hash is reused for substantial scene meshes, not just the initial flat quad; removing the hash disables that entire group. Counts2187 and54522 also occur in subsequent colored material draws, giving concrete candidates for depth/color geometry correspondence (counts alone do not establish identity). Next compare vertex fetch addresses, transforms, index data and draw order between depth and color passes before shader/driver changes. No clipping defect established.


### Depth/color geometry correspondence

Added prepare_geometry_pairs.py and analyze_geometry_pairs.py, opt-in ME_DIAGNOSTIC_GEOMETRY_PAIRS. First trace consumed320 entries during loading; corrected trigger to start at first BF8 draw. geometry-pairs-scene-game.log completed timeout124. Captured320 draws: full frame183 and partial frame184. All57 BF8 depth draws in complete frame183 have later colored matches on frame/count/DMA base+size+offset/position fetch95/FNV64 fingerprint of c0..c8 raw words. Frame184 has another19 matched and19 unmatched depth calls; those unmatched calls must not be interpreted as missing color passes because trace limit truncates before those passes. Matching detects descriptor/constant correspondence, not proof of identical generated clip positions, residency or shader execution. Materials include PS89670FB6FAB9E7C6,67175986A6155884,DABFFF86E69AD4B5. First six-index quad matches water PS DABFFF86E69AD4B5. Next test: selective depth bypass on primary city material while measuring target point.


### City material depth bypass

city-depth-bypass-game.log: ME_DIAGNOSTIC_DEPTH_BYPASS_PS=89670FB6FAB9E7C6 plus host-after-draw readback; expected timeout124, four activation markers,48 contexts,192 lane records. Target has80 zero and16 nonzero records: RGB1.6689453/2.6699219/3.8789062 (6),1.6123047/2.6269531/3.8691406 (5),1.875/3.078125/4.7460938 (5). Baseline target all96 records zero. Thus primary city material can shade the target when its depth test/writes are bypassed. This is consistent with depth-prepass versus color-position/precision mismatch but does not identify the exact cause; no production bypass enabled. Need compare source and translated position outputs plus depth quantization for matched geometry.


### Source position ALU comparison

city-position-shader-game.log completed expected timeout124 with material dump; pulled work/startup/city-position-vs.txt (VS22935DCEC9084FCC). Source position code differs in register allocation/swizzles from depth VS but follows equivalent multiply/add structure. Added compare_position_alu.py, evaluates source ALU through oPos with common captured position/constant inputs (snapshot2, first4 vertices). All4 produced zero absolute difference in Python double arithmetic. This is NOT a float32 GPU equivalence test, proof of SPIR-V correctness, or proof across all geometry. Next inspect translated ALU precision and post-position/float24 depth handling on matched draws. Current production path remains unchanged; all bypasses opt-in.


### Float24 fragment conversion A/B

Added prepare_float24_probe.py, opt-in ME_DIAGNOSTIC_FLOAT24 sets existing host-render-target flags round=true and convert_in_pixel_shader=true at initialization. Defaults remain unchanged. float24-fragment-game.log ended expected timeout124; activation marker present,48 contexts,192 RGBA records. Target has88 zero and8 nonzero lane records, all8 nonzero in guest lane0/host sample1. RGB(1.875,3.078125,4.7460938), alpha1442; guest lane1/host sample0 remains zero at all48 captures. Nonzero contexts are city PS89670FB6FAB9E7C6 and subsequent67175986A6155884. Baseline both lanes zero. This is a partial change with depth testing retained, not a verified visual fix. Existing sample handling inspected: color pipeline sample_rate_shading set for float24 conversion; generated depth-only shader also uses IsSampleRate via SampleID. Missing explicit pipeline flag on no-PS draws alone is not proof of a bug because shader SampleID can force sample shading. Next compare actual per-sample depth values before/after matched passes and verify image beyond one pixel.


### Native per-sample depth readback

Added prepare_host_depth_probe.py. With ME_DIAGNOSTIC_HOST_DEPTH plus HOST_AFTER_DRAW the existing compute readback selects used depth attachment0, transitions depth+stencil aspects, uses sampled-image-x2 descriptor layout, and reads the depth plane through descriptor binding0. Readback RGBA.r now means host depth, not color. Both runs below completed timeout124 with48 contexts/192 lane records and depth source markers.

Baseline target nonzero values: lane0 about.0039651133/.0039651208; lane1 about.0039618984/.003961906. Float24 target includes lane0 .003965106/.0039635412/.003965147 and lane1 .003963504/.0039635412. Zero records also occur while attachments/scene passes change, not evidence of missing texture. Captures span different launches; compare matching draw/attachment contexts, not aggregate ranges. Equal samples in portions of float24 trace motivated an opt-in explicit sample-rate test; slope-dependent baseline differences are expected MSAA behavior, not alone an error.


### Explicit sample shading for synthesized depth-only shaders

Added prepare_depth_sample_rate.py, opt-in ME_DIAGNOSTIC_DEPTH_SAMPLE_RATE: for host float24 fragment conversion, no guest PS, depth attachment kD24FS8 and enabled depth test/write, set existing pipeline description sample_rate_shading. Generated depth-only SPIR-V already references SampleID; explicit pipeline state tested because previous per-lane values suggested center-like interpolation. msaa-depth-explicit-rate-game.log completed expected timeout124, activation markers4,48 contexts. Target nonzero lane0 .0039650947/.0039651915; lane1 .003961876/.003961973. Thus explicit pipeline state materially affects depth values versus float24 alone, restoring distinct sample locations for tested cases. Different launches/cameras require contextual comparison; not a proven whole-scene fix. Color probe next, default flag remains off.


### Explicit sample shading color result: no fix

msaa-color-explicit-rate-game.log completed timeout124 with48 contexts/192 lane records. Both target lanes remain zero for all48 snapshots despite float24 conversion plus explicit sample-rate state. Therefore the depth-value change does NOT establish a rendering fix. Do not enable either diagnostic by default or present float24-only one-lane color as successful correction. Next capture incoming fragment depth and existing attachment depth for the same matched geometry/sample; stored-depth readback alone cannot determine whether test rejection is incorrect.


### Incoming depth encoded without half-float loss

Added prepare_incoming_depth.py and analyze_incoming_depth.py. Opt-in ME_DIAGNOSTIC_INCOMING_DEPTH: primary city PS89670FB6FAB9E7C6 writes gl_FragCoord.z bitcast bytes as four exact0..255 float channels after color exponent/gamma handling; enables fragment-coordinate/sample-ID interfaces and explicit sample shading. Decoding interprets RGBA byte order low-to-high as float32. Whole shader output is deliberately diagnostic; no release setting changed.

First run incoming-depth-game.log combined the earlier full depth bypass, decoded both lanes around.00787; rejected for direct comparison because disabling normalized z_enable also changes viewport float24 bounds. Updated CP diagnostic to retain z_enable=1, set zfuncAlways, z_write_enable=0 and stencil off. incoming-depth-preserved-game.log completed expected timeout124,4 mode markers,48 contexts. Byte decoding yielded3 records at target guest lane0 with exact float32 .003965220879763365; other records did not give a nonzero valid-byte result. This does not establish missing lane coverage: readback limit, later overwrites, masks and geometry context still matter.

The stored-depth baseline values around.00396511/.00396190 are from a different launch and cannot establish an incorrect comparison. Always comparison can expose legitimately occluded geometry. Need paired stored/incoming values plus stable geometry identity within the same draw, with a write-validity marker; do not label this a depth precision bug yet.


### Bounded final paired-depth experiment and branch decision

User authorized one last depth experiment, then switch to materials/textures if inconclusive. Added prepare_paired_depth_probe.py and analyze_paired_depth.py. Paired mode maintains separate persistent pipeline/layout/probe state for encoded color and depth texture; performs two readbacks after the same selected material draw, logs frame/IB/count/fetch identity. Before each selected draw clears only target/control1x1 color pixels to sentinel511RGBA; does not clear depth. Invalid/unwritten sentinel samples excluded. Incoming diagnostic keeps comparisonAlways, writes disabled, z_enable set for viewport consistency.

paired-depth-game.log completed timeout124;96 pair metadata entries (48 incoming+48 stored). Four valid target pairs, guest lanes0/1 in frames177/178, IB13EB16F2,count52422: incoming .00393678015/.00393358851/.00393686211/.00396196218, stored0 for all four. GreaterEqual would pass those values.188 point/lane cases invalid/unwritten excluded rather than treating retained colors as new depth. These pairs do not demonstrate wrong depth rejection; Always/no-write diagnostic may change attachment usage and reveal hidden geometry. No production depth change justified. Stop depth investigation here per agreed bound; preserve disabled probes for future evidence only.

Next branch: city PS89670FB6FAB9E7C6 samples three lightmap textures (tf0/1/2), applies signed decoding/powers and lighting constants before composition. Existing material PS pulled to private work/startup/city-material-ps.txt. Plan inspect actual sampled lightmap values and ALU output validity with original depth state, rather than more depth switches.


### Reliability correction: paired stored-depth values withdrawn

Subsequent lightmap work found diagnostic submissions not reopened after AwaitAllQueueOperationsCompletion. Paired-depth second phase also lacks BeginSubmission. Therefore the earlier paired stored=0 results must not be used to infer GreaterEqual success or validate actual saved depth. No depth bug or fix has been established. Probe remains disabled by default; do not reuse without lifecycle repair and validity test. Depth investigation stays stopped per user-agreed bound.
