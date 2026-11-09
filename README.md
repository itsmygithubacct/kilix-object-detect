# kilix-object-detect

What is in the picture — and only where something moved.

**Status:** installable on demand from the Kilix content catalog as
"Image Analyzer" (`kilix install kilix-object-detect`), and built automatically
the first time a user runs `kilix look`. Kilix for Plebian-OS 0.2.3 pins commit
`ee8ca067` from the `work/023-rc1-closure` branch, which has diverged from
`main` (it carries the receipt-verified sound provider and supervised detector
pipes); this README describes `main`. No weights are part of the repository;
the detector model is installed separately (`kilix install yolo`, or the YOLOX
runtime with `kilix install yolox`) and downloads on first use, except that the
Plebian-OS 0.2.3 ISO's offline default model bundle installs the `yolox_nano`
and `yolox_s` models at first boot.

A small C11 library that turns frames into labelled boxes, and
**`kilix-look`**, the command built on it: point it at an image, a recording or
a camera and see what a detector makes of it, with the sound classifier running
alongside if you want to hear it too.

```sh
git submodule update --init --recursive
make
make test

kilix-look image photo.jpg
kilix-look watch rtsp://camera/stream --listen
kilix-look scan yesterday.mkv --fast
```

## What it does

- Turns motion boxes into square crops (`kod_regions()`) and runs one
  inference per crop, so detection costs what happens rather than what the
  camera sends.
- Merges overlapping motion and reports an object seen by two crops once.
- Filters detections by an allowlist of class names, so a low threshold is safe.
- `kilix-look` shows an image, a recording or an RTSP camera with what was
  found, optionally with sound classes from kilix-sound-detect underneath.

## Commands

```sh
kilix-look image <file>        # what is in one picture
kilix-look scan <source>       # a line per frame that found something
kilix-look watch <source>      # the picture, with what was found on it
kilix-look classes             # what it will report
kilix-look --selftest
```

```
  --listen        hear as well as look        --whole-frame  skip the crops
  --hear F        sound threshold, 0.50       --regions      draw the crops
  --threshold F   detection threshold, 0.25   --render FILE  one frame, no terminal
  --size N        the model's square, 320     --decode WxH   decode size, 640x360
  --seconds N     stop after this long        --fast         read a file flat out
```

`--render` writes the first frame that found something, because a full-screen
program that can only be judged by looking at it cannot be judged on a build
server — and a picture of an empty driveway proves only that the program
starts.

Inside Kilix the same command is `kilix look` (also `kilix analyze`), for
example `kilix look image photo.jpg` or `kilix look watch <source> --listen`.

## How it works

```
source (image | recording | rtsp://)  --ffmpeg via kilix-rtsp-->  frames
   |
   v
kilix-motion-detect  ->  kod_regions(): square crops around motion
   |                        |
   |                        v  BGRA square on stdin
   |                 detector subprocess (KILIX_OBJECT_DETECTOR)
   |                        |  480-byte float32[20][6] reply
   v                        v
boxes in frame coordinates, allowlisted, de-duplicated -> picture or text
          (+ kilix-sound-detect classes with --listen)
```

The library links no ML runtime; the detector is a command that reads a square
frame and writes a fixed-size reply, so where inference runs is a launch
detail. The first reply may take 90 seconds while a framework loads; later ones
get five. Every coordinate transform back to frame pixels is in one function.
The full reasoning is in [docs/design.md](docs/design.md).

## Getting a model

The detector is `tools/kilix-look-detect`, which wants `ultralytics` in its
Python. On a Kilix system that is one command:

```sh
kilix install yolo
```

which builds the virtualenv with `uv`, fetches the weights and records where
they are. Elsewhere, any Python with ultralytics in it will do:

```sh
uv venv ~/.local/gpu_terminal/runtimes/yolo/venv
uv pip install --python ~/.local/gpu_terminal/runtimes/yolo/venv ultralytics
export KILIX_OBJECT_DETECTOR="$HOME/.local/gpu_terminal/runtimes/yolo/venv/bin/python \
                              /usr/local/bin/kilix-look-detect"
```

[kilix-yolox](https://github.com/itsmygithubacct/kilix-yolox#readme) provides an
Apache-2.0 detector for the same slot: `kilix install yolox` sets
`KILIX_OBJECT_DETECTOR` to it.

## Dependencies

C11 and POSIX for the library. The command adds the F120-staged public header
and static archive from
[`kilix-motion-detect`](https://github.com/itsmygithubacct/kilix-motion-detect#readme)
for the motion gate. Pass `F120_PREFIX=/absolute/prefix` to use an already
staged provider; without it, `make` stages the pinned submodule under
`build/f120-motion` first, so a clean checkout builds on its own. The command
also needs the vendored and pinned
[`kilix-rtsp`](https://github.com/itsmygithubacct/kilix-rtsp#readme) for decoding,
and `kilix-sound-detect` for `--listen`. The terminal stack comes through
kilix-rtsp's own closure. At runtime: the `ffmpeg` binary and a detector
command.

## Documentation

- [docs/design.md](docs/design.md): detection on motion crops, seeing and
  hearing at once, sources, and the detector-subprocess design.

## Related repositories

- [kilix-motion-detect](https://github.com/itsmygithubacct/kilix-motion-detect#readme),
  [kilix-rtsp](https://github.com/itsmygithubacct/kilix-rtsp#readme) and
  [kilix-sound-detect](https://github.com/itsmygithubacct/kilix-sound-detect#readme):
  the submodules under `third_party/`.
- [kilix-yolox](https://github.com/itsmygithubacct/kilix-yolox#readme): the
  Apache-2.0 detector subprocess.
- [kilix-nvr](https://github.com/itsmygithubacct/kilix-nvr#readme): the camera
  recorder that includes this module.
- [kilix-content](https://github.com/itsmygithubacct/kilix-content#readme): the
  catalog entry that installs it.

## Part of Plebian-OS

kilix-object-detect is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It is installed on demand from the Kilix content catalog as the Image Analyzer
app. The map of every repository and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT. See `LICENSE`. The weights it runs are not part of this repository.
