/* The reference: TB-3PO's DSP exactly as the TB-3PO tool builds it (its own
 * move_plugin_init_v2, its file, its thread). tests/test_engine.c runs it
 * beside dsp/tb_engine.c and compares their MIDI. TB3PO_STATE_DIR is pointed
 * at the build directory by scripts/test.sh. */
#include "tb3po/tb3po.c"
