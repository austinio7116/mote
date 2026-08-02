/* Shared code spells the include both ways — <SDL.h> and <SDL2/SDL.h>,
 * depending on whether it was written against a vendored SDL drop or a distro's
 * pkg-config path. Both land here in the VR build; see ../SDL.h for why any of
 * this exists. */
#include "../SDL.h"
