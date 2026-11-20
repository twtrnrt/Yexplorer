// ============================================================================
// Yexplorer version numbers - the single place to edit before a release.
//
// Who uses this file:
//   * res/app.rc        - FILEVERSION / PRODUCTVERSION numbers and the
//                         "FileVersion" / "ProductVersion" strings.
//   * tools/check-version.ps1 - verifies that the built exe really carries
//                         these numbers (both the binary fixed info and the
//                         string table), so a missed edit cannot ship.
//
// Why this file exists:
//   A VERSIONINFO resource holds two independent copies of the version:
//   the numeric FILEVERSION fields (compiled into VS_FIXEDFILEINFO) and the
//   "FileVersion" string. rc.exe never checks that they agree, and Windows /
//   installers read the numeric one while Explorer's details page shows the
//   string one. This project shipped a release where FILEVERSION still said
//   1.5.0.0 while the string said 1.6.0.0, and the same thing had already
//   happened once before (v1.2.0). Keeping both in one file plus the check
//   script makes that mistake visible instead of silent.
//
// How to bump:
//   1. change YEXP_VER_MAJOR / MINOR / PATCH / BUILD below;
//   2. change YEXP_VER_STRING to "major.minor.patch.build" (rc.exe cannot
//      reliably stringify macros, so this one is written out by hand);
//   3. rebuild and run:  powershell -File tools\check-version.ps1
//      The script fails when the numbers and the string disagree, or when a
//      built exe does not match them.
//
// Keep this file ASCII-only: it is included by rc.exe, which is sensitive to
// non-ASCII input encoding.
// ============================================================================
#pragma once

#define YEXP_VER_MAJOR 1
#define YEXP_VER_MINOR 6
#define YEXP_VER_PATCH 3
#define YEXP_VER_BUILD 0

#define YEXP_VER_STRING "1.6.3.0"
