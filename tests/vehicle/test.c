/* Host checks for vcpe_path/vcpe_vehicle: arc length, endpoint extension, heading convention,
 * rear-axle trailing on a corner, flat-road pitch, bumps fading when stopped. */
#include "vcpe_path.h"
#include "vcpe_vehicle.h"
#include "vcpe_math.h"
#include <assert.h>
#include <stdio.h>
static int near(float a, float b, float e) { return a - b < e && b - a < e; }
int main(void)
{
    vcpe_math_init();
    /* Straight 10 m north (-Z), then a 90 degree turn to the east (+X). */
    static const float pts[][3] = {{0,0,0},{0,0,-5},{0,0,-10},{5,0,-10},{10,0,-10},{20,0,-10},{30,0,-10}};
    VcpePath path;
    assert(vcpe_path_init(&path, pts, 7) == 0);
    assert(near(vcpe_path_length(&path), 40, 1e-4f));
    float p[3], yaw;
    vcpe_path_sample(&path, 2.5f, .5f, p, &yaw);
    /* Catmull-Rom eases inside end segments: distance is near, not exact, arc length there. */
    assert(near(p[2], -2.5f, .4f) && near(yaw, 0, .5f));             /* forward = -Z -> yaw 0 */
    vcpe_path_sample(&path, 17.5f, .5f, p, &yaw);
    assert(near(p[0], 7.5f, .4f) && near(yaw, -90, .5f));           /* heading east */
    vcpe_path_point(&path, 42, p);
    assert(near(p[0], 32, .05f));                                     /* extends past the end */
    VcpeVehicle v;
    vcpe_vehicle_init(&v, 3);
    vcpe_vehicle_update(&v, &path, 4, 1, 1);
    assert(near(v.yaw, 0, .5f) && near(v.pitch, 0, 1e-3f) && near(v.roll, 0, 1e-3f));
    float max_lag = 0;
    for (int t = 1; t <= 600; t++) {
        float d = 4 + t * .04f;  /* to 28 m: 18 m past the corner, six wheelbases */
        vcpe_vehicle_update(&v, &path, d, 1, 0);
        float path_yaw;vcpe_path_sample(&path, d, .5f, p, &path_yaw);
        float lag = vcpe_absf(vcpe_angle_diff_deg(v.yaw, path_yaw));
        if (lag > max_lag) max_lag = lag;
        float rx = v.pos[0] - v.rear[0], rz = v.pos[2] - v.rear[1];
        assert(near(rx * rx + rz * rz, 9, .01f));                    /* rigid wheelbase */
    }
    assert(max_lag > 10);                                             /* trails the corner */
    assert(near(v.yaw, -90, 2));                                      /* tractrix settles on the new heading */
    for (int t = 0; t < 200; t++) vcpe_vehicle_update(&v, &path, 28, 1, 0);
    assert(v.speed < .01f && near(v.roll, 0, .05f) && near(v.heave, 0, .001f));
    float m[16], q[3];const float o[3] = {1, 2, 3}, x[3] = {0, 0, -1};
    vcpe_matrix_ypr(m, o, -90, 0, 0);vcpe_matrix_point(m, x, q);
    assert(near(q[0], 2, 1e-4f) && near(q[1], 2, 1e-4f) && near(q[2], 3, 1e-4f));
    printf("PASS vcpe_path/vcpe_vehicle: arc length, heading, extension, trailing max %.1f deg, settle, stop\n", max_lag);
    return 0;
}
