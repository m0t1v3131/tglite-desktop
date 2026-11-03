#!/usr/bin/env bash
set -e

pushd `dirname $0` > /dev/null
FullScriptPath=`pwd`
popd > /dev/null

Extra=()
# Plume: сборка Mac на GitHub идёт заходами по ~5 ч, и Xcode после восстановления кэша `out/` перекомпилирует
# всё заново (сверяет у файлов не только время). Поэтому компилятор вызывается через ccache, а его кэш лежит
# в `out/` и сохраняется вместе с ней. За 15 мин до лимита шага сборка останавливается сама: компиляторы,
# оборванные лимитом, продолжают писать в `out/`, и кэш тогда не сохраняется.
if [ -n "$GITHUB_ACTIONS" ] && [ "$(uname)" = "Darwin" ]; then
  OutPath="$(cd "$FullScriptPath/.." && pwd)/out"
  CacheDir="$OutPath/ccache-dir"
  WrapDir="$OutPath/ccache-wrap"
  brew install ccache > /dev/null 2>&1 || true
  CcachePath=$(command -v ccache || echo env)
  mkdir -p "$CacheDir" "$WrapDir"
  for Tool in clang clang++; do
    RealPath=$(xcrun -f $Tool)
    cat > "$WrapDir/$Tool" <<WRAP
#!/bin/bash
export CCACHE_DIR="$CacheDir"
export CCACHE_MAXSIZE=3500M
export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime,modules,clang_index_store,system_headers
args=(); skip=; compile=
for a in "\$@"; do
  if [ -n "\$skip" ]; then skip=; continue; fi
  case "\$a" in
    -ivfsstatcache) skip=1; continue ;;
    -c|*-header) compile=1 ;;
  esac
  args+=("\$a")
done
if [ -n "\$compile" ]; then exec "$CcachePath" "$RealPath" -Xclang -fno-pch-timestamp "\${args[@]}"; fi
exec "$RealPath" "\$@"
WRAP
    chmod +x "$WrapDir/$Tool"
  done
  CCACHE_DIR="$CacheDir" "$CcachePath" -s || true
  Extra=(
    -D CMAKE_XCODE_ATTRIBUTE_COMPILER_INDEX_STORE_ENABLE=NO
    -D CMAKE_XCODE_ATTRIBUTE_CC="$WrapDir/clang"
    -D CMAKE_XCODE_ATTRIBUTE_CXX="$WrapDir/clang++"
    -D CMAKE_XCODE_ATTRIBUTE_LD="$WrapDir/clang"
    -D CMAKE_XCODE_ATTRIBUTE_LDPLUSPLUS="$WrapDir/clang++"
  )
  ( sleep 16200; pkill -9 xcodebuild; pkill -9 -f SWBBuildService; pkill -9 clang; pkill -9 ccache ) > /dev/null 2>&1 &
fi

python3 $FullScriptPath/configure.py "$@" "${Extra[@]}"

exit

