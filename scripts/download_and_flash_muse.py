import os
import sys
import subprocess
import urllib.request
from pathlib import Path
import serial.tools.list_ports

url = "https://github.com/tura96/xiaozhi-esp32/releases/download/v-muse-charm/merged-muse-charm.bin"
out_dir = Path(__file__).resolve().parent.parent / "build" / "muse-charm"
out_dir.mkdir(parents=True, exist_ok=True)
bin_file = out_dir / "merged-muse-charm.bin"

print(f"Downloading Meta Muse Charm Firmware from Release...\nURL: {url}")
try:
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as resp, open(bin_file, "wb") as f:
        f.write(resp.read())
    print(f"Downloaded successfully: {bin_file.stat().st_size} bytes")
except Exception as e:
    print(f"Download note: {e}")
    if not bin_file.exists():
        print("Binary file not found locally or online. Ensure GitHub Actions build completed.")
        sys.exit(1)

port = "COM13"
if len(sys.argv) > 1:
    port = sys.argv[1]
else:
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "usb" in desc or "ch34" in desc or "cp210" in desc or "jtag" in desc or "espressif" in desc or "usb" in hwid:
            port = p.device
            print(f"Auto-detected ESP32 serial port: {port} ({p.description})")
            break

esptool = Path(os.environ.get("USERPROFILE", "")) / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
python = sys.executable

cmd = [
    python, str(esptool),
    "-p", port,
    "-b", "460800",
    "--before", "default_reset",
    "--after", "hard_reset",
    "--chip", "esp32c3",
    "write_flash",
    "-z",
    "--flash_mode", "dio",
    "--flash_freq", "80m",
    "--flash_size", "4MB",
    "0x0", str(bin_file)
]

print(f"\n==========================================")
print(f"Flashing Meta Muse Charm Firmware to {port}...")
print(f"==========================================\n")
res = subprocess.run(cmd)
if res.returncode == 0:
    print("\n✅ NẠP FIRMWARE META MUSE CHARM THÀNH CÔNG!")
    print("👉 Hướng dẫn ghép nối:")
    print("1. Mở App Muse trên điện thoại > Settings > Devices > Bật 'Developer mode'.")
    print("2. Nhấn 'Add Device' (+) > Chọn 'MuseGadget-Charm-XXXXXX'.")
    print("3. Nhấn nút Key 1 (BOOT) trên ESP32-C3 để xác nhận ghép nối!")
sys.exit(res.returncode)
