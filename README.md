# RoadLapse

**Speed up and stabilize your journey videos**

RoadLapse is a command-line tool that transforms long driving or cycling videos
(like dash cam footage) into smooth, stabilized time-lapses.

## Features

- **Intelligent Speedup**: Automatically creates 8x faster videos with optimal frame rates
  - 25fps input → 50fps output
  - 30fps input → 60fps output
  - Higher framerates preserved as-is
- **Stabilization**: Uses FFmpeg's vidstab filters
- **Multi-file Support**: concatenates multiple video files
- **H.265 Encoding**
- **Progress Tracking**: Real-time progress updates during processing
- **Diagnostic Tools**: Built-in video file analysis and troubleshooting

## Perfect For

- Dash cam footage time-lapses
- Long journey documentation
- Road trip videos
- Any shaky video that needs speedup + stabilization

## Dependencies

### Build Dependencies

```bash
# Ubuntu/Debian
sudo apt install build-essential libavcodec-dev libavfilter-dev libavformat-dev \
                 libavutil-dev libpostproc-dev libswresample-dev libswscale-dev

# For video stabilization support
sudo apt install libavfilter-extra
```

### Runtime Dependencies

- FFmpeg with H.265/HEVC encoder support
- libvidstab (for stabilization filters)

## Building

```bash
git clone https://github.com/automaciej/roadlapse.git
cd roadlapse
make
```

## Usage

### Basic Usage

```bash
# Single input file
./roadlapse output.mp4 input.mp4

# Multiple input files (automatically concatenated)
./roadlapse output.mp4 video1.mp4 video2.mp4 video3.mp4
```

### Diagnostic Mode

```bash
# Check if your video file is compatible
./roadlapse --diagnose input.mp4
```

### Example Output

```
RoadLapse Processing Configuration:
Output: journey.mp4
Input files (2):
  1. dashcam_part1.mp4
  2. dashcam_part2.mp4

Detected input framerate: 30.00 fps
Target output framerate: 60 fps

=== Step 1: Creating 8x speedup ===
=== Step 2: Analyzing motion for stabilization ===
=== Step 3: Applying stabilization ===

🎉 RoadLapse processing completed successfully!
```

## Testing

Run the comprehensive regression test suite:

```bash
./regtest.sh [optional_path_to_roadlapse_binary]
```

The test suite validates:
- Single file processing (25fps and 30fps)
- Multi-file concatenation
- Output duration and framerate accuracy
- 8x speedup factor verification
- File integrity and playback compatibility

## Troubleshooting

### Common Issues

**"H.265 encoder not found"**
```bash
# Install FFmpeg with H.265 support
sudo apt install ffmpeg
# Or compile FFmpeg with --enable-libx265
```

**"vidstabdetect/vidstabtransform not available"**
```bash
# Install video stabilization filters
sudo apt install libavfilter-extra
```

**"Cannot read input file"**
- Check file permissions: `chmod +r your_video.mp4`
- Verify file format: `./roadlapse --diagnose your_video.mp4`

### Getting Help

1. Run diagnostics: `./roadlapse --diagnose input.mp4`
2. Check the regression test: `./regtest.sh`
3. Look for FFmpeg/libvidstab installation issues

## License

**This project is licensed under GPL 3.0 or later.**

We deliberately chose GPL 3.0+ to ensure that RoadLapse remains free and open
source forever. This license:

## Contributing

I welcome contributions! Since this is GPL 3.0+ software:

- All contributions must be compatible with GPL 3.0+
- Contributors retain copyright but agree to GPL 3.0+ licensing
- Improvements benefit the entire community
- Corporate contributors: your contributions must remain open source

## Acknowledgments

- **FFmpeg Project**: For the incredible multimedia framework
- **libvidstab**: For the excellent video stabilization algorithms
