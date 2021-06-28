## Rideau

### What's this?

A decent viewer and janky editor of tracks used by the rhythm game Theatrhythm
Final Fantasy: Curtain Call.

It opens trigger files extracted from a dumped ROM, and lets you change the
trigger type, positions, etc.  It also lets you view and playback a track
alongside its music.

It may also work for tracks of other games in the series, but I haven't tested.

### How do I build it?

You need git, CMake, a C++ compiler and the usual toolchain.  For example:

    git submodule init
    git submodule update
    mkdir build
    cd build
    cmake -GNinja ..
    ninja

Then simply run:

    ./rideau trigger_000.bytes.lz music.dspadpcm.bcstm

### How do I edit a track?

You need a trigger file, and a music file.  So first you need to dump the 3ds
cartridge to get a ROM, then extract the contents somewhere.

If you can run the game in the 3ds emulator citra, you can dump RomFS from
there.

The game stores all tracks in the `music` folder:

    music
    ├── 0100_BMS_001
    │   ├── music.bcsar
    │   ├── music.dspadpcm.bcstm
    │   ├── trigger000.bytes.lz
    │   ├── trigger001.bytes.lz
    │   └── trigger002.bytes.lz

The three trigger file are for the three difficulties.  You can open
(compressed) lz files directly with:

  ./rideau /path/to/romfs/music/0100_BMS_001/trigger000.bytes.lz /path/to/romfs/music/0100_BMS_001/music.dspadpcm.bcstm

If you want to edit them, I suggest first making a copy of the trigger files to
somewhere you can write.  Then, change stuff, and press "Save track" or Ctrl+s.

You can load your custom tracks in citra through layered FS (look up "Citra Game
modding").  You can do the same thing and run your custom track on real 3ds
using Luma.

### How do I change the music?

Just convert you track to BCSTM format at 32000Hz sample rate, and place it in
the folder of the track you want to mod using layered FS.

Here, with ffmpeg and [openrevolution](https://github.com/ic-scm/openrevolution):

    ffmpeg -i track.mp3 -ar 32000 track.wav
    brstm-converter track.wav -o track.bcstm
    cp track.bcstm /path/to/citra/mods/romfs/music/0100_BMS_001/music.dspadpcm.bcstm

### I don't want to modify existing tracks!  Can't I add a new track instead?

Possibly maybe.  The game had DLC tracks, so it should be possible to package
custom tracks as new DLC.  It may also be possible to just add the track to the
list in `table/MusicTable.csv`, but maybe there's a hardcoded limit somewhere.  I
haven't tested either way.

### Why did you make this?

To see if I could.  Now I know.

### Editing EMS tracks doesn't work!

EMS editing needs work, indubitably.  Who likes playing EMS tracks though?

### I have an idea for a new feature.  Also, can you fix `$annoying_bug`?

The code is here.  Help yourself.

## Trigger file format reference

LZ trigger files are compressed using LZ11.  See [GBATEK](http://problemkaputt.de/gbatek.htm#lzdecompressionfunctions).

Once decompressed, the format is just a sequence of 32-bit little-endian words, with a 28h header followed by trigger data:

    Offset   Size   Role
    --------------------------------------------------------
    00h      4      Track type (0=FMS, 1=BMS, 2=EMS)
    04h      4      Tick count
    08h      4      Tick start (usually 0)
    0Ch      4      Tick end   (usually same as tick count)
    10h      4      Feature zone start
    14h      4      Feature zone end
    18h      4      Summon start
    1Ch      4      Summon end
    20h      4      Summon trigger
    24h      4      Trigger count (N)
    28h      N*4*6  Trigger data (see below)

Tick count is chosen so that the number of ticks per second of music is 59.825.
Meaning that one tick equals to one in-game frame.

Trigger data is an array of triggers, each of which has 6 u32 fields:

    Offset   Size   Role
    --------------------------------------------------------
    00h      4      Tick
    04h      4      Type (see below)
    08h      4      X position (for EMS, 0 otherwise)
    0Ch      4      Y position ([0,3] in BMS, [0,100] in FMS)
    10h      4      Angle (for slide triggers, in degrees)
    14h      4      Flags (for EMS)

    Type  Role
    -----------
    0     Touch (red trigger)
    1     Slide (yellow trigger)
    2     Hold start (green trigger)
    3     Hold marker (smaller green trigger when notes are held)
    4     Hold end
    5     Hold end with arrow
    6     Path guide (EMS only)

Path guides are not visible in-game; they are used in EMS for laying out the
triggers on arbitrary paths, using X and Y coordinates.
