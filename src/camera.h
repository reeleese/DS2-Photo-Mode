// The free camera's state, shared by camera.cpp (the game hook, the sticks) and ui.cpp
// (the panels). The render thread changes it; the game thread reads it in the hook, so
// both hold camera_lock() around any access.
#pragma once
#include "common.h"
#include "cammath.h"

namespace pm {

struct CamState {
    Vec3 pos{};                          // camera position (metres, Y up)
    float yaw = 0, pitch = 0, roll = 0;  // radians; yaw 0 looks along +Z
    float fov = 0.7679f;                 // vertical field of view, radians
    float speed = 4.0f;                  // move speed, m/s
    bool orbit = false;                  // orbit around the pivot instead of flying
    int flight = FLIGHT_DEFAULT;         // Default; 6DOF: relative to the view, no fixed horizon;
                                         // Seamoth: momentum and water drag, level horizon
    Vec3 vel{};                          // Seamoth: velocity, m/s
    float yaw_rate = 0, pitch_rate = 0;  // Seamoth: the eased look, rad/s
    // Orbit and a flight mode other than Default are one-at-a-time: each setter turns the other off.
    Vec3 pivot{};                        // what orbit circles (the character)
    float orbit_dist = 3.5f;
};

const float SENSOR_H_MM = 24.0f;  // full-frame sensor height: focal length <-> vertical FOV
inline float fov_to_mm(float fov) { return SENSOR_H_MM / 2 / tanf(fov / 2); }
inline float mm_to_fov(float mm) { return 2 * atanf(SENSOR_H_MM / 2 / mm); }

void camera_lock();
void camera_unlock();
CamState& camera_state();      // only under camera_lock()
bool camera_hooked();          // the camera hook is in place
void camera_reset();           // back to the game's camera (under camera_lock())
void camera_set_orbit(bool on);    // under camera_lock()
void camera_set_flight(int f);     // under camera_lock(); a Flight
Vec3 cam_forward(const CamState& c);

}  // namespace pm
