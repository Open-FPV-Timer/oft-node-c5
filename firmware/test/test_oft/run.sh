#!/bin/sh
# Host test for the OFT Node-link codec. Needs only a C compiler.
set -e
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror -O1 -I../../src/proto -I. test_frames.c ../../src/proto/oft_frame.c -o /tmp/test_oft_frames
/tmp/test_oft_frames
