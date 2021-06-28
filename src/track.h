#ifndef TRACK_H
#define TRACK_H

#include "utils.h"

#include <vector>

namespace rideau {

struct Trigger {
  enum Type : u32 {
    Touch = 0,
    Slide,
    Hold,
    Holdlet,
    HoldEnd,
    HoldEndSlide,
    TrackGuide,
    Count,
  };

  enum Flag : u32 {
    None = 0,
    AbsoluteAngle = 1,
    CurveInward = 1 << 8,
    CurveOutward = 1 << 9,
  };

  u32 tick;
  Type type;
  s32 x;      // used in EMS only
  s32 y;      // lane [0,3] in BMS, [0,100] in FMS
  u32 angle;  // degrees
  Flag flags; // EMS only

  u32 id; // used by Editor
};

static const char *const TRIGGER_TYPE_NAMES[] = {
    "Touch",   "Slide",        "Hold",      "Holdlet",
    "HoldEnd", "HoldEndSlide", "TrackGuide"};

struct Track {
  enum Type : u32 {
    FMS = 0,
    BMS,
    EMS,
    Count,
  };

  Type trackType;
  u32 tickCount;
  u32 tickStart;
  u32 tickEnd;
  u32 featureZoneStart;
  u32 featureZoneEnd;
  u32 summonStart;
  u32 summonEnd;
  u32 summonTrigger; // has no effect?
  u32 triggerCount;

  std::vector<Trigger> triggers;

  bool isBMS() const { return trackType == BMS; }
  bool isFMS() const { return trackType == FMS; }
  bool isEMS() const { return trackType == EMS; }
};

static const char *const TRACK_TYPE_NAMES[] = {"FMS", "BMS", "EMS"};

void parseTrack(const u8 *raw, u32 rawSize, Track *track);
void checkTrack(Track &track);
usize getTrackRawSize(const Track &track);
void writeTrack(Track &track, u8 *raw, u32 rawSize);

} // namespace rideau

#endif
