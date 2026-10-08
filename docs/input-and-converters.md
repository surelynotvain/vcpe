# Input and converter practices

## Fixed updates and sampled buttons

`src/vcpe_input.h` is portable C and requires no PSP libraries. Keep one
`VcpeInput` per logical controller. Call `vcpe_input_sample(&input, buttons)`
once per displayed frame. Each simulation update reads `input.held` and calls
`vcpe_input_pressed(&input)` for newly pressed buttons. Edges accumulate while
no update runs and are consumed by the first update when several catch up.
Call `vcpe_input_reset(&input, current_buttons)` on scene changes and resume to
clear pending actions without treating an already held button as a new press.

`vcpe_input_union` combines shared control. `vcpe_input_majority` accepts a
button only when more than half the controllers hold it; a tie stays released.
Compute aggregate press edges from the aggregate held state, rather than voting
on individual press edges. Controller arrays may use any 32-bit button layout.

## Source data and assets

Keep world coordinates in the source game's units; scale only at rendering.
Validate each selected map variant's dimensions and actor rows during conversion.
Preserve actor argument positions, including string arguments. A successful
conversion does not establish that gameplay matches its source: verify runtime
rules and recorded scenes independently.

`tools/vcpe_tga.py` loads indexed/RLE TGA files with a 32-bit palette as well as
true-colour and greyscale files. It retains palette alpha, honours all image
origins and rejects truncated data, oversized packets and invalid indices.
It returns a Pillow RGBA image. Usage:

```python
from vcpe_tga import load
atlas = load("atlas.tga")
```

Use synthetic fixtures for public converter tests. Keep proprietary assets,
source dumps, project-specific test captures and unpublished project details
outside the engine repository.

## Host checks

```sh
cc -std=c99 -Wall -Wextra -Werror -Isrc tests/test_input.c -o /tmp/vcpe-input-test
/tmp/vcpe-input-test
python3 -m unittest discover -s tests -p 'test_*.py'
```
