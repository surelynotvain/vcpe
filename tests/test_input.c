#include <assert.h>
#include "vcpe_input.h"
int main(void)
{
    VcpeInput state;
    vcpe_input_reset(&state, 0);
    /* A tap on a display frame with no update must not vanish. */
    vcpe_input_sample(&state, 1);
    vcpe_input_sample(&state, 0);
    assert(state.held == 0);
    assert(vcpe_input_pressed(&state) == 1);
    assert(vcpe_input_pressed(&state) == 0);
    /* Two catch-up updates must not see the same jump twice. */
    vcpe_input_sample(&state, 2);
    assert(vcpe_input_pressed(&state) == 2);
    assert(vcpe_input_pressed(&state) == 0);
    vcpe_input_sample(&state, 2);
    assert(vcpe_input_pressed(&state) == 0);
    vcpe_input_reset(&state, 2);
    assert(vcpe_input_pressed(&state) == 0);
    uint32_t inputs[] = {1, 2, 1, 0x80000000u};
    assert(vcpe_input_union(inputs, 4) == 0x80000003u);
    assert(vcpe_input_majority(inputs, 2) == 0);
    assert(vcpe_input_majority(inputs, 3) == 1);
    assert(vcpe_input_majority(inputs, 4) == 0);
    assert(vcpe_input_majority(inputs + 3, 1) == 0x80000000u);
    assert(vcpe_input_majority(0, 0) == 0);
    assert(vcpe_input_union(0, 0) == 0);
    assert(vcpe_input_majority(0, -3) == 0);
    assert(vcpe_input_union(0, -3) == 0);
    return 0;
}
