#include <glad/glad.h>

#include <GLFW/glfw3.h>

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"

#include "brstm.h"
#include <soundio/soundio.h>

#include "lz11.h"
#include "track.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#include "utils.h"
#include "version.h"

// TODO: copy & paste?
// TODO: drag&drop?

// TODO: EMS auto angles
// TODO: EMS feature zone
// TODO: EMS curves (still messy)
// TODO: FMS auto curves

namespace rideau {

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static u32 g_idCounter = 1; // 0 is not assigned
u32 genId() { return g_idCounter++; }

void parseTrackFile(const char *filename, Track *track) {
  ENSURE(filename != nullptr);

  FILE *f = fopen(filename, "rb");
  ENSURE(f != nullptr);

  usize rawSize = getLZ11RawSize(f);
  ENSURE(rawSize > 0);
  u8 *raw = (u8 *)malloc(rawSize);
  ENSURE(raw != nullptr);
  decompressLZ11(f, raw, rawSize);

  int ret = fclose(f);
  ENSURE(ret == 0);

  parseTrack(raw, rawSize, track);

  for (u32 i = 0; i < track->triggerCount; ++i)
    track->triggers[i].id = genId();

  free(raw);
}

void printTrackStats(Track &track) {
  printf("%s\n%d ticks\n%d--%d feature zone\n%d--%d summon\n",
         TRACK_TYPE_NAMES[track.trackType], track.tickCount,
         track.featureZoneStart, track.featureZoneEnd, track.summonStart,
         track.summonEnd);

  u32 triggerTypeCount[Trigger::Type::Count];

  for (u32 i = 0; i < Trigger::Type::Count; ++i)
    triggerTypeCount[i] = 0;

  for (u32 i = 0; i < track.triggerCount; ++i) {
    triggerTypeCount[track.triggers[i].type]++;
  }

  printf("%d triggers\n", track.triggerCount);
  for (u32 i = 0; i < Trigger::Type::Count; ++i) {
    printf("  %3d %s\n", triggerTypeCount[i], TRIGGER_TYPE_NAMES[i]);
  }
}

void writeTrackFile(Track &track, const char *filename) {
  FILE *dst = fopen(filename, "wb");
  ENSURE(dst != nullptr);

  u32 rawSize = getTrackRawSize(track);
  u8 *raw = (u8 *)malloc(rawSize);
  ENSURE(raw != nullptr);
  writeTrack(track, raw, rawSize);

  compressLZ11(raw, rawSize, dst);

  free(raw);

  int ret = fclose(dst);
  ENSURE(ret == 0);
}

void resampleCubic(const s16 *samples, size_t sampleCount,
                   float *resampledBuffer, u32 inputFreq, u32 outputFreq) {
  const double ratio = (double)inputFreq / outputFreq;
  double mu = 0.0;
  float s[4] = {0, 0, 0, 0};

  for (size_t i = 0; i < sampleCount; ++i) {
    float sample = std::clamp((float)samples[i] / 32768.0f, -1.0f, 1.0f);

    s[0] = s[1];
    s[1] = s[2];
    s[2] = s[3];
    s[3] = sample;

    while (mu <= 1.0) {
      double A = s[3] - s[2] - s[0] + s[1];
      double B = s[0] - s[1] - A;
      double C = s[2] - s[0];
      double D = s[1];

      *resampledBuffer++ =
          std::clamp(A * mu * mu * mu + B * mu * mu + C * mu + D, -1.0, 1.0);
      mu += ratio;
    }

    mu -= 1.0;
  }
}

struct Editor {
  std::atomic<bool> isAudioPlaying;
  std::atomic<usize> currentFrame;
  std::atomic<float> audioVolume;

  std::vector<u32> selectedTriggers;
  bool isSeeking;
  bool shouldSortTriggers;
  bool trackModified;

  float estimatedCurrentFrame;
  float audioLatency;

  u32 sampleRate;
  float *samples[2];
  usize framesCount;

  GLuint waveformTexture;

  void init(Brstm *brstm) {
    selectedTriggers.clear();
    isSeeking = false;
    shouldSortTriggers = false;
    isAudioPlaying = false;
    audioVolume = 0.5f;
    currentFrame = 0;
    estimatedCurrentFrame = 0;
    trackModified = false;

    // Resample to 48000Hz float for greater backend compatibility (JACK at
    // least doesn't want anything else)
    sampleRate = 48000;
    framesCount = brstm->total_samples * sampleRate / brstm->sample_rate;
    ENSURE(brstm->num_channels == 2);
    for (int c = 0; c < 2; ++c) {
      samples[c] = (float *)malloc(framesCount * sizeof(float));
      ENSURE(samples[c] != nullptr);
      resampleCubic(brstm->PCM_samples[c], brstm->total_samples, samples[c],
                    brstm->sample_rate, sampleRate);
    }
  }

  bool isTriggerSelected(u32 triggerId) const {
    return std::find(selectedTriggers.begin(), selectedTriggers.end(),
                     triggerId) != selectedTriggers.end();
  }

  void selectTrigger(u32 triggerId, bool append) {
    if (!append)
      selectedTriggers.clear();
    selectedTriggers.push_back(triggerId);
  }

  void unselectTrigger(u32 triggerId) {
    auto it =
        std::find(selectedTriggers.begin(), selectedTriggers.end(), triggerId);
    if (it != selectedTriggers.end())
      selectedTriggers.erase(it);
  }

  void unselectAllTriggers() { selectedTriggers.clear(); }

  void initWaveformTexture() {
    const u32 texWidth = 32;
    const u32 texHeight = 8192;
    u8 *texData = (u8 *)malloc(texWidth * texHeight * 3);
    ENSURE(texData != nullptr);
    u8 *pData = texData;

    {
      ASSERT(texWidth < framesCount);
      const double texelPerFrame = (double)texHeight / framesCount;
      double t = 0;
      size_t frameIdx = 0;

      const u8 bgColor[3] = {44, 51, 56};
      const u8 waveformColor[3] = {77, 124, 160};

      for (u32 y = 0; y < texHeight; ++y) {
        float minSample = +1.0f;
        float maxSample = -1.0f;
        while (t < 1.0) {
          if (frameIdx < framesCount) {
            const float left = samples[0][frameIdx];
            const float right = samples[1][frameIdx++];
            const float sample = std::clamp((left + right) / 2.0f, -1.0f, 1.0f);
            minSample = std::min(minSample, sample);
            maxSample = std::max(maxSample, sample);
          } else {
            minSample = maxSample = 0;
            break;
          }
          t += texelPerFrame;
        }
        t -= 1.0;

        u32 lineStart = (minSample + 1.0f) / 2.0f * texWidth;
        u32 lineEnd = (maxSample + 1.0f) / 2.0f * texWidth;

        u32 x = 0;
        while (x < lineStart) {
          *pData++ = bgColor[0];
          *pData++ = bgColor[1];
          *pData++ = bgColor[2];
          ++x;
        }
        while (x < lineEnd) {
          *pData++ = waveformColor[0];
          *pData++ = waveformColor[1];
          *pData++ = waveformColor[2];
          ++x;
        }
        while (x < texWidth) {
          *pData++ = bgColor[0];
          *pData++ = bgColor[1];
          *pData++ = bgColor[2];
          ++x;
        }
      }
    }

    glGenTextures(1, &waveformTexture);
    glBindTexture(GL_TEXTURE_2D, waveformTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texWidth, texHeight, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, texData);

    free(texData);
  }

  void deinit() {
    for (int i = 0; i < 2; ++i) {
      free(samples[i]);
      samples[i] = nullptr;
    }

    glDeleteTextures(1, &waveformTexture);
  }

  void seekTo(usize frame) {
    currentFrame = frame;
    estimatedCurrentFrame = frame - audioLatency * sampleRate;
  }

  void togglePause() {
    isAudioPlaying = !isAudioPlaying;
    if (isAudioPlaying && currentFrame >= framesCount)
      seekTo(0);
  }
};

u8 parseBRSTM(const char *filename, Brstm *brstm) {
  ENSURE(filename != nullptr);
  ENSURE(brstm != nullptr);

  brstm_init(brstm);

  u8 *raw;
  {
    FILE *f = fopen(filename, "rb");
    ENSURE(f != nullptr);

    int ret = fseek(f, 0, SEEK_END);
    ENSURE(ret == 0);
    long fileSize = ftell(f);
    ENSURE(fileSize > 0);

    raw = (u8 *)malloc(fileSize);
    ENSURE(raw != nullptr);

    ret = fseek(f, 0, SEEK_SET);
    ENSURE(ret == 0);
    size_t readSize = fread(raw, sizeof(u8), fileSize, f);
    ENSURE(readSize == (size_t)fileSize);

    ret = fclose(f);
    ENSURE(ret == 0);
  }

  u8 ret = brstm_read(brstm, raw, 0, 1);
  free(raw);

  return ret;
}

void audioCallback(struct SoundIoOutStream *outstream, int frame_count_min,
                   int frame_count_max) {
  UNUSED(frame_count_min);

  const struct SoundIoChannelLayout *layout = &outstream->layout;
  Editor *editor = (Editor *)outstream->userdata;
  const bool isPlaying = editor->isAudioPlaying;
  struct SoundIoChannelArea *areas;
  int frames_left = frame_count_max;
  int err;

  const usize framesCount = editor->framesCount;
  usize currentFrame = editor->currentFrame;
  float **inputSamples = editor->samples;
  const float volume = editor->audioVolume;

  while (frames_left > 0 && currentFrame < framesCount) {
    int frame_count = frames_left;

    err = soundio_outstream_begin_write(outstream, &areas, &frame_count);
    ENSURE(err == 0);

    if (!frame_count)
      break;

    for (int frame = 0; frame < frame_count; ++frame) {
      for (int channel = 0; channel < layout->channel_count; ++channel) {
        float sample;
        if (isPlaying) {
          sample = inputSamples[channel][currentFrame];
          sample *= volume;
        } else {
          sample = 0;
        }
        float *ptr =
            (float *)(areas[channel].ptr + areas[channel].step * frame);
        *ptr = sample;
      }
      if (isPlaying) {
        currentFrame++;
        if (currentFrame == framesCount) {
          editor->isAudioPlaying = false;
        }
      }
    }

    err = soundio_outstream_end_write(outstream);
    ENSURE(err == 0);

    frames_left -= frame_count;
  }

  editor->currentFrame = currentFrame;
}

struct AudioStuff {
  struct SoundIo *soundio;
  struct SoundIoDevice *device;
  struct SoundIoOutStream *outstream;
};

int initAudio(int sampleRate, Editor *editor, AudioStuff &audio) {
  ENSURE(editor != nullptr);

  int err;
  struct SoundIo *soundio = soundio_create();
  if (!soundio) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  if ((err = soundio_connect(soundio))) {
    fprintf(stderr, "error connecting: %s", soundio_strerror(err));
    return 1;
  }

  soundio_flush_events(soundio);

  int default_out_device_index = soundio_default_output_device_index(soundio);
  if (default_out_device_index < 0) {
    fprintf(stderr, "no output device found");
    return 1;
  }

  struct SoundIoDevice *device =
      soundio_get_output_device(soundio, default_out_device_index);
  if (!device) {
    fprintf(stderr, "out of memory");
    return 1;
  }

  struct SoundIoOutStream *outstream = soundio_outstream_create(device);
  outstream->sample_rate = sampleRate;
  outstream->format = SoundIoFormatFloat32LE;
  outstream->software_latency = 0.100;
  outstream->write_callback = audioCallback;
  outstream->userdata = editor;

  ENSURE(soundio_device_supports_format(device, outstream->format));
  ENSURE(soundio_device_supports_sample_rate(device, outstream->sample_rate));

  if ((err = soundio_outstream_open(outstream))) {
    fprintf(stderr, "unable to open device: %s", soundio_strerror(err));
    return 1;
  }

  editor->audioLatency = outstream->software_latency;

  if (outstream->layout_error)
    fprintf(stderr, "unable to set channel layout: %s\n",
            soundio_strerror(outstream->layout_error));

  if ((err = soundio_outstream_start(outstream))) {
    fprintf(stderr, "unable to start device: %s", soundio_strerror(err));
    return 1;
  }

  audio.outstream = outstream;
  audio.device = device;
  audio.soundio = soundio;

  return 0;
}

void deinitAudio(AudioStuff &audio) {
  soundio_outstream_destroy(audio.outstream);
  soundio_device_unref(audio.device);
  soundio_destroy(audio.soundio);
}

enum KeyState {
  DOWN,
  UP,
  PRESSED,
  RELEASED,
};

static_assert(512 > GLFW_KEY_LAST);
bool g_previousKeys[512];
bool g_currentKeys[512];

static void clearKeys() {
  memset(g_previousKeys, true, ARRAY_SIZE(g_previousKeys) * sizeof(bool));
  memset(g_currentKeys, true, ARRAY_SIZE(g_currentKeys) * sizeof(bool));
}

static void updateKeys(GLFWwindow *window) {
  const int usefulKeys[] = {GLFW_KEY_ESCAPE, GLFW_KEY_SPACE,
                            GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_CONTROL,
                            GLFW_KEY_S};

  for (size_t i = 0; i < ARRAY_SIZE(usefulKeys); ++i) {
    int key = usefulKeys[i];
    g_previousKeys[key] = g_currentKeys[key];
    g_currentKeys[key] = glfwGetKey(window, key) == GLFW_RELEASE;
  }
}

static KeyState getKey(int key) {
  ASSERT((usize)key < ARRAY_SIZE(g_currentKeys));

  const bool isUp = g_currentKeys[key];

  if (g_previousKeys[key] == g_currentKeys[key]) {
    if (isUp)
      return UP;
    else
      return DOWN;
  } else {
    if (isUp)
      return RELEASED;
    else
      return PRESSED;
  }
}

void drawTrack(Track &track, Editor &editor) {

  const float triggerRadius = 10.0f;
  const float scaleX = 4.0f;
  const ImColor touchTriggerColor(0.8f, 0.2f, 0.2f);
  const ImColor touchTriggerColorTransparent(0.8f, 0.2f, 0.2f, 0.5f);
  const ImColor slideTriggerColor(0.8f, 0.8f, 0.2f);
  const ImColor slideTriggerArrowColor(1.0f, 1.0f, 1.0f);
  const ImColor holdTriggerColor(0.2f, 0.8f, 0.2f);
  const ImColor holdLineColor(0.15f, 0.5f, 0.15f);
  const ImColor featureZoneColor(0.2f, 0.3f, 0.7f, 0.2f);
  const ImColor summonColor(0.7f, 0.6f, 0.2f, 0.2f);
  const ImColor trackGuideColor(0.9f, 0.9f, 0.9f);
  const ImColor currentlyPlayingColor(1.0f, 1.0f, 1.0f);
  const ImColor unknownColor(0.8f, 0.2f, 0.8f);

  // Audio scrub zone
  {
    const int windowHeight = 32;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.17f, 0.20f, 0.22f, 1.0f));
    ImGui::BeginChild("Audio Scrub", ImVec2(0, windowHeight), false);
    ImGui::PopStyleColor();

    const ImColor cursorColor = ImColor(1.0f, 1.0f, 1.0f);

    ImVec2 orig = ImGui::GetCurrentWindow()->DC.CursorPos;
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const float windowWidth = ImGui::GetWindowWidth();

    // Waveform (from a texture, 'cause it's expensive to draw at runtime)
    drawList->AddImageQuad((ImTextureID)(intptr_t)editor.waveformTexture,
                           orig + ImVec2(windowWidth, windowHeight),
                           orig + ImVec2(windowWidth, 0), orig,
                           orig + ImVec2(0, windowHeight), ImVec2(1, 0),
                           ImVec2(0, 0), ImVec2(0, 1), ImVec2(1, 1));

    // Feature zones
    const float pixelsPerTick = (float)windowWidth / track.tickCount;
    drawList->AddRectFilled(
        orig + ImVec2(windowWidth - track.featureZoneStart * pixelsPerTick, 0),
        orig + ImVec2(windowWidth - track.featureZoneEnd * pixelsPerTick,
                      windowHeight),
        featureZoneColor);

    // Summon
    drawList->AddRectFilled(
        orig + ImVec2(windowWidth - track.summonStart * pixelsPerTick, 0),
        orig +
            ImVec2(windowWidth - track.summonEnd * pixelsPerTick, windowHeight),
        summonColor);

    // Cursor
    {
      const float scrubPos =
          (1.0f - ((float)editor.estimatedCurrentFrame / editor.framesCount)) *
          windowWidth;
      const int x = roundf(scrubPos);

      drawList->AddLine(orig + ImVec2(x, 0), orig + ImVec2(x, windowHeight),
                        cursorColor);
    }

    // Click to seek
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
      ImVec2 mouseRelPos = ImGui::GetMousePos() - orig;
      const usize seekFrame =
          (1.0f - (mouseRelPos.x / windowWidth)) * editor.framesCount;
      editor.seekTo(seekFrame);
      editor.isSeeking = true;
    }

    ImGui::EndChild();
  }

  const float ticksPerFrame = (float)track.tickCount / editor.framesCount;
  const float currentTick = editor.estimatedCurrentFrame * ticksPerFrame;

  if (track.isEMS()) {
    static float scale = 0.5f;
    ImGui::SliderFloat("Scale", &scale, -2.0f, 2.0f);

    static bool hideTriggers = false;
    ImGui::Checkbox("Hide triggers", &hideTriggers);

    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 30.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.17f, 0.20f, 0.22f, 1.0f));
    const int lastTriggerTick = track.triggers[track.triggerCount - 1].tick;
    const int contentPad = 2000;
    const int contentWidth = lastTriggerTick + contentPad;
    ImGui::SetNextWindowContentSize(ImVec2(contentWidth, 0));

    ImGui::BeginChild("Track", ImVec2(400, 200), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImVec2 orig = window->DC.CursorPos;

    const int tickWindow = 200;
    int currentScrollTick = ImGui::GetScrollX();
    const u32 tickStart =
        ImClamp(currentScrollTick - tickWindow, 0, lastTriggerTick);
    const u32 tickEnd =
        ImClamp(currentScrollTick + tickWindow, 0, lastTriggerTick);

    orig.x += currentScrollTick;

    ImVec2 prevHoldTriggerPos;
    bool hasPrevHoldTrigger = false;

    for (u32 i = 0; i < track.triggerCount; ++i) {
      const Trigger &t = track.triggers[i];

      if (t.tick < tickStart || t.tick > tickEnd)
        continue;

      ImColor col;
      if (t.type == Trigger::Touch)
        col = touchTriggerColor;
      else if (t.type == Trigger::Slide)
        col = slideTriggerColor;
      else if (t.type == Trigger::Hold || t.type == Trigger::Holdlet ||
               t.type == Trigger::HoldEnd || t.type == Trigger::HoldEndSlide)
        col = holdTriggerColor;
      else if (t.type == Trigger::TrackGuide)
        col = trackGuideColor;
      else
        col = unknownColor;

      s32 triggerX;
      s32 triggerY;
      if (t.type == Trigger::TrackGuide) {
        triggerX = t.x;
        triggerY = t.y;
      } else {
        Trigger *prevTrackGuide = nullptr;
        for (int j = i - 1; j >= 0; --j) {
          if (track.triggers[j].type == Trigger::TrackGuide) {
            prevTrackGuide = &track.triggers[j];
            break;
          }
        }
        ENSURE(prevTrackGuide != nullptr);

        Trigger *nextTrackGuide = nullptr;
        for (u32 j = i + 1; j < track.triggerCount; ++j) {
          if (track.triggers[j].type == Trigger::TrackGuide) {
            nextTrackGuide = &track.triggers[j];
            break;
          }
        }
        ENSURE(nextTrackGuide != nullptr);

        const float r = float(t.tick - prevTrackGuide->tick) /
                        (nextTrackGuide->tick - prevTrackGuide->tick);
        triggerX = ImLerp(prevTrackGuide->x, nextTrackGuide->x, r);
        triggerY = ImLerp(prevTrackGuide->y, nextTrackGuide->y, r);
      }

      const int posy = orig.y + 100 - triggerY;
      const int posx = orig.x + 200 + triggerX;

      // Draw track guide
      if (t.type == Trigger::TrackGuide) {
        Trigger *prevTrackGuide = nullptr;
        for (int j = i - 1; j >= 0; --j) {
          if (track.triggers[j].type == Trigger::TrackGuide) {
            prevTrackGuide = &track.triggers[j];
            break;
          }
        }
        if (prevTrackGuide != nullptr) {
          const ImVec2 prev = ImVec2(orig.x + 200 + prevTrackGuide->x,
                                     orig.y + 100 - prevTrackGuide->y);
          const ImVec2 pos = ImVec2(posx, posy);

          if (prevTrackGuide->flags & Trigger::Flag::CurveInward ||
              prevTrackGuide->flags & Trigger::Flag::CurveOutward) {
            const ImVec2 v0 = prev;
            const ImVec2 v1 = pos;
            const ImVec2 normal =
                ImRotate((v1 - v0), cosf(IM_PI / 2), sinf(IM_PI / 2));
            const float reverse =
                prevTrackGuide->flags & Trigger::Flag::CurveInward ? -1.0f
                                                                   : 1.0f;
            const ImVec2 cp = v0 + (v1 - v0) / 2 + (normal * scale * reverse);
            drawList->AddBezierQuadratic(v0, cp, v1, trackGuideColor, 1.0f, 0);
          } else {
            drawList->AddLine(prev, pos, trackGuideColor, 1.0f);
          }
        }
      }

      // Draw line from previous hold trigger
      if (hasPrevHoldTrigger && !hideTriggers &&
          (t.type == Trigger::HoldEnd || t.type == Trigger::HoldEndSlide ||
           t.type == Trigger::Holdlet)) {
        drawList->AddLine(prevHoldTriggerPos, ImVec2(posx, posy), holdLineColor,
                          10.0f);
      }
      if (t.type == Trigger::Hold || t.type == Trigger::HoldEnd ||
          t.type == Trigger::HoldEndSlide || t.type == Trigger::Holdlet) {
        prevHoldTriggerPos = ImVec2(posx, posy);
        hasPrevHoldTrigger = true;
      }

      // Draw trigger circle
      float radius = triggerRadius;
      if (t.type == Trigger::Holdlet)
        radius /= 2.0f;
      else if (t.type == Trigger::TrackGuide)
        radius = 3.0f;

      if (t.type == Trigger::TrackGuide) {
        drawList->AddCircleFilled(ImVec2(posx, posy), radius, col);
      } else {
        if (hideTriggers)
          continue;

        if (t.type == Trigger::Holdlet || t.type == Trigger::HoldEnd ||
            t.type == Trigger::HoldEndSlide)
          drawList->AddCircleFilled(ImVec2(posx, posy), radius, col);
        else
          drawList->AddCircle(ImVec2(posx, posy), radius, col, 0, 8.0f);
      }

      // Draw slide direction
      // FIXME: in EMS, angle is automatic unless the absolute angle flag is
      // set
      if (t.type == Trigger::Slide || t.type == Trigger::HoldEndSlide) {
        Trigger *nextTrackGuide = nullptr;
        for (u32 j = i + 1; j < track.triggerCount; ++j) {
          if (track.triggers[j].type == Trigger::TrackGuide) {
            nextTrackGuide = &track.triggers[j];
            break;
          }
        }
        ENSURE(nextTrackGuide != nullptr);

        const ImVec2 vec = ImVec2(orig.x + 200 + nextTrackGuide->x,
                                  orig.y + 100 - nextTrackGuide->y) -
                           ImVec2(posx, posy);

        float a;
        if (t.flags & Trigger::Flag::AbsoluteAngle) {
          a = 0.0f; // TODO:
        } else {
          a = atan2f(vec.y, vec.x);
        }
        const ImVec2 center = ImVec2(posx, posy);
        const ImVec2 dir = ImRotate(ImVec2(12.0f, 0.0f), cosf(a), sinf(a));
        drawList->AddCircleFilled(center + dir, 5.0f,
                                  ImColor(1.0f, 1.0f, 1.0f));
      }

      // Tooltip
      const ImRect bb(posx - radius, posy - radius, posx + radius,
                      posy + radius);

      if (ImGui::IsMouseHoveringRect(bb.Min, bb.Max)) {
        ImGui::BeginTooltip();
        ImGui::Text("%d %s (%d)", i, TRIGGER_TYPE_NAMES[t.type], t.type);
        ImGui::Text("tick: %u", t.tick);
        ImGui::Text("x: %d", t.x);
        ImGui::Text("y: %d", t.y);
        ImGui::Text("angle: %d", t.angle);
        ImGui::Text("flags: %04x", t.flags);
        if (t.flags & Trigger::Flag::AbsoluteAngle)
          ImGui::Text("Absolute angle");
        if (t.flags & Trigger::Flag::CurveInward)
          ImGui::Text("Curve inward");
        if (t.flags & Trigger::Flag::CurveOutward)
          ImGui::Text("Curve outward");
        ImGui::EndTooltip();
      }
    }

    ImGui::EndChild();
  } else {
    const int windowWidth = ImGui::GetWindowContentRegionWidth();
    const int windowHeight = 400;
    const int contentWidth = track.tickCount * scaleX;

    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 30.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.17f, 0.20f, 0.22f, 1.0f));
    ImGui::SetNextWindowContentSize(ImVec2(contentWidth, 0));

    static bool scrollFuse = true;
    if (scrollFuse) {
      ImGui::SetNextWindowScroll(ImVec2(contentWidth, 0));
      scrollFuse = false;
    }

    ImGui::BeginChild("Track", ImVec2(windowWidth, windowHeight), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (editor.isAudioPlaying || editor.isSeeking) {
      const float currentTickScreenOffset = windowWidth * 3.0f / 4.0f;
      ImGui::SetScrollX(contentWidth - currentTick * scaleX -
                        currentTickScreenOffset);
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const ImGuiWindow *window = ImGui::GetCurrentWindow();
    const ImVec2 orig = window->DC.CursorPos;

    ImVec2 prevHoldTriggerPos;
    float prevHoldTriggerTick;

    // Feature zone
    drawList->AddRectFilled(
        orig + ImVec2(contentWidth - track.featureZoneStart * scaleX, 0),
        orig +
            ImVec2(contentWidth - track.featureZoneEnd * scaleX, windowHeight),
        featureZoneColor);

    // Summon
    drawList->AddRectFilled(
        orig + ImVec2(contentWidth - track.summonStart * scaleX, 0),
        orig + ImVec2(contentWidth - track.summonEnd * scaleX, windowHeight),
        summonColor);

    const int laneHeight = track.isBMS() ? 60 : 1;
    bool mouseOverTrigger = false;

    const u32 tickAtScrollBegin = (contentWidth - ImGui::GetScrollX()) / scaleX;
    const u32 tickAtScrollEnd =
        (contentWidth - (ImGui::GetScrollX() + ImGui::GetWindowWidth())) /
        scaleX;
    const u32 slack = 20;
    const u32 cullTickMin =
        tickAtScrollEnd > slack ? tickAtScrollEnd - slack : 0;
    const u32 cullTickMax = tickAtScrollBegin + slack;

    for (u32 i = 0; i < track.triggerCount; ++i) {
      const Trigger &t = track.triggers[i];

      ImColor col;
      if (t.type == Trigger::Touch)
        col = touchTriggerColor;
      else if (t.type == Trigger::Slide)
        col = slideTriggerColor;
      else if (t.type == Trigger::Hold || t.type == Trigger::Holdlet ||
               t.type == Trigger::HoldEnd || t.type == Trigger::HoldEndSlide)
        col = holdTriggerColor;
      else
        col = unknownColor;

      // Highlight
      if (editor.isAudioPlaying && fabs((float)t.tick - currentTick) < 4.0f) {
        col = currentlyPlayingColor;
      }

      const int posy = orig.y + 100 + t.y * laneHeight;
      const int posx = orig.x + contentWidth - (t.tick * scaleX);

      // Draw line from previous hold trigger
      if (t.type == Trigger::HoldEnd || t.type == Trigger::HoldEndSlide ||
          t.type == Trigger::Holdlet) {

        ImColor c = holdLineColor;
        if (editor.isAudioPlaying && currentTick > prevHoldTriggerTick &&
            currentTick < t.tick)
          c = currentlyPlayingColor;

        drawList->AddLine(prevHoldTriggerPos - ImVec2(0.5f, 0.5f),
                          ImVec2(posx, posy) - ImVec2(0.5f, 0.5f), c, 10.0f);
      }
      if (t.type == Trigger::Hold || t.type == Trigger::Holdlet) {
        prevHoldTriggerPos = ImVec2(posx, posy);
        prevHoldTriggerTick = t.tick;
      }

      // Cull out of view triggers
      if (t.tick > cullTickMax || t.tick < cullTickMin)
        continue;

      // Draw trigger circle
      float radius = triggerRadius;
      if (t.type == Trigger::Holdlet)
        radius /= 2.0f;

      if (t.type == Trigger::Holdlet || t.type == Trigger::HoldEnd ||
          t.type == Trigger::HoldEndSlide)
        drawList->AddCircleFilled(ImVec2(posx, posy), radius, col);
      else
        drawList->AddCircle(ImVec2(posx, posy), radius, col, 0, 8.0f);

      if (editor.isTriggerSelected(t.id))
        drawList->AddCircle(ImVec2(posx, posy), radius * 2,
                            ImColor(1.0f, 1.0f, 1.0f), 0, 2.0f);

      // Draw slide arrow
      if (t.type == Trigger::Slide || t.type == Trigger::HoldEndSlide) {
        const float a = (t.angle + 180.0f) * 2.0f * IM_PI / 360.0f;
        const ImVec2 center = ImVec2(posx, posy);
        const ImVec2 dir = ImRotate(ImVec2(0, 12.0f), cosf(a), sinf(a));
        const ImVec2 head0 = ImRotate(ImVec2(-6.0f, 12.0f), cosf(a), sinf(a));
        const ImVec2 head1 =
            ImRotate(ImVec2(0.0f, 12.0f + 10.0f), cosf(a), sinf(a));
        const ImVec2 head2 = ImRotate(ImVec2(+6.0f, 12.0f), cosf(a), sinf(a));

        drawList->AddLine(center - ImVec2(0.5f, 0.5f),
                          center + dir - ImVec2(0.5f, 0.5f),
                          slideTriggerArrowColor, 3.0f);
        drawList->AddTriangleFilled(center + head0, center + head1,
                                    center + head2, slideTriggerArrowColor);
      }

      // Tooltip
      const ImRect bb(posx - radius, posy - radius, posx + radius,
                      posy + radius);

      if (ImGui::IsMouseHoveringRect(bb.Min, bb.Max)) {
        ImGui::BeginTooltip();
        ImGui::Text("idx: %d id: %d %s", i, t.id, TRIGGER_TYPE_NAMES[t.type]);
        ImGui::Text("tick: %u", t.tick);
        ImGui::Text("y: %d", t.y);
        ImGui::Text("angle: %d", t.angle);
        ImGui::EndTooltip();

        if (ImGui::IsMouseClicked(0)) {
          bool append = getKey(GLFW_KEY_LEFT_SHIFT) == DOWN;
          editor.selectTrigger(t.id, append);
        }

        mouseOverTrigger = true;

        if (ImGui::IsMouseClicked(1)) {
          // Remove trigger
          if (editor.isTriggerSelected(t.id))
            editor.unselectTrigger(t.id);
          track.triggers.erase(track.triggers.begin() + i);
          track.triggerCount--;
          editor.trackModified = true;
        }
      }
    }

    // Draw new trigger under cursor
    if (ImGui::IsWindowHovered() && !mouseOverTrigger) {
      ImVec2 mouseRelPos = ImGui::GetMousePos() - orig;
      // Snap to nearest lane
      int nearestLane = ((mouseRelPos.y - 100.0f) / (float)laneHeight) + 0.5f;
      nearestLane = ImClamp(nearestLane, 0, 3);
      ImVec2 overlayPos = ImVec2(mouseRelPos.x, nearestLane * laneHeight + 100);
      drawList->AddCircle(orig + overlayPos, triggerRadius,
                          touchTriggerColorTransparent, 0, 8.0f);

      int newTick = (contentWidth - overlayPos.x) / scaleX;

      ImGui::BeginTooltip();
      ImGui::Text("%d", newTick);
      ImGui::EndTooltip();

      // Add trigger
      if (ImGui::IsMouseClicked(0)) {
        Trigger t;
        t.type = Trigger::Type::Touch;
        t.tick = newTick;
        t.x = 0;
        t.y = nearestLane;
        t.angle = 0;
        t.flags = Trigger::Flag::None;
        t.id = genId();

        track.triggers.push_back(t);
        track.triggerCount++;

        editor.trackModified = true;
        editor.shouldSortTriggers = true;
        editor.selectTrigger(t.id, false);
      }
    }

    // Draw currently playing tick
    {
      const ImColor playingColor = ImColor(1.0f, 1.0f, 1.0f);
      const ImColor pausedColor = ImColor(0.5f, 0.5f, 0.5f);
      const ImColor color = editor.isAudioPlaying ? playingColor : pausedColor;
      const float tickPos = contentWidth - currentTick * scaleX;
      const int x = roundf(tickPos);

      drawList->AddLine(orig + ImVec2(x, 0), orig + ImVec2(x, windowHeight),
                        color);
    }

    ImGui::EndChild();
  }
}

} // namespace rideau

int main(int argc, char *argv[]) {
  using namespace rideau;

  int opt;
  bool batchMode = false;

  while ((opt = getopt(argc, argv, "b")) != -1) {
    switch (opt) {
    case 'b':
      batchMode = true;
      break;
    default:
      fprintf(stderr, "Usage: %s [-b] TRIGGER_FILE MUSIC_FILE\n", argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  if (argc - optind < 2) {
    fprintf(stderr, "Usage: %s [-b] TRIGGER_FILE MUSIC_FILE\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  const char *const triggerFile = argv[optind];
  const char *const musicFile = argv[optind + 1];

  Brstm brstm;
  {
    u8 ret = parseBRSTM(musicFile, &brstm);
    ENSURE(ret < 128);
  }

  Editor editor;
  editor.init(&brstm);

  AudioStuff audioStuff;

  {
    int ret = initAudio(editor.sampleRate, &editor, audioStuff);
    ENSURE(ret == 0);
  }

  Track track;
  parseTrackFile(triggerFile, &track);
  checkTrack(track);

  if (batchMode) {
    printTrackStats(track);
    return 0;
  }

  // Fix tickCount to 59.825 TPS
  const u32 newTickCount =
      59.825f * ((float)editor.framesCount / editor.sampleRate);
  if (track.tickCount != newTickCount) {
    track.tickCount = newTickCount;
    track.tickEnd = track.tickCount;
    editor.trackModified = true;
  }

  // Init video
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // GL 3.0 + GLSL 130
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_SAMPLES, 8);

  const char window_title[128] = {};
  snprintf(const_cast<char *>(window_title), ARRAY_SIZE(window_title),
           "rideau %d.%d", RIDEAU_VERSION_MAJOR, RIDEAU_VERSION_MINOR);

  const int window_width = 800;
  const int window_height = 600;

  GLFWwindow *window = glfwCreateWindow(window_width, window_height,
                                        window_title, nullptr, nullptr);
  if (window == nullptr)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // Enable vsync

  if (gladLoadGL() == 0) {
    fprintf(stderr, "Failed to initialize OpenGL loader!\n");
    return 1;
  }

  ImGui::CreateContext();
  ImVec4 clear_color = ImVec4(0.f, 0.f, 0.f, 0.f);

  // Setup Platform/Renderer bindings
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  ImGui::GetStyle().Colors[ImGuiCol_WindowBg] =
      ImVec4(0.15f, 0.17f, 0.18f, 1.0f);

  glfwMakeContextCurrent(window);

  editor.initWaveformTexture();

  // Input
  glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
  clearKeys();

  auto lastLoopTime = std::chrono::high_resolution_clock::now();

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    updateKeys(window);

    auto loopTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::micro> loopDtUs =
        (loopTime - lastLoopTime);
    lastLoopTime = loopTime;
    u32 loopUs = static_cast<u32>(loopDtUs.count());

    {
      if (getKey(GLFW_KEY_ESCAPE) == DOWN)
        glfwSetWindowShouldClose(window, GLFW_TRUE);

      if (getKey(GLFW_KEY_SPACE) == PRESSED)
        editor.togglePause();

      if (getKey(GLFW_KEY_S) == PRESSED &&
          getKey(GLFW_KEY_LEFT_CONTROL) == DOWN) {
        writeTrackFile(track, triggerFile);
        editor.trackModified = false;
      }
    }

    if (editor.isAudioPlaying) {
      const float usToSec = 1e-6f;
      editor.estimatedCurrentFrame += loopUs * usToSec * editor.sampleRate;
    }

    if (editor.shouldSortTriggers) {
      // Keep the triggers sorted by increasing tick
      std::sort(track.triggers.begin(), track.triggers.end(),
                [&](Trigger &a, Trigger &b) { return a.tick < b.tick; });
    }
    if (editor.isSeeking)
      editor.isSeeking = false;

    // Draw
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0f, display_w, display_h, 0.0f, 0.0f, 1.0f);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw ImGui
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    {
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImVec2(display_w, display_h));
      ImGui::Begin("Main", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoBringToFrontOnFocus);

      const ImColor headerColor = ImColor(0.6f, 0.8f, 1.0f);

      {
        ImGui::BeginGroup();
        ImGui::TextColored(headerColor, "Track info");

        ImGui::Text("Type: ");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(0.5f, 1.0f, 0.5f));
        if (ImGui::RadioButton("FMS", track.isFMS())) {
          track.trackType = Track::Type::FMS;
          editor.trackModified = true;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(1.0f, 0.5f, 0.5f));
        if (ImGui::RadioButton("BMS", track.isBMS())) {
          track.trackType = Track::Type::BMS;
          editor.trackModified = true;
        }
        ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(0.3f, 0.7f, 1.0f));
        if (ImGui::RadioButton("EMS", track.isEMS())) {
          track.trackType = Track::Type::EMS;
          editor.trackModified = true;
        }
        ImGui::PopStyleColor();

        ImGui::Text("Triggers: %d", track.triggerCount);

        ImGui::Text("Ticks: ");
        ImGui::SameLine();
        ImGui::Text("%d", track.tickCount);

        const float ticksPerSecond =
            track.tickCount / ((float)editor.framesCount / editor.sampleRate);
        ImGui::SameLine();
        ImGui::Text("TPS: %.3f", ticksPerSecond);

        ImGui::Text("Feature Zone: ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::SliderInt("##featureZoneStart",
                             (int *)&track.featureZoneStart, track.tickStart,
                             track.featureZoneEnd)) {
          editor.trackModified = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::SliderInt("##featureZoneEnd", (int *)&track.featureZoneEnd,
                             track.featureZoneStart, track.tickEnd)) {
          editor.trackModified = true;
        }

        ImGui::Text("Summon: ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::SliderInt("##summonStart", (int *)&track.summonStart,
                             track.tickStart, track.summonEnd)) {
          editor.trackModified = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::SliderInt("##summonEnd", (int *)&track.summonEnd,
                             track.summonStart, track.tickEnd)) {
          editor.trackModified = true;
        }

        ImGui::EndGroup();
      }

      ImGui::SameLine();

      // Edit trigger
      {
        ImGui::BeginGroup();
        ImGui::TextColored(headerColor, "Trigger info");

        if (editor.selectedTriggers.empty()) {
          ImGui::Text("Select a trigger to edit it");

          if (ImGui::Button("Select all")) {
            for (u32 i = 0; i < track.triggers.size(); ++i)
              editor.selectTrigger(track.triggers[i].id, true);
          }

        } else if (editor.selectedTriggers.size() > 1) {
          ImGui::Text("%zu triggers selected", editor.selectedTriggers.size());

          std::vector<u32> toErase;
          toErase.reserve(editor.selectedTriggers.size());

          Trigger *rep = nullptr;
          for (u32 i = 0; i < track.triggers.size(); ++i) {
            if (editor.isTriggerSelected(track.triggers[i].id)) {
              rep = &track.triggers[i];
              break;
            }
          }

          if (ImGui::Button("Delete selected")) {
            for (u32 i = 0; i < track.triggers.size();) {
              if (editor.isTriggerSelected(track.triggers[i].id)) {
                track.triggers.erase(track.triggers.begin() + i);
                track.triggerCount--;
              } else {
                ++i;
              }
            }
            editor.trackModified = true;
            editor.unselectAllTriggers();
          }

          ImGui::SetNextItemWidth(100.0f);
          if (ImGui::SliderInt("Lane", &rep->y, 0, 3)) {
            for (u32 i = 0; i < track.triggers.size(); ++i) {
              if (editor.isTriggerSelected(track.triggers[i].id)) {
                track.triggers[i].y = rep->y;
              }
            }
            editor.trackModified = true;
          }

        } else {
          ASSERT(editor.selectedTriggers.size() == 1);

          Trigger *t = nullptr;
          u32 selectedTriggerIndex;
          for (u32 i = 0; i < track.triggers.size(); ++i) {
            if (editor.isTriggerSelected(track.triggers[i].id)) {
              t = &track.triggers[i];
              selectedTriggerIndex = i;
              break;
            }
          }
          ASSERT(t != nullptr);

          if (ImGui::Button("Delete")) {
            editor.unselectTrigger(t->id);
            track.triggers.erase(track.triggers.begin() + selectedTriggerIndex);
            track.triggerCount--;
            editor.trackModified = true;
          }

          for (u32 i = 0; i < Trigger::Type::TrackGuide; ++i) {
            if (i > 0)
              ImGui::SameLine();
            if (ImGui::RadioButton(TRIGGER_TYPE_NAMES[i], t->type == i)) {
              t->type = (Trigger::Type)i;
              editor.trackModified = true;
            }
          }

          if (ImGui::SliderInt("Tick", (int *)&t->tick, track.tickStart,
                               track.tickEnd)) {
            editor.shouldSortTriggers = true;
            editor.trackModified = true;
          }
          ImGui::SetNextItemWidth(100.0f);
          if (ImGui::SliderInt("Lane", &t->y, 0, 3)) {
            editor.trackModified = true;
          }
          if (t->type == Trigger::Slide || t->type == Trigger::HoldEndSlide) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::SliderInt("Angle", (int *)&t->angle, 0, 360)) {
              editor.trackModified = true;
            }
          }
        }

        ImGui::EndGroup();
      }

      {
        ImGui::BeginGroup();

        const bool colorButton = editor.trackModified;
        if (colorButton) {
          ImGui::PushStyleColor(ImGuiCol_Button,
                                (ImVec4)ImColor::HSV(0, 0.6f, 0.6f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                (ImVec4)ImColor::HSV(0, 0.7f, 0.7f));
          ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                (ImVec4)ImColor::HSV(0, 0.8f, 0.8f));
        }
        if (ImGui::Button("Save track")) {
          writeTrackFile(track, triggerFile);
          editor.trackModified = false;
        }
        if (colorButton)
          ImGui::PopStyleColor(3);

        ImGui::SameLine();

        const char *playLabel = editor.isAudioPlaying ? "Pause" : "Play";
        if (ImGui::Button(playLabel))
          editor.togglePause();

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        float volumeDb = 10 * log10f(editor.audioVolume);
        if (ImGui::SliderFloat("Volume", &volumeDb, -50.0f, 0.0f)) {
          editor.audioVolume = powf(10.0f, volumeDb / 10.0f);
        }

        ImGui::EndGroup();
      }

      drawTrack(track, editor);

      ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Swap
    glfwSwapBuffers(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  deinitAudio(audioStuff);

  brstm_close(&brstm);

  return 0;
}
