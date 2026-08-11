# kilix-object-detect

What is in the picture — and only where something moved.

A small C11 library that turns frames into labelled boxes, and
**`kilix-look`**, the command built on it: point it at an image, a recording or
a camera and see what a detector makes of it, with the sound classifier running
alongside if you want to hear it too.

```sh
git submodule update --init --recursive
make && make test

kilix-look image photo.jpg
kilix-look watch rtsp://camera/stream --listen
kilix-look scan yesterday.mkv --fast
```

## Detection runs on motion crops

This is the whole reason the module exists, and it is Frigate's shape:

```
motion boxes  ->  kod_regions()  ->  square crops  ->  one inference each
                                                            |
                                                    boxes, in frame coordinates
```

A detector run on whole frames pays full price for every frame and sees a
person forty pixels tall as forty pixels. Run on crops around what moved, it
pays only for the crops and sees that person filling the frame it is given.
On a garden recording: **1071 frames, 46 with motion, 93 crops** — nine tenths
of the inference never happened, and what did happen was looking at something.

It also closes a hole whole-frame detection cannot. A region the operator
marked as one to ignore stops costing detector time, because a crop is never
taken from it. Ignoring what the model has already looked at saves nothing.

The crops are square, padded a fifth beyond what moved, at least the model's
input size, and shifted rather than shrunk at the frame edge. Overlapping
motion merges — including chains, where A touches B and B touches C — and the
same object seen by two crops is reported once, at the better score.

## Seeing and hearing at once

`--listen` runs [`kilix-sound-detect`](https://github.com/itsmygithubacct/kilix-sound-detect)
on the same source and draws its classes under the picture. Its own ffmpeg and
its own connection: audio-only is a fraction of the video pull, and sharing
would mean a wedged audio model stopping the picture.

A bark and a dog in the same second is a different fact from either alone.

## Anything ffmpeg can open

An image, a recording, an `rtsp://` camera. A recording is paced at its own
frame rate, so pointing this at footage behaves like pointing it at the thing
that recorded it; `--fast` reads it as quickly as it decodes, for getting
through an archive.

That is not a convenience. It is what makes the whole path testable without
hardware, and it needed a fix in
[`kilix-rtsp`](https://github.com/itsmygithubacct/kilix-rtsp), which had been
handing RTSP options to whatever it was pointed at — and ffmpeg exits rather
than ignore an option its demuxer does not have.

## Design

**The model is a subprocess.** The library links no ML runtime and knows
nothing about accelerators; it writes a square of BGRA and reads back
`float32[20][6]` — the same 480-byte contract the sound classifier uses, so one
reader handles both. Where inference happens is a launch detail:

```sh
export KILIX_OBJECT_DETECTOR="ssh gpubox kilix-look-detect"
```

**Loading a model is not wedging.** The first reply gets 90 seconds and every
later one gets five. A cold detector spends tens of seconds importing a
framework, and a uniform timeout turns that into a permanent failure that looks
exactly like "the detector never ran" — which it did, on a live run, before
this was fixed.

**An allowlist, not a threshold**, is the defence against nonsense: on still
footage the models invented toilets and aeroplanes. Filtering by name is what
makes a 0.25 confidence safe, and 0.45 demonstrably dropped real people in
infrared.

**Every coordinate transform is in one function.** A crop is scaled into the
model's square, the model answers in normalised square space, and getting back
to frame pixels means undoing the letterbox, the scale and the crop's origin.
Boxes in the wrong place look like a working program; this family has shipped
that bug once already.

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

## Getting a model

The detector is `tools/kilix-look-detect`, which wants `ultralytics` in its
Python. On a Kilix system that is one command:

```sh
kilix install yolo
```

which builds the virtualenv, fetches the weights and records where they are.
Elsewhere, any Python with ultralytics in it will do — point
`KILIX_OBJECT_DETECTOR` at it.

## Dependencies

C11 and POSIX for the library. The command adds
[`kilix-rtsp`](https://github.com/itsmygithubacct/kilix-rtsp) for decoding,
[`kilix-motion-detect`](https://github.com/itsmygithubacct/kilix-motion-detect)
for the gate and `kilix-sound-detect` for `--listen`, all vendored and pinned;
the terminal stack comes through kilix-rtsp's own closure. At runtime: the
`ffmpeg` binary and a detector command.

## License

MIT. See `LICENSE`. The weights it runs are not part of this repository.
