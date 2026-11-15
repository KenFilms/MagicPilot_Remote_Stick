"""PlatformIO post-build step: write firmware-merged.bin, one image flashed at offset 0x0.

It combines the bootloader, partition table, boot_app0 and application with the same
flash mode, frequency and size that `pio run -t upload` uses.
"""

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

board = env.BoardConfig()
images = [(offset, path) for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
images.append(("$ESP32_APP_OFFSET", "$BUILD_DIR/${PROGNAME}.bin"))

merged = env.Command(
    "$BUILD_DIR/${PROGNAME}-merged.bin",
    [path for _, path in images],
    env.VerboseAction(" ".join([
        '"$PYTHONEXE" "$OBJCOPY"',
        "--chip", board.get("build.mcu"),
        "merge_bin", "-o", '"$TARGET"',
        "--flash_mode", "${__get_board_flash_mode(__env__)}",
        "--flash_freq", "${__get_board_f_image(__env__)}",
        "--flash_size", board.get("upload.flash_size", "keep"),
    ] + ['%s "%s"' % image for image in images]), "Merging $TARGET"),
)
env.Alias("buildprog", merged)
