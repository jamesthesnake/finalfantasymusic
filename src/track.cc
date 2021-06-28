#include "track.h"

#include "file_utils.h"

#include <algorithm>

namespace rideau {

void parseTrack(const u8 *raw, u32 rawSize, Track *track) {
  ENSURE(raw != nullptr);
  ENSURE(track != nullptr);

  const u8 *r = raw;

  track->trackType = Track::Type(readu32le(&r));
  ASSERT(track->trackType < Track::Type::Count);
  track->tickCount = readu32le(&r);
  track->tickStart = readu32le(&r);
  track->tickEnd = readu32le(&r);
  track->featureZoneStart = readu32le(&r);
  track->featureZoneEnd = readu32le(&r);
  track->summonStart = readu32le(&r);
  track->summonEnd = readu32le(&r);
  track->summonTrigger = readu32le(&r);
  track->triggerCount = readu32le(&r);

  for (u32 i = 0; i < track->triggerCount; ++i) {
    Trigger t;
    t.tick = readu32le(&r);
    t.type = Trigger::Type(readu32le(&r));
    ASSERT(t.type < Trigger::Type::Count);
    t.x = (s32)readu32le(&r);
    t.y = (s32)readu32le(&r);
    t.angle = readu32le(&r);
    t.flags = Trigger::Flag(readu32le(&r));

    track->triggers.push_back(t);
  }

  ASSERT(track->triggerCount == track->triggers.size());

  ASSERT((r - raw) == rawSize);
}

void checkTrack(Track &track) {
  // More user-friendly would be to emit a message for each assert violation
  // pointing to the corresponding trigger instead of bailing on first fail.

  ASSERT(track.trackType < Track::Type::Count);

  ASSERT(track.tickStart == 0);
  ASSERT(track.tickEnd == track.tickCount);

  ASSERT(track.featureZoneStart < track.featureZoneEnd);
  ASSERT(track.summonStart < track.summonEnd);
  ASSERT(track.featureZoneEnd <= track.summonStart);

  if (track.isBMS()) {
    // summonTrigger actually has no effect in game
    //
    // ASSERT((track.summonStart <= track.summonTrigger) &&
    //        (track.summonTrigger <= track.summonEnd));
  } else if (track.isFMS()) {
    ASSERT(track.summonTrigger == 0);
  } else if (track.isEMS()) {
    ASSERT(track.summonTrigger == 0);
  }

  ASSERT(track.triggers.size() == track.triggerCount);

  for (u32 i = 0; i < track.triggerCount; ++i) {
    Trigger &t = track.triggers[i];
    USED_ONLY_FOR_ASSERT(t);
    ASSERT(t.type < Trigger::Type::Count);

    ASSERT(track.tickStart < t.tick && t.tick < track.tickEnd);

    if (track.isBMS()) {
      ASSERT(t.type != Trigger::TrackGuide && t.type != Trigger::Holdlet);
      ASSERT(t.x == 0);
      ASSERT(0 <= t.y && t.y < 4);
      ASSERT(t.flags == Trigger::Flag::None);
    } else if (track.isFMS()) {
      ASSERT(t.type < Trigger::TrackGuide);
      ASSERT(t.x == 0);
      ASSERT(0 <= t.y && t.y <= 100);
      ASSERT(t.flags == Trigger::Flag::None);
    } else if (track.isEMS()) {
      if (t.type == Trigger::TrackGuide) {
        ASSERT(-150 <= t.x && t.x <= 150);
        ASSERT(-75 <= t.y && t.y <= 75);
      } else {
        ASSERT(t.x == 0 && t.y == 0);
        ASSERT(t.flags == Trigger::Flag::None ||
               t.flags == Trigger::Flag::AbsoluteAngle);
      }
    }

    ASSERT(t.angle < 360);
  }
}

usize getTrackRawSize(const Track &track) {
  return 10 * sizeof(u32) + track.triggerCount * 6 * sizeof(u32);
}

void writeTrack(Track &track, u8 *raw, u32 rawSize) {
  ENSURE(raw != nullptr);
  ENSURE(rawSize >= getTrackRawSize(track));

  u8 *r = raw;

  track.tickStart = 0;
  track.tickEnd = track.tickCount;
  track.summonTrigger = track.summonEnd;

  writeu32le(&r, track.trackType);
  writeu32le(&r, track.tickCount);
  writeu32le(&r, track.tickStart);
  writeu32le(&r, track.tickEnd);
  writeu32le(&r, track.featureZoneStart);
  writeu32le(&r, track.featureZoneEnd);
  writeu32le(&r, track.summonStart);
  writeu32le(&r, track.summonEnd);
  writeu32le(&r, track.summonTrigger);
  writeu32le(&r, track.triggerCount);

  ASSERT(track.triggers.size() == track.triggerCount);

  // Keep the triggers sorted by increasing tick
  std::sort(track.triggers.begin(), track.triggers.end(),
            [&](Trigger &a, Trigger &b) { return a.tick < b.tick; });

  for (u32 i = 0; i < track.triggerCount; ++i) {
    Trigger &t = track.triggers[i];
    writeu32le(&r, t.tick);
    writeu32le(&r, t.type);
    writeu32le(&r, t.x);
    writeu32le(&r, t.y);
    writeu32le(&r, t.angle);
    writeu32le(&r, t.flags);
  }

  ASSERT((r - raw) == rawSize);
}

} // namespace rideau
