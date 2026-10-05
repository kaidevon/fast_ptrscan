# docs demo: ptrscan scan on a live Android process
# Regenerate: ./record-demo.sh

Output docs/demo.gif

# --- 1. sanity check ---
Type "adb devices"
Enter
Sleep 800ms

# --- 2. push binary and index (idempotent) ---
Type "adb push ptrscan /data/local/tmp/ >/dev/null && adb shell chmod 755 /data/local/tmp/ptrscan"
Enter
Sleep 600ms

# --- 3. run the scan ---
# -d 4   : 4 layers (keeps the demo short)
# -k 3   : per-layer cap, bounded runtime
# -O 0x1000 : 4 KB window under each target
Type "adb shell su -c '/data/local/tmp/ptrscan scan 0x6CD95918F0 -i /data/local/tmp/13674.idx -O 0x1000 -d 4 -k 0 -o /data/local/tmp/13674_hp.out'"
Enter
Sleep 12s

# --- 4. show result ---
Type "adb shell head -n 12 /data/local/tmp/13674_hp.out"
Enter
Sleep 5s