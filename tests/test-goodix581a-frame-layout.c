/* Goodix 27c6:581a frame layout unit test */

#include <glib.h>

#include "drivers/goodixtls/goodix581a.h"

static void
test_stream_to_image_index (void)
{
  g_assert_cmpuint (GOODIX_581A_WIDTH, ==, GOODIX_581A_STREAM_COLUMNS);
  g_assert_cmpuint (GOODIX_581A_HEIGHT, ==, GOODIX_581A_STREAM_ROWS);
  g_assert_cmpuint (GOODIX_581A_FRAME_SIZE, ==,
                    GOODIX_581A_STREAM_ROWS * GOODIX_581A_STREAM_COLUMNS);

  /* Stream samples advance down each 64-pixel column before the next column. */
  g_assert_cmpuint (goodix_581a_stream_to_image_index (0), ==, 0);
  g_assert_cmpuint (goodix_581a_stream_to_image_index (1), ==,
                    GOODIX_581A_WIDTH);
  g_assert_cmpuint (goodix_581a_stream_to_image_index (63), ==,
                    63 * GOODIX_581A_WIDTH);
  g_assert_cmpuint (goodix_581a_stream_to_image_index (64), ==, 1);
  g_assert_cmpuint (goodix_581a_stream_to_image_index (5119), ==,
                    GOODIX_581A_FRAME_SIZE - 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix581a/stream-to-image-index",
                   test_stream_to_image_index);

  return g_test_run ();
}
