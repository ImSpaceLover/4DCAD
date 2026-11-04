// Keyframe animation: tracks of keyed values (one per animated value of an object or light),
// per-key interpolation, 4D rotation interpolation along the shortest rotation path.
#pragma once
#include <string>
#include <vector>

#include "scene.hpp"

namespace fd {

/** How a key's value goes over to the next key's value. */
enum class Interp { Constant, Linear, Bezier, EaseIn, EaseOut, EaseInOut, Count };
inline const char* INTERP_NAMES[(int)Interp::Count] = {"Constant", "Linear", "Bezier", "Ease in", "Ease out", "Ease in-out"};

struct Key {
  double frame = 1;
  double value = 0;      // scalar tracks
  M4 rot = identity();   // rotation tracks
  Interp interp = Interp::Bezier;
  // bezier handles relative to the key, in (frames, value); automatic until edited
  double lx = -1, ly = 0, rx = 1, ry = 0;
  bool autoHandles = true;
  bool selected = false;  // UI state (not saved)
};

/** All keys of one value. target: node id, or "light:<id>". path: see animatableValues(). */
struct Track {
  std::string target, path;
  std::vector<Key> keys;  // sorted by frame
  // before the first / after the last key: hold the value (false), or carry on with the curve's
  // slope there (true, Blender's linear extrapolation; rotations keep turning)
  bool linearExtrap = false;
  bool rotation() const { return path == "rot"; }
};

struct Animation {
  double fps = 24;
  int start = 1, end = 120;
  double frame = 1;  // current frame
  std::vector<Track> tracks;
  bool empty() const { return tracks.empty(); }
};

// ---- values that can be animated ----
struct AnimValue {
  std::string path, label;
  bool stepped = false;  // integer / on-off: no interpolation between keys
};
/** Every animatable scalar value of a node (plus "rot" for its rotation), for keying "all values". */
std::vector<AnimValue> animatableValues(const Node& n);
std::vector<AnimValue> animatableLightValues();
/** Transform values of a node (position, scale, rotation). */
std::vector<std::string> transformPaths();
bool getValue(const Node& n, const std::string& path, double& v);
bool setValue(Node& n, const std::string& path, double v);
bool getLightValue(const PointLight& l, const std::string& path, double& v);
bool setLightValue(PointLight& l, const std::string& path, double v);
bool isSteppedPath(const std::string& path);

// ---- evaluation ----
/** Value of a scalar track at a frame (holds the first / last key's value outside them). */
double evalTrack(const Track& t, double frame);
/** Rotation of a rotation track at a frame. */
M4 evalRotation(const Track& t, double frame);
/** 0..1 progress between two keys after the first key's interpolation (non-bezier modes). */
double easeProgress(Interp mode, double u);
/** Shortest-path interpolation between two 4D rotations (u = 0..1). */
M4 interpolateRotation(const M4& a, const M4& b, double u);
/** Recomputes automatic bezier handles (smooth, clamped at extremes) of a scalar track. */
void autoHandles(Track& t);

// ---- editing ----
Track* findTrack(Animation& a, const std::string& target, const std::string& path);
/** Inserts (or replaces) a key at the frame; sorts; returns the track. */
Track& setKey(Animation& a, const std::string& target, const std::string& path, double frame, double value, Interp interp = Interp::Bezier);
Track& setRotKey(Animation& a, const std::string& target, double frame, const M4& rot, Interp interp = Interp::Bezier);
/** Key every animatable value (or only the transform) of a node at the frame. */
void keyNode(Animation& a, const Node& n, double frame, bool transformOnly);
void keyLight(Animation& a, const PointLight& l, double frame);
bool hasKeyAt(const Animation& a, const std::string& target, const std::string& path, double frame);
bool isAnimated(const Animation& a, const std::string& target, const std::string& path);
void deleteKey(Animation& a, const std::string& target, const std::string& path, double frame);
/** Removes tracks of objects / lights that no longer exist and tracks without keys. */
void pruneTracks(Animation& a, const Node& root, const std::vector<PointLight>& lights);
void sortKeys(Track& t);

/** Sets every animated value to its value at the frame. Rotations of nodes attached to a world
 *  surface are their full frame (anchor basis x rotation): see App::applyAnimation. */
void applyAnimation(const Animation& a, Node& root, std::vector<PointLight>& lights, double frame);

// ---- JSON (in the scene file and the undo snapshot) ----
std::string animationToJson(const Animation& a);
Animation animationFromJson(const std::string& text);

}  // namespace fd
