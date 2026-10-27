Import("env")
import os
import gzip
import shutil

_compressed = set()

def compress_file(file_path):
    if not file_path:
        return
    norm_path = os.path.normpath(file_path)
    if not os.path.isfile(norm_path):
        return
    mtime = os.path.getmtime(norm_path)
    if (norm_path, mtime) in _compressed:
        return

    gz_path = norm_path + ".gz"
    with open(norm_path, "rb") as f_in:
        with gzip.open(gz_path, "wb", compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)

    _compressed.add((norm_path, mtime))
    orig_size = os.path.getsize(norm_path)
    gz_size = os.path.getsize(gz_path)
    ratio = (1.0 - (gz_size / orig_size)) * 100.0 if orig_size > 0 else 0.0
    print(f"[GZIP] Compressed {os.path.basename(norm_path)} -> {os.path.basename(gz_path)}: {orig_size:,} bytes -> {gz_size:,} bytes ({ratio:.1f}% reduction)")

def post_action_firmware(source, target, env):
    for t in target:
        compress_file(str(t))

def post_action_filesystem(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    compress_file(os.path.join(build_dir, "littlefs.bin"))
    for t in target:
        t_str = str(t)
        if t_str.endswith(".bin"):
            compress_file(t_str)

# Hook firmware.bin generation
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", post_action_firmware)

# Hook LittleFS / filesystem image generation
env.AddPostAction("$BUILD_DIR/littlefs.bin", post_action_filesystem)
env.AddPostAction("buildfs", post_action_filesystem)

