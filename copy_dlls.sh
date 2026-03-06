#!/bin/bash
# 从 D:\msys64\mingw64 复制 DLL 到 build/
# 以 dll/ 目录为参考，自动匹配新版本号
# 用法：在项目根目录执行 bash copy_dlls.sh

MSYS64_BIN="/d/msys64/mingw64/bin"
MSYS64_GST="/d/msys64/mingw64/lib/gstreamer-1.0"
DLL_REF="./dll"
BUILD_DIR="./build"

ok=0
updated=0
missed=()

# 在 msys64/bin 中查找 DLL（精确匹配，失败则按前缀模糊匹配新版本）
find_in_bin() {
    local name="$1"
    if [ -f "$MSYS64_BIN/$name" ]; then
        echo "$MSYS64_BIN/$name"
        return
    fi
    # 去掉末尾版本号后模糊匹配，例如 avcodec-61 -> avcodec-62
    local prefix
    prefix=$(echo "$name" | sed -E 's/(-[0-9]+(_[0-9]+)*)?\.dll$//')
    local found
    found=$(find "$MSYS64_BIN" -maxdepth 1 -name "${prefix}-*.dll" 2>/dev/null | sort -V | tail -1)
    echo "$found"
}

# 在 msys64 gstreamer 插件目录中查找（精确匹配）
find_in_gst() {
    local name="$1"
    [ -f "$MSYS64_GST/$name" ] && echo "$MSYS64_GST/$name"
}

mkdir -p "$BUILD_DIR/lib/gstreamer-1.0"

# ── 主目录 DLL ──────────────────────────────────────────
echo "=== 主目录 DLL ==="
for dll in "$DLL_REF"/*.dll; do
    [ -f "$dll" ] || continue
    ref_name=$(basename "$dll")
    src=$(find_in_bin "$ref_name")
    if [ -n "$src" ]; then
        dst_name=$(basename "$src")
        cp "$src" "$BUILD_DIR/$dst_name"
        if [ "$dst_name" != "$ref_name" ]; then
            echo "  更新: $ref_name -> $dst_name"
            ((updated++))
        fi
        ((ok++))
    else
        missed+=("$ref_name")
    fi
done

# ── GStreamer 插件 ───────────────────────────────────────
echo ""
echo "=== GStreamer 插件 (lib/gstreamer-1.0/) ==="
for dll in "$DLL_REF/lib/gstreamer-1.0/"*.dll; do
    [ -f "$dll" ] || continue
    ref_name=$(basename "$dll")
    src=$(find_in_gst "$ref_name")
    if [ -n "$src" ]; then
        cp "$src" "$BUILD_DIR/lib/gstreamer-1.0/$ref_name"
        ((ok++))
    else
        # 尝试模糊匹配（如 libgsty4mdec -> libgsty4m）
        prefix=$(echo "$ref_name" | sed -E 's/\.dll$//')
        fuzzy=$(find "$MSYS64_GST" -maxdepth 1 -name "${prefix}*.dll" 2>/dev/null | head -1)
        if [ -z "$fuzzy" ]; then
            # 更宽松：去掉末尾单词再匹配
            short=$(echo "$prefix" | sed -E 's/(dec|enc)$//')
            fuzzy=$(find "$MSYS64_GST" -maxdepth 1 -name "${short}*.dll" 2>/dev/null | head -1)
        fi
        if [ -n "$fuzzy" ]; then
            dst_name=$(basename "$fuzzy")
            cp "$fuzzy" "$BUILD_DIR/lib/gstreamer-1.0/$dst_name"
            echo "  更新: $ref_name -> $dst_name"
            ((updated++))
            ((ok++))
        else
            missed+=("gstreamer-1.0/$ref_name")
        fi
    fi
done

# ── 汇总 ────────────────────────────────────────────────
echo ""
echo "=========================================="
echo "成功: $ok 个  版本更新: $updated 个  缺失: ${#missed[@]} 个"
if [ ${#missed[@]} -gt 0 ]; then
    echo "以下 DLL 在 msys64/mingw64 中未找到："
    for m in "${missed[@]}"; do
        echo "  $m"
    done
fi
echo "=========================================="
