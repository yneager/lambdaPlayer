#pragma once
#include <algorithm>
#include <cmath>

// Commit picture identities only at a completed window swap. Multiple paints
// before one swap count once; redrawing a held picture never adds a frame.
class VideoPresentationTracker
{
public:
    bool swapped(double picture) {
        if (!std::isfinite(picture) || picture < 0 || picture == lastPicture_) return false;
        lastPicture_ = picture;
        return true;
    }
    void reset() { lastPicture_ = -1; }
private:
    double lastPicture_ = -1;
};

inline double reusableOutputRate(double source, double requested, double refresh, double scale)
{
    const double target = std::max(source, std::min(requested, refresh));
    // Preserve normal 2x/60 FPS. Higher targets may shed generated frames
    // under sustained pressure, but never collapse to the source cadence.
    const double floor = std::min(target, std::max(source * 2.0, 60.0));
    return std::max(floor, target * scale);
}
