@echo off
rem Rime build script for MSYS2 mingw64 toolchain (school machine WIN11-HV1).
rem Usage: build-msys2.bat [clean] [deps] [test] [debug|release]
rem   deps   build+install the 5 vendored deps into the repo root
rem   test   build librime with BUILD_TEST=ON, then run ctest
rem Requirements:
rem   MSYS2 root with mingw64 gcc/cmake/ninja (default D:\yzd\MSYS2)
rem   mingw-w64-x86_64-boost installed via pacman
rem Differences from build.bat (MSVC):
rem   no -llibcmt, no MSVC runtime/override flags; mingw64\bin must stay on
rem   PATH when the test binary runs (DLL resolution).
setlocal

if not defined MSYS2_ROOT set MSYS2_ROOT=D:\yzd\MSYS2
if not defined RIME_ROOT set RIME_ROOT=%CD%
set MINGW_BIN=%MSYS2_ROOT%\mingw64\bin
if not defined BOOST_ROOT set BOOST_ROOT=%MSYS2_ROOT%\mingw64

if not exist "%MINGW_BIN%\gcc.exe" (
  echo ERROR: mingw64 gcc not found at %MINGW_BIN%
  exit /b 1
)
if not exist "%MINGW_BIN%\cmake.exe" (
  echo ERROR: cmake not found at %MINGW_BIN%
  exit /b 1
)
if not exist "%BOOST_ROOT%\include\boost\version.hpp" (
  echo ERROR: boost headers not found under %BOOST_ROOT%\include\boost
  exit /b 1
)

set "PATH=%MINGW_BIN%;%PATH%"
set CMAKE_GENERATOR=Ninja
set CC=%MINGW_BIN%\gcc.exe
set CXX=%MINGW_BIN%\g++.exe

set build_config=Release
set build_dir=build
set build_deps=0
set build_librime=0
set build_test=OFF
set clean=0
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="clean" set clean=1
if /i "%~1"=="deps" set build_deps=1
if /i "%~1"=="librime" set build_librime=1
if /i "%~1"=="test" (
  set build_librime=1
  set build_test=ON
)
if /i "%~1"=="debug" (
  set build_config=Debug
  set build_dir=debug
)
if /i "%~1"=="release" set build_config=Release
shift
goto parse
:parsed
if %clean%==0 if %build_librime%==0 if %build_deps%==0 set build_librime=1
if %clean%==1 rmdir /s /q %build_dir%

set common=-G Ninja -DCMAKE_BUILD_TYPE=%build_config%
set deps_flags=%common% -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=%RIME_ROOT%

if %build_deps%==1 (
  echo building glog.
  pushd deps\glog
  cmake . -B%build_dir% %deps_flags% -DBUILD_TESTING=OFF -DWITH_GFLAGS=OFF || goto error
  cmake --build %build_dir% --target install || goto error
  popd

  echo building leveldb.
  pushd deps\leveldb
  cmake . -B%build_dir% %deps_flags% -DLEVELDB_BUILD_BENCHMARKS=OFF -DLEVELDB_BUILD_TESTS=OFF || goto error
  cmake --build %build_dir% --target install || goto error
  popd

  echo building yaml-cpp.
  pushd deps\yaml-cpp
  cmake . -B%build_dir% %deps_flags% -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF || goto error
  cmake --build %build_dir% --target install || goto error
  popd

  echo building googletest.
  pushd deps\googletest
  cmake . -B%build_dir% %deps_flags% -DBUILD_GMOCK=OFF || goto error
  cmake --build %build_dir% --target install || goto error
  popd

  echo building marisa.
  pushd deps\marisa-trie
  cmake . -B%build_dir% %deps_flags% -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_TESTING=OFF -DENABLE_TOOLS=OFF || goto error
  cmake --build %build_dir% --target install || goto error
  popd
)

if %build_librime%==0 goto done

echo configuring librime.
cmake . -B%build_dir% %common% ^
  -DBUILD_STATIC=ON ^
  -DBUILD_SHARED_LIBS=ON ^
  -DBUILD_TEST=%build_test% ^
  -DENABLE_LOGGING=ON ^
  -DCMAKE_PREFIX_PATH="%RIME_ROOT%;%MSYS2_ROOT%\mingw64" ^
  -DCMAKE_INSTALL_PREFIX=%RIME_ROOT%\dist || goto error

echo building librime.
cmake --build %build_dir% --target install || goto error

if not "%build_test%"=="ON" goto done
if exist dist\bin\rime.dll copy /y dist\bin\rime.dll %build_dir%\test >nul
if exist dist\lib\rime.dll copy /y dist\lib\rime.dll %build_dir%\test >nul
rem MinGW names the shared object librime.dll
if exist dist\lib\librime.dll copy /y dist\lib\librime.dll %build_dir%\test >nul
if exist dist\bin\librime.dll copy /y dist\bin\librime.dll %build_dir%\test >nul
ctest --test-dir %build_dir%\test --output-on-failure || goto error

:done
echo.
echo ready.
exit /b 0

:error
echo.
echo BUILD FAILED (errorlevel %errorlevel%)
exit /b 1
