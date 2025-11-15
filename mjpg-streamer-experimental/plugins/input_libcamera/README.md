# input_libcamera Plugin

## Overview

The `input_libcamera` plugin provides modern Raspberry Pi camera support using the libcamera framework. This is the recommended plugin for **Raspberry Pi OS Bullseye and newer**, which have deprecated the legacy camera stack.

### Differences from input_raspicam

| Feature | input_libcamera | input_raspicam |
|---------|----------------|----------------|
| Camera API | libcamera (modern) | MMAL (legacy) |
| OS Support | Bullseye+ | Buster and older |
| Raspberry Pi 5 | ✓ Supported | ✗ Not supported |
| Camera Module 3 | ✓ Supported | ✗ Not supported |
| Active Development | ✓ Yes | ✗ Deprecated |

## Requirements

- Raspberry Pi OS Bullseye or newer
- libcamera library (usually pre-installed)
- Raspberry Pi camera module (any version)

To verify libcamera is installed:
```bash
pkg-config --modversion libcamera
```

## Usage

### Basic Example

Stream at 640x480 resolution at 30 fps:

```bash
mjpg_streamer -i "input_libcamera.so" -o "output_http.so -w ./www"
```

Then open your browser to: `http://[raspberry-pi-ip]:8080`

### Custom Resolution and Frame Rate

```bash
mjpg_streamer -i "input_libcamera.so -x 1920 -y 1080 -fps 15" -o "output_http.so -w ./www"
```

### Using Multiple Cameras

If you have multiple cameras connected:

```bash
# Camera 0
mjpg_streamer -i "input_libcamera.so -camera 0" -o "output_http.so -p 8080 -w ./www"

# Camera 1 (in another terminal)
mjpg_streamer -i "input_libcamera.so -camera 1" -o "output_http.so -p 8081 -w ./www"
```

## Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `-x`, `--width` | Frame width in pixels | 640 |
| `-y`, `--height` | Frame height in pixels | 480 |
| `-fps`, `--framerate` | Frames per second | 30 |
| `-quality` | JPEG quality (0-100) | 85 |
| `-camera` | Camera device number | 0 |

## Examples

### High Quality Still Images
```bash
mjpg_streamer -i "input_libcamera.so -x 1920 -y 1080 -fps 5 -quality 95" -o "output_http.so -w ./www"
```

### Low Latency Streaming
```bash
mjpg_streamer -i "input_libcamera.so -x 640 -y 480 -fps 30 -quality 70" -o "output_http.so -w ./www"
```

### 4K Streaming (Pi 4/5)
```bash
mjpg_streamer -i "input_libcamera.so -x 3840 -y 2160 -fps 10 -quality 80" -o "output_http.so -w ./www"
```

## Troubleshooting

### "No cameras available"

1. Check if the camera is detected:
   ```bash
   libcamera-hello --list-cameras
   ```

2. Ensure the camera connector is properly seated

3. For older Raspberry Pis, enable the camera in `raspi-config`:
   ```bash
   sudo raspi-config
   # Interface Options -> Camera -> Enable
   ```

### "Failed to acquire camera"

Another application might be using the camera. Check running processes:
```bash
ps aux | grep -E 'libcamera|mjpg'
```

### Low Frame Rate

- Reduce resolution or quality
- Ensure adequate lighting (camera reduces frame rate in low light)
- Check CPU usage with `top`

### Build Issues

If the plugin didn't compile, ensure libcamera development files are installed:
```bash
sudo apt-get install libcamera-dev
```

## Technical Details

- **Output Format**: MJPEG (Motion JPEG)
- **Color Space**: YUV420 converted to JPEG
- **Buffer Management**: Zero-copy via memory mapping (mmap)
- **Thread Safety**: Protected with mutexes for multi-threaded access
- **Frame Control**: Uses libcamera's FrameDurationLimits control for precise timing

## Compatibility Matrix

| Hardware | Compatibility | Notes |
|----------|--------------|-------|
| Raspberry Pi 5 | ✓ Excellent | Full support |
| Raspberry Pi 4 | ✓ Excellent | Full support |
| Raspberry Pi 3 | ✓ Good | Works well with lower resolutions |
| Raspberry Pi Zero 2 W | ✓ Good | May need lower fps/resolution |
| Raspberry Pi Zero | ⚠ Limited | Very low performance |
| Camera Module 3 | ✓ Excellent | Full support with autofocus |
| Camera Module 2 | ✓ Excellent | Full support |
| Camera Module 1 | ✓ Good | Works well |
| HQ Camera | ✓ Excellent | Full support |

## Performance Tips

1. **Resolution vs Frame Rate**: Higher resolutions need lower frame rates
   - 4K: 10-15 fps
   - 1080p: 15-30 fps
   - 720p: 30 fps
   - 480p: 30 fps

2. **Quality Settings**:
   - 95-100: High quality, larger files, more CPU
   - 80-90: Good quality, balanced
   - 70-80: Acceptable quality, smaller files
   - Below 70: Noticeable quality loss

3. **CPU Usage**: Monitor with `top` and adjust resolution/quality/fps accordingly

## Known Limitations

- JPEG encoding is done in hardware, quality parameter may have limited effect
- Some advanced camera controls (exposure, white balance) are not yet exposed as runtime controls
- Preview output is not supported (unlike input_raspicam)

## Future Enhancements

- Runtime camera control commands (exposure, gain, white balance, etc.)
- Support for RAW format capture
- Auto exposure/white balance controls
- Region of interest (ROI) support

## License

GPL v2 - Same as mjpg-streamer

## See Also

- [libcamera documentation](https://libcamera.org/)
- [Raspberry Pi Camera documentation](https://www.raspberrypi.com/documentation/accessories/camera.html)
- [input_raspicam plugin](../input_raspicam/README.md) - For legacy Raspberry Pi OS
