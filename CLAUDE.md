# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Qt Widgets x64 Windows desktop app (UI text is Chinese) for **coded-target detection**: import an image, detect encoded circular fiducials, and get sub-pixel centers + fitted ellipses with quality metrics. A second tab hosts a point-cloud merging UI that lives in a separate DLL.

Not a git repository — there is no branch/commit workflow here.

## Build & run

Visual Studio 2017 toolset (`v141`), Qt MSVC2017 (`core;gui;widgets`), x64 Debug/Release. MSBuild `.vcxproj` only — no CMake, no Makefile.

```bash
# from the solution root (D:\1yjy\software\c++sourse\0807testDetect)
msbuild 0807testDetect.sln /p:Configuration=Debug /p:Platform=x64
```

Or open `0807testDetect.sln` in VS. There is **no build script, no test suite, and no linter** — verification is manual: run the exe, click 导入图片, and pick a sample image from `0807testDetect/0630/`, `0807testDetect/0805/`, `0807testDetect/08052/`, or `0807testDetect/11.bmp`.

### Two different `x64\Debug` folders — this trips people up

- `<root>\x64\Debug\` — **link output**: `0807testDetect.exe` plus the runtime DLLs it loads (`schneider0807.dll`, `MergeMarksLib.dll`). Anything the exe needs at runtime must be here.
- `<root>\0807testDetect\x64\Debug\` — **intermediates**: `.obj`, `moc/`, `uic/`, `rcc/`, `.pdb`.

### Dependencies (all machine-absolute paths, no package manager)

| Dep | How it gets in |
| --- | --- |
| Qt 5 MSVC2017 | QtMsBuild / `Qt.props` (`$(QtMsBuild)`) |
| OpenCV 4.3.0 (`opencv_world430d.lib`) | `..\..\..\propsSet\opencv.props` → `D:\1yjy\software\propsSet\opencv.props` |
| `schneider0807.dll` (TargetDetector) | `include/TargetDetector.h` + `lib/schneider0807.lib`, added via `IncludePath`/`LibraryPath`/`AdditionalDependencies` in the vcxproj |
| `MergeMarksLib.dll` (point-cloud tab) | `LoadLibrary` at runtime — see below |

`Release|x64` is now wired too (needed for the distributable package — see "Release package" below). It links `schneider0807.lib` and imports the **release** OpenCV property sheet `propsSet\opencv_r.props` (`opencv_world430.lib`), because the debug sheet `opencv.props` pulls `opencv_world430d.lib` and with it the non-redistributable debug CRT (`LNK2038 _ITERATOR_DEBUG_LEVEL` mismatch if you try).

`CodecLib.dll` / `ImageProcessingLib.dll` (with `include/Codec.h`, `include/ImageProcessing.h`) sit in the tree but are **not linked** — no source uses them yet. Wire them in only when actually needed.

## Release package (发给别人的免安装版)

`0807testDetect\build_release.ps1` assembles a self-contained folder (and `-Zip` also produces
`dist\0807testDetect.zip`, ~63 MB) that runs on a machine with **no** dev environment:

```powershell
powershell -ExecutionPolicy Bypass -File 0807testDetect\build_release.ps1 -Zip
```

It copies the Release `0807testDetect.exe` / `schneider0807.dll` / `MergeMarksLib.dll`, Qt (3 DLLs +
`platforms\qwindows.dll` + `imageformats\`), `opencv_world430.dll`, the **redistributable** VC++ runtime
(`msvcp140.dll` etc. + `vcomp140.dll`), and every PCL/VTK **release** DLL into `runtime\`. It fails loudly
on a missing source and self-checks that no `_d`/`_debug`/`MSVCP140D` file leaked in.

Traps this encodes — each one cost real debugging time, don't "simplify" them away:

- **Debug builds cannot be shipped at all.** They import `MSVCP140D.dll` / `VCRUNTIME140D.dll` /
  `ucrtbased.dll`, which Microsoft does not permit redistributing and which exist only where Visual
  Studio is installed. Debug OpenCV/PCL is also several times slower. So a shipped build *must* be
  Release, which in turn means every project's Release config has to actually work.
- **`runtime\` must be searched first, and that is why `mergeMarksRuntimeSearchPath()` exists** in
  `testDetect.cpp` — it prepends `<exe dir>\runtime` before the machine-absolute PCL/VTK paths (kept as
  a dev-machine fallback). Nothing hardcodes the package layout; change the folder name in one place.
- **The third-party debug suffixes are not uniform**: PCL `_debug`, VTK `_d`, Qhull `_d`, FLANN `-gd`.
  Filtering on `_debug` alone silently ships `qhull_d.dll` (caught by the script's self-check).
- **`MSVCR100.dll` (VS2010 CRT) must be shipped.** `pcl_io_release.dll` hard-imports `OpenNI2.dll`,
  which was built with VS2010 and needs it. It is not part of Windows nor of the VS2017 CRT, so on a
  clean machine `LoadLibrary(MergeMarksLib.dll)` fails with **Win32 error 126 (module not found)** and
  the error names neither file. Verified by walking the dependency graph.
- Qt's `platforms\qwindows.dll` is mandatory (otherwise "no Qt platform plugin could be initialized").
- `.ps1` files here **must be saved as UTF-8 with BOM**: Windows PowerShell 5.1 reads a BOM-less file as
  GBK, and the mangled Chinese comments in the script then break the *parser* (looks like a syntax error
  in an unrelated line). Also `$PSScriptRoot` is empty inside a `param()` default — compute derived
  defaults in the script body.

- Verifying a package without clicking through the GUI: `LoadLibraryW` the packaged `MergeMarksLib.dll`
  with `<pkg>;<pkg>\runtime` on `PATH`. ⚠ When declaring those P/Invokes from PowerShell, `LoadLibraryW`
  needs `CharSet=CharSet.Unicode` (**`GetProcAddress` is ANSI-only** — declaring *it* Unicode makes every
  export look missing). Getting this wrong produces a convincing but entirely fake "error 126" for every
  DLL, including ones that obviously load.

### Shipping a change to an existing customer

`0807testDetect\build_update.ps1` packs just the three artifacts a source change can affect —
`0807testDetect.exe` (244 KB), `schneider0807.dll` (125 KB), `MergeMarksLib.dll` (1.8 MB) ≈ **2 MB
total, 0.9 MB zipped** — plus a 更新说明.txt. The customer closes the app, overwrites those three files
in place, reopens it; their `settings.ini` is untouched because the script never includes it. Everything
else in the full package (Qt/OpenCV/PCL/VTK/plugins) only changes when those libraries are upgraded, and
that needs the full `build_release.ps1` instead.

⚠ **Always ship all three, never a subset.** `schneider0807.dll` and the exe are coupled through object
layout — the host holds `TargetDetector` **by value**, so both sides must agree on its `sizeof`; a header
change means both must be rebuilt and replaced together, or the host reads/writes the object at wrong
offsets (silent memory corruption, not a link error). `MergeMarksLib.dll` goes through `LoadLibrary`, so
it *could* travel alone, but bundling all three costs nothing.

The script refuses to package in two cases, and both guards are worth keeping:
- the two copies of `TargetDetector.h` differ by content hash (see the header-sync section above);
- an artifact's timestamp is older than one of its sources — i.e. you edited something and did not
  rebuild Release, which reaches the customer as "your fix does nothing". Verified working in both
  directions.

## Architecture

`main.cpp` → `testDetect` (a `QMainWindow`) → two tabs.

The UI is built **in code** in `testDetect::buildUi()`. `testDetect.ui` is a near-empty stub whose only job is to give `Ui::testDetectClass` a `centralWidget`/`statusBar`; don't expect to find layout in it.

### Tab 1 — 靶标识别 (target detection)

- Detection is done by `TargetDetector` from `schneider0807.dll`, held as a member (`m_detector`) so its undistort remap tables are computed once per image size and reused.
- ⚠ **Never** include `TargetDetector1215.h` alongside `include/TargetDetector.h`. Both define `TargetInfo` / `DetectionResult` / `TargetDetector` in the global namespace with different layouts → C2011 redefinition.
- Detection runs on a `QThread`; results return to the GUI thread via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` into `finishDetect()`. The thread handle is a `QPointer<QThread>` because the thread `deleteLater`s itself.
- **Coordinate-system invariant**: detection results (`DetectionResult::center`) are in the *undistorted* image frame, so the displayed pixmap must be `m_detector.undistortImage(m_srcMat)` — never `m_srcMat` directly, or every overlay shifts.
- `DetectionResult::subpixel` / `rms` / `nPoints` are the quality triple. `rms` is a **fit residual, not calibrated accuracy** — with a uniform point band its floor is roughly `band/√3 ≈ 0.7px`, so a large value isn't automatically bad. It's usable for *relative* comparison between runs; absolute accuracy needs a repeatability test. Non-sub-pixel results fall back to integer contour centers (~0.3px) and are drawn orange/dashed to make that visible.
- The ROI polygon is built by sorting the checked targets by polar angle around their centroid, so it can never self-intersect regardless of click order. Checkbox selection means "highlight / participate in ROI"; the polygon is only *drawn* while 画框 mode is on.

#### 识别参数（界面上直接调）

Every tuning constant of the algorithm lives in `DetectorParams` (`include/TargetDetector.h`), and the
**参数设置** pane at the bottom right of tab 1 is its UI: 32 fields in four groups (预处理 / 轮廓筛选 /
亚像素精化 / 解码与去重). Before this, every threshold was a hardcoded literal in `TargetDetector.cpp`
and tuning one meant editing source, rebuilding the DLL and restarting.
Per-parameter meaning, tuning direction and a symptom→parameter table live in `参数说明.md` at the
repo root; the in-app tooltips carry the same conclusions in condensed form.

- Flow: edit a field → 【应用参数】→ `m_detector.setParams()` **and** write `<exe dir>/settings.ini`.
  Startup reads that ini back; no ini → factory defaults. 保存参数…/载入参数… export/import the same
  format, 恢复默认 resets *and* applies + saves. Unapplied edits are deliberately not persisted.
- The pane's widget↔parameter mapping is a single table (`m_fields` in `testDetect.h`, built by
  `addIntField`/`addDoubleField`/`addBoolField`). Read/write go through a pointer-to-member, so adding
  a parameter needs **three** coordinated edits: the header field, one clamp line in
  `TargetDetector::setParams`, one `addXxxField` line in `buildParamPane`.
- `setParams` clamps every field (out-of-range entries keep the old value, no throw) and the host
  re-reads `params()` after applying so the widgets always show what is *actually* in effect.
- **Snapshot, not live read**: `detectTarget` / `detectTargets` copy the parameters once at entry and
  thread that copy through the whole call chain, so a mid-run change can't produce a half-old/half-new
  detection. `setBusy(true)` also disables the whole pane.
- Every default in `DetectorParams` **must** stay equal to the literal it replaced (`defaultParams()`);
  changing a default is a behaviour change. Verify with a before/after run on a sample image — the
  detected ID set, coordinates (3 decimals), `nPoints` and `rms` should be identical.
- ⚠ Two pairs of near-identical numbers that are **not** the same parameter, do not merge them:
  `maxRing` (4.2, contour screening) vs `codeRingOuter` (4.1, decode sampling), and
  `refineBandRatio` (0.18) vs the hardcoded radial step coefficient 0.18 inside `codeBrightAtAngle`.
- The dependent-disable greying (`updateParamDependents`) is a *hint* only: disabled fields are still
  read by `collectParams()` and passed to the DLL, the algorithm just stops using them.
- **Two-tier visibility**: default users see only the 8 keys listed in `isBasicParamKey()`; the other
  24 (all of 亚像素精化 + 解码与去重, plus the geometric ratios in 轮廓筛选) stay hidden until
  【高级参数…】 receives the owner password. `verifyAdvancedPassword()` stores only an FNV-1a 64
  hash, not the plaintext. Unlock state is deliberately **not** persisted (locked on every launch).
  Hidden widgets are still constructed and keep their values, so `collectParams()`/`saveParamsIni()`
  stay complete — this is a UI gate, not a data wall. Three bypasses are closed while locked:
  恢复默认 resets only visible keys, 载入参数… ignores advanced keys from the file, and
  【收起高级参数】 discards unapplied advanced edits. New parameters default to advanced; add the
  key to `kBasic` to expose one. See `参数说明.md` §1.5.
- **Camera calibration profiles** (【相机参数…】 in the top bar): the dialog accepts a 3×3 K (or
  `fx, fy, cx, cy`), 4/5/8/12/14 distortion coefficients, clipboard paste/copy, and saves named
  profiles to `<exe>/cameras.ini` (QSettings UTF-8 INI array). The last applied profile is reloaded at
  startup (`loadActiveCameraProfile`). Applying calls `setCameraParameters`, re-undistorts the shown
  image, clears results and forces re-detection. The factory K/D in `factoryCameraProfile()` mirror
  the DLL constructor's defaults (the DLL exposes no getters) — keep them in sync. The active profile
  cannot be deleted. **Scope: detection tab only** — it writes solely to `m_detector`; the
  `MergeMarksLib` panel is a separate DLL with its own data path and never receives K/D. The button
  lives inside `tabDetect`'s toolbar, and `onCameraParams()` also guards `m_tabs->currentIndex() == 0`.
  See `参数说明.md` §14.

#### ⚠ `TargetDetector.h` exists twice and the two copies must stay identical

`schneider0807/schneider0807/TargetDetector.h` and `0807testDetect/0807testDetect/include/TargetDetector.h`
are independent copies (`diff` must be empty). The host holds `TargetDetector` **by value** (`m_detector`),
so its `sizeof` has to match the DLL's: changing one copy means the two sides disagree about the layout
of the same object and memory gets corrupted. Any change to the exported interface must update, in one
batch: both `.h`, `schneider0807.lib`, `schneider0807.dll`. Build order is DLL first, then the host:

```bash
MSB="/d/Software/VS2017/MSBuild/15.0/Bin/MSBuild.exe"
"$MSB" schneider0807/schneider0807.sln   -p:Configuration=Debug -p:Platform=x64 -m
cp schneider0807/x64/Debug/schneider0807.dll     0807testDetect/x64/Debug/
cp schneider0807/x64/Debug/schneider0807.lib     0807testDetect/0807testDetect/lib/
cp schneider0807/schneider0807/TargetDetector.h  0807testDetect/0807testDetect/include/
"$MSB" 0807testDetect/0807testDetect.sln -p:Configuration=Debug -p:Platform=x64 -m
```

⚠ In git-bash, msbuild switches must use the dash form (`-p:Configuration=...`). The `/p:...` form gets
mangled by MSYS path conversion and fails with "只能指定一个项目".

### Tab 2 — 点云拼接 (point cloud merging)

Lazily loaded so the host links against no PCL/VTK:

1. First switch to the tab triggers `ensureMergeMarksLoaded()`.
2. It prepends the PCL/VTK runtime dirs (`kMergeMarksRuntimeDirs` in `testDetect.cpp`, currently hard-coded under `D:\1yjy\software\newcomputer\...`) to the process `PATH` — this **must** happen before `LoadLibrary`, because `MergeMarksLib.dll`'s PCL/VTK imports resolve at load time and the DLL can't do this for itself.
3. It then `LoadLibrary`s `MergeMarksLib.dll` from the exe directory and resolves the two exports.
4. Teardown: the destructor calls `MergeMarks_DestroyPanel` and **deliberately never calls `FreeLibrary`**. The panel must be destroyed first (it holds VTK objects built by PCL), but the module itself is left loaded until process exit — and `ensureMergeMarksLoaded`'s error path was changed the same way.
   - **Why:** `MergeMarksLib.dll` depends on a whole set of DLLs (`pcl_*_debug.dll`, `vtk*_d.dll`, `vtkGUISupportQt-8.1_d.dll`, …), so `FreeLibrary` drops their refcounts to zero and the loader unloads them in a cascade. VTK does not support in-process unloading: each VTK DLL's `vtkObjectFactory` is registered in a global table inside `vtkCommonCore`, and once the unload order diverges that table holds pointers into already-unmapped modules. The result is a `0xC0000005` raised **inside `vtkCommonCore-8.1_d.dll` with the reported line sitting on the `FreeLibrary` call** — which is easy to misread as `FreeLibrary` itself being wrong. Unloading at shutdown buys nothing; the process is already ending. `dumpbin -dependents` is the quick way to see the dependency set.

`MergeMarksBridge.h` is a header-only declaration of that C ABI (no PCL/VTK includes, no `.lib` to link). Its signatures must stay in sync with `MergeMarksLib/MergeMarksLib.h` in the sibling repo:

- The panel has its own **日志 (log) panel** at the bottom of `MergeMarks1215.ui` (`groupLog` → `textEdit`, a read-only `QTextEdit`, plus a 清空日志 button). All user-facing numbers — alignment accuracy, the RT matrix, the loop-closure report, the per-frame RMS summary and the consecutive-frame relative transforms — go there via `MergeMarks1215::logMsg()`, which mirrors to `qDebug()`. This matters because **this host is a Windows-subsystem app** (`<SubSystem>Windows</SubSystem>`), so there is no console: `qDebug()`/`std::cout` are invisible unless run under the VS debugger, making the on-screen log the only real sink.
  - ⚠ In `logMsg`, append via `QTextCursor::insertText`, **not** `QTextEdit::append()` — `append()` parses its argument as *rich text*, and the matrix labels contain `<-`, which it would swallow as an HTML tag.
- Sibling project: `D:\1yjy\software\c++sourse\MergeMarks1215` (`MergeMarksLib` sub-project) — that repo has its own CLAUDE.md.
  - The panel's 对齐与拼接 group offers 左侧/右侧 + 对齐 (single pair) and 顺序导入并拼接 (whole folder at once). Alongside them, the 单步拼接（逐帧手动）group does the same per-frame work one button at a time: ① 载入参考帧 → ② 载入下一帧 → ③ 对齐并拼入左侧 → (repeat ②③) → ④ 全局优化. It shares the batch pipeline's state, so only one of the two flows can be active at a time; see the sibling repo's CLAUDE.md for the code map.
- Deploy the freshly built DLL next to the exe: `powershell -File 0807testDetect\deploy_mergemarks.ps1` (copies only our ~14 MB DLL; the ~254 MB of PCL/VTK runtimes are deliberately *not* copied).
  - ⚠ The script hard-codes its source as `MergeMarks1215\x64\Debug\MergeMarksLib.dll`, which is only populated when building **through the solution** (VS sets `$(SolutionDir)`, and that directory is the default `OutDir`). Building `MergeMarksLib.vcxproj` *directly* instead puts the DLL in `MergeMarksLib\x64\Debug\`. The script does not compare timestamps — it only checks that the source file exists — so running it after a direct-project build silently deploys a **stale** DLL and any fix appears not to work. Build via `MergeMarks1215.sln` before deploying.
  - The host does not statically link this DLL (it `LoadLibrary`s it), so after swapping the DLL there is **no need to rebuild `0807testDetect`** — just restart the app.
- So: to change point-cloud behaviour, edit `MergeMarks1215`, build its `MergeMarksLib`, run the deploy script, then rebuild/run this host. Changing the module's location for a different machine means editing `kMergeMarksRuntimeDirs`.

## Theming

The whole app (both tabs) is skinned by one dark theme — cyan accent `#22d3ee` on `#12161c`. There is no per-widget styling. Three pieces, all in the host:

- `0807testDetect\theme.qss` — the stylesheet, embedded by `testDetect.qrc` as `:/testDetect/theme.qss`. ASCII-only by design (see the encoding note under the compiler rules).
- `main.cpp` → `applyDarkTechTheme()` — sets `Fusion` style, a dark `QPalette`, the base font, then loads the qss and calls `qApp->setStyleSheet()`. All of it runs before `w.show()`.
- `testDetect.cpp` — the handful of colours a stylesheet cannot reach, changed in code.

**Why one stylesheet covers both tabs:** the 点云拼接 panel comes from `MergeMarksLib.dll` *inside the same process* and shares the host's `QApplication`. `qApp->setStyleSheet()` installs a process-wide `QStyleSheetStyle` that also re-polishes widgets created later — which matters because the panel is built lazily on first tab switch.

Colours a stylesheet cannot reach, and the traps to know before editing the theme:

1. **Never set `font-family`/`font-size` on a broad selector** (`QWidget`, or `QListWidget`/`QTextEdit` directly). A stylesheet font property **outranks** `QWidget::setFont()` in code, and `buildUi()` gives the result list a Consolas font so the numeric columns line up — a stylesheet font silently destroys that alignment. The base font is set on the `QApplication` in `main.cpp` instead, and the qss deliberately sets no fonts except two per-class sizes.
2. **Some colours must be changed in code, not QSS**: `QGraphicsView::setBackgroundBrush` (an explicit brush beats the stylesheet), `QListWidgetItem::setForeground` (per-item foreground beats the row colour), everything painted by `QGraphicsScene` (marker overlays, and the ID labels' inline HTML), the parameter pane's dirty/idle hint label (`setStyleSheet` per state, since a dynamic state has no QSS selector), and the VTK viewport backgrounds in `MergeMarks1215.cpp` (`PCLVisualizer::setBackgroundColor`, VTK paints its own scene).
3. **Don't write `QScrollArea > QWidget > QWidget { background: transparent }`** to make the parameter scroll area see-through. Three type selectors out-specify the single-type `QGroupBox` card rule, so it wins and the whole pane goes flat black. Instead name the one container that needs it (`inner->setObjectName("paramPane")` + `QWidget#paramPane`) and turn the viewport transparency on in code (`m_paramScroll->viewport()->setAutoFillBackground(false)` in `buildParamPane`).

Host widgets mostly have **no `objectName`** (`buildUi()` rarely calls `setObjectName`), so the theme is written with type selectors. The exceptions exist precisely so a rule can be scoped: the section headings (`sectionTitle`, used by both 识别结果 and 参数设置) and the parameter container (`paramPane`).

Because `rcc` embeds the qss at build time, editing `theme.qss` alone may not re-trigger it — if a change doesn't show up, `touch testDetect.qrc` (or rebuild) so the resource is regenerated.

`QFileDialog` uses the Windows native dialog and is deliberately unthemed — a platform limitation, not a theme bug. The standalone `MergeMarks1215.exe` is also unthemed (it never calls `applyDarkTechTheme`); only the host applies the theme.

## Compiler / include-order rules (deliberate, don't "tidy" them)

- `testDetect.cpp` includes `<windows.h>` with `NOMINMAX` + `WIN32_LEAN_AND_MEAN` **before** `testDetect.h`. Two reasons, both documented at the top of the file: `windows.h`'s `min`/`max` macros break `std::min`/`std::max` used by OpenCV and the STL; and `TargetDetector.h` does `using namespace cv;` while OpenCV defines its own `ACCESS_MASK`, which then makes `winnt.h`'s `ACCESS_MASK` ambiguous.
- Source files carry Chinese comments in UTF-8 (some with a BOM). Preserve the encoding when editing.
- **`QStringLiteral` is the correct idiom here — deliberately the opposite of the sibling repo.** These sources are UTF-8-with-BOM but carry **no** `#pragma execution_character_set`, so MSVC encodes narrow literals in the system codepage (936); `QStringLiteral` (`u""` + widening through the same codepage) round-trips correctly, whereas a plain `QString("中文")` would decode GBK bytes as UTF-8 and garble. `MergeMarks1215.cpp` *does* have that pragma, which inverts the rule there (plain literals correct, `QStringLiteral` garbles — see its CLAUDE.md). Verified with a minimal compile probe; do not copy string idioms between the two repos.
- **`QStringLiteral` text must stay inside GBK (codepage 936).** A character outside it (e.g. U+00B2 `²`) emits C4566 and can garble at runtime; write `平方像素`/`px^2` instead. Comments are unaffected (they are not compiled into literals).
- Qt MSBuild integration means any header with `Q_OBJECT` must be listed as `<QtMoc>` in the vcxproj; `.ui`/`.qrc` need `<QtUic>`/`<QtRcc>`. New source files must be added to both the vcxproj **and** `0807testDetect.vcxproj.filters`.

## Non-obvious UI mechanics

These are engineered workarounds with comments explaining them — changing them casually will reintroduce the original bugs.

- **Whole-row checkbox toggling in the result list**: `QStyledItemDelegate::editorEvent` swallows `MouseButtonPress` when the click lands on the checkbox, so `QListWidget::itemPressed` never fires. Instead `testDetect::eventFilter` records the item and its check state on press; `onItemClicked` compares against that to tell whether Qt already toggled, and only toggles itself when it didn't.
- **`m_updatingList`** guards every programmatic list mutation to suppress `itemChanged` re-entry.
- **Labels** (`QGraphicsTextItem`) set `ItemIgnoresTransformations` so ID tags stay a fixed screen size at any zoom; entity sizes (`m_markRadius`, `m_penW`) instead scale with image resolution in `showImage()`.
- **`m_autoFit`** goes false as soon as the user zooms manually, so a window resize stops snapping back to fit. `eventFilter`'s `Resize` branch must null-check `m_pixmapItem`: `QGraphicsView::fitInView(const QGraphicsItem*, ...)` does not, and dereferences it (crash on startup).
- Zoom uses `pow(1.0015, delta)` with a clamped range so in/out are exact inverses; the wheel event is consumed so the view doesn't scroll.
- **Result rows and `m_markers` are 1:1 by index.** The item's `Qt::UserRole` stores the marker index. Both right-click delete and 【删除干扰点】 go through `removeMarkerRows()`, which removes rows **from the highest index down** and rewrites `UserRole` for the rows that shift. Any new list mutation must preserve this invariant.
- **Undo owns removed graphics.** `removeMarkerRows()` does **not** delete the scene items or the list item: it calls `scene->removeItem()` / `takeItem()` and stores them in `m_undoStack` (max 20 ops). `onUndo()` pops the last op and re-inserts rows **ascending** so the recorded indices stay valid. `clearUndoStack()` is the only owner of still-orphaned items and must run on a new detection (`clearResults()`) and in the destructor; forgetting it leaks, and deleting them on undo double-frees.
- **Export** writes the remaining `m_markers` in list order to a tab-separated txt with a UTF-8 BOM (`QIODevice::Text` gives CRLF). Columns: type, ID, X, Y, area, aspect, subpixel, RMS, nPoints; coordinates are the undistorted-image frame, 3 decimals.
- **Outline geometry** stored per result: `area` = π·(w/2)·(h/2) when `hasEllipse`, otherwise π·r²; `major` = long semi-axis; `aspect` = short/long (1 for circles). 【删除干扰点】 opens a dialog to pick one result as a template, then deletes every other result whose **area and aspect** both fall outside `template × (1 ± tolerance)` (default 50%). The preview count updates live; the template is always kept.
